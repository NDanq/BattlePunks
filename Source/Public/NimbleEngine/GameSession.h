#ifndef NIMBLEENGINE_GAMESESSION_H
#define NIMBLEENGINE_GAMESESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/Core/CoreArchive.h"      // EngineCore::Archive, for RestoreSaveInto below

namespace GameCore
{
    class World;
    class ScriptEngine;
}

namespace RenderCore
{
    class Renderer;
}

namespace NimbleEngine
{
    class GameSession;
    class GameState;
    class ILevel;

    /* Declared ahead of GameSession so its friend declarations need not repeat
     * NE_API -- MSVC calls that inconsistent linkage (C4273), as with
     * CoreConfig.h's ConfigRegisterVar. Documented in SaveGame.h. */
    NE_API bool SaveGameToFile  (GameSession& InSession, const std::string& InPath);
    NE_API bool LoadGameFromFile(GameSession& InSession, const std::string& InPath);

    /*
     * What the program running a session offers the game beyond the session
     * itself -- the few things that belong to the HOST, not the game. The game
     * executable's Application implements it; an editor running a game in its
     * viewport will implement it too, answering for its own loop.
     *
     * Deliberately small: add a method here only for a genuine host service,
     * never as a back door to the host's internals.
     */
    class IGameHost
    {
    public:
        virtual ~IGameHost() = default;

        /* Frame pacing target; 0 is unlimited. Takes effect from the next frame. */
        virtual void     SetFrameRateLimit(uint32_t InFramesPerSecond) = 0;
        virtual uint32_t GetFrameRateLimit() const = 0;
    };

    /*
     * ONE RUNNING GAME: the active level, its World, its mode and state, and the
     * load / swap / save / restore sequence around them -- everything that used
     * to live inside Application except the program around it. A host creates a
     * session, loads a level into it, and every frame calls ApplyPendingLevel()
     * then Tick(); the host alone owns the window, the event pump, pacing and
     * the renderer's frame fence and present.
     *
     * WHY THE SPLIT. The same game has to run in more than one host: the game
     * executable (Application, an SDL window and loop) and, for Play-in-Editor,
     * the Qt editor (its own loop and viewport). A level or mode is handed this
     * session, never the host, so nothing in game code knows which one it is in.
     *
     * NOT OWNED: the ScriptEngine and Renderer are process-lifetime and belong
     * to the host (one lua_State and one render thread serve every session and
     * every level). The session binds them to its World while a level is
     * loaded and unbinds them on unload, so a host can bind the Renderer to a
     * different World (the editor's) whenever no level is loaded here. The
     * World itself IS the session's, one per loaded level.
     *
     * GAME THREAD ONLY, like everything it drives.
     */
    class NE_API GameSession
    {
    public:
        GameSession(IGameHost& InHost, GameCore::ScriptEngine& InScripts, RenderCore::Renderer& InRenderer);

        /* Unloads whatever is still loaded. */
        ~GameSession();

        GameSession(const GameSession&)            = delete;
        GameSession& operator=(const GameSession&) = delete;

        // === Driving it (the host) =============================================

        /*
         * THE LOAD SEQUENCE -- the ordering every other comment points at:
         *
         *   World created, scripts and renderer bound
         *   ILevel::OnLoad            the level populates the World
         *   IGameMode::OnLoad
         *   the startup script        between the mode's two callbacks, which is
         *                             the whole point of having two
         *   GameState::OnReset
         *   [only when restoring a save]
         *       ILevel::OnPrepareForLoadedGame
         *       the World, then the GameState, from the pending save
         *       IGameMode::OnPostLoadSave
         *   IGameMode::OnStart
         *
         * The restore comes after OnReset, so fields a save does not cover are
         * defaults rather than stale values, and before OnStart, so a mode starts
         * against restored state.
         *
         * Immediate. For the first level, or anywhere no World of this session is
         * ticking; a game asks for a swap with RequestLevel instead. InLevel must
         * outlive its time loaded. False if the level or mode refused -- the
         * level stays loaded either way, so UnloadLevel() still pairs with OnLoad
         * having been called.
         */
        bool LoadLevel(ILevel& InLevel);

        /*
         * The reverse, in order: mode OnUnload, level OnUnload, unbind scripts,
         * unbind the renderer, destroy the World. Null-safe and idempotent --
         * LoadLevel's failure path runs it too.
         */
        void UnloadLevel();

