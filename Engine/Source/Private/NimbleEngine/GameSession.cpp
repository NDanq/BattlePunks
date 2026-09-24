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

/* The save format's own tags, shared with SaveGame.cpp -- LoadLevel reads the
 * World and GameState chunks that file wrote. */
#include "Private/NimbleEngine/SaveGameFormat.h"

/* Bundle-relative path to a Lua file run once per LEVEL LOAD, after
 * IGameMode::OnLoad and before the first tick -- see LoadLevel's own comment on
 * why that ordering is where this runs, and why it re-runs on every swap. Empty
 * (the default) skips scripting entirely; a game with nothing to say in Lua
 * pays nothing for this. */
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

        ActiveLevel             = &InLevel;
        ActiveLevel->OwnedWorld = LevelWorld;

        Scripts.BindWorld(LevelWorld);
        Rendering.BindWorld(*LevelWorld);

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

        /*
         * Between the mode's two callbacks: the script sees the loaded level,
         * OnStart sees what the script made. Skipped when empty; a failure is
         * logged, not fatal. Re-run on every load -- registered classes survive
         * a swap, but what the script spawned died with the old World.
         */
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

        /* The save restore; the header says why here. PendingSaveBytes is
         * emptied first, so a failed restore is not retried by the next
         * ordinary level load. */
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
        /* The World first, so a GameState can look entities up while it loads.
         * The WithScripts overload re-instantiates restored ScriptComponents. */
        if(!GameCore::SerializeWorldWithScripts(Ar, *LevelWorld, Scripts))
        {
            return false;
        }

        /* Optional: absent when the saving level had no GameState. Missing, the
         * state keeps its reset values, so older saves still load. */
        uint16_t StateVersion = 0;
        if(Ar.OpenChunk(TAG_GAMESTATE, StateVersion))
        {
            if(InState)
            {
                /* Inside the GMST chunk, so Serialize reads StateVersion through
                 * Ar.GetChunkVersion(). */
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
         *   1. mode, then level, OnUnload -- both unbind from a live World.
         *   2. Scripts.UnbindWorld(): OnDestroy for live scripts; needs the
         *      lua_State and the World.
         *   3. Rendering.UnbindWorld(): WorldBridge's delegate bindings must go
         *      before ~World, which warns about any left.
         *   4. destroy the World.
         * Null-safe throughout: LoadLevel's failure path runs this too.
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

        /* The game first, then World's phases (the spatial refit ends
         * TP_Physics). The World never touches RenderCore; WorldBridge reads it
         * inside Renderer::EndFrame, which the host calls. */
        if(LevelWorld)
        {
            LevelWorld->Tick(float(InFrame.DeltaSeconds));
        }
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
