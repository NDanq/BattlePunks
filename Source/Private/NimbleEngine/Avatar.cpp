#include "Public/NimbleEngine/Avatar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/PlayerState.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/CameraComponent.h"

/*
 * Avatar.h: the two components, possession, the per-World system that feeds
 * harnesses their input (and the server its clients'), and prediction.
 */

/* A remote client's avatar that has heard nothing from it for this long
 * reads as nothing held: a client that stalled does not keep running. */
NIMBLE_CONFIG_INT(GAvatarInputTimeoutMs, "Avatar", "InputTimeoutMs", 500, nullptr);

namespace NimbleEngine
{
namespace
{
    using GameCore::Entity;
    using GameCore::EntityID;
    using GameCore::NetComponent;
    using GameCore::World;

    // === Simulations ==========================================================

    struct Simulation
    {
        AvatarSimulationHandle Handle = AVATAR_SIMULATION_INVALID;
        std::string            Name;
        AvatarStepFn           Step = nullptr;
        float                  MaxStepSeconds = 0.1f;
    };

    struct SimulationRegistry
    {
        std::vector<Simulation> Entries;
        AvatarSimulationHandle  NextHandle = 1;
    };

    SimulationRegistry& Simulations()
    {
        static SimulationRegistry Instance;
        return Instance;
    }

    const Simulation* FindSimulation(const char* InName)
    {
        if(!InName || InName[0] == '\0') { return nullptr; }
        for(const Simulation& Each : Simulations().Entries)
        {
            if(Each.Name == InName) { return &Each; }
        }
        return nullptr;
    }

    // === AvatarWorld lookup ===================================================

    struct WorldEntry
    {
        const World* Owner  = nullptr;
        AvatarWorld* Avatars = nullptr;
    };

    std::vector<WorldEntry>& AvatarWorlds()
    {
        static std::vector<WorldEntry> Entries;
        return Entries;
    }

    // === Small helpers ========================================================

    NetComponent* NetOf(World& InWorld, const EntityID InEntity)
    {
        return InWorld.IsAlive(InEntity) ? InWorld.GetEntity(InEntity).GetComponent<NetComponent>() : nullptr;
    }

    AvatarHarnessComponent* HarnessOf(World& InWorld, const EntityID InEntity)
    {
        return InWorld.IsAlive(InEntity) ? InWorld.GetEntity(InEntity).GetComponent<AvatarHarnessComponent>() : nullptr;
    }

    PossessorComponent* PossessorOf(World& InWorld, const EntityID InEntity)
    {
        return InWorld.IsAlive(InEntity) ? InWorld.GetEntity(InEntity).GetComponent<PossessorComponent>() : nullptr;
    }

    /* A client's map of a NetId it was told about to its entity. */
    EntityID FindByNetId(World& InWorld, const int32_t InNetId)
    {
        EntityID Found;
        if(InNetId == 0) { return Found; }
        InWorld.Each<NetComponent>([&](Entity InEntity, NetComponent& InNet)
        {
            if(!Found.IsSet() && InNet.NetId == InNetId) { Found = InEntity.GetID(); }
        });
        return Found;
    }

    void ClearInput(AvatarHarnessComponent& InHarness)
    {
        InHarness.Down = InHarness.Pressed = InHarness.Released = 0;
        std::memset(InHarness.Axes, 0, sizeof(InHarness.Axes));
        InHarness.PendDown = InHarness.PendPressed = InHarness.PendReleased = 0;
        std::memset(InHarness.PendAxes, 0, sizeof(InHarness.PendAxes));
        InHarness.bPendFresh        = 0;
        InHarness.SecondsSinceInput = 0.0f;
    }

    bool IsIdle(const InputSnapshot& InInput)
    {
        if(InInput.Down != 0 || InInput.Pressed != 0 || InInput.Released != 0) { return false; }
        for(const float Axis : InInput.Axes)
        {
            if(Axis != 0.0f) { return false; }
        }
        return true;
    }

    IGameMode* ModeOf(const World& InWorld)
    {
        GameSession* Session = FindGameSession(InWorld);
        ILevel*      Level   = Session ? Session->GetLevel() : nullptr;
        return Level ? Level->GetGameMode() : nullptr;
    }

    /* Everyone who should hear that InPlayer took (or let go of) InAvatar:
     * the World's event, the avatar's script, the mode. */
    void Announce(World& InWorld, const EntityID InPlayer, const EntityID InAvatar, const bool bInPossessed)
    {
        GameCore::EventArgs Args;
        Args.Args[0] = GameCore::EventArg::Make(InPlayer);
        Args.Args[1] = GameCore::EventArg::Make(InAvatar);
        Args.Count   = 2;
        InWorld.GetEvents().Emit(bInPossessed ? "Possessed" : "Unpossessed", Args);

        if(InWorld.IsAlive(InAvatar))
        {
            /* A script not started yet hears OnPossessed once it has (Update). */
            const bool bTold = InWorld.CallScriptMethod(InAvatar, bInPossessed ? "OnPossessed" : "OnUnpossessed", InPlayer);
            if(AvatarHarnessComponent* Harness = InWorld.GetEntity(InAvatar).GetComponent<AvatarHarnessComponent>())
            {
                Harness->bScriptTold = bInPossessed && bTold ? 1 : 0;
            }
        }

        if(IGameMode* Mode = ModeOf(InWorld))
        {
            if(bInPossessed) { Mode->OnAvatarPossessed(InPlayer, InAvatar); }
            else             { Mode->OnAvatarUnpossessed(InPlayer, InAvatar); }
        }
    }

