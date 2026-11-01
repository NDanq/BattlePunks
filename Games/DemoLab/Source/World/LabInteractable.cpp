#include "World/LabInteractable.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/SequenceFacade.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "Core/LabHealth.h"
#include "Weapons/LabCombat.h"
#include "World/LabSignals.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "LabVersion.h"

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

namespace
{
    constexpr float RADIUS_SLOP = 120.f;   // the authority's allowance over Radius for a client's press
    constexpr float HEIGHT_BAND = 200.f;   // how far above or below the feet a thing can be used

    bool IsUpPlayer(World& InWorld, const EntityID InAvatar)
    {
        const Entity E = InWorld.GetEntity(InAvatar);
        if(!E.IsAlive()) { return false; }
        const LabHealthComponent* Health = E.GetComponent<LabHealthComponent>();
        return E.GetComponent<AvatarHarnessComponent>() && (!Health || (!Health->bDead && !Health->bDown));
    }

    EntityID FindChild(World& InWorld, const EntityID InParent, const char* InName)
    {
        for(EntityID C = InWorld.GetEntity(InParent).GetFirstChild(); C.IsSet(); C = InWorld.GetEntity(C).GetNextSibling())
        {
            if(std::strcmp(InWorld.GetEntity(C).GetName(), InName) == 0) { return C; }
        }
        return EntityID();
    }

    void SetVisibleTree(World& InWorld, const EntityID InRoot, const bool bInVisible)
    {
        if(RenderCore::RenderComponent* Render = InWorld.GetEntity(InRoot).GetComponent<RenderCore::RenderComponent>())
        {
            Render->bVisible = bInVisible ? 1 : 0;
        }
        for(EntityID C = InWorld.GetEntity(InRoot).GetFirstChild(); C.IsSet(); C = InWorld.GetEntity(C).GetNextSibling())
        {
            SetVisibleTree(InWorld, C, bInVisible);
        }
    }

    bool InReach(World& InWorld, const EntityID InAvatar, const EntityID InThing, const float InRadius, float* OutDistance = nullptr)
    {
        const Vector3 Feet = InWorld.GetEntity(InAvatar).GetWorldPose().Position;
        const Vector3 At = InWorld.GetEntity(InThing).GetWorldPose().Position;
        const Vector3 To = At - Feet;
        if(To.GetY() < -HEIGHT_BAND || To.GetY() > HEIGHT_BAND + 100.0) { return false; }
        const double Flat = Vector3(To.GetX(), 0, To.GetZ()).GetLength();
        if(OutDistance) { *OutDistance = float(Flat); }
        return Flat <= double(InRadius);
    }
}

// === Components =============================================================

GAMECORE_DEFINE_COMPONENT(LabInteractableComponent, "LabInteractable")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabInteractableComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Prompt", C.Prompt);
    Ar.Field("Radius", C.Radius);
    Ar.Field("WeaponID", C.WeaponID);
    Ar.Field("Ammo", C.Ammo);
    Ar.Field("Channel", C.Channel);
    Ar.Flag("Toggle", C.bToggle);
    Ar.Field("Sequence", C.Sequence);
    Ar.Field("EnableOn", C.EnableOn);
    Ar.Flag("Once", C.bOnce);
    Ar.Flag("HideWhenUsed", C.bHideWhenUsed);
    Ar.Field("HoldSeconds", C.HoldSeconds);
    Ar.Field("Sound", C.Sound);
    Ar.Flag("Used", C.bUsed);
    Ar.Flag("On", C.bOn);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabInteractableComponent) {
    Rep.Flag("Used", &LabInteractableComponent::bUsed);
    Rep.Flag("On", &LabInteractableComponent::bOn);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabInteractableComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Prompt", &LabInteractableComponent::Prompt);
    Props.Field("Radius", &LabInteractableComponent::Radius);
    Props.Category("Use");
    Props.Field("WeaponID", &LabInteractableComponent::WeaponID);
    Props.Field("Ammo", &LabInteractableComponent::Ammo);
    Props.Field("Channel", &LabInteractableComponent::Channel);
    Props.Flag("Toggle", &LabInteractableComponent::bToggle);
    Props.Field("Sequence", &LabInteractableComponent::Sequence);
    Props.Field("EnableOn", &LabInteractableComponent::EnableOn);
    Props.Flag("Once", &LabInteractableComponent::bOnce);
    Props.Flag("HideWhenUsed", &LabInteractableComponent::bHideWhenUsed);
    Props.Field("HoldSeconds", &LabInteractableComponent::HoldSeconds);
    Props.Field("Sound", &LabInteractableComponent::Sound, PF_AssetSound);
    Props.Category("State");
    Props.Flag("Used", &LabInteractableComponent::bUsed);
    Props.Flag("On", &LabInteractableComponent::bOn);
}
#endif

