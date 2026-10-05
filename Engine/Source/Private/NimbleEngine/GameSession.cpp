#include "Public/NimbleEngine/GameSession.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Public/NimbleEngine/GameConsole.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerState.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldSerializer.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/UI/UISystem.h"

/* Chunk tags shared with SaveGame.cpp. */
#include "Private/NimbleEngine/InputInternal.h"
#include "Private/NimbleEngine/SaveGameFormat.h"

/* Lua file run on every level load, after IGameMode::OnLoad (see LoadLevel).
 * Empty skips it. */
NIMBLE_CONFIG_STRING(GStartupScript, "Scripting", "StartupScript", "", nullptr);

namespace NimbleEngine
{
    namespace
    {
        /* World -> session, for FindGameSession. A handful at most (a game, or
         * the editor's Play), so a linear scan. Entries come and go with
         * LoadLevel / UnloadLevel, which is what makes a reused World address
         * safe. */
        struct WorldSessionEntry
        {
            const GameCore::World* World   = nullptr;
            GameSession*           Session = nullptr;
        };

        std::vector<WorldSessionEntry>& WorldSessions()
        {
            static std::vector<WorldSessionEntry> Entries;
            return Entries;
        }

        void AddWorldSession(const GameCore::World* InWorld, GameSession* InSession)
        {
            WorldSessions().push_back(WorldSessionEntry{ InWorld, InSession });
        }

        void RemoveWorldSession(const GameCore::World* InWorld)
        {
            std::vector<WorldSessionEntry>& Entries = WorldSessions();
            Entries.erase(std::remove_if(Entries.begin(), Entries.end(),
                                         [InWorld](const WorldSessionEntry& E) { return E.World == InWorld; }),
                          Entries.end());
        }

        GameCore::EventArgs MakeArgs(const GameCore::EventArg& InA, const GameCore::EventArg& InB)
        {
            GameCore::EventArgs Args;
            Args.Args[0] = InA;
            Args.Args[1] = InB;
            Args.Count   = 2;
            return Args;
        }
    }

    GameSession* FindGameSession(const GameCore::World& InWorld)
    {
        for(const WorldSessionEntry& Entry : WorldSessions())
        {
            if(Entry.World == &InWorld)
            {
                return Entry.Session;
            }
        }
        return nullptr;
    }

    GameSession::GameSession(IGameHost& InHost, GameCore::ScriptEngine& InScripts, RenderCore::Renderer& InRenderer)
        : GameThread(EngineCore::appThreadID())
        , Host(InHost)
        , Scripts(InScripts)
        , Rendering(InRenderer)
    {
        InstallCollisionMeshProvider();   // CS_Mesh colliders (CollisionMeshes.cpp)

        Interface = EngineCore::Memory::Alloc<UI::UISystem>();
        Interface->SetRenderer(&Rendering, RenderCore::RENDER_DEFAULT_SCENE);
        Interface->SetScriptEngine(&Scripts);
        Interface->SetHostData(this);
        Interface->SetClipboard([this]() { return Host.GetClipboardText(); },
                                [this](const std::string& InText) { Host.SetClipboardText(InText); });
        UI::SetScriptUISystem(Interface);

        Console = EngineCore::Memory::Alloc<GameConsole>(*this);
    }

    GameSession::~GameSession()
    {
        UnloadLevel();

        EngineCore::Memory::Dealloc(Console);
        Console = nullptr;

        /* Nothing of this session's stays drawn over the scene after it. */
        Interface->CloseAll(true);
        Interface->ClearRenderer();
        if(UI::GetScriptUISystem() == Interface) { UI::SetScriptUISystem(nullptr); }
        EngineCore::Memory::Dealloc(Interface);
        Interface = nullptr;
    }

    void GameSession::CheckGameThread(const char* InWhat) const
    {
        CORE_CHECK(EngineCore::appThreadID() == GameThread,
                   std::string("GameSession::") + InWhat + " called off the game thread (the one that made the session)")
    }