    /*
     * The link between InPlayer and InAvatar undone, on the server: either
     * end may already be on its way out (a component being removed, an
     * entity being destroyed), so each side is checked on its own.
     */
    void Unlink(World& InWorld, const EntityID InPlayer, const EntityID InAvatar)
    {
        bool bWasLinked = false;
        if(PossessorComponent* Possessor = PossessorOf(InWorld, InPlayer))
        {
            if(Possessor->Avatar == InAvatar)
            {
                Possessor->Avatar      = EntityID();
                Possessor->AvatarNetId = 0;
                bWasLinked = true;
            }
        }
        if(AvatarHarnessComponent* Harness = HarnessOf(InWorld, InAvatar))
        {
            if(Harness->Possessor == InPlayer)
            {
                Harness->Possessor      = EntityID();
                Harness->PossessorNetId = 0;
                Harness->SeenPossessor  = EntityID();
                ClearInput(*Harness);
                if(NetComponent* Net = NetOf(InWorld, InAvatar)) { Net->OwnerClient = Harness->RestOwner; }
                bWasLinked = true;
            }
        }
        if(!bWasLinked) { return; }

        if(AvatarWorld* Avatars = AvatarWorld::Find(InWorld)) { Avatars->ResetPrediction(InAvatar); }
        Announce(InWorld, InPlayer, InAvatar, false);
    }

}

// === The components ===========================================================

bool AvatarHarnessComponent::IsDown(const char* InAction) const
{
    const int Index = FindInputAction(InputMap, InAction);
    return Index >= 0 && (Down >> Index & 1u) != 0;
}

bool AvatarHarnessComponent::WasPressed(const char* InAction) const
{
    const int Index = FindInputAction(InputMap, InAction);
    return Index >= 0 && (Pressed >> Index & 1u) != 0;
}

bool AvatarHarnessComponent::WasReleased(const char* InAction) const
{
    const int Index = FindInputAction(InputMap, InAction);
    return Index >= 0 && (Released >> Index & 1u) != 0;
}

float AvatarHarnessComponent::GetAxis(const char* InAxis) const
{
    const int Index = FindInputAxis(InputMap, InAxis);
    return Index >= 0 ? Axes[Index] : 0.0f;
}

bool AvatarHarnessComponent::IsPossessed() const
{
    return Possessor.IsSet() && Owner.OwningWorld && Owner.OwningWorld->IsAlive(Possessor);
}

GameCore::EntityID AvatarHarnessComponent::GetPossessor() const
{
    return IsPossessed() ? Possessor : GameCore::EntityID();
}

bool AvatarHarnessComponent::IsLocallyControlled() const
{
    if(!IsPossessed()) { return false; }
    World& InWorld = *Owner.OwningWorld;

    /* The avatar's own owner says; an avatar that is not networked is its
     * player's process's. */
    if(const NetComponent* Net = NetOf(InWorld, Owner.Entity)) { return IsLocallyOwned(*Net); }
    if(const NetComponent* Net = NetOf(InWorld, Possessor))    { return IsLocallyOwned(*Net); }
    return true;
}

void AvatarHarnessComponent::ServerInput(const GameCore::RpcContext& InContext, const int32_t InSequence, const float InSeconds,
                                         const uint32_t InDown, const uint32_t InPressed, const uint32_t InReleased,
                                         const GameCore::RpcFloats<INPUT_AXES_MAX>& InAxes)
{
    /* NetSession has checked the sender owns this avatar; an avatar nobody
     * drives (its owner kept it, unpossessed) takes no input. */
    if(!IsPossessed() || InSequence <= LastSequence) { return; }
    LastSequence = InSequence;
    if(NetComponent* Net = Owner.GetSibling<NetComponent>()) { Net->InputAck = InSequence; }

    /* Presses add up until the next tick reads them; held keys and stick
     * positions are the latest; mouse motion sums. */
    AvatarInputFrame Frame;
    Frame.Sequence = InSequence;
    Frame.Seconds  = std::isfinite(InSeconds) ? std::max(0.0f, InSeconds) : 0.0f;
    Frame.Down     = InDown;
    Frame.Pressed  = InPressed;
    Frame.Released = InReleased;
    PendDown      = InDown;
    PendPressed  |= InPressed;
    PendReleased |= InReleased;
    for(uint8_t Axis = 0; Axis < INPUT_AXES_MAX; ++Axis)
    {
        const float Value = Axis < InAxes.Count && std::isfinite(InAxes.Values[Axis]) ? InAxes.Values[Axis] : 0.0f;
        Frame.Axes[Axis] = Value;
        PendAxes[Axis]   = IsInputAxisAccumulated(InputMap, Axis) ? PendAxes[Axis] + Value : Value;
    }
    bPendFresh        = 1;
    SecondsSinceInput = 0.0f;

    (void)InContext;
    /* Last: a simulation may add components, which can move this one. */
    const GameCore::EntityID Self = Owner.Entity;
    if(AvatarWorld* Avatars = AvatarWorld::Find(*Owner.OwningWorld)) { Avatars->ApplyRemoteInput(Self, Frame); }
}

GameCore::EntityID PossessorComponent::GetAvatar() const
{
    return Avatar.IsSet() && Owner.OwningWorld && Owner.OwningWorld->IsAlive(Avatar) ? Avatar : GameCore::EntityID();
}

void PossessorComponent::ServerRequestPossess(const GameCore::RpcContext& InContext, const GameCore::EntityID InAvatar)
{
    (void)InContext;
    World& InWorld = *Owner.OwningWorld;
    const EntityID Player = Owner.Entity;
    const AvatarHarnessComponent* Harness = HarnessOf(InWorld, InAvatar);
    if(!Harness || !Harness->bRequestable || (Harness->IsPossessed() && Harness->Possessor != Player))
    {
        CORE_LOG_WARN("[Avatar] " + std::string(InWorld.GetEntity(Player).GetName())
                      + " asked to drive an avatar that is not requestable, or is taken; refused")
        return;
    }
    IGameMode* Mode = ModeOf(InWorld);
    if(Mode && !Mode->CanPossess(Player, InAvatar)) { return; }
    Possess(InWorld, Player, InAvatar);
}

void PossessorComponent::ServerRequestUnpossess(const GameCore::RpcContext& InContext)
{
    (void)InContext;
    Unpossess(*Owner.OwningWorld, Owner.Entity);
}

// === Possession ===============================================================

bool Possess(GameCore::World& InWorld, const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar, const uint8_t InFlags)
{
    if(!HasAuthority())
    {
        CORE_LOG_WARN("[Avatar] Possess is the server's; a client asks with its Possessor's ServerRequestPossess")
        return false;
    }
    if(!InWorld.IsAlive(InPlayer) || !InWorld.IsAlive(InAvatar)) { return false; }

    AvatarHarnessComponent* Harness = HarnessOf(InWorld, InAvatar);
    if(!Harness)
    {
        CORE_LOG_WARN("[Avatar] Possess: '" + std::string(InWorld.GetEntity(InAvatar).GetName()) + "' has no Avatar Harness")
        return false;
    }

    Entity Player = InWorld.GetEntity(InPlayer);
    if(!Player.HasComponent<PossessorComponent>()) { Player.AddComponent<PossessorComponent>(); }
    if(Harness->Possessor == InPlayer && PossessorOf(InWorld, InPlayer)->Avatar == InAvatar) { return true; }

    if(Harness->IsPossessed() && Harness->Possessor != InPlayer)
    {
        if((InFlags & POSSESS_Steal) == 0) { return false; }
        Unpossess(InWorld, Harness->Possessor);
    }
    if(PossessorOf(InWorld, InPlayer)->GetAvatar().IsSet()) { Unpossess(InWorld, InPlayer); }

    /* Re-read: the hooks above ran game code. */
    Harness = HarnessOf(InWorld, InAvatar);
    PossessorComponent* Possessor = PossessorOf(InWorld, InPlayer);
    if(!Harness || !Possessor) { return false; }

    NetComponent* AvatarNet = NetOf(InWorld, InAvatar);
    const NetComponent* PlayerNet = NetOf(InWorld, InPlayer);
    Harness->RestOwner = AvatarNet ? AvatarNet->OwnerClient : GameCore::NET_CLIENT_NONE;
    if(AvatarNet)
    {
        AvatarNet->OwnerClient = PlayerNet ? PlayerNet->OwnerClient : GameCore::NET_CLIENT_NONE;
        AvatarNet->InputAck    = 0;
    }
    else if(PlayerNet && PlayerNet->OwnerClient != GameCore::NET_CLIENT_NONE)
    {
        CORE_LOG_WARN("[Avatar] '" + std::string(InWorld.GetEntity(InAvatar).GetName())
                      + "' has no Net component, so its player's client cannot drive it")
    }

    Harness->Possessor      = InPlayer;
    Harness->PossessorNetId = PlayerNet ? PlayerNet->NetId : 0;
    Harness->SeenPossessor  = InPlayer;
    Harness->LastSequence   = 0;
    ClearInput(*Harness);
    Possessor->Avatar      = InAvatar;
    Possessor->AvatarNetId = AvatarNet ? AvatarNet->NetId : 0;

    if(AvatarWorld* Avatars = AvatarWorld::Find(InWorld)) { Avatars->ResetPrediction(InAvatar); }
    Announce(InWorld, InPlayer, InAvatar, true);
    return true;
}

bool Unpossess(GameCore::World& InWorld, const GameCore::EntityID InPlayer)
{
    if(!HasAuthority()) { return false; }
    const PossessorComponent* Possessor = PossessorOf(InWorld, InPlayer);
    if(!Possessor || !Possessor->Avatar.IsSet()) { return false; }
    Unlink(InWorld, InPlayer, Possessor->Avatar);
    return true;
}

GameCore::EntityID GetAvatar(const GameCore::World& InWorld, const GameCore::EntityID InPlayer)
{
    World& Mutable = const_cast<World&>(InWorld);
    const PossessorComponent* Possessor = PossessorOf(Mutable, InPlayer);
    return Possessor ? Possessor->GetAvatar() : GameCore::EntityID();
}

GameCore::EntityID GetPossessor(const GameCore::World& InWorld, const GameCore::EntityID InAvatar)
{
    World& Mutable = const_cast<World&>(InWorld);
    const AvatarHarnessComponent* Harness = HarnessOf(Mutable, InAvatar);
    return Harness ? Harness->GetPossessor() : GameCore::EntityID();
}

GameCore::EntityID FindLocalPlayer(GameCore::World& InWorld, const uint8_t InLocalPlayer)
{
    EntityID Found;
    InWorld.Each<PossessorComponent>([&](Entity InEntity, PossessorComponent& InPossessor)
    {
        if(Found.IsSet() || InPossessor.LocalPlayer != InLocalPlayer) { return; }
        const NetComponent* Net = InEntity.GetComponent<NetComponent>();
        if(!Net || IsLocallyOwned(*Net)) { Found = InEntity.GetID(); }
    });
    return Found;
}

GameCore::EntityID GetLocalAvatar(GameCore::World& InWorld, const uint8_t InLocalPlayer)
{
    EntityID Found;
    InWorld.Each<AvatarHarnessComponent>([&](Entity InEntity, AvatarHarnessComponent& InHarness)
    {
        if(Found.IsSet() || !InHarness.IsLocallyControlled()) { return; }
        const PossessorComponent* Possessor = PossessorOf(InWorld, InHarness.Possessor);
        if((Possessor ? Possessor->LocalPlayer : 0) == InLocalPlayer) { Found = InEntity.GetID(); }
    });
    return Found;
}

GameCore::EntityID CreateLocalPlayer(GameCore::World& InWorld, const uint8_t InLocalPlayer)
{
    const EntityID Existing = FindLocalPlayer(InWorld, InLocalPlayer);
    if(Existing.IsSet() || !HasAuthority()) { return Existing; }

    char Name[PLAYER_NAME_MAX];
    std::snprintf(Name, sizeof(Name), "Local Player %d", int(InLocalPlayer) + 1);
    Entity Player = InWorld.CreateEntity(Name);

    /* Networked, owned by nobody (the server): clients see the host's player
     * as they see their own. Its Possessor before its PlayerState, so a mode
     * hearing OnPlayerAdded can already give it an avatar. */
    Player.AddComponent<NetComponent>();
    PossessorComponent Possessor;
    Possessor.LocalPlayer = InLocalPlayer;
    Player.AddComponent(Possessor);
    PlayerStateComponent State;
    std::snprintf(State.PlayerName, sizeof(State.PlayerName), "%s", Name);
    State.PlayerIndex = FindFreePlayerIndex(InWorld);
    Player.AddComponent(State);
    return Player.GetID();
}

void ReleaseClientAvatars(GameCore::World& InWorld, const GameCore::NetClientID InClient)
{
    if(!HasAuthority() || InClient == GameCore::NET_CLIENT_NONE) { return; }
    std::vector<EntityID> Players;
    InWorld.Each<PossessorComponent>([&](Entity InEntity, PossessorComponent& InPossessor)
    {
        const NetComponent* Net = InEntity.GetComponent<NetComponent>();
        if(Net && Net->OwnerClient == InClient && InPossessor.Avatar.IsSet()) { Players.push_back(InEntity.GetID()); }
    });
    for(const EntityID Player : Players) { Unpossess(InWorld, Player); }
}

// === Simulations ==============================================================

AvatarSimulationHandle RegisterAvatarSimulation(const char* InName, const AvatarStepFn InStep, const float InMaxStepSeconds)
{
    const std::string Name = InName ? InName : "";
    if(Name.empty() || Name.size() >= AVATAR_NAME_MAX || !InStep || FindSimulation(Name.c_str()))
    {
        CORE_LOG_ERROR("[Avatar] simulation '" + Name + "' is unnamed, too long, has no step, or is already registered; refused")
        return AVATAR_SIMULATION_INVALID;
    }
    Simulation Added;
    Added.Handle         = Simulations().NextHandle++;
    Added.Name           = Name;
    Added.Step           = InStep;
    Added.MaxStepSeconds = InMaxStepSeconds > 0.0f ? InMaxStepSeconds : 0.1f;
    Simulations().Entries.push_back(Added);
    return Added.Handle;
}

void UnregisterAvatarSimulation(const AvatarSimulationHandle InHandle)
{
    std::vector<Simulation>& Entries = Simulations().Entries;
    Entries.erase(std::remove_if(Entries.begin(), Entries.end(), [InHandle](const Simulation& InEach) { return InEach.Handle == InHandle; }),
                  Entries.end());
}

// === AvatarWorld ==============================================================

struct AvatarWorld::Impl
{
    World*              Bound  = nullptr;
    const InputState*   Input  = nullptr;
    AvatarInputFilterFn Filter = nullptr;
    void*               FilterData = nullptr;
    GameCore::DelegateHandle RemovedHandle;