GAMECORE_DEFINE_COMPONENT(LabGateComponent, "LabGate")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabGateComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("OpenOn", C.OpenOn);
    Ar.Field("OpenHeight", C.OpenHeight);
    Ar.Field("Speed", C.Speed);
    Ar.Flag("Invert", C.bInvert);
    Ar.Field("Sound", C.Sound);
    Ar.Flag("Open", C.bOpen);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabGateComponent) {
    Rep.Flag("Open", &LabGateComponent::bOpen);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabGateComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("OpenOn", &LabGateComponent::OpenOn);
    Props.Field("OpenHeight", &LabGateComponent::OpenHeight);
    Props.Field("Speed", &LabGateComponent::Speed);
    Props.Flag("Invert", &LabGateComponent::bInvert);
    Props.Field("Sound", &LabGateComponent::Sound, PF_AssetSound);
    Props.Flag("Open", &LabGateComponent::bOpen);
}
#endif

GAMECORE_DEFINE_COMPONENT(LabTriggerComponent, "LabTrigger")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabTriggerComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Extent", C.Extent);
    Ar.Field("Channel", C.Channel);
    Ar.Field("Sequence", C.Sequence);
    Ar.Field("EnableOn", C.EnableOn);
    Ar.Flag("Once", C.bOnce);
    Ar.Flag("Checkpoint", C.bCheckpoint);
    Ar.Flag("Fired", C.bFired);   // a checkpoint load must not fire it again
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabTriggerComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Extent", &LabTriggerComponent::Extent);
    Props.Field("Channel", &LabTriggerComponent::Channel);
    Props.Field("Sequence", &LabTriggerComponent::Sequence);
    Props.Field("EnableOn", &LabTriggerComponent::EnableOn);
    Props.Flag("Once", &LabTriggerComponent::bOnce);
    Props.Flag("Checkpoint", &LabTriggerComponent::bCheckpoint);
}
#endif


GAMECORE_DEFINE_COMPONENT(LabObjectiveComponent, "LabObjective")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabObjectiveComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("OnSignal", C.OnSignal);
    Ar.Field("Text", C.Text);
    Ar.Flag("Shown", C.bShown);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabObjectiveComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("OnSignal", &LabObjectiveComponent::OnSignal);
    Props.Field("Text", &LabObjectiveComponent::Text);
}
#endif

// === Interaction ============================================================

bool LabWorld::IsUsable(World& InWorld, const LabInteractableComponent& InThing)
{
    if(InThing.bOnce && InThing.bUsed) { return false; }
    if(InThing.EnableOn[0] && !LabSignals::Get(InWorld, InThing.EnableOn)) { return false; }
    return true;
}

EntityID LabWorld::FindInteractable(World& InWorld, const EntityID InAvatar)
{
    if(!IsUpPlayer(InWorld, InAvatar)) { return EntityID(); }

    Vector3 Origin, Aim;
    const bool bAim = GetCharacterAim(InWorld, InAvatar, Origin, Aim);
    const Vector3 Feet = InWorld.GetEntity(InAvatar).GetWorldPose().Position;

    EntityID Best;
    double BestScore = 1e30;
    InWorld.Each<LabInteractableComponent>([&](Entity InThing, LabInteractableComponent& InData)
    {
        if(!IsUsable(InWorld, InData)) { return; }
        float Distance = 0.f;
        if(!InReach(InWorld, InAvatar, InThing.GetID(), InData.Radius, &Distance)) { return; }

        /* Nearer wins; one in front of the view wins more. */
        double Score = Distance;
        if(bAim)
        {
            Vector3 To = InThing.GetWorldPose().Position - Feet;
            To.SetY(0.0);
            Vector3 Flat(Aim.GetX(), 0, Aim.GetZ());
            if(To.GetLength() > 1.0 && Flat.GetLength() > 1e-4)
            {
                Score -= To.GetNormalize().Dot(Flat.GetNormalize()) * 80.0;
            }
        }
        if(Score < BestScore) { BestScore = Score; Best = InThing.GetID(); }
    });
    return Best;
}

