#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/TerrainFacade.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/NimbleEngine/Destruction.h"
#include "Public/NimbleEngine/GameConsole.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/NimbleEngine/SaveGame.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldSerializer.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/Sequencer/SequenceWorld.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"
#include "Public/GameCore/Components/NetComponent.h"

#if WITH_JOLTPHYSICS
#include "Public/JoltPhysics/PhysicsWorld.h"
#endif
#if WITH_AUDIO
#include "Public/Audio/AudioDevice.h"
#include "Public/Audio/AudioWorld.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#endif

/* Chunk tags shared with SaveGame.cpp. */
#include "Private/NimbleEngine/AIInternal.h"
#include "Private/NimbleEngine/InputInternal.h"
#include "Private/NimbleEngine/SaveGameFormat.h"

/* "stat physics": the level's simulation -- bodies, contacts, step times --
 * in the stats panel over the viewport (Renderer::SetStatText). The editor's
 * console and the game's toggle it with "stat physics". */
NIMBLE_CONFIG_BOOL(GStatsPhysics, "Stats", "Physics", false, nullptr);

/* "stat audio": the level's sound -- voices, the listener, the device. */
NIMBLE_CONFIG_BOOL(GStatsAudio, "Stats", "Audio", false, nullptr);

/* SPLITSCREEN (SPLITSCREEN.md): a view per local player (SetViewCount); off,
 * one view whatever the players. Two players' views side by side rather than
 * top and bottom. The game's settings, so its DefaultGame.ini. */
NIMBLE_GAME_CONFIG_BOOL(GSplitscreenEnabled, "Splitscreen", "Enabled", true, nullptr);
NIMBLE_GAME_CONFIG_BOOL(GSplitscreenSideBySide, "Splitscreen", "SideBySide", false, nullptr);

/* The dynamic split (SplitCamera.h), until GameSession::SetSplitCamera: on,
 * and its cameras' pitch and yaw (degrees), distance, field of view and how
 * far out a player may get (0-1 of half the screen) before it splits. */
NIMBLE_GAME_CONFIG_BOOL(GSplitscreenDynamic, "Splitscreen", "Dynamic", false, nullptr);

/* OCCLUSION REVEAL (RENDERPIPELINE.md, "Revealing players"): the local players'
 * avatars kept in sight through what is marked Reveal Players -- the circle's
 * size round each, world units, and how high above its feet it is centred. */
NIMBLE_GAME_CONFIG_BOOL(GRevealPlayers, "Rendering", "RevealPlayers", true, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GRevealRadius, "Rendering", "RevealRadius", 150.0f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GRevealHeight, "Rendering", "RevealHeight", 90.0f, nullptr);

/* A whole-window menu (a pause menu) navigated by any local player's pad, not
 * only player 1's; the documents opened for one player always take theirs. */
NIMBLE_GAME_CONFIG_BOOL(GSplitscreenMenuAnyPad, "Splitscreen", "MenuAnyPad", true, nullptr);

/* Drop-in: Start on a pad no player has joins that player; unplugging a joined
 * player's pad leaves (GameSession::JoinLocalPlayer, LeaveLocalPlayer). */
NIMBLE_GAME_CONFIG_BOOL(GSplitscreenDropIn, "Splitscreen", "DropIn", false, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GSplitscreenDynamicPitch, "Splitscreen", "DynamicPitch", 55.0f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GSplitscreenDynamicYaw, "Splitscreen", "DynamicYaw", 0.0f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GSplitscreenDynamicDistance, "Splitscreen", "DynamicDistance", 1500.0f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GSplitscreenDynamicFov, "Splitscreen", "DynamicFov", 50.0f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GSplitscreenDynamicMargin, "Splitscreen", "DynamicMargin", 0.6f, nullptr);

/* LEVEL TRANSITION (GameSession.h, IsInTransition): every level change fades
 * out, shows the loading card, and fades in once the new level has drawn
 * RevealFrames frames -- or only those a travel asks for (TT_Fade). Seconds
 * of each fade; the card document ("" the engine's, made in code: it binds
 * Transition.Title, Transition.Tip and Transition.Level) and its text when
 * the travel gives none. The game's settings, so its DefaultGame.ini. */
NIMBLE_GAME_CONFIG_BOOL(GTransitionEnabled, "LevelTransition", "Enabled", false, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GTransitionFadeOut, "LevelTransition", "FadeOut", 0.4f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GTransitionFadeIn, "LevelTransition", "FadeIn", 0.6f, nullptr);
NIMBLE_GAME_CONFIG_INT(GTransitionRevealFrames, "LevelTransition", "RevealFrames", 2, nullptr);
/* THE WARM-UP (GLOBAL_ILLUMINATION.md, "Capture warm-up"): after RevealFrames, the card stays up and the
 * World holds still -- at most WarmupSeconds -- while the renderer's captures run at [Rendering]
 * CaptureWarmupBudgetMs: until the nearest GI Volume has drawn every probe (it lights only then) and no
 * capture wants work. A level without GI Volumes or probes passes straight through; 0 turns it off. */
NIMBLE_GAME_CONFIG_FLOAT(GTransitionWarmupSeconds, "LevelTransition", "WarmupSeconds", 15.0f, nullptr);
NIMBLE_GAME_CONFIG_STRING(GTransitionDocument, "LevelTransition", "Document", "", nullptr);
NIMBLE_GAME_CONFIG_STRING(GTransitionTitle, "LevelTransition", "Title", "Loading", nullptr);
NIMBLE_GAME_CONFIG_STRING(GTransitionTip, "LevelTransition", "Tip", "", nullptr);

#if WITH_AUDIO
namespace
{
    void PoseFromCamera(const GameCore::Entity& InEntity, Audio::AudioListenerPose& OutPose)
    {
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        const GameCore::Rotation3     Turn = GameCore::TransformMath::EulerToRotation(Pose.Rotation);
        OutPose.Position = Pose.Position;
        OutPose.Forward  = Turn * EngineCore::Vector3(0.0, 0.0, 1.0);
        OutPose.Up       = Turn * EngineCore::Vector3(0.0, 1.0, 0.0);
    }

    /* SPLITSCREEN: a listener per view this frame, each at the camera it is
     * viewed through; 0 when no view has one (CameraListener's rule then). */
    uint32_t ViewListeners(GameCore::World& InWorld, Audio::AudioListenerPose* OutPoses, const uint32_t InMax, void* InUserData)
    {
        auto* Session = static_cast<NimbleEngine::GameSession*>(InUserData);
        if(!Session) { return 0; }
        const uint32_t Views = std::min(Session->GetViewCount(), InMax);
        uint32_t Count = 0;
        for(uint32_t View = 0; View < Views; ++View)
        {
            const GameCore::EntityID Viewed = Session->GetRenderer().GetViewCamera(RenderCore::RENDER_DEFAULT_SCENE, View);
            if(!Viewed.IsSet() || !InWorld.IsAlive(Viewed)) { continue; }
            PoseFromCamera(InWorld.GetEntity(Viewed), OutPoses[Count++]);
        }
        return Count;
    }

    /* The level heard from its camera when it has no active Audio Listener:
     * the one it is viewed through (the view target), else the first active
     * Camera, as WorldBridge views through it. */
    bool CameraListener(GameCore::World& InWorld, Audio::AudioListenerPose& OutPose, void* InUserData)
    {
        auto* Session = static_cast<NimbleEngine::GameSession*>(InUserData);
        const GameCore::EntityID Viewed = Session ? Session->GetRenderer().GetViewCamera() : GameCore::EntityID();
        if(Viewed.IsSet() && InWorld.IsAlive(Viewed))
        {
            PoseFromCamera(InWorld.GetEntity(Viewed), OutPose);
            return true;
        }

        bool bFound = false;
        InWorld.Each<RenderCore::CameraComponent>([&](GameCore::Entity InEntity, RenderCore::CameraComponent& InCamera)
        {
            if(bFound || !InCamera.bActive) { return; }
            PoseFromCamera(InEntity, OutPose);
            bFound = true;
        });
        return bFound;
    }

