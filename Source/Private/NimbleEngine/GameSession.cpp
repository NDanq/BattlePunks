#include "Public/NimbleEngine/GameSession.h"

#include <string>
#include <utility>

#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldSerializer.h"
#include "Public/RenderCore/Renderer.h"

/* Chunk tags shared with SaveGame.cpp. */
#include "Private/NimbleEngine/InputInternal.h"
#include "Private/NimbleEngine/SaveGameFormat.h"

/* Lua file run on every level load, after IGameMode::OnLoad (see LoadLevel).
 * Empty skips it. */
NIMBLE_CONFIG_STRING(GStartupScript, "Scripting", "StartupScript", "", nullptr);

namespace NimbleEngine
{
    GameSession::GameSession(IGameHost& InHost, GameCore::ScriptEngine& InScripts, RenderCore::Renderer& InRenderer)
        : Host(InHost)
        , Scripts(InScripts)
        , Rendering(InRenderer)
    {
    }

    GameSession::~GameSession()
    {
        UnloadLevel();
    }

    bool GameSession::LoadLevel(ILevel& InLevel)
    {
        CORE_CHECK(LevelWorld == nullptr, "GameSession::LoadLevel called with a level already loaded")

        /* Bound before OnLoad, or scripts and WorldBridge would miss the
         * level's first entities. */
        LevelWorld = EngineCore::Memory::Alloc<GameCore::World>();
        ++LoadCount;

        ActiveLevel             = &InLevel;
        ActiveLevel->OwnedWorld = LevelWorld;

        Scripts.BindWorld(LevelWorld);
        Rendering.BindWorld(*LevelWorld);

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

        /* The script sees the loaded level; OnStart sees what it made. Failure
         * is logged, not fatal. Re-run per load: spawned entities died with the
         * old World. */
        const char* StartupScriptPath = GStartupScript.GetString();
        if(StartupScriptPath && StartupScriptPath[0] != '\0')
        {
            Scripts.RunFile(StartupScriptPath);
        }

        /* Before the restore and OnStart -- see the header's sequence. */
        GameState* State = InLevel.GetGameState();
        if(State)
        {
            State->OnReset();
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

        return !Ar.HasError();
    }

    void GameSession::UnloadLevel()
    {
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
        if(IGameMode* Mode = ActiveLevel->GetGameMode())
        {
            Mode->OnUnload();
        }

        ActiveLevel->OnUnload();

        if(LevelWorld)
        {
            Scripts.UnbindWorld();
            Rendering.UnbindWorld();

            EngineCore::Memory::Dealloc(LevelWorld);
            LevelWorld = nullptr;
        }

        CORE_LOG("[Level] unloaded '" + std::string(ActiveLevel->GetName()) + "'")

        ActiveLevel->OwnedWorld = nullptr;
        ActiveLevel             = nullptr;
    }

    void GameSession::ApplyPendingLevel()
    {
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

    void GameSession::Tick(const FrameContext& InFrame)
    {
        /* Re-read: a level swap may have replaced the mode. */
        if(ActiveLevel)
        {
            if(IGameMode* Mode = ActiveLevel->GetGameMode())
            {
                Mode->OnTick(InFrame);
            }
        }

        /* WorldBridge reads the World later, in the host's Renderer::EndFrame. */
        if(LevelWorld)
        {
            SetScriptInputState(&Host.GetInput());
            SetScriptInputHost(&Host);
            LevelWorld->Tick(float(InFrame.DeltaSeconds));
            SetScriptInputHost(nullptr);
            SetScriptInputState(nullptr);
        }
    }

    void GameSession::ResolveInput(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        ResolveInputComponents(InWorld, static_cast<GameSession*>(InUserData)->Host.GetInput());
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
}