std::string LabWorld::PromptFor(World& InWorld, const EntityID InAvatar)
{
    const EntityID Thing = FindInteractable(InWorld, InAvatar);
    if(!Thing.IsSet()) { return std::string(); }
    const LabInteractableComponent* Data = InWorld.GetEntity(Thing).GetComponent<LabInteractableComponent>();
    if(!Data) { return std::string(); }
    if(Data->bToggle && Data->bOn) { return std::string(Data->Prompt) + " (off)"; }
    if(Data->HoldSeconds > 0.f) { return std::string(Data->Prompt) + " (hold)"; }
    return std::string(Data->Prompt);
}

float LabWorld::HoldFraction(World& InWorld, const EntityID InAvatar)
{
    const EntityID Thing = FindInteractable(InWorld, InAvatar);
    const LabInteractableComponent* Data = Thing.IsSet() ? InWorld.GetEntity(Thing).GetComponent<LabInteractableComponent>() : nullptr;
    if(!Data || Data->HoldSeconds <= 0.f) { return -1.f; }
    return std::min(1.f, Data->HoldProgress / Data->HoldSeconds);
}

bool LabWorld::PlaySequence(World& InWorld, const char* InNameOrPath)
{
    if(!InNameOrPath || !InNameOrPath[0]) { return false; }
    const size_t Length = std::strlen(InNameOrPath);
    if(Length > 6 && std::strcmp(InNameOrPath + Length - 6, ".nmseq") == 0)
    {
        return NimbleEngine::PlaySequence(InWorld, InNameOrPath).IsSet();
    }
    const EntityID Player = InWorld.FindEntityByName(InNameOrPath);
    if(!Player.IsSet())
    {
        CORE_LOG_WARN(std::string("[DemoLab] no Sequence Player named '") + InNameOrPath + "' in this level")
        return false;
    }
    return StartSequence(InWorld, Player);
}

void LabWorld::Interact(World& InWorld, const EntityID InUser, const EntityID InTarget)
{
    if(!HasAuthority() || !IsUpPlayer(InWorld, InUser) || !InWorld.IsAlive(InTarget)) { return; }
    LabInteractableComponent* Thing = InWorld.GetEntity(InTarget).GetComponent<LabInteractableComponent>();
    if(!Thing || !IsUsable(InWorld, *Thing)) { return; }
    if(!InReach(InWorld, InUser, InTarget, Thing->Radius + RADIUS_SLOP)) { return; }

    const LabInteractableComponent Data = *Thing;   // a copy: what follows may spawn
    Thing->bUsed = 1;
    if(Data.bToggle) { Thing->bOn = Thing->bOn ? 0 : 1; }
    const bool bState = Data.bToggle ? (Thing->bOn != 0) : true;

    if(Data.Sound[0]) { PlaySoundAt(InWorld, Data.Sound, InWorld.GetEntity(InTarget).GetWorldPose().Position); }
    if(Data.WeaponID) { LabCombat::GiveWeapon(InWorld, InUser, Data.WeaponID, true); }
    if(Data.Ammo > 0) { LabCombat::GiveAmmo(InWorld, InUser, 0, Data.Ammo); }

    EventArgs Args;
    Args.Args[Args.Count++] = EventArg::Make(InTarget);
    Args.Args[Args.Count++] = EventArg::Make(InUser);
    InWorld.GetEvents().Emit("Lab.Interacted", Args);

    if(Data.Channel[0]) { LabSignals::Emit(InWorld, Data.Channel, bState, InTarget); }
    if(Data.Sequence[0] && (!Data.bToggle || bState)) { PlaySequence(InWorld, Data.Sequence); }
}

void LabCombatComponent::ServerInteract(const RpcContext& InContext, const EntityID InTarget)
{
    if(InContext.OwningWorld) { LabWorld::Interact(*InContext.OwningWorld, InContext.Entity, InTarget); }
}