    bool GameSession::LoadLevel(ILevel& InLevel)
    {
        CheckGameThread("LoadLevel");
        CORE_CHECK(LevelWorld == nullptr, "GameSession::LoadLevel called with a level already loaded")

        /* Bound before OnLoad, or scripts and WorldBridge would miss the
         * level's first entities. */
        LevelWorld = EngineCore::Memory::Alloc<GameCore::World>();
        ++LoadCount;

        ActiveLevel             = &InLevel;
        ActiveLevel->OwnedWorld = LevelWorld;

        /* Before anything can run a script that asks for the rules. */
        AddWorldSession(LevelWorld, this);
        QueuedPhases.clear();
        bChangingPhase = false;

        Scripts.BindWorld(LevelWorld);
        Rendering.BindWorld(*LevelWorld);
        Interface->SetWorld(LevelWorld);   // Event: actions emit on its bus
        UI::SetScriptUISystem(Interface);
        bPaused = false;

        /* Players come and go as PlayerState components, from anywhere. */
        ComponentAddedHandle   = LevelWorld->OnComponentAdded().Add(&GameSession::OnComponentAdded, this);
        ComponentRemovedHandle = LevelWorld->OnComponentRemoved().Add(&GameSession::OnComponentRemoved, this);

        /* First thing in TP_PrePhysics, ahead of the phase's scripts, so every
         * script and system this frame reads this frame's input. */
        LevelWorld->RegisterSystem("Engine.Input", GameCore::TP_PrePhysics, &GameSession::ResolveInput, this,
                                   GameCore::SO_BeforeScripts);

        CORE_LOG("[Level] loading '" + std::string(InLevel.GetName()) + "'")

        if(!InLevel.OnLoad(*this))
        {
            CORE_LOG_ERROR("Level '" + std::string(InLevel.GetName()) + "' refused to load")
            return false;
        }

        /* After the level has populated the World. */
        IGameMode* Mode = InLevel.GetGameMode();
        if(Mode && !Mode->OnLoad(*this))
        {
            CORE_LOG_ERROR("Game mode refused to load")
            return false;
        }

        /* From here the mode hears of every player: those the level made, now,
         * and the rest as they come. */
        bModeLoaded = true;
        if(Mode)
        {
            std::vector<GameCore::EntityID> Players;
            LevelWorld->Each<PlayerStateComponent>([&Players](GameCore::Entity InEntity, PlayerStateComponent&)
            {
                Players.push_back(InEntity.GetID());
            });
            for(const GameCore::EntityID Player : Players)
            {
                Mode->OnPlayerAdded(Player);
            }
        }

        /* The script sees the loaded level; OnStart sees what it made. Failure
         * is logged, not fatal. Re-run per load: spawned entities died with the
         * old World. */
        const char* StartupScriptPath = InLevel.GetStartupScript();
        if(StartupScriptPath == nullptr)
        {
            StartupScriptPath = GStartupScript.GetString();
        }
        if(StartupScriptPath && StartupScriptPath[0] != '\0')
        {
            Scripts.RunFile(StartupScriptPath);
        }

        /* Before the restore and OnStart -- see the header's sequence. */
        GameState* State = InLevel.GetGameState();
        if(State)
        {
            State->OnReset();

            /* A reset, not a transition: the mode is not asked. */
            State->WritePhase(State->GetInitialPhase());
            State->SetRound(0);
        }

        /* Emptied first so a failed restore is not retried. */
        if(!PendingSaveBytes.empty())
        {
            std::vector<uint8_t> SaveBytes;
            SaveBytes.swap(PendingSaveBytes);

            /* The level clears what OnLoad and the script made (Level.h). */
            InLevel.OnPrepareForLoadedGame();

            EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::move(SaveBytes));

            if(RestoreSaveInto(Ar, InLevel, State))
            {
                CORE_LOG("[Level] restored a saved game into '" + std::string(InLevel.GetName()) + "'")

                if(Mode)
                {
                    /* Handles cached in OnLoad are stale; re-derive before OnStart. */
                    Mode->OnPostLoadSave();
                }
            }
            else
            {
                /* Left loaded and playable: a bad save costs progress, not the game. */
                CORE_LOG_ERROR("Failed to restore the saved game into '"
                               + std::string(InLevel.GetName()) + "'; continuing with a fresh level")
            }
        }

        if(Mode)
        {
            Mode->OnStart();
        }

