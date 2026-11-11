#ifndef NIMBLEENGINE_GAMEMODE_H
#define NIMBLEENGINE_GAMEMODE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/EventBus.h"     // EventID, EventArgs
#include "Public/GameCore/TickPhase.h"

namespace GameCore { class World; }

namespace NimbleEngine
{
    class GameSession;
    class GameState;
    class IGameMode;
    struct InputSnapshot;

    namespace GameModeDetail
    {
        /* The class a pointer to member function belongs to. */
        template <class MethodType> struct MethodClass;
        template <class ClassType, class ReturnType, class... ArgTypes>
        struct MethodClass<ReturnType (ClassType::*)(ArgTypes...)> { using Type = ClassType; };
        template <class ClassType, class ReturnType, class... ArgTypes>
        struct MethodClass<ReturnType (ClassType::*)(ArgTypes...) const> { using Type = const ClassType; };

        template <auto Method>
        using ClassOf = typename MethodClass<decltype(Method)>::Type;
    }

    /* The mode of the level running InWorld, as the session has it: nullptr
     * outside GameSession::LoadLevel..UnloadLevel, or with no mode. */
    NE_API IGameMode* FindLevelGameMode(const GameCore::World& InWorld);

    /*
     * The mode running InWorld's level if it is (or derives from) ModeType,
     * else nullptr -- also outside a loaded level. For code that has only the
     * World and needs the level's rules or state: a component's RPC handler,
     * a behaviour-tree task, a native prefab's callback. ModeType may be an
     * interface the mode implements, so code shared by several modes can ask
     * for that instead of one mode.
     *
     * The mode is the level's context: created as it loads, destroyed as it
     * unloads, so its members need no resetting between loads.
     */
    template <class ModeType>
    ModeType* FindGameMode(const GameCore::World& InWorld)
    {
        return dynamic_cast<ModeType*>(FindLevelGameMode(InWorld));
    }

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
     * CONTEXT: the mode itself. GetSession() and GetWorld() are set from
     * before OnLoad to after OnUnload; keep per-level state in members, and
     * register systems, event handlers and the avatars' input filter as
     * member functions (AddSystem, Subscribe, SetAvatarInputFilter below),
     * which the session takes back after OnUnload. Code with only a World
     * reaches the mode through FindGameMode<T>.
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

        /*
         * SPLITSCREEN (SPLITSCREEN.md, "Joining and leaving"): local player
         * InLocalPlayer (1-3: player 1 is always in) asks to join -- Start on
         * their pad with [Splitscreen] DropIn, or GameSession::JoinLocalPlayer
         * -- or to leave: their pad unplugged with DropIn, or LeaveLocalPlayer.
         * True if the mode took them in, or let them go. The default refuses;
         * NetGameMode makes their player and avatar, and destroys both.
         */
        virtual bool OnLocalPlayerJoin(uint8_t InLocalPlayer)  { (void)InLocalPlayer; return false; }
        virtual bool OnLocalPlayerLeave(uint8_t InLocalPlayer) { (void)InLocalPlayer; return false; }

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

        /* LEVEL TRAVEL: the server changed level, and InClient -- connected
         * before -- is a player in this one, as soon as it loads (the host
         * makes InPlayer as for a joining client, its PlayerState carried
         * over: name, index, score, lives). The default treats it as joining
         * (OnClientConnected), which gives it its pawn; a mode that welcomes
         * players by name, or scores per level, tells the two apart here.
         * A client away at the time gets this, then OnClientAway. */
        virtual void OnClientTravelled(int32_t InClient, GameCore::EntityID InPlayer) { OnClientConnected(InClient, InPlayer); }

        /* A client left. Its entities -- every NetComponent it owned -- are
         * still alive, and are destroyed when this returns. A client that
         * lost its connection (not one that quit) is first Away, and this
         * comes only if it is not back within [Net] ReconnectGraceMs. */
        virtual void OnClientDisconnected(int32_t InClient, GameCore::EntityID InPlayer) { (void)InClient; (void)InPlayer; }

        /* A client's connection was lost; its place is kept -- InPlayer and
         * everything it owns stay, its avatar idle -- while it tries to rejoin.
         * Pause its respawn timer, mark it on the scoreboard. */
        virtual void OnClientAway(int32_t InClient, GameCore::EntityID InPlayer) { (void)InClient; (void)InPlayer; }