void LabWorld::TickInteractInput(World& InWorld, const float InSeconds, const bool bInHeld)
{
    std::vector<EntityID> Pressed;
    std::vector<EntityID> Holding;
    InWorld.Each<AvatarHarnessComponent>([&](Entity InEntity, AvatarHarnessComponent& InHarness)
    {
        if(bInHeld || !InHarness.bEnabled || !InHarness.IsLocallyControlled()) { return; }
        if(InHarness.WasPressed("Interact")) { Pressed.push_back(InEntity.GetID()); }
        if(InHarness.IsDown("Interact")) { Holding.push_back(InEntity.GetID()); }
    });

    /* Holds (single-player: the authority's own avatar): the progress grows
     * while Interact is down in reach, and drains when it is let go. */
    std::vector<EntityID> Held;
    std::vector<std::pair<EntityID, EntityID>> Completed;
    if(HasAuthority())
    {
        for(const EntityID Avatar : Holding)
        {
            const EntityID Target = FindInteractable(InWorld, Avatar);
            LabInteractableComponent* Data = Target.IsSet() ? InWorld.GetEntity(Target).GetComponent<LabInteractableComponent>() : nullptr;
            if(!Data || Data->HoldSeconds <= 0.f) { continue; }
            Held.push_back(Target);
            Data->HoldProgress += InSeconds;
            if(Data->HoldProgress >= Data->HoldSeconds) { Completed.emplace_back(Avatar, Target); }
        }
        InWorld.Each<LabInteractableComponent>([&](Entity InEntity, LabInteractableComponent& InThing)
        {
            if(InThing.HoldSeconds <= 0.f || InThing.HoldProgress <= 0.f) { return; }
            if(std::find(Held.begin(), Held.end(), InEntity.GetID()) != Held.end()) { return; }
            InThing.HoldProgress = std::max(0.f, InThing.HoldProgress - InSeconds * 2.f);
        });
        for(const auto& Pair : Completed) { Interact(InWorld, Pair.first, Pair.second); }
    }

    for(const EntityID Avatar : Pressed)
    {
        const EntityID Target = FindInteractable(InWorld, Avatar);
        if(!Target.IsSet()) { continue; }
        const LabInteractableComponent* Data = InWorld.GetEntity(Target).GetComponent<LabInteractableComponent>();
        if(Data && Data->HoldSeconds > 0.f) { continue; }   // a hold, above
        if(HasAuthority()) { Interact(InWorld, Avatar, Target); }
        else { GAMECORE_RPC(LabCombatComponent, ServerInteract)(InWorld.GetEntity(Avatar), Target); }
    }
}

// === Signals, gates, triggers ===============================================

void LabWorld::OnSignal(World& InWorld, const char* InChannel, const bool bInValue)
{
    if(!HasAuthority() || !InChannel) { return; }
    std::vector<std::pair<std::string, Vector3>> Sounds;
    InWorld.Each<LabGateComponent>([&](Entity InEntity, LabGateComponent& InGate)
    {
        if(!InGate.OpenOn[0] || std::strcmp(InGate.OpenOn, InChannel) != 0) { return; }
        const uint8_t Open = (bInValue != (InGate.bInvert != 0)) ? 1 : 0;
        if(Open != InGate.bOpen && InGate.Sound[0]) { Sounds.emplace_back(InGate.Sound, InEntity.GetWorldPose().Position); }
        InGate.bOpen = Open;
    });
    for(const auto& Sound : Sounds) { PlaySoundAt(InWorld, Sound.first.c_str(), Sound.second); }
}