    /* This process's input sequence, across every avatar it drives: a
     * re-possessed avatar's server has reset to 0, and a number only grows. */
    int32_t NextSequence = 0;

    /* A predicting client's state for one avatar. */
    struct Predicted
    {
        std::deque<AvatarInputFrame> Frames;   // sent, not yet acknowledged
        EngineCore::Vector3          Shown;    // where it was after the last step shown
        bool                         bShown = false;
        AvatarPredictionStats        Stats;
        bool                         bLastSentIdle = false;
    };
    std::unordered_map<uint64_t, Predicted> Avatars;   // by packed EntityID

    EntityID ViewCamera;

    static void System(World& InWorld, const float InSeconds, void* InUserData)
    {
        (void)InWorld;
        static_cast<AvatarWorld*>(InUserData)->Update(InSeconds, true);
    }

    /* After TP_PrePhysics's scripts: an avatar whose script has just started
     * (spawned and possessed as the level began) hears OnPossessed now,
     * before its first OnTick has missed anything more. */
    static void TellScripts(World& InWorld, const float /*InSeconds*/, void* /*InUserData*/)
    {
        std::vector<EntityID> Untold;
        InWorld.Each<AvatarHarnessComponent>([&](Entity InEntity, AvatarHarnessComponent& InHarness)
        {
            if(!InHarness.bScriptTold && InHarness.IsPossessed()) { Untold.push_back(InEntity.GetID()); }
        });
        for(const EntityID Avatar : Untold)
        {
            AvatarHarnessComponent* Harness = HarnessOf(InWorld, Avatar);
            if(!Harness) { continue; }
            const EntityID Player = Harness->Possessor;
            const bool bTold = InWorld.CallScriptMethod(Avatar, "OnPossessed", Player);
            if((Harness = HarnessOf(InWorld, Avatar)) == nullptr) { continue; }
            /* Not in a prefab instance: no script will ever start for it. */
            Harness->bScriptTold = bTold || !InWorld.GetEntity(Avatar).IsInInstance() ? 1 : 0;
        }
    }