    /* A clip's notify sound (footsteps), once where it fired. */
    void PlayNotifySound(const char* InSound, const EngineCore::Vector3& InAt, void* InUserData)
    {
        static_cast<Audio::AudioWorld*>(InUserData)->PlaySoundAt(InSound, InAt);
    }
}
#endif

namespace
{
    /* "Engine.CharacterMover" (CharacterMover.h). */
    void CharacterMoverSystem(GameCore::World& InWorld, const float InDeltaSeconds, void*)
    {
        NimbleEngine::UpdateCharacterMovers(InWorld, InDeltaSeconds);
    }
}

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

    /* GameSession::TransitionStage. */
    enum : uint8_t
    {
        TS_None    = 0,
        TS_FadeOut = 1,   // the old level runs on under the black
        TS_Loading = 2,   // the card is up; the next ApplyPendingLevel swaps
        TS_Reveal  = 3,   // the new level draws its first frames behind the card
        TS_FadeIn  = 4,   // the card is gone; the black lifts
    };

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
        InstallTerrainProvider();         // heightfield bodies (TerrainFacade.cpp)

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
        return LoadLevel(InLevel, LevelTravel());
    }

    bool GameSession::LoadLevel(ILevel& InLevel, const LevelTravel& InTravel)
    {
        CheckGameThread("LoadLevel");
        CORE_CHECK(LevelWorld == nullptr, "GameSession::LoadLevel called with a level already loaded")

        /* Readable from OnLoad on (GetTravel). */
        Travel = InTravel;

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

#if WITH_JOLTPHYSICS
        /* Before OnLoad, so the level's first bodies step with everything else.
         * A client leaves what the server replicates to the server. */
        {
            JoltPhysics::PhysicsSettings Settings = JoltPhysics::PhysicsSettings::FromConfig();
            Settings.bReplicaKinematic = IsClient();
            Physics = EngineCore::Memory::Alloc<JoltPhysics::PhysicsWorld>();
            if(!Physics->Attach(*LevelWorld, Settings))
            {
                EngineCore::Memory::Dealloc(Physics);
                Physics = nullptr;
            }
        }
#endif
#if WITH_AUDIO
        /* Before OnLoad, so a level's AutoPlay sources start with it. A
         * headless server has no device, and so no audio. */
        LevelAudio = EngineCore::Memory::Alloc<Audio::AudioWorld>();
        if(LevelAudio->Attach(*LevelWorld))
        {
            LevelAudio->SetListenerFallback(&CameraListener, this);
            LevelAudio->SetListenersFallback(&ViewListeners, this);
            /* Rendering.BindWorld above attached the animation system. */
            if(RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(LevelWorld))
            {
                Animation->SetSoundSink(&PlayNotifySound, LevelAudio);
            }
        }
        else
        {
            EngineCore::Memory::Dealloc(LevelAudio);
            LevelAudio = nullptr;
        }
#endif
        bInactiveChecked = false;
        bInactiveWarned  = false;
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

        /* Right after: each AvatarHarness from its player's devices, or (a
         * server) its client's (Avatar.h). */
        for(GameCore::EntityID& Each : ViewTargets) { Each = GameCore::EntityID(); }
        for(GameCore::EntityID& Each : SplitCameras) { Each = GameCore::EntityID(); }
        bSequenceView  = false;
        bSplitAngleSet = false;
        Avatars = EngineCore::Memory::Alloc<AvatarWorld>();
        Avatars->Attach(*LevelWorld);
        Avatars->SetInput(&FilteredInput);

        /* Ragdolls: after physics and the animation system, which it runs
         * between (TP_PostPhysics, and TP_PostUpdate after the animation). */
        RagdollWorld::Attach(*LevelWorld);

        /* Destructibles: after physics, whose impacts break them (TP_PostPhysics). */
        DestructionWorld::Attach(*LevelWorld);

        /* Character Movers' anim params and camera booms, from where this
         * frame's steps (TP_PrePhysics) and physics left them. */
        LevelWorld->RegisterSystem("Engine.CharacterMover", GameCore::TP_PostPhysics, &CharacterMoverSystem, this);

        /* Cutscenes: the level's Sequence Players, from TP_PostPhysics.
         * Before OnLoad, so an AutoPlay player starts with the level. */
        bCinematic    = false;
        CinematicFade = -1.0f;
        if(Sequencer::SequenceWorld* Sequences = Sequencer::SequenceWorld::Attach(*LevelWorld))
        {
            Sequencer::SequenceHost SequenceHost;
            SequenceHost.SetViewTarget = &GameSession::SequenceSetViewTarget;
            SequenceHost.GetViewTarget = &GameSession::SequenceGetViewTarget;
            SequenceHost.SetFade       = &GameSession::SequenceSetFade;
            SequenceHost.SetCinematic  = &GameSession::SequenceSetCinematic;
            SequenceHost.IsReplica     = &GameSession::SequenceIsReplica;
            SequenceHost.SetBusGain    = &GameSession::SequenceSetBusGain;
            SequenceHost.SetSubtitle   = &GameSession::SequenceSetSubtitle;
            SequenceHost.UserData      = this;
            Sequences->SetHost(SequenceHost);
        }

        /* AI: navigation, brains and perception (AI.Brains and AI.Nav in
         * TP_PrePhysics, after the avatars; AI.Perception in TP_PostPhysics).
         * An authority's only: a client follows the server's. */
        AIDetail::AttachLevelAI(*LevelWorld);

        /* Plugins' systems (Plugin.h, PluginContext::AddWorldHook): after
         * the engine's, before OnLoad, so the level's entities start with them. */
        Plugins::AttachWorld(*LevelWorld);

        CORE_LOG("[Level] loading '" + std::string(InLevel.GetName()) + "'"
                 + (Travel.Entry.empty() ? std::string() : " at '" + Travel.Entry + "'")
                 + (Travel.Values.empty() ? std::string() : " (" + Travel.ToText() + ")"))

        if(!InLevel.OnLoad(*this))
        {
            CORE_LOG_ERROR("Level '" + std::string(InLevel.GetName()) + "' refused to load")
            return false;
        }

        /* After the level has populated the World. */
        IGameMode* Mode = InLevel.GetGameMode();
        if(Mode)
        {
            Mode->Session = this;   // its context (GameMode.h), until UnloadLevel takes it back
        }
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
        if(StartupScriptPath && StartupScriptPath[0] != '\0')
        {
            Scripts.RunFile(StartupScriptPath);
        }

        /* What the level and its script made, all at once; later additions
         * are caught as they come (OnComponentAdded). */
        bInactiveWarned  = WarnInactiveModuleComponents(*LevelWorld, InLevel.GetName());
        bInactiveChecked = true;

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

        /* Avatars placed for local players are theirs before the mode starts,
         * so OnStart sees who already has one. */
        ApplyAutoPossess(*LevelWorld);
        if(!Travel.Entry.empty())
        {
            if(!FindEntryPoint(*LevelWorld, Travel.Entry.c_str()).IsSet())
            {
                CORE_LOG_WARN("[Level] '" + std::string(InLevel.GetName()) + "' has no enabled Player Start for the entry '"
                              + Travel.Entry + "' (its Entry, or its entity's name): the usual start")
            }
            PlaceAvatarsAtEntry();
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

            /* What the mode bound as itself (IGameMode::AddSystem, Subscribe,
             * SetAvatarInputFilter): the level's OnUnload may destroy it, and
             * the World outlives it a while yet. */
            void* const Self = static_cast<IGameMode*>(Mode);
            if(LevelWorld)
            {
                LevelWorld->UnregisterAllSystems(Self);
                LevelWorld->GetEvents().UnsubscribeAll(Self);
            }
            if(Avatars)
            {
                Avatars->ClearInputFilter(Self);
            }
            Mode->Session = nullptr;
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

            /* Before the World goes: its removals would reach the avatars. */
            if(Avatars)
            {
                Avatars->Detach();
                EngineCore::Memory::Dealloc(Avatars);
                Avatars = nullptr;
            }
            for(GameCore::EntityID& Each : ViewTargets) { Each = GameCore::EntityID(); }
            for(GameCore::EntityID& Each : SplitCameras) { Each = GameCore::EntityID(); }
            bSequenceView  = false;
            bSplitAngleSet = false;

            /* Plugins first, in reverse: they attached last. */
            Plugins::DetachWorld(*LevelWorld);

            /* Brains let go of their agents while the components still exist. */
            AIDetail::DetachLevelAI(*LevelWorld);

            /* Before the physics and animation it holds ragdolls in. */
            RagdollWorld::Detach(*LevelWorld);
            DestructionWorld::Detach(*LevelWorld);
            LevelWorld->UnregisterSystem(&CharacterMoverSystem, this);

            /* While scripts are bound: what sequences spawned is destroyed
             * with its OnDestroy run. The overlay closed with the level's
             * documents above. */
            if(Sequencer::SequenceWorld* Sequences = Sequencer::SequenceWorld::Find(LevelWorld))
            {
                Sequences->SetHost(Sequencer::SequenceHost());
            }
            Sequencer::SequenceWorld::Detach(*LevelWorld);
            bCinematic    = false;
            CinematicFade = -1.0f;
            bSubtitle     = false;
#if WITH_AUDIO
            /* No level's sequence holds the mix now. */
            for(uint8_t Bus = 0; Bus < uint8_t(Audio::EAudioBus::Count); ++Bus) { Audio::AudioDevice::Get().SetBusGain(Audio::EAudioBus(Bus), 1.0f); }
#endif

            Scripts.UnbindWorld();
            Rendering.UnbindWorld();

            /* "stat physics" shows no World's numbers once it is gone. */
            if(bPhysicsStatShown)
            {
                Rendering.SetStatText("Physics", std::string());
                bPhysicsStatShown = false;
            }

#if WITH_JOLTPHYSICS
            if(Physics)
            {
                Physics->Detach();
                EngineCore::Memory::Dealloc(Physics);
                Physics = nullptr;
            }
#endif
#if WITH_AUDIO
            if(LevelAudio)
            {
                if(RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(LevelWorld))
                {
                    Animation->SetSoundSink(nullptr, nullptr);
                }
                LevelAudio->Detach();
                EngineCore::Memory::Dealloc(LevelAudio);
                LevelAudio = nullptr;
            }
#endif
            if(bAudioStatShown)
            {
                Rendering.SetStatText("Audio", std::string());
                bAudioStatShown = false;
            }

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
        LevelTravel NextTravel = PendingTravel;
        NextTravel.Previous    = ActiveLevel ? ActiveLevel->GetName() : "";
        PendingTravel          = LevelTravel();

        /* Fully down before the next comes up; the two never coexist. */
        UnloadLevel();

        if(!LoadLevel(*Next, NextTravel))
        {
            CORE_LOG_ERROR("Requested level failed to load; ending the session")
            UnloadLevel();
            bQuitRequested = true;
            EndTransition();
            return;
        }

        /* Behind the card: it stays up until the new level has drawn a frame or two. */
        if(TransitionStage == TS_Loading)
        {
            TransitionStage  = TS_Reveal;
            TransitionFrames = 0;
            /* The warm-up's clock and the renderer's frame count at the load (UpdateTransitionWarmup). */
            RenderCore::RenderStats Stats;
            Rendering.GetStats(Stats);
            TransitionWarmupStart = Stats.FramesRendered;
            TransitionWarmupTime  = 0.0f;
        }
    }

    // === Level transition =======================================================

    namespace
    {
        constexpr const char* TRANSITION_FADE_DOCUMENT = "Engine/Transition/Fade.nmui";
        constexpr const char* TRANSITION_CARD_DOCUMENT = "Engine/Transition/Card.nmui";

        /* The two documents, made in code like the cinematic overlay: the black
         * (Layer 900, over a game's HUD, under a cinematic's bars) and the
         * loading card over it (950; a game's own card needs a Layer above 900). */
        std::shared_ptr<UI::UIDocumentAsset> MakeTransitionAsset(const char* InLayer)
        {
            auto Asset = std::make_shared<UI::UIDocumentAsset>();
            Asset->Settings.SetFromText("InputMode", "GameOnly");
            Asset->Settings.SetFromText("Layer", InLayer);
            return Asset;
        }

        void AddWidget(UI::UIDocumentAsset& InAsset, const char* InType, const int32_t InParent,
                       std::initializer_list<std::pair<const char*, const char*>> InProperties)
        {
            UI::UIWidgetDesc Widget;
            Widget.Type   = InType;
            Widget.Parent = InParent;
            for(const auto& Property : InProperties) { Widget.Set(Property.first, Property.second); }
            InAsset.Widgets.push_back(std::move(Widget));
        }

        std::shared_ptr<const UI::UIDocumentAsset> MakeTransitionFadeDocument()
        {
            std::shared_ptr<UI::UIDocumentAsset> Asset = MakeTransitionAsset("900");
            AddWidget(*Asset, "Canvas", -1, { { "Name", "Root" } });
            AddWidget(*Asset, "Border", 0, { { "Name", "Fade" }, { "Anchors", "0,0,1,1" }, { "Background.Tint", "#000000FF" },
                                             { "Opacity", "{Bind:Transition.Fade}" } });
            return Asset;
        }

        std::shared_ptr<const UI::UIDocumentAsset> MakeTransitionCardDocument()
        {
            std::shared_ptr<UI::UIDocumentAsset> Asset = MakeTransitionAsset("950");
            AddWidget(*Asset, "Canvas", -1, { { "Name", "Root" } });
            AddWidget(*Asset, "Border", 0, { { "Name", "Back" }, { "Anchors", "0,0,1,1" }, { "Background.Tint", "#0A0B0EFF" } });
            AddWidget(*Asset, "Text", 0, { { "Name", "Title" }, { "Anchors", "0.1,0.42,0.9,0.52" }, { "Text", "{Bind:Transition.Title}" },
                                           { "FontSize", "48" }, { "Justify", "Center" }, { "Color", "#F2EEE4FF" } });
            AddWidget(*Asset, "Text", 0, { { "Name", "Tip" }, { "Anchors", "0.15,0.78,0.85,0.9" }, { "Text", "{Bind:Transition.Tip}" },
                                           { "FontSize", "22" }, { "Justify", "Center" }, { "Wrap", "true" }, { "Color", "#B8B4AAFF" } });
            return Asset;
        }
    }

    void GameSession::UpdateTransition(const float InDeltaSeconds)
    {
        if(TransitionStage == TS_None) { return; }
        /* A frame after a load is as long as the load: the fades run on at most a 30th. */
        const float Dt = (std::min)((std::max)(InDeltaSeconds, 0.0f), 1.0f / 30.0f);
        switch(TransitionStage)
        {
        case TS_FadeOut:
        {
            TransitionTime += Dt;
            const float Out = (std::max)(0.0f, GTransitionFadeOut.GetFloat());
            SetTransitionFade(Out > 0.0f ? (std::min)(1.0f, TransitionTime / Out) : 1.0f);
            if(TransitionFade < 1.0f || !TransitionLevel) { break; }
            /* Black: the card goes up and is drawn this frame; the next frame's
             * ApplyPendingLevel swaps behind it. */
            OpenTransitionCard();
            if(PendingLevel)
            {
                CORE_LOG_WARN("GameSession: a level transition replaces a RequestLevel that had not taken effect")
            }
            PendingLevel     = TransitionLevel;
            PendingTravel    = TransitionTravel;
            TransitionLevel  = nullptr;
            TransitionStage  = TS_Loading;
            break;
        }
        case TS_Loading:
            break;   // ApplyPendingLevel moves it on
        case TS_Reveal:
        {
            /* Ticked before each frame is drawn: past RevealFrames, that many have been; then the warm-up. */
            const int32_t Reveal = (std::max)(0, GTransitionRevealFrames.GetInt());
            if(TransitionFrames <= Reveal) { ++TransitionFrames; }
            if(TransitionFrames > Reveal && !UpdateTransitionWarmup(InDeltaSeconds))
            {
                CloseTransitionCard();
                TransitionStage = TS_FadeIn;
                TransitionTime  = 0.0f;
            }
            break;
        }
        case TS_FadeIn:
        {
            TransitionTime += Dt;
            const float In = (std::max)(0.0f, GTransitionFadeIn.GetFloat());
            const float Alpha = In > 0.0f ? 1.0f - TransitionTime / In : 0.0f;
            if(Alpha <= 0.0f) { EndTransition(); }
            else              { SetTransitionFade(Alpha); }
            break;
        }
        default:
            EndTransition();
            break;
        }
    }

    void GameSession::SetTransitionFade(const float InAlpha)
    {
        TransitionFade = InAlpha;
        Interface->SetValue("Transition.Fade", UI::UIValue::Number(InAlpha));
        if(!Interface->IsOpen(TRANSITION_FADE_DOCUMENT))
        {
            /* Persistent: it outlives the level change it covers. */
            static const std::shared_ptr<const UI::UIDocumentAsset> Document = MakeTransitionFadeDocument();
            Interface->OpenAsset(Document, TRANSITION_FADE_DOCUMENT, true, false);
        }
    }

    void GameSession::OpenTransitionCard()
    {
        const std::string Title = !TransitionTravel.Title.empty() ? TransitionTravel.Title : GTransitionTitle.GetString();
        const std::string Tip   = !TransitionTravel.Tip.empty() ? TransitionTravel.Tip : GTransitionTip.GetString();
        Interface->SetValue("Transition.Title", UI::UIValue::String(Title));
        Interface->SetValue("Transition.Tip", UI::UIValue::String(Tip));
        Interface->SetValue("Transition.Level", UI::UIValue::String(TransitionLevel ? TransitionLevel->GetName() : ""));

        CloseTransitionCard();
        const std::string Custom = GTransitionDocument.GetString();
        if(!Custom.empty())
        {
            if(Interface->Open(Custom, true) != UI::UI_DOCUMENT_INVALID)
            {
                TransitionCard = Custom;
                return;
            }
            CORE_LOG_WARN("[LevelTransition] Document '" + Custom + "' did not open; the engine's card instead")
        }
        static const std::shared_ptr<const UI::UIDocumentAsset> Document = MakeTransitionCardDocument();
        Interface->OpenAsset(Document, TRANSITION_CARD_DOCUMENT, true, false);
        TransitionCard = TRANSITION_CARD_DOCUMENT;
    }

    void GameSession::CloseTransitionCard()
    {
        if(TransitionCard.empty()) { return; }
        if(Interface->IsOpen(TransitionCard)) { Interface->Close(TransitionCard); }
        TransitionCard.clear();
    }

    void GameSession::SetCaptureWarmup(const bool bInOn)
    {
        bTransitionWarming = bInOn;
        if(EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Rendering", "CaptureWarmup")) { Var->SetBool(bInOn); }
    }

    bool GameSession::UpdateTransitionWarmup(const float InDeltaSeconds)
    {
        const float Most = (std::max)(0.0f, GTransitionWarmupSeconds.GetFloat());
        RenderCore::RenderStats Stats;
        Rendering.GetStats(Stats);
        /* Nothing drawn (a renderer not running) or no warm-up wanted: nothing to wait for. */
        if(Most <= 0.0f || !LevelWorld || Stats.FramesRendered == 0)
        {
            if(bTransitionWarming) { SetCaptureWarmup(false); }
            return false;
        }
        /* Real time, not the fades' clamped step: the cap is the player's wait. */
        TransitionWarmupTime += (std::max)(InDeltaSeconds, 0.0f);
        /* The captures' state is the last drawn frame's: trusted once three frames of the new level are
         * drawn since it loaded (the render thread runs a frame behind; before, the last level's). Until
         * then the card stays and the World runs on, as through RevealFrames. */
        constexpr uint64_t TRUSTED_AFTER = 3;
        const bool bPending = Stats.GIProbesUnlit > 0 || Stats.bCapturesBusy;
        if(!bTransitionWarming)
        {
            if(Stats.FramesRendered < TransitionWarmupStart + TRUSTED_AFTER) { return TransitionWarmupTime < Most; }
            if(!bPending) { return false; }   // nothing to fill: the World never held
            SetCaptureWarmup(true);
            TransitionWarmupFrames = 0;
        }
        ++TransitionWarmupFrames;
        if(bPending && TransitionWarmupTime < Most) { return true; }
        char Line[192];
        std::snprintf(Line, sizeof(Line), "[Level] warm-up %s after %.2f s, %d frames (GI Volume %u of %u probes unlit)",
                      bPending ? "timed out" : "settled", double(TransitionWarmupTime), int(TransitionWarmupFrames),
                      unsigned(Stats.GIProbesUnlit), unsigned(Stats.GIProbesTotal));
        CORE_LOG(Line)
        SetCaptureWarmup(false);
        return false;
    }

    void GameSession::EndTransition()
    {
        if(bTransitionWarming) { SetCaptureWarmup(false); }
        CloseTransitionCard();
        if(Interface->IsOpen(TRANSITION_FADE_DOCUMENT)) { Interface->Close(std::string(TRANSITION_FADE_DOCUMENT)); }
        TransitionStage  = TS_None;
        TransitionTime   = 0.0f;
        TransitionFade   = 0.0f;
        TransitionFrames = 0;
        TransitionLevel  = nullptr;
        TransitionTravel = LevelTravel();
    }

    void GameSession::PlaceAvatarsAtEntry()
    {
        /* Placed avatars (AutoPossess) of the local players; a mode's spawns
         * come later and ask FindPlayerStart, which prefers the entry. */
        for(uint8_t Local = 0; Local < INPUT_PLAYERS_MAX; ++Local)
        {
            const GameCore::EntityID Player = FindLocalPlayer(*LevelWorld, Local);
            const GameCore::EntityID Avatar = Player.IsSet() ? GetAvatar(*LevelWorld, Player) : GameCore::EntityID();
            const GameCore::EntityID Start  = Avatar.IsSet() ? FindEntryPoint(*LevelWorld, Travel.Entry.c_str(), Local) : GameCore::EntityID();
            if(Start.IsSet()) { PlaceAvatarAt(*LevelWorld, Avatar, Start); }
        }
    }

    bool GameSession::OpenLevel(const char* InLevelName, const LevelTravel& InTravel)
    {
        CheckGameThread("OpenLevel");
        ILevel* Level = FindLevelByName(InLevelName ? InLevelName : "");
        if(!Level)
        {
            CORE_LOG_WARN(std::string("[Level] OpenLevel: no level named '") + (InLevelName ? InLevelName : "") + "'")
            return false;
        }
        RequestLevel(*Level, InTravel);
        return true;
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
        /* The cursor, text and shortcuts are the devices', whoever has them (INPUT.md, "Local players"). */
        const uint8_t Keys = INPUT_PLAYER_DEVICES;
        const char* const MouseButtons[3] = { "Mouse.Left", "Mouse.Right", "Mouse.Middle" };
        for(int Each = 0; Each < 3; ++Each)
        {
            const InputKey Key = FindInputKey(MouseButtons[Each]);
            Frame.MouseDown[Each]     = FilteredInput.IsDown(Key, Keys);
            Frame.MousePressed[Each]  = FilteredInput.WasPressed(Key, Keys);
            Frame.MouseReleased[Each] = FilteredInput.WasReleased(Key, Keys);
        }
        Frame.Wheel = FilteredInput.GetWheelDelta();

        /* Navigation through the "UI" input map (UIInputMap.cpp): rebindable like any other --
         * the window's documents by player 1's pad (any player's, with [Splitscreen] MenuAnyPad)
         * and the keyboard whoever has it; a player's own documents by their own (INPUT.md,
         * "Local players"). */
        const uint8_t MenuPads = GSplitscreenMenuAnyPad.GetBool() ? uint8_t(INPUT_PLAYERS_MAX) : uint8_t(1);
        bool bPad = false, bAnyPad = false;
        const auto Pressed = [this, &bPad, &bAnyPad, Keys, MenuPads](const char* InAction)
        {
            bool bFromPad = false;
            bool bPressed = false;
            for(uint8_t Pad = 0; Pad < MenuPads && !bPressed; ++Pad)
            {
                bPressed = WasInputActionPressed("UI", InAction, FilteredInput, Pad, &bFromPad);
            }
            if(!bPressed)
            {
                bPressed = WasInputActionPressed("UI", InAction, FilteredInput, Keys, &bFromPad);
            }
            bPad    = bFromPad;
            bAnyPad = bAnyPad || (bPressed && bFromPad);
            return bPressed;
        };
        for(uint8_t Local = 0; Local < UI::UI_PLAYERS_MAX && Local < INPUT_PLAYERS_MAX; ++Local)
        {
            UI::UIPlayerNavigation& Own = Frame.PlayerNav[Local];
            Own.bUp     = WasInputActionPressed("UI", "Up", FilteredInput, Local);
            Own.bDown   = WasInputActionPressed("UI", "Down", FilteredInput, Local);
            Own.bLeft   = WasInputActionPressed("UI", "Left", FilteredInput, Local);
            Own.bRight  = WasInputActionPressed("UI", "Right", FilteredInput, Local);
            Own.bAccept = WasInputActionPressed("UI", "Accept", FilteredInput, Local);
            Own.bBack   = WasInputActionPressed("UI", "Back", FilteredInput, Local);
        }
        Frame.bNavUp    = Pressed("Up");
        Frame.bNavDown  = Pressed("Down");
        Frame.bNavLeft  = Pressed("Left");
        Frame.bNavRight = Pressed("Right");
        Frame.bAccept   = Pressed("Accept");
        Frame.bBack     = Pressed("Back");
        const bool bTab   = Pressed("Next");
        const bool bShift = FilteredInput.IsDown("LeftShift", Keys) || FilteredInput.IsDown("RightShift", Keys);
        Frame.bNext       = bTab && !bShift;
        Frame.bPrevious   = bTab && bShift;
        Frame.bNavFromGamepad = bAnyPad;

        /* Text editing, for a focused text field (the console's own handling is the model). */
        std::snprintf(Frame.Text, sizeof(Frame.Text), "%s", FilteredInput.GetText());
        const auto Count = [this, Keys](const char* InKey) { return uint8_t(std::min(FilteredInput.GetPressesAndRepeats(FindInputKey(InKey), Keys), 255)); };
        Frame.Backspace = Count("Backspace");
        Frame.Delete    = Count("Delete");
        Frame.Left      = Count("Left");
        Frame.Right     = Count("Right");
        Frame.Home      = Count("Home");
        Frame.End       = Count("End");
        Frame.bReturn   = FilteredInput.WasPressed(FindInputKey("Return"), Keys) || FilteredInput.WasPressed(FindInputKey("KP_Enter"), Keys);
        Frame.bShift    = bShift;
        Frame.bCtrl     = FilteredInput.IsDown("LeftCtrl", Keys) || FilteredInput.IsDown("RightCtrl", Keys);
        Frame.bSelectAll = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("A"), Keys);
        Frame.bCopy      = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("C"), Keys);
        Frame.bCut       = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("X"), Keys);
        Frame.bPaste     = Frame.bCtrl && FilteredInput.WasPressed(FindInputKey("V"), Keys);

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
        if(Taken.bConsumeGamepad)   // the pads it navigates from; the others play on
        {
            for(uint8_t Pad = 0; Pad < MenuPads; ++Pad) { FilteredInput.ConsumeGamepad(Pad); }
        }
        for(uint8_t Local = 0; Local < UI::UI_PLAYERS_MAX && Local < INPUT_PLAYERS_MAX; ++Local)
        {
            /* A player's own document used their pad (and the keyboard, if theirs). */
            if(!Taken.bConsumePlayer[Local]) { continue; }
            FilteredInput.ConsumeGamepad(Local);
            if(GetKeyboardMousePlayer() == int32_t(Local)) { FilteredInput.ConsumeKeyboard(); }
        }

        /* A cinematic holds the players' input back; Back the UI left skips it. */
        if(bCinematic && LevelWorld && !bConsoleOpen)
        {
            if(Frame.bBack && !Taken.bConsumeKeyboard)
            {
                if(Sequencer::SequenceWorld* Sequences = Sequencer::SequenceWorld::Find(LevelWorld))
                {
                    Sequences->SkipCinematics();
                }
            }
            FilteredInput.ConsumeMouseButtons();
            FilteredInput.ConsumeKeyboard();
            FilteredInput.ConsumeGamepad();
        }

        /* A level transition holds the players' input back until it fades in. */
        if(TransitionStage != TS_None && TransitionStage != TS_FadeIn)
        {
            FilteredInput.ConsumeMouseButtons();
            FilteredInput.ConsumeKeyboard();
            FilteredInput.ConsumeGamepad();
        }

        /* Before the game ticks: a Resume or a Quit takes effect this frame. */
        Interface->RunQueuedActions();
    }

    // === Cutscenes ==============================================================

    namespace
    {
        constexpr const char* CINEMATIC_DOCUMENT = "Engine/Sequencer/Cinematic.nmui";

        /* Over everything, taking no input: a black box for the fade,
         * letterbox bars, and the subtitle line over both. Made in code, so
         * no build has a file to stage. */
        std::shared_ptr<const UI::UIDocumentAsset> MakeCinematicDocument()
        {
            auto Asset = std::make_shared<UI::UIDocumentAsset>();
            Asset->Settings.SetFromText("InputMode", "GameOnly");
            Asset->Settings.SetFromText("Layer", "1000");
            const auto Add = [&Asset](const char* InType, const int32_t InParent,
                                      std::initializer_list<std::pair<const char*, const char*>> InProperties)
            {
                UI::UIWidgetDesc Widget;
                Widget.Type   = InType;
                Widget.Parent = InParent;
                for(const auto& Property : InProperties) { Widget.Set(Property.first, Property.second); }
                Asset->Widgets.push_back(std::move(Widget));
            };
            Add("Canvas", -1, { { "Name", "Root" } });
            Add("Border", 0, { { "Name", "BarTop" },    { "Anchors", "0,0,1,0.11" }, { "Background.Tint", "#000000FF" },
                               { "Opacity", "{Bind:Sequencer.Letterbox}" } });
            Add("Border", 0, { { "Name", "BarBottom" }, { "Anchors", "0,0.89,1,1" }, { "Background.Tint", "#000000FF" },
                               { "Opacity", "{Bind:Sequencer.Letterbox}" } });
            Add("Border", 0, { { "Name", "Fade" },      { "Anchors", "0,0,1,1" },    { "Background.Tint", "#000000FF" },
                               { "Opacity", "{Bind:Sequencer.Fade}" } });
            Add("Text", 0, { { "Name", "Speaker" }, { "Anchors", "0.1,0.795,0.9,0.835" }, { "Text", "{Bind:Sequencer.Speaker}" },
                             { "FontSize", "22" }, { "Justify", "Center" }, { "Color", "#FFD27AFF" },
                             { "ShadowOffset", "2,2" }, { "ShadowColor", "#000000C0" } });
            Add("Text", 0, { { "Name", "Subtitle" }, { "Anchors", "0.1,0.835,0.9,0.95" }, { "Text", "{Bind:Sequencer.Subtitle}" },
                             { "FontSize", "28" }, { "Justify", "Center" }, { "Wrap", "true" }, { "Color", "#FFFFFFFF" },
                             { "ShadowOffset", "2,2" }, { "ShadowColor", "#000000C0" } });
            return Asset;
        }
    }

    void GameSession::UpdateCinematicOverlay()
    {
        const bool bWanted = bCinematic || CinematicFade > 0.0f || bSubtitle;
        Interface->SetValue("Sequencer.Fade", UI::UIValue::Number(std::max(0.0f, CinematicFade)));
        Interface->SetValue("Sequencer.Letterbox", UI::UIValue::Number(bCinematic ? 1.0 : 0.0));
        if(bWanted && !Interface->IsOpen(CINEMATIC_DOCUMENT))
        {
            static const std::shared_ptr<const UI::UIDocumentAsset> Document = MakeCinematicDocument();
            Interface->OpenAsset(Document, CINEMATIC_DOCUMENT, false, false);
        }
        else if(!bWanted && Interface->IsOpen(CINEMATIC_DOCUMENT))
        {
            Interface->Close(std::string(CINEMATIC_DOCUMENT));
        }
    }

    void GameSession::SequenceSetViewTarget(const GameCore::EntityID InCamera, void* InUserData)
    {
        /* A camera cut: the whole window, one view, until it lets go. */
        auto* Session = static_cast<GameSession*>(InUserData);
        Session->bSequenceView = InCamera.IsSet();
        Session->SetViewTarget(InCamera);
    }

    GameCore::EntityID GameSession::SequenceGetViewTarget(void* InUserData)
    {
        return static_cast<GameSession*>(InUserData)->GetViewTarget();
    }

    void GameSession::SequenceSetFade(const float InAlpha, void* InUserData)
    {
        auto* Session = static_cast<GameSession*>(InUserData);
        Session->CinematicFade = InAlpha;
        Session->UpdateCinematicOverlay();
    }

    void GameSession::SequenceSetCinematic(const bool bInOn, void* InUserData)
    {
        auto* Session = static_cast<GameSession*>(InUserData);
        Session->bCinematic = bInOn;
        Session->UpdateCinematicOverlay();
    }

    /* Bus Volume tracks: the device's runtime gain, under the player's own
     * volume settings and never saved. */
    void GameSession::SequenceSetBusGain(const char* InBus, const float InGain, void* InUserData)
    {
        (void)InUserData;
#if WITH_AUDIO
        Audio::EAudioBus Bus;
        if(Audio::FindBus(InBus, Bus)) { Audio::AudioDevice::Get().SetBusGain(Bus, InGain); }
#else
        (void)InBus; (void)InGain;
#endif
    }

    void GameSession::SequenceSetSubtitle(const char* InSpeaker, const char* InText, void* InUserData)
    {
        auto* Session = static_cast<GameSession*>(InUserData);
        Session->Interface->SetValue("Sequencer.Speaker", UI::UIValue::String(InSpeaker));
        Session->Interface->SetValue("Sequencer.Subtitle", UI::UIValue::String(InText));
        Session->bSubtitle = InText[0] != '\0';
        Session->UpdateCinematicOverlay();
    }

    /* A client's copy of a replicated player follows the server's. */
    bool GameSession::SequenceIsReplica(const GameCore::EntityID InPlayer, void* InUserData)
    {
        auto* Session = static_cast<GameSession*>(InUserData);
        return IsClient() && Session->LevelWorld && Session->LevelWorld->IsAlive(InPlayer)
            && Session->LevelWorld->GetEntity(InPlayer).HasComponent<GameCore::NetComponent>();
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
#if WITH_AUDIO
        /* The World stops ticking, so its sounds are held here; the UI's
         * (PlaySound2D from a menu) keep the device. */
        if(LevelAudio) { LevelAudio->SetPaused(bPaused); }
#endif
    }

    void GameSession::Tick(const FrameContext& InFrame)
    {
        /* The mode's OnTick overlaps the render thread: game state only. */
        CheckGameThread("Tick");
        FrameDelta  = float(InFrame.DeltaSeconds);
        FramePixels = InFrame.ViewportPixels;

        /* First: a fade-out finishing arms the swap for the next ApplyPendingLevel. */
        UpdateTransition(FrameDelta);

        /* The UI sees the input first; the game, what it leaves. */
        RouteInputThroughUI();
        UpdateDropIn();

        /* Re-read: a level swap may have replaced the mode. */
        if(ActiveLevel && !bPaused && !bTransitionWarming)
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
        if(LevelWorld && !bPaused && !bTransitionWarming)
        {
            SetScriptInputState(&FilteredInput);
            SetScriptInputHost(&Host);
            LevelWorld->Tick(float(InFrame.DeltaSeconds));
            SetScriptInputHost(nullptr);
            SetScriptInputState(nullptr);
        }
        else if(LevelWorld && !bTransitionWarming)
        {
            /* Paused: the components still follow the input, so what the pause
             * menu consumed is not waiting for the game when it resumes. (Not while a
             * transition warms up: nothing reaches the World then, not even the input
             * filters -- a mode's filter would run on with the load's long frame.) */
            ResolveInputComponents(*LevelWorld, FilteredInput);
            if(Avatars) { Avatars->Update(0.0f, false); }
        }
        PublishViewTarget();
        if(LevelWorld) { AIDetail::DrawAIDebug(*LevelWorld, Rendering); }

        PublishPhysicsStat();
        PublishAudioStat();

        /* After the World: this frame's values, then layout, paint and submit
         * of whatever changed -- before the host's EndFrame renders. */
        PublishUIValues();
        Console->Update();
        Interface->Tick(float(InFrame.DeltaSeconds));
        PublishPlayerAreas();
        Interface->Update(UI::UIVec2{ float(InFrame.ViewportPixels.Width), float(InFrame.ViewportPixels.Height) });
    }

    bool GameSession::JoinLocalPlayer(const uint8_t InLocalPlayer)
    {
        CheckGameThread("JoinLocalPlayer");
        if(InLocalPlayer == 0 || InLocalPlayer >= INPUT_PLAYERS_MAX || !ActiveLevel || !LevelWorld) { return false; }
        IGameMode* Mode = ActiveLevel->GetGameMode();
        if(!Mode || !Mode->OnLocalPlayerJoin(InLocalPlayer)) { return false; }
        CORE_LOG("[Splitscreen] local player " + std::to_string(int(InLocalPlayer) + 1) + " joined")
        return true;
    }

    bool GameSession::LeaveLocalPlayer(const uint8_t InLocalPlayer)
    {
        CheckGameThread("LeaveLocalPlayer");
        if(InLocalPlayer == 0 || InLocalPlayer >= INPUT_PLAYERS_MAX || !ActiveLevel || !LevelWorld) { return false; }
        IGameMode* Mode = ActiveLevel->GetGameMode();
        if(!Mode || !Mode->OnLocalPlayerLeave(InLocalPlayer)) { return false; }
        ViewTargets[InLocalPlayer] = GameCore::EntityID();
        CORE_LOG("[Splitscreen] local player " + std::to_string(int(InLocalPlayer) + 1) + " left")
        return true;
    }

    void GameSession::UpdateDropIn()
    {
        const bool bDropIn = GSplitscreenDropIn.GetBool() && LevelWorld && !bPaused && !bCinematic;
        for(uint8_t Local = 1; Local < INPUT_PLAYERS_MAX; ++Local)
        {
            const bool bConnected = FilteredInput.IsPadConnected(Local);
            const bool bWas       = bPadWasConnected[Local];
            bPadWasConnected[Local] = bConnected;
            if(!bDropIn) { continue; }
            const bool bJoined = FindLocalPlayer(*LevelWorld, Local).IsSet();
            if(bConnected && !bJoined && FilteredInput.WasPressed(FindInputKey("Gamepad.Start"), Local))
            {
                JoinLocalPlayer(Local);
                FilteredInput.Consume(FindInputKey("Gamepad.Start"), Local);   // a joining press is not the game's
            }
            else if(bWas && !bConnected && bJoined)
            {
                LeaveLocalPlayer(Local);
            }
        }
    }

    void GameSession::PublishPlayerAreas()
    {
        /* A fixed layout: each player's view's rectangle. The dynamic split: halves (quadrants)
         * of the window that stay put while its line turns -- a HUD in each player's corner. */
        RenderCore::RenderViewRect Rects[RenderCore::RENDER_VIEWS_MAX];
        const bool     bDynamic = SplitPlayers >= 2;
        const uint32_t Players  = bDynamic ? SplitPlayers : ViewCount;
        const uint32_t Count    = Players > 1 ? RenderCore::MakeSplitscreenLayout(FramePixels, Players, bDynamic || GSplitscreenSideBySide.GetBool(), Rects) : 0;
        for(int32_t Local = 0; Local < UI::UI_PLAYERS_MAX; ++Local)
        {
            UI::UIRect Area;   // empty: the whole window
            if(uint32_t(Local) < Count)
            {
                const RenderCore::RenderViewRect& Rect = Rects[Local];
                Area = UI::UIRect{ float(Rect.X), float(Rect.Y), float(Rect.Width), float(Rect.Height) };
            }
            Interface->SetPlayerArea(Local, Area);
        }
    }

    void GameSession::PublishPhysicsStat()
    {
        if(!GStatsPhysics.GetBool() || !LevelWorld)
        {
            if(bPhysicsStatShown)
            {
                Rendering.SetStatText("Physics", std::string());
                bPhysicsStatShown = false;
            }
            return;
        }

        std::string Text;
#if WITH_JOLTPHYSICS
        if(Physics)
        {
            JoltPhysics::PhysicsStats Stats;
            Physics->GetStats(Stats);
            char Buffer[512];
            std::snprintf(Buffer, sizeof(Buffer),
                          "Bodies    %6u\n"
                          "  Awake   %6u\n"
                          "  Dynamic %6u\n"
                          "  Kinemat %6u\n"
                          "  Static  %6u\n"
                          "  Sensors %6u\n"
                          "Joints    %6u\n"
                          "Vehicles  %6u\n"
                          "Ragdolls  %6u\n"
                          "Contacts  %6u\n"
                          "Steps     %6u\n"
                          "Step   %6.2f ms\n"
                          "Sync   %6.2f ms",
                          Stats.Bodies, Stats.ActiveBodies, Stats.DynamicBodies, Stats.KinematicBodies,
                          Stats.StaticBodies, Stats.Sensors, Stats.Constraints, Stats.Vehicles, Stats.Ragdolls, Stats.Contacts,
                          Stats.StepsLastFrame, double(Stats.StepMs), double(Stats.SyncMs));
            Text = Buffer;
        }
        else
        {
            Text = "not running";
        }
#else
        Text = "not compiled in\n(NIMBLE_WITH_\n JOLTPHYSICS=OFF)";
#endif
        Rendering.SetStatText("Physics", Text);
        bPhysicsStatShown = true;
    }

    void GameSession::PublishAudioStat()
    {
        if(!GStatsAudio.GetBool() || !LevelWorld)
        {
            if(bAudioStatShown)
            {
                Rendering.SetStatText("Audio", std::string());
                bAudioStatShown = false;
            }
            return;
        }

        std::string Text;
#if WITH_AUDIO
        if(LevelAudio)
        {
            Audio::AudioWorldStats Stats;
            LevelAudio->GetStats(Stats);
            const Audio::AudioDevice&      Device   = Audio::AudioDevice::Get();
            const Audio::AudioListenerPose Listener = LevelAudio->GetListener();
            char Buffer[512];
            std::snprintf(Buffer, sizeof(Buffer),
                          "Sources   %6u\n"
                          "  Playing %6u\n"
                          "One-shots %6u\n"
                          "Listener %.0f %.0f %.0f\n"
                          "Device %u Hz %u ch%s",
                          Stats.Sources, Stats.Playing, Stats.OneShots,
                          Listener.Position.GetX(), Listener.Position.GetY(), Listener.Position.GetZ(),
                          Device.GetSampleRate(), Device.GetChannels(),
                          Device.IsOffline() ? " offline" : (LevelAudio->IsPaused() ? " paused" : ""));
            Text = Buffer;
        }
        else
        {
            Text = "no output device";
        }
#else
        Text = "not compiled in\n(NIMBLE_WITH_\n AUDIO=OFF)";
#endif
        Rendering.SetStatText("Audio", Text);
        bAudioStatShown = true;
    }

    void GameSession::SetViewTarget(const GameCore::EntityID InCamera, const uint8_t InLocalPlayer)
    {
        CheckGameThread("SetViewTarget");
        if(InLocalPlayer >= INPUT_PLAYERS_MAX) { return; }
        ViewTargets[InLocalPlayer] = InCamera;
        PublishViewTarget();
    }

    GameCore::EntityID GameSession::GetViewTarget(const uint8_t InLocalPlayer) const
    {
        if(InLocalPlayer >= INPUT_PLAYERS_MAX) { return GameCore::EntityID(); }
        const GameCore::EntityID Target = ViewTargets[InLocalPlayer];
        if(LevelWorld && Target.IsSet() && LevelWorld->IsAlive(Target)) { return Target; }
        return Avatars ? Avatars->GetViewCamera(InLocalPlayer) : GameCore::EntityID();
    }

    void GameSession::SetViewCount(const uint32_t InCount)
    {
        CheckGameThread("SetViewCount");
        ViewCountWanted = std::min<uint32_t>(InCount, RenderCore::RENDER_VIEWS_MAX);
        PublishViewTarget();
    }

    void GameSession::PublishRevealPoints()
    {
        EngineCore::Vector3 Points[RenderCore::RENDER_REVEAL_MAX];
        uint32_t Count = 0;
        if(LevelWorld && GRevealPlayers.GetBool())
        {
            for(uint8_t Local = 0; Local < INPUT_PLAYERS_MAX && Count < RenderCore::RENDER_REVEAL_MAX; ++Local)
            {
                const GameCore::EntityID Avatar = GetLocalAvatar(*LevelWorld, Local);
                if(!Avatar.IsSet() || !LevelWorld->IsAlive(Avatar)) { continue; }
                Points[Count++] = LevelWorld->GetEntity(Avatar).GetWorldPose().Position
                                + EngineCore::Vector3(0.0, double(GRevealHeight.GetFloat()), 0.0);
            }
        }
        Rendering.SetRevealPoints(RenderCore::RENDER_DEFAULT_SCENE, Points, Count, GRevealRadius.GetFloat());
    }

    void GameSession::PublishViewTarget()
    {
        if(!LevelWorld) { return; }
        PublishRevealPoints();

        /* A target's camera: its own, else its first descendant's. */
        const auto CameraOf = [this](GameCore::EntityID InTarget)
        {
            for(int Depth = 0; InTarget.IsSet() && Depth < 16; ++Depth)
            {
                const GameCore::Entity Node = LevelWorld->GetEntity(InTarget);
                if(Node.HasComponent<RenderCore::CameraComponent>()) { break; }
                InTarget = Node.GetFirstChild();
            }
            return InTarget;
        };

        /* SPLITSCREEN's dynamic split, when it is on and there are players to split. */
        const bool bMayShare = GSplitscreenEnabled.GetBool() && !bCinematic && !bSequenceView;
        if(bMayShare && PublishSplitCamera())
        {
            return;
        }
        SplitPlayers = 0;
        Rendering.SetViewSites(RenderCore::RENDER_DEFAULT_SCENE, RenderCore::RenderViewSites());

        /* SPLITSCREEN: a view per local player with one, up to the last;
         * one while a sequence holds the camera. */
        constexpr uint32_t ViewsMax = std::min<uint32_t>(RenderCore::RENDER_VIEWS_MAX, INPUT_PLAYERS_MAX);
        uint32_t Count = 1;
        if(GSplitscreenEnabled.GetBool() && !bCinematic && !bSequenceView)
        {
            if(ViewCountWanted > 0)
            {
                Count = std::min(ViewCountWanted, ViewsMax);
            }
            else
            {
                for(uint32_t Local = 1; Local < ViewsMax; ++Local)
                {
                    if(GetViewTarget(uint8_t(Local)).IsSet()) { Count = Local + 1; }
                }
            }
        }
        for(uint32_t View = 0; View < RenderCore::RENDER_VIEWS_MAX; ++View)
        {
            /* A view past the count lets go of its camera; a player without one sees player 1's. */
            const GameCore::EntityID Camera = View < Count ? CameraOf(GetViewTarget(uint8_t(View))) : GameCore::EntityID();
            if(Camera != Rendering.GetViewCamera(RenderCore::RENDER_DEFAULT_SCENE, View))
            {
                Rendering.SetViewCamera(RenderCore::RENDER_DEFAULT_SCENE, View, Camera);
            }
        }
        Rendering.SetViewCount(RenderCore::RENDER_DEFAULT_SCENE, Count, GSplitscreenSideBySide.GetBool());
        ViewCount = Count;
    }

    void GameSession::SetSplitCamera(const SplitCameraSettings& InSettings)
    {
        CheckGameThread("SetSplitCamera");
        SplitCamera     = InSettings;
        bSplitCameraSet = true;
        PublishViewTarget();
    }

    SplitCameraSettings GameSession::GetSplitCamera() const
    {
        if(bSplitCameraSet) { return SplitCamera; }
        SplitCameraSettings Settings;
        Settings.bEnabled = GSplitscreenDynamic.GetBool();
        Settings.Rotation = EngineCore::Vector3(GSplitscreenDynamicPitch.GetFloat(), GSplitscreenDynamicYaw.GetFloat(), 0.0);
        Settings.Distance = GSplitscreenDynamicDistance.GetFloat();
        Settings.FovY     = GSplitscreenDynamicFov.GetFloat();
        Settings.Margin   = GSplitscreenDynamicMargin.GetFloat();
        return Settings;
    }

    bool GameSession::PublishSplitCamera()
    {
        const SplitCameraSettings Settings = GetSplitCamera();
        if(!Settings.bEnabled || !LevelWorld) { SplitPlayers = 0; return false; }

        /* Local players 1, 2, ... while each has an avatar. */
        EngineCore::Vector3 Players[4];
        uint32_t Count = 0;
        while(Count < 4 && Count < INPUT_PLAYERS_MAX)
        {
            const GameCore::EntityID Avatar = GetLocalAvatar(*LevelWorld, uint8_t(Count));
            if(!Avatar.IsSet() || !LevelWorld->IsAlive(Avatar)) { break; }
            Players[Count++] = LevelWorld->GetEntity(Avatar).GetWorldPose().Position;
        }
        if(Count < 2) { SplitPlayers = 0; return false; }
        SplitPlayers = Count;

        SplitCameraFrame Frame;
        SolveSplitCamera(Settings, Players, Count, FramePixels, FrameDelta, SplitAngle, bSplitAngleSet, Frame);

        /* Its cameras: made on first use, turned the settings' way, each on its focus. */
        for(uint32_t View = 0; View < Frame.ViewCount; ++View)
        {
            GameCore::EntityID& Id = SplitCameras[View];
            if(!Id.IsSet() || !LevelWorld->IsAlive(Id))
            {
                char Name[32];
                std::snprintf(Name, sizeof(Name), "Split Camera %u", View + 1);
                GameCore::Entity Made = LevelWorld->CreateEntity(Name);
                Made.AddComponent<GameCore::TransformComponent>();
                RenderCore::CameraComponent Lens;
                Lens.bActive    = 0;   // never the level's first active camera
                Lens.Projection = RenderCore::CP_Perspective;
                Made.AddComponent(Lens);
                Id = Made.GetID();
            }
            GameCore::Entity Camera = LevelWorld->GetEntity(Id);
            if(GameCore::TransformComponent* At = Camera.GetComponent<GameCore::TransformComponent>())
            {
                At->Position = SplitCameraPosition(Settings, Frame.Focus[View]);
                At->Rotation = Settings.Rotation;
            }
            if(RenderCore::CameraComponent* Lens = Camera.GetComponent<RenderCore::CameraComponent>())
            {
                Lens->FovY  = Settings.FovY;
                Lens->ZNear = Settings.ZNear;
                Lens->ZFar  = Settings.ZFar;
            }
        }

        for(uint32_t View = 0; View < RenderCore::RENDER_VIEWS_MAX; ++View)
        {
            const GameCore::EntityID Camera = View < Frame.ViewCount ? SplitCameras[View] : GameCore::EntityID();
            if(Camera != Rendering.GetViewCamera(RenderCore::RENDER_DEFAULT_SCENE, View))
            {
                Rendering.SetViewCamera(RenderCore::RENDER_DEFAULT_SCENE, View, Camera);
            }
        }
        RenderCore::RenderViewSites Sites;
        if(Frame.bSites)
        {
            Sites.Count = Frame.ViewCount;
            for(uint32_t View = 0; View < Frame.ViewCount; ++View)
            {
                Sites.X[View] = Frame.SiteX[View];
                Sites.Y[View] = Frame.SiteY[View];
            }
            Sites.DividerPixels = Settings.DividerPixels;
            Sites.DividerAlpha  = Frame.DividerAlpha;
            Sites.DividerColor  = Settings.DividerColor;
        }
        Rendering.SetViewSites(RenderCore::RENDER_DEFAULT_SCENE, Sites);
        Rendering.SetViewCount(RenderCore::RENDER_DEFAULT_SCENE, Frame.ViewCount, GSplitscreenSideBySide.GetBool());
        ViewCount = Frame.ViewCount;
        return true;
    }

    void GameSession::ResolveInput(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        ResolveInputComponents(InWorld, static_cast<GameSession*>(InUserData)->FilteredInput);
    }

    void GameSession::RequestLevel(ILevel& InLevel)
    {
        RequestLevel(InLevel, LevelTravel());
    }

    void GameSession::RequestLevel(ILevel& InLevel, const LevelTravel& InTravel)
    {
        /* With a transition, from a loaded level: fade out first (UpdateTransition
         * arms the swap). Once armed (TS_Loading), a request just retargets it. */
        const bool bFade = InTravel.Transition == TT_Fade || (InTravel.Transition == TT_Default && GTransitionEnabled.GetBool());
        if(bFade && LevelWorld && TransitionStage != TS_Loading)
        {
            if(TransitionStage != TS_FadeOut)
            {
                CloseTransitionCard();
                /* From a fade-in under way, on from the black it has reached. */
                const float Out  = (std::max)(0.0f, GTransitionFadeOut.GetFloat());
                TransitionTime   = TransitionStage == TS_FadeIn ? TransitionFade * Out : 0.0f;
                TransitionStage  = TS_FadeOut;
            }
            TransitionLevel  = &InLevel;
            TransitionTravel = InTravel;
            return;
        }
        if(!bFade && TransitionStage == TS_FadeOut)
        {
            EndTransition();   // a straight swap overrides a fade under way
        }

        if(PendingLevel)
        {
            CORE_LOG_WARN("GameSession::RequestLevel called twice before the first took effect; "
                          "the later request wins")
        }
        PendingLevel  = &InLevel;
        PendingTravel = InTravel;
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
        if(Session->bInactiveChecked && !Session->bInactiveWarned && Session->ActiveLevel && Session->LevelWorld
           && IsInactiveModuleComponent(InType))
        {
            Session->bInactiveWarned = WarnInactiveModuleComponents(*Session->LevelWorld, Session->ActiveLevel->GetName());
        }
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
