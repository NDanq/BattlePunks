#include "World/LabLogic.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/RenderCore/Components/PointLightComponent.h"
#include "Public/RenderCore/Components/RectLightComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/Components/SpotLightComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "World/LabSignals.h"
#include "LabVersion.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GameCore;
using namespace NimbleEngine;

// === Components =============================================================

GAMECORE_DEFINE_COMPONENT(LabSignalToggleComponent, "LabSignalToggle")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabSignalToggleComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Channel", C.Channel);
    Ar.Flag("ShowWhenOn", C.bShowWhenOn);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabSignalToggleComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Channel", &LabSignalToggleComponent::Channel);
    Props.Flag("ShowWhenOn", &LabSignalToggleComponent::bShowWhenOn);
}
#endif

GAMECORE_DEFINE_COMPONENT(LabTimerComponent, "LabTimer")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabTimerComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("StartOn", C.StartOn);
    Ar.Field("Seconds", C.Seconds);
    Ar.Field("DoneChannel", C.DoneChannel);
    Ar.Field("Label", C.Label);
    Ar.Field("State", C.State);
    Ar.Field("Remaining", C.Remaining);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabTimerComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("StartOn", &LabTimerComponent::StartOn);
    Props.Field("Seconds", &LabTimerComponent::Seconds);
    Props.Field("DoneChannel", &LabTimerComponent::DoneChannel);
    Props.Field("Label", &LabTimerComponent::Label);
    Props.Choice("State", &LabTimerComponent::State, "Waiting|Running|Done");
}
#endif

GAMECORE_DEFINE_COMPONENT(LabAllOfComponent, "LabAllOf")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabAllOfComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Inputs", C.Inputs);
    Ar.Field("Output", C.Output);
    Ar.Flag("Fired", C.bFired);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabAllOfComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Inputs", &LabAllOfComponent::Inputs);
    Props.Field("Output", &LabAllOfComponent::Output);
}
#endif

GAMECORE_DEFINE_COMPONENT(LabTravelComponent, "LabTravel")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabTravelComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("OnSignal", C.OnSignal);
    Ar.Field("World", C.World);
    Ar.Field("Entry", C.Entry);
    Ar.Field("Flags", C.Flags);
    Ar.Field("Title", C.Title);
    Ar.Flag("Fired", C.bFired);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabTravelComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("OnSignal", &LabTravelComponent::OnSignal);
    Props.Field("World", &LabTravelComponent::World);
    Props.Field("Entry", &LabTravelComponent::Entry);
    Props.Field("Flags", &LabTravelComponent::Flags);
    Props.Field("Title", &LabTravelComponent::Title);
}
#endif

// === Logic ==================================================================

namespace
{
    bool AllTrue(World& InWorld, const char* InInputs)
    {
        std::string Name;
        const std::string List = std::string(InInputs) + ",";
        bool bAny = false;
        for(const char C : List)
        {
            if(C != ',') { if(C != ' ') { Name += C; } continue; }
            if(Name.empty()) { continue; }
            if(!LabSignals::Get(InWorld, Name.c_str())) { return false; }
            bAny = true;
            Name.clear();
        }
        return bAny;
    }
}

void LabLogic::OnSignal(World& InWorld, const char* InChannel, const bool bInValue)
{
    if(!HasAuthority() || !InChannel) { return; }
    if(bInValue)
    {
        InWorld.Each<LabTimerComponent>([&](Entity, LabTimerComponent& InTimer)
        {
            if(InTimer.State == 0 && InTimer.StartOn[0] && std::strcmp(InTimer.StartOn, InChannel) == 0)
            {
                InTimer.State = 1;
                InTimer.Remaining = InTimer.Seconds;
                CORE_LOG(std::string("[DemoLab] timer '") + InTimer.Label + "' started")
            }
        });
    }
    std::vector<std::string> Outputs;
    InWorld.Each<LabAllOfComponent>([&](Entity, LabAllOfComponent& InGate)
    {
        if(InGate.bFired || !InGate.Output[0] || !AllTrue(InWorld, InGate.Inputs)) { return; }
        InGate.bFired = 1;
        Outputs.emplace_back(InGate.Output);
    });
    for(const std::string& Out : Outputs) { LabSignals::Emit(InWorld, Out.c_str(), true); }
}