    /* Either end of a link losing its component (or its entity) lets go, on
     * the server. A client waits for the server's word; Update notices a
     * link to something gone. */
    static void OnComponentRemoved(const EntityID InEntity, const GameCore::ComponentTypeID InType, void* InUserData)
    {
        AvatarWorld& Self = *static_cast<AvatarWorld*>(InUserData);
        World& InWorld = *Self.P->Bound;
        if(InType == AvatarHarnessComponent::StaticType().ID)
        {
            Self.P->Avatars.erase(InEntity.Pack());
            if(Self.P->ViewCamera.IsSet()) { Self.P->ViewCamera = EntityID(); }
            const AvatarHarnessComponent* Harness = InWorld.GetEntity(InEntity).GetComponent<AvatarHarnessComponent>();
            if(Harness && Harness->Possessor.IsSet() && HasAuthority()) { Unlink(InWorld, Harness->Possessor, InEntity); }
        }
        else if(InType == PossessorComponent::StaticType().ID && HasAuthority())
        {
            const PossessorComponent* Possessor = InWorld.GetEntity(InEntity).GetComponent<PossessorComponent>();
            if(Possessor && Possessor->Avatar.IsSet()) { Unlink(InWorld, InEntity, Possessor->Avatar); }
        }
    }

    /* The camera an avatar is seen through: its own, or its first
     * descendant's, depth first. */
    EntityID FindCamera(const EntityID InEntity, const int InDepth = 0) const
    {
        Entity Node = Bound->GetEntity(InEntity);
        if(Node.HasComponent<RenderCore::CameraComponent>()) { return InEntity; }
        if(InDepth > 16) { return EntityID(); }
        for(EntityID Child = Node.GetFirstChild(); Child.IsSet(); Child = Bound->GetEntity(Child).GetNextSibling())
        {
            const EntityID Found = FindCamera(Child, InDepth + 1);
            if(Found.IsSet()) { return Found; }
        }
        return EntityID();
    }

