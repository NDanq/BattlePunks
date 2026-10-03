#ifndef NIMBLEENGINE_GAMESESSION_H
#define NIMBLEENGINE_GAMESESSION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/Input.h"
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

    /* Gives GameCore's CS_Mesh colliders their shapes from RenderCore's mesh
     * headers (CollisionMeshes.cpp). Done when this image loads and again by
     * every GameSession; idempotent. A host without a session calls it. */
    NE_API void InstallCollisionMeshProvider();

    /* Declared ahead of the friend declarations below to avoid MSVC C4273
     * (inconsistent linkage). Documented in SaveGame.h. */
    NE_API bool SaveGameToFile  (GameSession& InSession, const std::string& InPath);
    NE_API bool LoadGameFromFile(GameSession& InSession, const std::string& InPath);

    /*
     * Services the program running a session (Application, or the editor's
     * Play-in-Editor) offers the game. Keep it small: genuine host services
     * only, never a back door to the host's internals.
     */
    class IGameHost
    {
    public:
        virtual ~IGameHost() = default;

        /* Frame pacing target; 0 is unlimited. Takes effect from the next frame. */
        virtual void     SetFrameRateLimit(uint32_t InFramesPerSecond) = 0;
        virtual uint32_t GetFrameRateLimit() const = 0;

        /* This frame's input, latched after the event pump (InputRouter). A
         * host without input -- a test, a server -- keeps the default: nothing
         * pressed. */
        virtual const InputState& GetInput() const
        {
            static const InputState None;
            return None;
        }

        /* Mouse capture for mouse-look (InputRouter::SetMouseCaptured). A host
         * without a window ignores it. */
        virtual void SetMouseCaptured(bool /*bInCaptured*/) {}
        virtual bool IsMouseCaptured() const { return false; }
    };

    /*
     * ONE RUNNING GAME: the active level, its World, mode and state, and the
     * load / swap / save / restore sequence. Each frame the host calls
     * ApplyPendingLevel() then Tick(); the host owns the window, event pump,
     * pacing, frame fence and present.
     *
     * The split lets the same game run in the game executable and in the
     * editor: game code only ever sees the session, never the host.
     *
     * NOT OWNED: the ScriptEngine and Renderer belong to the host and live for
     * the process. The session binds them to its World only while a level is
     * loaded. The World is the session's, one per loaded level.
     *
     * Game thread only.
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
         * THE LOAD SEQUENCE:
         *
         *   World created, scripts and renderer bound
         *   ILevel::OnLoad            the level populates the World
         *   IGameMode::OnLoad
         *   the startup script        why the mode has two callbacks
         *   GameState::OnReset
         *   [only when restoring a save]
         *       ILevel::OnPrepareForLoadedGame
         *       the World, then the GameState, from the pending save
         *       IGameMode::OnPostLoadSave
         *   IGameMode::OnStart
         *
         * Restoring after OnReset leaves uncovered fields at defaults; before
         * OnStart, so the mode starts against restored state.
         *
         * Immediate: only where no World of this session is ticking (games use
         * RequestLevel). InLevel must outlive its time loaded. False if the
         * level or mode refused; the level stays loaded, so call UnloadLevel().
         */
        bool LoadLevel(ILevel& InLevel);

        /* Mode OnUnload, level OnUnload, unbind scripts and renderer, destroy
         * the World. Idempotent. */
        void UnloadLevel();

        /* Applies a pending RequestLevel() at the top of a frame. A level that
         * fails to load is unloaded and the session requests quit. */
        void ApplyPendingLevel();

        /* IGameMode::OnTick, then World::Tick. Overlaps the render thread
         * draining the previous frame. */
        void Tick(const FrameContext& InFrame);

        /* RequestQuit() was called or a requested level failed to load. The
         * host decides what that means (exit, or end play). */
        bool IsQuitRequested() const { return bQuitRequested; }

        // === What a game uses ====================================================

        /* Swap to InLevel (which must outlive its time loaded) at the next
         * ApplyPendingLevel(): a World cannot be destroyed inside its own Tick.
         * The outgoing level unloads first. The last request in a frame wins. */
        void RequestLevel(ILevel& InLevel);

        /* The game is finished. The host finishes the frame in flight. */
        void RequestQuit() { bQuitRequested = true; }

        /* nullptr before the first load and after the last unload. */
        ILevel*    GetLevel()     { return ActiveLevel; }
        /* GetLevel()->GetGameState(), or nullptr. */
        GameState* GetGameState();

        /* Valid from ILevel::OnLoad through OnUnload (CORE_CHECKed). A swap
         * replaces it; don't hold the reference. */
        GameCore::World& GetWorld();
        bool             HasWorld() const { return LevelWorld != nullptr; }

        /* Bumped by every LoadLevel. Use this, not the World pointer (which
         * can be reused), to detect a replaced World. */
        uint32_t GetLoadCount() const { return LoadCount; }

        /* The next ApplyPendingLevel will free the current World; drop any
         * bindings to it first. */
        bool HasPendingLevel() const { return PendingLevel != nullptr; }

        /* Lent by the host. Script classes survive a level swap; instances do
         * not. The Renderer is for RegisterProxyType and RenderStats. */
        GameCore::ScriptEngine& GetScriptEngine() { return Scripts; }
        RenderCore::Renderer&   GetRenderer()     { return Rendering; }

        IGameHost& GetHost() { return Host; }

        /* This frame's devices (Input.h). Gameplay usually reads an
         * InputComponent, resolved from this at the start of TP_PrePhysics. */
        const InputState& GetInput() const { return Host.GetInput(); }

        /* Hides and holds the cursor, for mouse-look; Mouse.DeltaX keeps
         * coming past the screen's edge. The host lets go when the run ends. */
        void SetMouseCaptured(bool bInCaptured) { Host.SetMouseCaptured(bInCaptured); }
        bool IsMouseCaptured() const            { return Host.IsMouseCaptured(); }

    private:
        /* Friends, not a public setter: only SaveGame.cpp may arm a restore. */
        friend NE_API bool SaveGameToFile(GameSession& InSession, const std::string& InPath);
        friend NE_API bool LoadGameFromFile(GameSession& InSession, const std::string& InPath);

        /* The before-scripts TP_PrePhysics system: InputComponents from GetInput(). */
        static void ResolveInput(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

        /* LoadLevel's restore step. False on a corrupt save; the level stays loaded. */
        bool RestoreSaveInto(EngineCore::Archive& Ar, ILevel& InLevel, GameState* InState);

        IGameHost&              Host;
        GameCore::ScriptEngine& Scripts;
        RenderCore::Renderer&   Rendering;

        /* LevelWorld lives from LoadLevel to UnloadLevel; same as ILevel::OwnedWorld. */
        ILevel*           ActiveLevel = nullptr;
        GameCore::World*  LevelWorld  = nullptr;

        /* Set by RequestLevel, consumed by the next ApplyPendingLevel. */
        ILevel*           PendingLevel = nullptr;

        /* Armed with PendingLevel by LoadGameFromFile, cleared by the LoadLevel
         * that consumes it. (C4251 on this private member is harmless.) */
        std::vector<uint8_t> PendingSaveBytes;

        bool bQuitRequested = false;

        uint32_t LoadCount = 0;
    };
}

#endif //NIMBLEENGINE_GAMESESSION_H