        return true;
    }

    bool GameSession::RestoreSaveInto(EngineCore::Archive& Ar, ILevel& InLevel, GameState* InState)
    {
        /* World first, so the GameState can look entities up. */
        if(!GameCore::SerializeWorld(Ar, *LevelWorld))
        {
            return false;
        }

        /* Optional; when absent the state keeps its reset values. */
        uint16_t StateVersion = 0;
        if(Ar.OpenChunk(TAG_GAMESTATE, StateVersion))
        {
            if(InState)
            {
                /* Inside the chunk, so Ar.GetChunkVersion() is StateVersion. */
                InState->Serialize(Ar);
            }
            else
            {
                /* Skipped by chunk size, not fatal. */
                CORE_LOG_WARN("[Level] the save contains game state but '"
                              + std::string(InLevel.GetName()) + "' has none; skipping it")
            }
            Ar.CloseChunk();
        }

        /* The engine's half of the state; restored, not entered, so no hooks.
         * Older saves have none and keep the initial phase. */
        uint16_t PhaseVersion = 0;
        if(Ar.OpenChunk(TAG_GAMEPHASE, PhaseVersion))
        {
            std::string Phase;
            double      PhaseSeconds = 0.0;
            int32_t     Round        = 0;
            Ar << Phase;
            Ar << PhaseSeconds;
            Ar << Round;
            if(InState && !Ar.HasError())
            {
                InState->WritePhase(Phase.c_str());
                InState->PhaseSeconds = PhaseSeconds;
                InState->Round        = Round;
            }
            Ar.CloseChunk();
        }

        return !Ar.HasError();
    }

    void GameSession::UnloadLevel()
    {
        CheckGameThread("UnloadLevel");
        if(!ActiveLevel)
        {
            return;
        }

        /*
         *   1. mode, then level, OnUnload (World still live).
         *   2. Scripts.UnbindWorld(): runs OnDestroy, needs the World.
         *   3. Rendering.UnbindWorld(): before ~World, which warns otherwise.
         *   4. destroy the World.
         */
        bModeLoaded = false;
        if(IGameMode* Mode = ActiveLevel->GetGameMode())
        {
            Mode->OnUnload();
        }

        ActiveLevel->OnUnload();

        /* The level's documents go with it; a persistent one stays (UI::UISystem::Open). */
        Interface->CloseAll(false);
        Interface->RunQueuedActions();   // their OnClose, while the World is still here
        Interface->SetWorld(nullptr);
        bPaused = false;

        if(LevelWorld)
        {
            LevelWorld->OnComponentAdded().Remove(ComponentAddedHandle);
            LevelWorld->OnComponentRemoved().Remove(ComponentRemovedHandle);

            Scripts.UnbindWorld();
            Rendering.UnbindWorld();

            RemoveWorldSession(LevelWorld);
            QueuedPhases.clear();

            EngineCore::Memory::Dealloc(LevelWorld);
            LevelWorld = nullptr;
        }

        CORE_LOG("[Level] unloaded '" + std::string(ActiveLevel->GetName()) + "'")

        ActiveLevel->OwnedWorld = nullptr;
        ActiveLevel             = nullptr;
    }

    void GameSession::ApplyPendingLevel()
    {
        CheckGameThread("ApplyPendingLevel");
        if(!PendingLevel)
        {
            return;
        }

        /* Why RequestLevel defers: never tear a World down inside its Tick. */
        CORE_CHECK(!LevelWorld || !LevelWorld->IsTicking(),
                   "GameSession::ApplyPendingLevel ran while the World was ticking")

        ILevel* Next = PendingLevel;
        PendingLevel = nullptr;

        /* Fully down before the next comes up; the two never coexist. */
        UnloadLevel();

        if(!LoadLevel(*Next))
        {
            CORE_LOG_ERROR("Requested level failed to load; ending the session")
            UnloadLevel();
            bQuitRequested = true;
        }
    }

    void GameSession::RouteInputThroughUI()
    {
        FilteredInput = Host.GetInput();

        /* The console first: while open, it takes everything. */
        Console->ProcessInput(FilteredInput);

        UI::UIInputFrame Frame;
        const EngineCore::Point Mouse = FilteredInput.GetMousePosition();
        Frame.MousePixels = UI::UIVec2{ float(Mouse.X), float(Mouse.Y) };
        Frame.bHasMouse   = !Host.IsMouseCaptured() && FilteredInput.HasFocus();   // a captured mouse is the game's
        const char* const MouseButtons[3] = { "Mouse.Left", "Mouse.Right", "Mouse.Middle" };
        for(int Each = 0; Each < 3; ++Each)
        {
            const InputKey Key = FindInputKey(MouseButtons[Each]);
            Frame.MouseDown[Each]     = FilteredInput.IsDown(Key);
            Frame.MousePressed[Each]  = FilteredInput.WasPressed(Key);
            Frame.MouseReleased[Each] = FilteredInput.WasReleased(Key);
        }
        Frame.Wheel = FilteredInput.GetWheelDelta();

        /* Navigation through the "UI" input map (UIInputMap.cpp): rebindable like any other. */
        bool bPad = false, bAnyPad = false;
        const auto Pressed = [this, &bPad, &bAnyPad](const char* InAction)
        {
            const bool bPressed = WasInputActionPressed("UI", InAction, FilteredInput, 0, &bPad);
            bAnyPad = bAnyPad || (bPressed && bPad);
            return bPressed;
        };
        Frame.bNavUp    = Pressed("Up");
        Frame.bNavDown  = Pressed("Down");
        Frame.bNavLeft  = Pressed("Left");
        Frame.bNavRight = Pressed("Right");
        Frame.bAccept   = Pressed("Accept");
        Frame.bBack     = Pressed("Back");
        const bool bTab   = Pressed("Next");
        const bool bShift = FilteredInput.IsDown("LeftShift") || FilteredInput.IsDown("RightShift");
        Frame.bNext       = bTab && !bShift;
        Frame.bPrevious   = bTab && bShift;
        Frame.bNavFromGamepad = bAnyPad;

        /* Text editing, for a focused text field (the console's own handling is the model). */
        std::snprintf(Frame.Text, sizeof(Frame.Text), "%s", FilteredInput.GetText());
        const auto Count = [this](const char* InKey) { return uint8_t(std::min(FilteredInput.GetPressesAndRepeats(FindInputKey(InKey)), 255)); };
        Frame.Backspace = Count("Backspace");
        Frame.Delete    = Count("Delete");
        Frame.Left      = Count("Left");
        Frame.Right     = Count("Right");
        Frame.Home      = Count("Home");
        Frame.End       = Count("End");
        Frame.bReturn   = FilteredInput.WasPressed(FindInputKey("Return")) || FilteredInput.WasPressed(FindInputKey("KP_Enter"));
        Frame.bShift    = bShift;
        Frame.bCtrl     = FilteredInput.IsDown("LeftCtrl") || FilteredInput.IsDown("RightCtrl");
        Frame.bSelectAll = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("A"));
        Frame.bCopy      = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("C"));
        Frame.bCut       = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("X"));
        Frame.bPaste     = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("V"));

        const UI::UIInputResult Taken = Interface->ProcessInput(Frame);

        /* The window's text input follows a focused text field -- and comes back
         * on after the console, which turns it off as it closes. */
        const bool bConsoleOpen = Console->IsOpen();
        if(!bConsoleOpen && (Taken.bWantsTextInput != bUITextInput || (bConsoleOpen != this->bConsoleOpen && bUITextInput)))
        {
            bUITextInput = Taken.bWantsTextInput;
            Host.SetTextInputActive(bUITextInput);
        }
        this->bConsoleOpen = bConsoleOpen;
        if(Taken.bConsumeMouse)    { FilteredInput.ConsumeMouseButtons(); }
        if(Taken.bConsumeKeyboard) { FilteredInput.ConsumeKeyboard(); }
        if(Taken.bConsumeGamepad)  { FilteredInput.ConsumeGamepad(); }

        /* Before the game ticks: a Resume or a Quit takes effect this frame. */
        Interface->RunQueuedActions();
    }

    void GameSession::PublishUIValues()
    {
        UI::UISystem& System = *Interface;
        if(LevelWorld)
        {
            /* Keys built once: "Player0.Score" and its kin. */
            static const std::string ScoreKeys[] = { "Player0.Score", "Player1.Score", "Player2.Score", "Player3.Score" };
            static const std::string LivesKeys[] = { "Player0.Lives", "Player1.Lives", "Player2.Lives", "Player3.Lives" };
            LevelWorld->Each<PlayerStateComponent>([&System](GameCore::Entity, PlayerStateComponent& InPlayer)
            {
                if(InPlayer.PlayerIndex >= 4) return;
                System.SetValue(ScoreKeys[InPlayer.PlayerIndex], UI::UIValue::Number(InPlayer.Score));
                System.SetValue(LivesKeys[InPlayer.PlayerIndex], UI::UIValue::Number(InPlayer.Lives));
            });
        }
        const char* Phase = GetPhase();
        if(Phase && PublishedPhase != Phase)
        {
            PublishedPhase = Phase;
            System.SetValue("Game.Phase", UI::UIValue::String(PublishedPhase));
        }
    }

    void GameSession::SetPaused(const bool bInPaused)
    {
        CheckGameThread("SetPaused");
        if(bPaused == bInPaused) return;
        bPaused = bInPaused;
        CORE_LOG(bPaused ? "[Game] paused" : "[Game] resumed")
    }

    void GameSession::Tick(const FrameContext& InFrame)
    {
        /* The mode's OnTick overlaps the render thread: game state only. */
        CheckGameThread("Tick");

        /* The UI sees the input first; the game, what it leaves. */
        RouteInputThroughUI();

        /* Re-read: a level swap may have replaced the mode. */
        if(ActiveLevel && !bPaused)
        {
            if(GameState* State = ActiveLevel->GetGameState())
            {
                State->PhaseSeconds += InFrame.DeltaSeconds;
            }

            if(IGameMode* Mode = ActiveLevel->GetGameMode())
            {
                Mode->OnTick(InFrame);
            }
        }

        /* WorldBridge reads the World later, in the host's Renderer::EndFrame. */
        if(LevelWorld && !bPaused)
        {
            SetScriptInputState(&FilteredInput);
            SetScriptInputHost(&Host);
            LevelWorld->Tick(float(InFrame.DeltaSeconds));
            SetScriptInputHost(nullptr);
            SetScriptInputState(nullptr);
        }
        else if(LevelWorld)
        {
            /* Paused: the components still follow the input, so what the pause
             * menu consumed is not waiting for the game when it resumes. */
            ResolveInputComponents(*LevelWorld, FilteredInput);
        }

        /* After the World: this frame's values, then layout, paint and submit
         * of whatever changed -- before the host's EndFrame renders. */
        PublishUIValues();
        Console->Update();
        Interface->Tick(float(InFrame.DeltaSeconds));
        Interface->Update(UI::UIVec2{ float(InFrame.ViewportPixels.Width), float(InFrame.ViewportPixels.Height) });
    }

    void GameSession::ResolveInput(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        ResolveInputComponents(InWorld, static_cast<GameSession*>(InUserData)->FilteredInput);
    }

    void GameSession::RequestLevel(ILevel& InLevel)
    {
        if(PendingLevel)
        {
            CORE_LOG_WARN("GameSession::RequestLevel called twice before the first took effect; "
                          "the later request wins")
        }
        PendingLevel = &InLevel;
    }

    GameState* GameSession::GetGameState()
    {
        return ActiveLevel ? ActiveLevel->GetGameState() : nullptr;
    }

    GameCore::World& GameSession::GetWorld()
    {
        /* The active level's -- null outside a loaded level. */
        CORE_CHECK(LevelWorld != nullptr, "GameSession::GetWorld() called with no level loaded")
        return *LevelWorld;
    }

    // === The game framework =====================================================

    const char* GameSession::GetPhase()
    {
        const GameState* State = GetGameState();
        return State ? State->GetPhase() : "";
    }

    bool GameSession::SetPhase(const char* InPhase)
    {
        CheckGameThread("SetPhase");
        GameState* State = GetGameState();
        if(!State || !InPhase || InPhase[0] == '\0')
        {
            return false;
        }

        if(std::strlen(InPhase) >= GAME_PHASE_NAME_MAX)
        {
            CORE_LOG_ERROR("GameSession::SetPhase: phase name '" + std::string(InPhase) + "' is longer than "
                           + std::to_string(GAME_PHASE_NAME_MAX - 1) + " characters")
            return false;
        }

        /* Mid-announcement: after it, in order. Asked of the mode then, against
         * the phase it will be leaving. */
        if(bChangingPhase)
        {
            QueuedPhases.emplace_back(InPhase);
            return true;
        }

        if(State->IsPhase(InPhase))
        {
            return true;
        }

        IGameMode* Mode = ActiveLevel->GetGameMode();
        if(Mode && !Mode->CanEnterPhase(State->GetPhase(), InPhase))
        {
            return false;
        }

        ApplyPhase(InPhase);

        /* What the announcement asked for. Each is a fresh request: the mode
         * may refuse it, and its own announcement may queue more. */
        while(!QueuedPhases.empty() && ActiveLevel)
        {
            const std::string Next = QueuedPhases.front();
            QueuedPhases.erase(QueuedPhases.begin());

            GameState* Now = GetGameState();
            if(!Now || Now->IsPhase(Next.c_str()))
            {
                continue;
            }
            IGameMode* NowMode = ActiveLevel->GetGameMode();
            if(NowMode && !NowMode->CanEnterPhase(Now->GetPhase(), Next.c_str()))
            {
                continue;
            }
            ApplyPhase(Next.c_str());
        }
        return true;
    }

    void GameSession::ApplyPhase(const char* InPhase)
    {
        GameState* State = GetGameState();

        char Previous[GAME_PHASE_NAME_MAX];
        std::snprintf(Previous, sizeof(Previous), "%s", State->GetPhase());

        State->WritePhase(InPhase);
        CORE_LOG("[Game] phase " + std::string(Previous[0] ? Previous : "(none)") + " -> " + InPhase)

        bChangingPhase = true;
        if(IGameMode* Mode = ActiveLevel->GetGameMode())
        {
            Mode->OnPhaseChanged(Previous, State->GetPhase());
        }
        if(LevelWorld)
        {
            LevelWorld->GetEvents().Emit("PhaseChanged", MakeArgs(GameCore::EventArg::Make(InPhase),
                                                                  GameCore::EventArg::Make(static_cast<const char*>(Previous))));
        }
        bChangingPhase = false;
    }

    bool GameSession::AddScore(const GameCore::EntityID InPlayer, const int32_t InAmount)
    {
        CheckGameThread("AddScore");
        if(!LevelWorld)
        {
            return false;
        }
        PlayerStateComponent* Player = LevelWorld->GetEntity(InPlayer).GetComponent<PlayerStateComponent>();
        if(!Player)
        {
            return false;
        }

        Player->Score += InAmount;
        const int32_t Score = Player->Score;   // the pointer may not survive the hooks

        if(IGameMode* Mode = ActiveLevel->GetGameMode())
        {
            Mode->OnScoreChanged(InPlayer, Score);
        }
        LevelWorld->GetEvents().Emit("ScoreChanged", MakeArgs(GameCore::EventArg::Make(InPlayer),
                                                              GameCore::EventArg::Make(double(Score))));
        return true;
    }

    bool GameSession::AddLives(const GameCore::EntityID InPlayer, const int32_t InAmount)
    {
        CheckGameThread("AddLives");
        if(!LevelWorld)
        {
            return false;
        }
        PlayerStateComponent* Player = LevelWorld->GetEntity(InPlayer).GetComponent<PlayerStateComponent>();
        if(!Player)
        {
            return false;
        }

        Player->Lives += InAmount;
        const int32_t Lives = Player->Lives;

        if(IGameMode* Mode = ActiveLevel->GetGameMode())
        {
            Mode->OnLivesChanged(InPlayer, Lives);
        }
        LevelWorld->GetEvents().Emit("LivesChanged", MakeArgs(GameCore::EventArg::Make(InPlayer),
                                                              GameCore::EventArg::Make(double(Lives))));
        return true;
    }

    GameCore::EntityID GameSession::GetPlayer(const uint8_t InIndex)
    {
        GameCore::EntityID Found;
        if(LevelWorld)
        {
            LevelWorld->Each<PlayerStateComponent>([&Found, InIndex](GameCore::Entity InEntity, PlayerStateComponent& InPlayer)
            {
                if(!Found.IsSet() && InPlayer.PlayerIndex == InIndex)
                {
                    Found = InEntity.GetID();
                }
            });
        }
        return Found;
    }

    uint32_t GameSession::GetPlayerCount()
    {
        uint32_t Count = 0;
        if(LevelWorld)
        {
            LevelWorld->Each<PlayerStateComponent>([&Count](GameCore::Entity, PlayerStateComponent&) { ++Count; });
        }
        return Count;
    }

    void GameSession::OnComponentAdded(const GameCore::EntityID InEntity, const GameCore::ComponentTypeID InType, void* InUserData)
    {
        auto* Session = static_cast<GameSession*>(InUserData);
        if(InType != PlayerStateComponent::StaticType().ID || !Session->ActiveLevel || !Session->bModeLoaded)
        {
            return;
        }
        if(IGameMode* Mode = Session->ActiveLevel->GetGameMode())
        {
            Mode->OnPlayerAdded(InEntity);
        }
    }

    void GameSession::OnComponentRemoved(const GameCore::EntityID InEntity, const GameCore::ComponentTypeID InType, void* InUserData)
    {
        auto* Session = static_cast<GameSession*>(InUserData);
        if(InType != PlayerStateComponent::StaticType().ID || !Session->ActiveLevel || !Session->bModeLoaded)
        {
            return;
        }
        if(IGameMode* Mode = Session->ActiveLevel->GetGameMode())
        {
            Mode->OnPlayerRemoved(InEntity);
        }
    }
}
