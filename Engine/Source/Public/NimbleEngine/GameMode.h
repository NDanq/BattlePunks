#ifndef NIMBLEENGINE_GAMEMODE_H
#define NIMBLEENGINE_GAMEMODE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/EntityID.h"

namespace NimbleEngine
{
    class GameSession;
    class GameState;

    /*
     * Per-frame values a game is told, with no pointers into engine state. The
     * host fills ViewportPixels from whatever surface it draws to.
     *
     * Not NE_API: header-only aggregate.
     */
    struct FrameContext
    {
        /* Since the previous tick; zero on the first frame. */
        double           DeltaSeconds = 0.0;

        /* Ticks completed before this one, so the first tick sees 0. */
        uint64_t         FrameNumber  = 0;

        /* PHYSICAL pixels. Size a viewport from this, not from logical units. */
        EngineCore::Size ViewportPixels;
    };

    /*
     * THE GAME'S RULES. The host owns the loop, the GameSession owns the level
     * lifecycle and World::Tick, ILevel owns the entities, and a mode supplies
     * logic at the points below. A mode sees only the GameSession; host
     * services are on GameSession::GetHost().
     *
     * THREADING. Game thread only, which the GameSession enforces: every hook
     * is called from a session entry that CORE_CHECKs it, so a mode need not
     * track or check the thread itself. OnTick overlaps the render thread, so
     * never touch a RenderContext or RenderProxy directly.
     *
     * LIFECYCLE (full sequence: GameSession::LoadLevel):
     *
     *   OnLoad    World populated. Cache handles, bind delegates.
     *   OnStart   startup script run, GameState reset (in its initial phase).
     *             Read Lua-made state here; SetPhase to begin.
     *   OnTick    once per frame.
     *   OnUnload  undo OnLoad; above all, unbind delegates.
     *
     * RULES (GAMEFRAMEWORK.md): the session asks the mode before a phase
     * change (CanEnterPhase) and tells it after (OnPhaseChanged), and tells it
     * when a player (a PlayerStateComponent) comes or goes and when a score or
     * life count changes through GameSession::AddScore / AddLives -- from C++,
     * Lua or a script graph alike. Each may call back into the session; a
     * phase change asked for inside one is applied once it returns.
     *
     * DRAWING: no OnRender. Add a RenderComponent, register a component +
     * RenderProxy pair with RegisterProxyType, or ENQUEUE_RENDER_COMMAND in
     * OnTick (one frame of lag). Commands capture trivially copyable values
     * only: a proxy handle, never a pointer to game-thread state.
     */
    class IGameMode
    {
    public:
        virtual ~IGameMode() = default;

        /* World populated; no tick or startup script yet. False aborts the load
         * (OnUnload still runs). InSession outlives the mode; keep it. */
        virtual bool OnLoad(GameSession& InSession) { (void)InSession; return true; }

        /* Script run, state reset, no tick yet. Skipped if OnLoad failed. */
        virtual void OnStart() {}

        /* Once per frame. The only method a mode must implement. */
        virtual void OnTick(const FrameContext& InFrame) = 0;

        /* A save was restored (before OnStart). Re-derive cached EntityIDs:
         * restored entities are new, so handles from OnLoad are stale. */
        virtual void OnPostLoadSave() {}

        /* Before a save. The mode is not saved; flush what must survive into
         * the GameState. */
        virtual void OnPreSaveGame() {}

        /* After the last tick; World and Renderer still valid. Runs whenever
         * OnLoad was called, even if it failed. Must unbind what OnLoad bound. */
        virtual void OnUnload() {}

        /* The state this mode uses, or nullptr. Owned by the mode; reset by
         * the engine before OnStart. */
        virtual GameState* GetGameState() { return nullptr; }

        /*
         * A saved game is about to be restored into this World (load-from-save
         * only). The world file, OnLoad and the startup script have all run,
         * and the save holds its own copies of their entities: destroy what
         * should not be doubled -- usually everything (outside a tick,
         * DestroyEntity is immediate). Re-point cached handles in
         * OnPostLoadSave.
         */
        virtual void OnPrepareForLoadedGame() {}

        // === Rules ==============================================================

        /* May the game go from InFrom to InTo? False refuses the change, and
         * GameSession::SetPhase (Game.RequestPhase) returns false. */
        virtual bool CanEnterPhase(const char* InFrom, const char* InTo) { (void)InFrom; (void)InTo; return true; }

        /* The phase changed; GameState::GetPhase() is already InCurrent. */
        virtual void OnPhaseChanged(const char* InPrevious, const char* InCurrent) { (void)InPrevious; (void)InCurrent; }

        /* A PlayerStateComponent was added to InPlayer, or removed (also when
         * its entity is destroyed; the component is still readable). */
        virtual void OnPlayerAdded(GameCore::EntityID InPlayer)   { (void)InPlayer; }
        virtual void OnPlayerRemoved(GameCore::EntityID InPlayer) { (void)InPlayer; }

        /* After GameSession::AddScore / AddLives wrote the new value. */
        virtual void OnScoreChanged(GameCore::EntityID InPlayer, int32_t InScore) { (void)InPlayer; (void)InScore; }
        virtual void OnLivesChanged(GameCore::EntityID InPlayer, int32_t InLives) { (void)InPlayer; (void)InLives; }

        // === Avatars (Avatar.h) =================================================

        /* A client asked to drive InAvatar (Possessor's ServerRequestPossess,
         * or a script's Possess on a client) -- one marked Requestable that
         * nobody drives; the engine has checked that much. The mode's own
         * rules on top (close enough, same team). Server only. */
        virtual bool CanPossess(GameCore::EntityID InPlayer, GameCore::EntityID InAvatar) { (void)InPlayer; (void)InAvatar; return true; }

        /* InPlayer took or let go of InAvatar. Every process: the server as it
         * happens, a client as it sees the change replicated. */
        virtual void OnAvatarPossessed(GameCore::EntityID InPlayer, GameCore::EntityID InAvatar)   { (void)InPlayer; (void)InAvatar; }
        virtual void OnAvatarUnpossessed(GameCore::EntityID InPlayer, GameCore::EntityID InAvatar) { (void)InPlayer; (void)InAvatar; }

        // === Networking (NetFacade.h) ===========================================
        // A server's only. InClient is the client's id (GameCore::NetClientID).

        /* A client joined. With [Net] SpawnPlayerPerClient (the default) the
         * host has made InPlayer for it -- a PlayerStateComponent and a
         * NetComponent it owns -- and OnPlayerAdded has run; give it its pawn
         * here. Without, InPlayer is unset and the mode makes what it wants. */
        virtual void OnClientConnected(int32_t InClient, GameCore::EntityID InPlayer) { (void)InClient; (void)InPlayer; }

        /* A client left. Its entities -- every NetComponent it owned -- are
         * still alive, and are destroyed when this returns. */
        virtual void OnClientDisconnected(int32_t InClient, GameCore::EntityID InPlayer) { (void)InClient; (void)InPlayer; }
    };
}

#endif //NIMBLEENGINE_GAMEMODE_H