    /* A client's links that arrived as NetIds before the entities they name. */
    void ResolveClientLinks()
    {
        Bound->Each<AvatarHarnessComponent>([&](Entity, AvatarHarnessComponent& InHarness)
        {
            if(InHarness.PossessorNetId != 0 && !InHarness.IsPossessed()) { InHarness.Possessor = FindByNetId(*Bound, InHarness.PossessorNetId); }
            if(InHarness.PossessorNetId == 0 && InHarness.Possessor.IsSet() && !Bound->IsAlive(InHarness.Possessor)) { InHarness.Possessor = EntityID(); }
        });
        Bound->Each<PossessorComponent>([&](Entity, PossessorComponent& InPossessor)
        {
            if(InPossessor.AvatarNetId != 0 && !InPossessor.GetAvatar().IsSet()) { InPossessor.Avatar = FindByNetId(*Bound, InPossessor.AvatarNetId); }
        });
    }
};

AvatarWorld::AvatarWorld() : P(new Impl()) {}

AvatarWorld::~AvatarWorld()
{
    Detach();
    delete P;
}

bool AvatarWorld::Attach(GameCore::World& InWorld)
{
    if(Find(InWorld))
    {
        CORE_LOG_ERROR("[Avatar] this World already has an AvatarWorld")
        return false;
    }
    Detach();
    P->Bound = &InWorld;
    AvatarWorlds().push_back(WorldEntry{ &InWorld, this });
    InWorld.RegisterSystem("Engine.Avatar", GameCore::TP_PrePhysics, &Impl::System, this, GameCore::SO_BeforeScripts);
    InWorld.RegisterSystem("Engine.AvatarScripts", GameCore::TP_PrePhysics, &Impl::TellScripts, this, GameCore::SO_AfterScripts);
    P->RemovedHandle = InWorld.OnComponentRemoved().Add(&Impl::OnComponentRemoved, this);
    return true;
}

void AvatarWorld::Detach()
{
    if(!P->Bound) { return; }
    P->Bound->OnComponentRemoved().Remove(P->RemovedHandle);
    P->Bound->UnregisterSystem(&Impl::System, this);
    P->Bound->UnregisterSystem(&Impl::TellScripts, this);
    std::vector<WorldEntry>& Entries = AvatarWorlds();
    Entries.erase(std::remove_if(Entries.begin(), Entries.end(), [this](const WorldEntry& InEach) { return InEach.Avatars == this; }),
                  Entries.end());
    P->Bound = nullptr;
    P->Avatars.clear();
    P->ViewCamera = EntityID();
}

AvatarWorld* AvatarWorld::Find(const GameCore::World& InWorld)
{
    for(const WorldEntry& Entry : AvatarWorlds())
    {
        if(Entry.Owner == &InWorld) { return Entry.Avatars; }
    }
    return nullptr;
}

void AvatarWorld::SetInput(const InputState* InState) { P->Input = InState; }

void AvatarWorld::SetInputFilter(const AvatarInputFilterFn InFilter, void* InUserData)
{
    P->Filter     = InFilter;
    P->FilterData = InFilter ? InUserData : nullptr;
}

GameCore::EntityID AvatarWorld::GetViewCamera() const { return P->ViewCamera; }

void AvatarWorld::ResetPrediction(const GameCore::EntityID InAvatar)
{
    P->Avatars.erase(InAvatar.Pack());
}

AvatarPredictionStats AvatarWorld::TakePredictionStats(const GameCore::EntityID InAvatar)
{
    const auto Found = P->Avatars.find(InAvatar.Pack());
    if(Found == P->Avatars.end()) { return AvatarPredictionStats(); }
    AvatarPredictionStats Stats = Found->second.Stats;
    Stats.Pending = uint32_t(Found->second.Frames.size());
    Found->second.Stats = AvatarPredictionStats();
    return Stats;
}

/* One frame of a simulation, then its avatar's quantized replicated fields
 * snapped to what a client reads (ComponentReplication.h): the server, the
 * predicting client and a replay all step on from the very same values. */
static void StepSimulation(const Simulation& InSim, World& InWorld, const EntityID InAvatar,
                           const AvatarInputFrame& InFrame)
{
    InSim.Step(InWorld, InAvatar, InFrame);
    GameCore::QuantizeEntityReplicatedFields(InWorld, InAvatar);
}

void AvatarWorld::ApplyRemoteInput(const GameCore::EntityID InAvatar, const AvatarInputFrame& InFrame)
{
    if(!P->Bound) { return; }
    const AvatarHarnessComponent* Harness = HarnessOf(*P->Bound, InAvatar);
    const Simulation* Sim = Harness ? FindSimulation(Harness->Simulation) : nullptr;
    if(!Sim) { return; }
    AvatarInputFrame Frame = InFrame;
    Frame.Seconds = std::min(Frame.Seconds, Sim->MaxStepSeconds);
    StepSimulation(*Sim, *P->Bound, InAvatar, Frame);
}

bool AvatarWorld::HandleCorrection(const GameCore::EntityID InEntity, const int32_t InInputAck)
{
    if(!P->Bound) { return false; }
    World& InWorld = *P->Bound;
    const AvatarHarnessComponent* Harness = HarnessOf(InWorld, InEntity);
    const Simulation* Sim = Harness ? FindSimulation(Harness->Simulation) : nullptr;
    if(!Sim) { return false; }

    /* The server's state, as of frame InInputAck, is in: drop what it has
     * taken and replay the rest on top -- the same code from the same place,
     * so the avatar ends where it was shown unless the server disagreed. */
    Impl::Predicted& State = P->Avatars[InEntity.Pack()];
    while(!State.Frames.empty() && State.Frames.front().Sequence <= InInputAck) { State.Frames.pop_front(); }
    for(const AvatarInputFrame& Frame : State.Frames) { StepSimulation(*Sim, InWorld, InEntity, Frame); }

    if(const GameCore::TransformComponent* Transform = InWorld.GetEntity(InEntity).GetComponent<GameCore::TransformComponent>())
    {
        if(State.bShown)
        {
            const EngineCore::Vector3 Moved = Transform->Position - State.Shown;
            const double Distance = std::sqrt(double(Moved.GetX()) * double(Moved.GetX()) + double(Moved.GetY()) * double(Moved.GetY())
                                              + double(Moved.GetZ()) * double(Moved.GetZ()));
            State.Stats.MaxCorrection = std::max(State.Stats.MaxCorrection, Distance);
        }
        State.Shown  = Transform->Position;
        State.bShown = true;
    }
    ++State.Stats.Corrections;
    return true;
}

void AvatarWorld::Update(const float InSeconds, const bool bInSend)
{
    if(!P->Bound) { return; }
    World& InWorld = *P->Bound;
    static const InputState NoInput;
    const InputState& Devices = P->Input ? *P->Input : NoInput;
    const bool bAuthority = HasAuthority();
    const bool bClient    = IsClient();

    if(bClient) { P->ResolveClientLinks(); }

    /* By id: the hooks below run game code, which may add components. */
    std::vector<EntityID> Harnesses;
    InWorld.Each<AvatarHarnessComponent>([&](Entity InEntity, AvatarHarnessComponent&) { Harnesses.push_back(InEntity.GetID()); });

    EntityID ViewAvatar;
    for(const EntityID Avatar : Harnesses)
    {
        AvatarHarnessComponent* Harness = HarnessOf(InWorld, Avatar);
        if(!Harness) { continue; }

        /* A player gone without the server's word (a client): forget it. */
        if(Harness->Possessor.IsSet() && !InWorld.IsAlive(Harness->Possessor)) { Harness->Possessor = EntityID(); }
        if(bAuthority && Harness->Possessor.IsSet())
        {
            /* NetIds are given out as entities first replicate. */
            const NetComponent* PlayerNet = NetOf(InWorld, Harness->Possessor);
            Harness->PossessorNetId = PlayerNet ? PlayerNet->NetId : 0;
            if(PossessorComponent* Possessor = PossessorOf(InWorld, Harness->Possessor))
            {
                const NetComponent* AvatarNet = NetOf(InWorld, Avatar);
                Possessor->AvatarNetId = AvatarNet ? AvatarNet->NetId : 0;
            }
        }

        /* A change this process did not make (a client's view of the
         * server's): tell its own listeners. */
        if(Harness->SeenPossessor != Harness->Possessor)
        {
            const EntityID Was = Harness->SeenPossessor;
            const EntityID Now = Harness->Possessor;
            Harness->SeenPossessor = Now;
            ClearInput(*Harness);
            ResetPrediction(Avatar);
            if(Was.IsSet()) { Announce(InWorld, Was, Avatar, false); }
            if(Now.IsSet()) { Announce(InWorld, Now, Avatar, true); }
            Harness = HarnessOf(InWorld, Avatar);
            if(!Harness) { continue; }
        }

        if(!Harness->bEnabled || !Harness->IsPossessed())
        {
            ClearInput(*Harness);
            continue;
        }

        const PossessorComponent* Possessor = PossessorOf(InWorld, Harness->Possessor);
        if(Harness->IsLocallyControlled())
        {
            const uint8_t LocalPlayer = Possessor ? Possessor->LocalPlayer : 0;
            if(LocalPlayer == 0 && Harness->bViewTarget && !ViewAvatar.IsSet()) { ViewAvatar = Avatar; }

            InputSnapshot Read;
            ResolveInputSnapshot(Harness->InputMap, LocalPlayer, Devices, Harness->Down, Read);
            if(P->Filter)
            {
                P->Filter(InWorld, Avatar, Read, P->FilterData);
                Harness = HarnessOf(InWorld, Avatar);
                if(!Harness) { continue; }
            }
            Harness->Down     = Read.Down;
            Harness->Pressed  = Read.Pressed;
            Harness->Released = Read.Released;
            std::memcpy(Harness->Axes, Read.Axes, sizeof(Harness->Axes));
            if(!bInSend) { continue; }

            const Simulation* Sim = FindSimulation(Harness->Simulation);
            AvatarInputFrame Frame;
            Frame.Sequence = ++P->NextSequence;
            Frame.Seconds  = std::max(0.0f, std::min(InSeconds, Sim ? Sim->MaxStepSeconds : 0.1f));
            Frame.Down     = Read.Down;
            Frame.Pressed  = Read.Pressed;
            Frame.Released = Read.Released;
            std::memcpy(Frame.Axes, Read.Axes, sizeof(Frame.Axes));

            const NetComponent* Net = NetOf(InWorld, Avatar);
            if(bClient && Net && Net->HasNetId())
            {
                /* To the server. Without a simulation, a run of idle frames
                 * is one message: the server holds the last. With one, every
                 * frame is a step the server must take too. */
                Impl::Predicted& State = P->Avatars[Avatar.Pack()];
                const bool bIdle = IsIdle(Read);
                if(Sim || !(bIdle && State.bLastSentIdle))
                {
                    GameCore::RpcFloats<INPUT_AXES_MAX> Axes;
                    Axes.Count = uint8_t(INPUT_AXES_MAX);
                    std::memcpy(Axes.Values, Read.Axes, sizeof(Axes.Values));
                    GAMECORE_RPC(AvatarHarnessComponent, ServerInput)(InWorld.GetEntity(Avatar), Frame.Sequence, Frame.Seconds,
                                                                      Frame.Down, Frame.Pressed, Frame.Released, Axes);
                }
                State.bLastSentIdle = bIdle;

                if(Sim && Net->bPredicted)
                {
                    StepSimulation(*Sim, InWorld, Avatar, Frame);
                    Impl::Predicted& After = P->Avatars[Avatar.Pack()];   // the step may have reached in
                    After.Frames.push_back(Frame);
                    while(After.Frames.size() > AVATAR_HISTORY_MAX) { After.Frames.pop_front(); }   // the server has stopped answering
                    if(const auto* Transform = InWorld.GetEntity(Avatar).GetComponent<GameCore::TransformComponent>())
                    {
                        After.Shown  = Transform->Position;
                        After.bShown = true;
                    }
                }
            }
            else if(Sim && !bClient)
            {
                /* The server's own avatar, or a standalone game's. */
                StepSimulation(*Sim, InWorld, Avatar, Frame);
            }
            continue;
        }

        if(bAuthority)
        {
            /* A remote client's: what its ServerInputs left since last tick. */
            Harness->SecondsSinceInput += std::max(0.0f, InSeconds);
            if(Harness->bPendFresh)
            {
                Harness->Down     = Harness->PendDown;
                Harness->Pressed  = Harness->PendPressed;
                Harness->Released = Harness->PendReleased;
                for(int Axis = 0; Axis < int(INPUT_AXES_MAX); ++Axis)
                {
                    Harness->Axes[Axis] = Harness->PendAxes[Axis];
                    if(IsInputAxisAccumulated(Harness->InputMap, Axis)) { Harness->PendAxes[Axis] = 0.0f; }
                }
                Harness->PendPressed = Harness->PendReleased = 0;
                Harness->bPendFresh  = 0;
            }
            else
            {
                Harness->Pressed = Harness->Released = 0;
                for(int Axis = 0; Axis < int(INPUT_AXES_MAX); ++Axis)
                {
                    if(IsInputAxisAccumulated(Harness->InputMap, Axis)) { Harness->Axes[Axis] = 0.0f; }
                }
            }
            if(Harness->SecondsSinceInput * 1000.0f > float(std::max(0, GAvatarInputTimeoutMs.GetInt())))
            {
                Harness->Down = Harness->PendDown = 0;
                std::memset(Harness->Axes, 0, sizeof(Harness->Axes));
                std::memset(Harness->PendAxes, 0, sizeof(Harness->PendAxes));
            }
            continue;
        }

        /* Someone else's, on a client: theirs to read, not ours. */
        ClearInput(*Harness);
    }

    P->ViewCamera = ViewAvatar.IsSet() ? P->FindCamera(ViewAvatar) : EntityID();
}
}