        /* Applies a pending RequestLevel(). Call at the top of a frame, where no
         * World is ticking. A requested level that fails to load is unloaded and
         * the session asks to quit (IsQuitRequested). */
        void ApplyPendingLevel();

        /* IGameMode::OnTick, then World::Tick. The mode is re-read each frame,
         * since a swap may have replaced it. Runs while the render thread drains
         * the previous frame; enqueues nothing itself, a mode's OnTick may. */
        void Tick(const FrameContext& InFrame);

        /* True once the game has called RequestQuit(), or a requested level
         * failed to load. What the host does about it is the host's business:
         * the executable exits, an editor ends play. */
        bool IsQuitRequested() const { return bQuitRequested; }

        // === What a game uses ====================================================

        /*
         * Swap to InLevel, which must outlive its time loaded. Always deferred to
         * the host's next ApplyPendingLevel() -- destroying a World from inside
         * its own Tick would free the pools being iterated. The outgoing level is
         * fully unloaded first, so the two never coexist. The later of two
         * requests in one frame wins.
         */
        void RequestLevel(ILevel& InLevel);

        /* The game is finished. The host finishes the frame in flight. */
        void RequestQuit() { bQuitRequested = true; }

        /* nullptr before the first load and after the last unload. */
        ILevel*    GetLevel()     { return ActiveLevel; }
        /* GetLevel()->GetGameState(), or nullptr. */
        GameState* GetGameState();

        /* The active level's World; valid from the start of ILevel::OnLoad to the
         * end of ILevel::OnUnload. A swap replaces it, so re-ask rather than
         * holding the reference. CORE_CHECKed rather than null outside that
         * window -- asking then is a lifecycle bug. */
        GameCore::World& GetWorld();
        bool             HasWorld() const { return LevelWorld != nullptr; }

        /* Bumped by every LoadLevel, a save restore's reload included. How a
         * host that watches the World (the editor's panels) tells that it was
         * replaced: comparing World pointers is not enough, since a new World
         * can be allocated at the address the old one was freed from. */
        uint32_t GetLoadCount() const { return LoadCount; }

        /* A RequestLevel (or a save load) is waiting for the next
         * ApplyPendingLevel, which will free the current World. A host with
         * bindings on that World drops them first. */
        bool HasPendingLevel() const { return PendingLevel != nullptr; }

        /* The host's, lent to the session. The ScriptEngine outlives every level
         * (one lua_State, rebound per level): registered classes survive a swap,
         * instances do not. The Renderer is for RegisterProxyType and
         * RenderStats; the host already handles the frame fence and EndFrame. */
        GameCore::ScriptEngine& GetScriptEngine() { return Scripts; }
        RenderCore::Renderer&   GetRenderer()     { return Rendering; }

        IGameHost& GetHost() { return Host; }

    private:
        /* SaveGame.cpp reads the active level and World and arms
         * PendingSaveBytes. Friends rather than a public setter: LoadLevel's
         * ordering only holds if exactly one caller can arm a restore. */
        friend NE_API bool SaveGameToFile(GameSession& InSession, const std::string& InPath);
        friend NE_API bool LoadGameFromFile(GameSession& InSession, const std::string& InPath);

        /* The restore step of LoadLevel, split out so the sequence above stays
         * readable. False on a corrupt save; the level stays loaded. */
        bool RestoreSaveInto(EngineCore::Archive& Ar, ILevel& InLevel, GameState* InState);

        IGameHost&              Host;
        GameCore::ScriptEngine& Scripts;
        RenderCore::Renderer&   Rendering;

        /* LevelWorld is allocated by LoadLevel, destroyed by UnloadLevel, and is
         * the same object as ILevel::OwnedWorld meanwhile. */
        ILevel*           ActiveLevel = nullptr;
        GameCore::World*  LevelWorld  = nullptr;

        /* Set by RequestLevel, consumed by the next ApplyPendingLevel -- the
         * request usually arrives from inside the outgoing World's Tick. */
        ILevel*           PendingLevel = nullptr;

        /*
         * A save waiting to be restored, armed with PendingLevel by
         * LoadGameFromFile and cleared by the LoadLevel that consumes it, so an
         * ordinary level load never picks up a stale save. A private std::vector
         * on an exported class is C4251 noise, not a hazard.
         */
        std::vector<uint8_t> PendingSaveBytes;

        bool bQuitRequested = false;

        uint32_t LoadCount = 0;
    };
}

#endif //NIMBLEENGINE_GAMESESSION_H