void LabLogic::Tick(World& InWorld, const float InSeconds)
{
    if(!HasAuthority()) { return; }
    std::vector<std::string> Done;
    InWorld.Each<LabTimerComponent>([&](Entity, LabTimerComponent& InTimer)
    {
        if(InTimer.State != 1) { return; }
        InTimer.Remaining -= InSeconds;
        if(InTimer.Remaining > 0.f) { return; }
        InTimer.Remaining = 0.f;
        InTimer.State = 2;
        if(InTimer.DoneChannel[0]) { Done.emplace_back(InTimer.DoneChannel); }
    });
    for(const std::string& Channel : Done) { LabSignals::Emit(InWorld, Channel.c_str(), true); }
}

void LabLogic::TickPresentation(World& InWorld)
{
    std::vector<std::pair<EntityID, bool>> Wanted;
    InWorld.Each<LabSignalToggleComponent>([&](Entity InEntity, LabSignalToggleComponent& InToggle)
    {
        const bool bOn = LabSignals::Get(InWorld, InToggle.Channel);
        Wanted.emplace_back(InEntity.GetID(), InToggle.bShowWhenOn ? bOn : !bOn);
    });
    for(const auto& Pair : Wanted)
    {
        Entity E = InWorld.GetEntity(Pair.first);
        const uint8_t Value = Pair.second ? 1 : 0;
        if(RenderCore::RenderComponent* Render = E.GetComponent<RenderCore::RenderComponent>()) { if(Render->bVisible != Value) { Render->bVisible = Value; } }
        if(RenderCore::PointLightComponent* Light = E.GetComponent<RenderCore::PointLightComponent>()) { if(Light->bActive != Value) { Light->bActive = Value; } }
        if(RenderCore::SpotLightComponent* Light = E.GetComponent<RenderCore::SpotLightComponent>()) { if(Light->bActive != Value) { Light->bActive = Value; } }
        if(RenderCore::RectLightComponent* Light = E.GetComponent<RenderCore::RectLightComponent>()) { if(Light->bActive != Value) { Light->bActive = Value; } }
    }
}

std::string LabLogic::TimerLine(World& InWorld)
{
    std::string Line;
    InWorld.Each<LabTimerComponent>([&](Entity, LabTimerComponent& InTimer)
    {
        if(InTimer.State != 1 || !Line.empty()) { return; }
        const int Seconds = int(InTimer.Remaining + 0.999f);
        char Text[64];
        std::snprintf(Text, sizeof(Text), "%s  %d:%02d", InTimer.Label, Seconds / 60, Seconds % 60);
        Line = Text;
    });
    return Line;
}

int LabLogic::ApplyStateGroups(World& InWorld, const bool bInAlarm)
{
    const char* const Drop = bInAlarm ? "Calm_" : "Alarm_";
    const size_t Length = std::strlen(Drop);
    std::vector<EntityID> Doomed;
    InWorld.ForEachEntity([](EntityID InEntity, void* InUserData)
    {
        static_cast<std::vector<EntityID>*>(InUserData)->push_back(InEntity);
    }, &Doomed);
    int Count = 0;
    for(const EntityID E : Doomed)
    {
        if(!InWorld.IsAlive(E)) { continue; }
        const char* Name = InWorld.GetEntity(E).GetName();
        if(Name && std::strncmp(Name, Drop, Length) == 0)
        {
            InWorld.DestroyEntity(E);
            ++Count;
        }
    }
    return Count;
}