// === Registration =============================================================

using NimbleEngine::AvatarHarnessComponent;
using NimbleEngine::PossessorComponent;

GAMECORE_DEFINE_COMPONENT(AvatarHarnessComponent, "AvatarHarness")
GAMECORE_DEFINE_COMPONENT(PossessorComponent, "Possessor")

/* The authored half, and who drives it: a save restores the possession. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(AvatarHarnessComponent, 1)
{
    Ar.Field("InputMap",    C.InputMap);
    Ar.Field("Simulation",  C.Simulation);
    Ar.Field("AutoPossess", C.AutoPossess);
    Ar.Flag("Enabled",      C.bEnabled);
    Ar.Flag("ViewTarget",   C.bViewTarget);
    Ar.Flag("Requestable",  C.bRequestable);
    Ar.Entity("Possessor",  C.Possessor);
}

GAMECORE_DEFINE_COMPONENT_SERIALIZER(PossessorComponent, 1)
{
    Ar.Entity("Avatar",     C.Avatar);
    Ar.Field("LocalPlayer", C.LocalPlayer);
}

/* A client learns who drives what. The NetIds cover a link that names an
 * entity the client has not been sent yet. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(AvatarHarnessComponent)
{
    Rep.Entity("Possessor",     &AvatarHarnessComponent::Possessor);
    Rep.Field("PossessorNetId", &AvatarHarnessComponent::PossessorNetId);
    Rep.Flag("Enabled",         &AvatarHarnessComponent::bEnabled);
}

GAMECORE_DEFINE_COMPONENT_REPLICATION(PossessorComponent)
{
    Rep.Entity("Avatar",     &PossessorComponent::Avatar);
    Rep.Field("AvatarNetId", &PossessorComponent::AvatarNetId);
}

GAMECORE_DEFINE_COMPONENT_RPCS(AvatarHarnessComponent)
{
    Rpc.Server<&AvatarHarnessComponent::ServerInput>("ServerInput");
}

GAMECORE_DEFINE_COMPONENT_RPCS(PossessorComponent)
{
    Rpc.Server<&PossessorComponent::ServerRequestPossess>("ServerRequestPossess");
    Rpc.Server<&PossessorComponent::ServerRequestUnpossess>("ServerRequestUnpossess");
}

/* harness:IsDown("Jump"), :WasPressed, :WasReleased, :GetAxis("MoveX") --
 * the same as an Input component's, from whoever drives it. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(AvatarHarnessComponent)
{
    Bind.Field("InputMap",    &AvatarHarnessComponent::InputMap);
    Bind.Field("Simulation",  &AvatarHarnessComponent::Simulation);
    Bind.Field("AutoPossess", &AvatarHarnessComponent::AutoPossess);
    Bind.Flag("Enabled",      &AvatarHarnessComponent::bEnabled);
    Bind.Flag("ViewTarget",   &AvatarHarnessComponent::bViewTarget);
    Bind.Flag("Requestable",  &AvatarHarnessComponent::bRequestable);

    Bind.NativePure<&AvatarHarnessComponent::IsDown>("IsDown", "Action -> Down");
    Bind.NativePure<&AvatarHarnessComponent::WasPressed>("WasPressed", "Action -> Pressed");
    Bind.NativePure<&AvatarHarnessComponent::WasReleased>("WasReleased", "Action -> Released");
    Bind.NativePure<&AvatarHarnessComponent::GetAxis>("GetAxis", "Axis -> Value");
    Bind.NativePure<&AvatarHarnessComponent::IsPossessed>("IsPossessed", "-> Possessed");
    Bind.NativePure<&AvatarHarnessComponent::IsLocallyControlled>("IsLocallyControlled", "-> Local");
    Bind.NativePure<&AvatarHarnessComponent::GetPossessor>("GetPossessor", "-> Player");
}

/* possessor:GetAvatar(); possessor:Possess(avatar) and :Unpossess() -- on a
 * client, a request the server's mode may refuse. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(PossessorComponent)
{
    Bind.Field("LocalPlayer", &PossessorComponent::LocalPlayer);
    Bind.NativePure<&PossessorComponent::GetAvatar>("GetAvatar", "-> Avatar");

    Bind.Method("Possess", [](PossessorComponent& C, GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Avatar = Call.GetEntity(1);
        if(Call.HasError()) { return; }
        if(NimbleEngine::HasAuthority())
        {
            Call.PushBool(NimbleEngine::Possess(*C.Owner.OwningWorld, C.Owner.Entity, Avatar));
            return;
        }
        Call.PushBool(GAMECORE_RPC(PossessorComponent, ServerRequestPossess)(C.Owner.GetEntity(), Avatar));
    }, "Avatar:Entity -> Accepted:bool");

    Bind.Method("Unpossess", [](PossessorComponent& C, GameCore::ScriptCall& Call)
    {
        if(NimbleEngine::HasAuthority())
        {
            Call.PushBool(NimbleEngine::Unpossess(*C.Owner.OwningWorld, C.Owner.Entity));
            return;
        }
        Call.PushBool(GAMECORE_RPC(PossessorComponent, ServerRequestUnpossess)(C.Owner.GetEntity()));
    }, "-> Accepted:bool");
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(AvatarHarnessComponent)
{
    Props.Category("Avatar");
    Props.Field("InputMap",     &AvatarHarnessComponent::InputMap);
    Props.Choice("AutoPossess", &AvatarHarnessComponent::AutoPossess, "Disabled|Player 1|Player 2|Player 3|Player 4");
    Props.Flag("ViewTarget",    &AvatarHarnessComponent::bViewTarget);
    Props.Flag("Requestable",   &AvatarHarnessComponent::bRequestable);
    Props.Flag("Enabled",       &AvatarHarnessComponent::bEnabled);
    Props.Field("Simulation",   &AvatarHarnessComponent::Simulation);
}

GAMECORE_DEFINE_COMPONENT_PROPERTIES(PossessorComponent)
{
    Props.Choice("LocalPlayer", &PossessorComponent::LocalPlayer, "Player 1|Player 2|Player 3|Player 4");
}
#endif
