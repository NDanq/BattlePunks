#ifndef NIMBLEENGINE_AVATAR_H
#define NIMBLEENGINE_AVATAR_H

#include <cstddef>
#include <cstdint>

#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

/*
 * AVATARS: an entity a player drives. Engine/Source/AVATARS.md has the model.
 *
 *   AvatarHarness  "AvatarHarness", on what is driven (a pawn, a vehicle, a
 *                  turret). Any entity or prefab with one can be possessed,
 *                  and while it is, it reads its player's input -- the same
 *                  way standalone, split-screen, on a listen or dedicated
 *                  server, and on a client:
 *
 *                      if(Harness->WasPressed("Jump")) { ... }
 *                      const float Turn = Harness->GetAxis("MoveX");
 *
 *   Possessor      "Possessor", on the player (the entity with the
 *                  PlayerState). Which avatar it drives, and which local
 *                  player's devices it reads.
 *
 * POSSESSION is the server's (HasAuthority): Possess(World, Player, Avatar).
 * It gives the avatar to the player's client (its Net component's
 * OwnerClient), so IsLocallyOwned is true there and that client's Server RPCs
 * on it are accepted. A player changes avatars -- gets into a vehicle,
 * respawns, spectates -- without its score or ownership moving.
 *
 * INPUT. Each frame, on the process that controls the avatar, its harness
 * reads its player's devices through its input map, just after InputComponents
 * do (TP_PrePhysics, before scripts). A client also sends them to the server
 * in one built-in RPC (ServerInput), so the server's copy of the harness
 * reads the same actions and axes -- the presses of every frame since its last
 * tick, none lost to a slower server.
 *
 * PREDICTION. A harness naming a Simulation (NIMBLE_DEFINE_AVATAR_SIMULATION)
 * is moved by that function from each frame of input: on the server for every
 * frame received, and on its owning client at once when its Net component is
 * Predicted -- then replayed over the server's state as it arrives. No
 * SetNetCorrectionHandler needed.
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    class InputState;

    enum : size_t
    {
        AVATAR_NAME_MAX    = INPUT_NAME_MAX,   // a simulation's name, including the NUL
        AVATAR_HISTORY_MAX = 256,              // predicted frames a client keeps unacknowledged
    };

    /* "AvatarHarness". The authored half is saved; the link is saved and
     * replicated; the rest is this frame's. */
    struct NE_API AvatarHarnessComponent
    {
        GAMECORE_COMPONENT_BODY()

        // === Authored ===========================================================
        char    InputMap[INPUT_NAME_MAX]     = "Default";   // the map its player's devices are read through
        char    Simulation[AVATAR_NAME_MAX]  = "";          // a registered simulation, or none
        uint8_t AutoPossess  = 0;   // 1..4: local player N takes it as the level starts (authority only); 0 never
        uint8_t bEnabled     = 1;   // disabled: reads as nothing held
        uint8_t bViewTarget  = 1;   // its camera becomes its player's view
        uint8_t bRequestable = 0;   // a client may ask to drive it (ServerRequestPossess): a vehicle

        // === The link ===========================================================
        GameCore::EntityID    Possessor;                            // the player driving it; unset if nobody
        int32_t               PossessorNetId = 0;                   // the same, by NetId, for a client still to see it
        GameCore::NetClientID RestOwner = GameCore::NET_CLIENT_NONE; // its owner while nobody drives it (server's)

        // === This frame =========================================================
        uint32_t Down     = 0;   // one bit per action of InputMap
        uint32_t Pressed  = 0;
        uint32_t Released = 0;
        float    Axes[INPUT_AXES_MAX] = {};

        // === The server's: a client's input since the last tick ==================
        uint32_t PendDown     = 0;
        uint32_t PendPressed  = 0;
        uint32_t PendReleased = 0;
        float    PendAxes[INPUT_AXES_MAX] = {};
        int32_t  LastSequence      = 0;      // the newest ServerInput applied
        float    SecondsSinceInput = 0.0f;   // since one arrived
        uint8_t  bPendFresh        = 0;      // one arrived since the last tick

        /* Who this process last saw driving it: the Possessed and Unpossessed
         * a client hears are this changing. */
        GameCore::EntityID SeenPossessor;

        /* Its prefab script has heard OnPossessed for Possessor. A script
         * that starts after the possession (an avatar spawned and possessed
         * as the level starts) hears it once it has started. */
        uint8_t bScriptTold = 0;

        bool  IsDown     (const char* InAction) const;
        bool  WasPressed (const char* InAction) const;
        bool  WasReleased(const char* InAction) const;
        float GetAxis    (const char* InAxis) const;

        bool               IsPossessed() const;
        /* Driven, and by this process: its devices are the ones being read. */
        bool               IsLocallyControlled() const;
        GameCore::EntityID GetPossessor() const;

        /* The owning client's frame of input. Built in; the harness sends it. */
        void ServerInput(const GameCore::RpcContext& InContext, int32_t InSequence, float InSeconds,
                         uint32_t InDown, uint32_t InPressed, uint32_t InReleased,
                         const GameCore::RpcFloats<INPUT_AXES_MAX>& InAxes);
    };

    /* "Possessor". On a player: the avatar it drives. */
    struct NE_API PossessorComponent
    {
        GAMECORE_COMPONENT_BODY()

        GameCore::EntityID Avatar;             // saved and replicated; unset if none
        int32_t            AvatarNetId = 0;    // the same, by NetId, for a client still to see it
        uint8_t            LocalPlayer = 0;    // whose pad on its own process (the keyboard and mouse are every local player's, as for an InputComponent)

        GameCore::EntityID GetAvatar() const;

        /* A client asking to drive InAvatar (a vehicle it walked up to).
         * The server asks the mode (IGameMode::CanPossess) and possesses. */
        void ServerRequestPossess(const GameCore::RpcContext& InContext, GameCore::EntityID InAvatar);
        void ServerRequestUnpossess(const GameCore::RpcContext& InContext);
    };

    // === Possession ===========================================================

    enum EPossessFlags : uint8_t
    {
        POSSESS_None  = 0,
        POSSESS_Steal = 1 << 0,   // take it from a player already driving it
    };

    /*
     * InPlayer (a Possessor; added if missing) drives InAvatar (an
     * AvatarHarness). Authority only: false on a client, for an entity
     * not alive, an avatar without a harness, or one already driven by
     * another player without POSSESS_Steal. The player's previous avatar is
     * let go first. Possessing what it already drives is true and changes
     * nothing.
     *
     * Then: InAvatar's Net component (if any) is owned by InPlayer's client;
     * the "Possessed" event (Player, Avatar) is emitted; the avatar's prefab
     * script hears OnPossessed(self, player) -- at once if it has started,
     * else in the frame it starts, after its OnStart; the mode's OnAvatarPossessed
     * runs. Its input is read from the next frame.
     */
    NE_API bool Possess(GameCore::World& InWorld, GameCore::EntityID InPlayer, GameCore::EntityID InAvatar,
                        uint8_t InFlags = POSSESS_None);

    /* InPlayer lets its avatar go: its owner goes back to RestOwner, and
     * "Unpossessed" / OnUnpossessed / OnAvatarUnpossessed follow. False on a
     * client, or if it drives nothing. Also done for you when either end is
     * destroyed or loses its component. */
    NE_API bool Unpossess(GameCore::World& InWorld, GameCore::EntityID InPlayer);

    NE_API GameCore::EntityID GetAvatar(const GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API GameCore::EntityID GetPossessor(const GameCore::World& InWorld, GameCore::EntityID InAvatar);

    /* The avatar local player InLocalPlayer drives on this process, if any. */
    NE_API GameCore::EntityID GetLocalAvatar(GameCore::World& InWorld, uint8_t InLocalPlayer = 0);

    /* This process's player for local player InLocalPlayer: a Possessor of
     * that LocalPlayer this process owns. Unset if there is none. */
    NE_API GameCore::EntityID FindLocalPlayer(GameCore::World& InWorld, uint8_t InLocalPlayer);

    /* FindLocalPlayer, or a new one (authority only): a PlayerState, a Net
     * component nobody owns (the server's) and a Possessor. */
    NE_API GameCore::EntityID CreateLocalPlayer(GameCore::World& InWorld, uint8_t InLocalPlayer);

    /* Every avatar InClient's players drive is let go. NetHost calls it
     * before a leaving client's entities are destroyed, so a level's vehicle
     * goes back to the server rather than with them. */
    NE_API void ReleaseClientAvatars(GameCore::World& InWorld, GameCore::NetClientID InClient);

    // === Prediction ===========================================================

    /* One frame of an avatar's input, as its simulation steps it. */
    struct AvatarInputFrame
    {
        int32_t  Sequence = 0;
        float    Seconds  = 0.0f;
        uint32_t Down     = 0;
        uint32_t Pressed  = 0;
        uint32_t Released = 0;
        float    Axes[INPUT_AXES_MAX] = {};
    };

    /*
     * Moves InAvatar by one frame of input. The same function runs on the
     * server and on a predicting client, from the same starting state, so
     * keep it deterministic and free of what only the server knows. Look up
     * map indices once (FindInputAxis), not per call.
     */
    using AvatarStepFn = void (*)(GameCore::World& InWorld, GameCore::EntityID InAvatar, const AvatarInputFrame& InFrame);

    using AvatarSimulationHandle = uint32_t;
    constexpr AvatarSimulationHandle AVATAR_SIMULATION_INVALID = 0;

    /* What the macro calls. InMaxStepSeconds clamps a frame a client sends. */
    NE_API AvatarSimulationHandle RegisterAvatarSimulation(const char* InName, AvatarStepFn InStep, float InMaxStepSeconds = 0.1f);
    NE_API void                   UnregisterAvatarSimulation(AvatarSimulationHandle InHandle);

    /* How a predicting client is doing: corrections since the last call, the
     * frames waiting on the server, and the furthest a correction moved it
     * (world units). */
    struct AvatarPredictionStats
    {
        uint32_t Corrections   = 0;
        uint32_t Pending       = 0;
        double   MaxCorrection = 0.0;
    };

    // === The per-World half ===================================================

    /*
     * Rewrites one frame of a local player's input before the harness takes
     * it: a bot, an auto-mover, aim assist, input blocked by a menu.
     */
    using AvatarInputFilterFn = void (*)(GameCore::World& InWorld, GameCore::EntityID InAvatar, InputSnapshot& InOutInput,
                                         void* InUserData);

    /*
     * Each level's avatars: GameSession attaches one to its World, after
     * "Engine.Input", as the "Engine.Avatar" TP_PrePhysics system. A test
     * or another host attaches its own.
     */
    class NE_API AvatarWorld
    {
    public:
        AvatarWorld();
        ~AvatarWorld();
        AvatarWorld(const AvatarWorld&)            = delete;
        AvatarWorld& operator=(const AvatarWorld&) = delete;

        /* Registers the system and the component hooks. One per World. */
        bool Attach(GameCore::World& InWorld);
        void Detach();

        static AvatarWorld* Find(const GameCore::World& InWorld);

        /* What the harnesses read; null reads nothing held. Not copied. */
        void SetInput(const InputState* InState);

        void SetInputFilter(AvatarInputFilterFn InFilter, void* InUserData);
        /* The filter, if InUserData set it; another's stays. */
        void ClearInputFilter(void* InUserData);

        /*
         * The system: every harness's input for this frame, and its
         * simulation's step where it runs here. bInSend false (paused) still
         * follows the devices but sends nothing and steps nothing.
         */
        void Update(float InSeconds, bool bInSend = true);

        /* Local player 0's avatar's camera, if it is a view target: the
         * camera the game views through. Unset to view the level's. */
        GameCore::EntityID GetViewCamera() const;

        /* NetHost's: a predicted avatar's server state arrived. True if
         * InEntity is a simulated avatar and its frames were replayed. */
        bool HandleCorrection(GameCore::EntityID InEntity, int32_t InInputAck);

        AvatarPredictionStats TakePredictionStats(GameCore::EntityID InAvatar);

        /* Forget InAvatar's predicted frames (possession changed). */
        void ResetPrediction(GameCore::EntityID InAvatar);

        /* The server's half of ServerInput, for the component. */
        void ApplyRemoteInput(GameCore::EntityID InAvatar, const AvatarInputFrame& InFrame);

    private:
        struct Impl;
        Impl* P = nullptr;
    };
}