        /* It rejoined in time: the same id, the same InPlayer, driving what it drove. */
        virtual void OnClientReturned(int32_t InClient, GameCore::EntityID InPlayer) { (void)InClient; (void)InPlayer; }

        // === Context ============================================================

        /* The session running this mode, from before OnLoad until after
         * OnUnload; CORE_CHECKed outside that (HasSession asks). */
        NE_API GameSession&     GetSession() const;
        /* GetSession().GetWorld(). */
        NE_API GameCore::World& GetWorld() const;
        bool                    HasSession() const { return Session != nullptr; }

    protected:
        /*
         * Member-function bindings on the level's World, taken back by the
         * session after OnUnload: the mode need not unbind them. Each passes
         * this mode as its user data, so World::UnregisterSystem(...) and
         * EventBus::UnsubscribeAll(this) still work for one taken back early.
         * OnLoad onward.
         */

        /* &MyMode::Tick, a void (MyMode::*)(GameCore::World&, float), as the
         * World system InName in InPhase. */
        template <auto Method>
        void AddSystem(const char* InName, GameCore::ETickPhase InPhase)
        {
            BindSystem(InName, InPhase, &SystemThunk<Method>, false);
        }

        /* The same, ahead of InPhase's scripts (SO_BeforeScripts). */
        template <auto Method>
        void AddSystemBeforeScripts(const char* InName, GameCore::ETickPhase InPhase)
        {
            BindSystem(InName, InPhase, &SystemThunk<Method>, true);
        }

        /* &MyMode::OnX, a void (MyMode::*)(const GameCore::EventArgs&), for
         * the World event InEvent (Events.Emit in Lua, EventBus::Emit). */
        template <auto Method>
        void Subscribe(const char* InEvent)
        {
            BindEvent(InEvent, &EventThunk<Method>);
        }

        /* &MyMode::Filter, a void (MyMode::*)(GameCore::World&, GameCore::EntityID,
         * InputSnapshot&), as the level's AvatarWorld::SetInputFilter: each
         * frame of a local player's input before its avatar takes it. */
        template <auto Method>
        void SetAvatarInputFilter()
        {
            BindAvatarInputFilter(&InputFilterThunk<Method>);
        }

        /* Set by the session; derived modes read it (or use GetSession()). */
        GameSession* Session = nullptr;

    private:
        friend class GameSession;

        using SystemThunkFn = void (*)(GameCore::World&, float, void*);
        using EventThunkFn  = void (*)(GameCore::EventID, const GameCore::EventArgs&, void*);
        using FilterThunkFn = void (*)(GameCore::World&, GameCore::EntityID, InputSnapshot&, void*);

        NE_API void BindSystem(const char* InName, GameCore::ETickPhase InPhase, SystemThunkFn InFn, bool bInBeforeScripts);
        NE_API void BindEvent(const char* InEvent, EventThunkFn InFn);
        NE_API void BindAvatarInputFilter(FilterThunkFn InFn);

        /* Always this IGameMode* as the void*, so a cast back is exact under
         * multiple inheritance, and the session can unbind by it. */
        template <auto Method>
        static void SystemThunk(GameCore::World& InWorld, float InDeltaSeconds, void* InSelf)
        {
            auto* Self = static_cast<GameModeDetail::ClassOf<Method>*>(static_cast<IGameMode*>(InSelf));
            (Self->*Method)(InWorld, InDeltaSeconds);
        }

        template <auto Method>
        static void EventThunk(GameCore::EventID, const GameCore::EventArgs& InArgs, void* InSelf)
        {
            auto* Self = static_cast<GameModeDetail::ClassOf<Method>*>(static_cast<IGameMode*>(InSelf));
            (Self->*Method)(InArgs);
        }

        template <auto Method>
        static void InputFilterThunk(GameCore::World& InWorld, GameCore::EntityID InAvatar, InputSnapshot& InOutInput, void* InSelf)
        {
            auto* Self = static_cast<GameModeDetail::ClassOf<Method>*>(static_cast<IGameMode*>(InSelf));
            (Self->*Method)(InWorld, InAvatar, InOutInput);
        }
    };
}

#endif //NIMBLEENGINE_GAMEMODE_H