void LabWorld::TickTriggers(World& InWorld, float)
{
    if(!HasAuthority()) { return; }

    /* The players to test: up, possessed characters. */
    std::vector<Vector3> Players;
    InWorld.Each<AvatarHarnessComponent>([&](Entity InEntity, AvatarHarnessComponent& InHarness)
    {
        if(InHarness.IsPossessed() && IsUpPlayer(InWorld, InEntity.GetID())) { Players.push_back(InEntity.GetWorldPose().Position); }
    });

    struct Fired { EntityID Trigger; LabTriggerComponent Data; };
    std::vector<Fired> Fires;
    InWorld.Each<LabTriggerComponent>([&](Entity InEntity, LabTriggerComponent& InTrigger)
    {
        if(InTrigger.bOnce && InTrigger.bFired) { return; }
        if(InTrigger.EnableOn[0] && !LabSignals::Get(InWorld, InTrigger.EnableOn)) { return; }
        const Vector3 Centre = InEntity.GetWorldPose().Position;
        bool bAny = false;
        for(const Vector3& P : Players)
        {
            const Vector3 D = P - Centre;
            if(std::fabs(D.GetX()) <= InTrigger.Extent.GetX() && std::fabs(D.GetY()) <= InTrigger.Extent.GetY()
               && std::fabs(D.GetZ()) <= InTrigger.Extent.GetZ())
            {
                bAny = true;
                break;
            }
        }
        if(bAny && !InTrigger.bOccupied)
        {
            InTrigger.bFired = 1;
            Fires.push_back({ InEntity.GetID(), InTrigger });
        }
        InTrigger.bOccupied = bAny ? 1 : 0;
    });

    for(const Fired& F : Fires)
    {
        if(F.Data.bCheckpoint)
        {
            EventArgs Args;
            Args.Args[Args.Count++] = EventArg::Make(F.Trigger);
            InWorld.GetEvents().Emit("Lab.Checkpoint", Args);
        }
        if(F.Data.Channel[0]) { LabSignals::Emit(InWorld, F.Data.Channel, true, F.Trigger); }
        if(F.Data.Sequence[0]) { PlaySequence(InWorld, F.Data.Sequence); }
    }
}

void LabWorld::TickPresentation(World& InWorld, const float InSeconds)
{
    /* Gates slide toward open or shut; a Kinematic body follows its Transform. */
    InWorld.Each<LabGateComponent>([&](Entity InEntity, LabGateComponent& InGate)
    {
        TransformComponent* Transform = InEntity.GetComponent<TransformComponent>();
        if(!Transform) { return; }
        if(!InGate.bHasClosed)
        {
            InGate.ClosedPosition = Transform->Position;
            InGate.bHasClosed = 1;
        }
        const float Target = InGate.bOpen ? InGate.OpenHeight : 0.f;
        const float Step = InGate.Speed * InSeconds;
        if(std::fabs(Target - InGate.Offset) <= Step) { InGate.Offset = Target; }
        else { InGate.Offset += Target > InGate.Offset ? Step : -Step; }
        const Vector3 Want = InGate.ClosedPosition + Vector3(0, double(InGate.Offset), 0);
        if((Transform->Position - Want).GetLength() > 1e-3) { Transform->Position = Want; }
    });

    /* Levers: a child "Handle" leans one way off, the other way on. Used
     * pickups disappear. */
    std::vector<std::pair<EntityID, bool>> Hide;
    InWorld.Each<LabInteractableComponent>([&](Entity InEntity, LabInteractableComponent& InThing)
    {
        if(InThing.bHideWhenUsed) { Hide.emplace_back(InEntity.GetID(), InThing.bUsed == 0); }
        if(InThing.HoldSeconds > 0.f)
        {
            /* A crank: its Handle turns about Z as the hold goes on. */
            const EntityID Crank = FindChild(InWorld, InEntity.GetID(), "Handle");
            TransformComponent* C = Crank.IsSet() ? InWorld.GetEntity(Crank).GetComponent<TransformComponent>() : nullptr;
            if(C) { C->Rotation.SetZ(double(InThing.bUsed ? InThing.HoldSeconds : InThing.HoldProgress) * 240.0); }
            return;
        }
        if(!InThing.bToggle) { return; }
        const EntityID Handle = FindChild(InWorld, InEntity.GetID(), "Handle");
        TransformComponent* T = Handle.IsSet() ? InWorld.GetEntity(Handle).GetComponent<TransformComponent>() : nullptr;
        if(!T) { return; }
        const double Want = InThing.bOn ? 40.0 : -40.0;
        const double Now = T->Rotation.GetX();
        const double Step = 240.0 * double(InSeconds);
        T->Rotation.SetX(std::fabs(Want - Now) <= Step ? Want : Now + (Want > Now ? Step : -Step));
    });
    for(const auto& Pair : Hide)
    {
        const RenderCore::RenderComponent* Render = InWorld.GetEntity(Pair.first).GetComponent<RenderCore::RenderComponent>();
        if(Render && (Render->bVisible != 0) == Pair.second) { continue; }
        SetVisibleTree(InWorld, Pair.first, Pair.second);
    }
}