/*
 * An avatar simulation, at file scope in one .cpp; harnesses name it in their
 * Simulation field. Unregisters with its module.
 *
 *     NIMBLE_DEFINE_AVATAR_SIMULATION(CapsuleMove, 0.1f)
 *     {
 *         // InWorld, InAvatar, InFrame
 *     }
 */
#define NIMBLE_DEFINE_AVATAR_SIMULATION(SimName, MaxStepSeconds)                                                   \
    static void SimName##_AvatarStep(::GameCore::World& InWorld, ::GameCore::EntityID InAvatar,                  \
                                     const ::NimbleEngine::AvatarInputFrame& InFrame);                           \
    namespace                                                                                                     \
    {                                                                                                             \
        struct SimName##_AvatarSimRegistrar                                                                       \
        {                                                                                                         \
            SimName##_AvatarSimRegistrar()                                                                        \
            {                                                                                                     \
                Handle = ::NimbleEngine::RegisterAvatarSimulation(#SimName, &SimName##_AvatarStep, MaxStepSeconds); \
            }                                                                                                     \
            ~SimName##_AvatarSimRegistrar() { ::NimbleEngine::UnregisterAvatarSimulation(Handle); }               \
            ::NimbleEngine::AvatarSimulationHandle Handle = ::NimbleEngine::AVATAR_SIMULATION_INVALID;            \
        };                                                                                                        \
        SimName##_AvatarSimRegistrar G##SimName##_AvatarSimRegistrar;                                             \
    }                                                                                                             \
    static void SimName##_AvatarStep(::GameCore::World& InWorld, ::GameCore::EntityID InAvatar,                  \
                                     const ::NimbleEngine::AvatarInputFrame& InFrame)

#endif //NIMBLEENGINE_AVATAR_H
