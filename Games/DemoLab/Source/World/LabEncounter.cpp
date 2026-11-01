#include "World/LabEncounter.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/NimbleEngine.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "AI/LabBots.h"
#include "Core/LabHealth.h"
#include "World/LabSignals.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "LabVersion.h"

using namespace GameCore;
using namespace NimbleEngine;

#define LAB_ENCOUNTER_VER (1)

GAMECORE_DEFINE_COMPONENT(LabEncounterComponent, "LabEncounter")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabEncounterComponent, LAB_ENCOUNTER_VER) {
    Ar.Field("Id", C.Id);
    Ar.Field("StartOn", C.StartOn);
    Ar.Field("DoneChannel", C.DoneChannel);
    Ar.Field("SpawnPrefab", C.SpawnPrefab);
    Ar.Field("State", C.State);
    Ar.Field("Waves", C.Waves);
    Ar.Field("WaveDelay", C.WaveDelay);
    Ar.Field("LockChannel", C.LockChannel);
    Ar.Field("StopOn", C.StopOn);
    Ar.Field("SpawnTeam", C.SpawnTeam);
    Ar.Field("EnemyHealth", C.EnemyHealth);
    Ar.Field("Wave", C.Wave);
    Ar.Field("WaveCount", C.WaveCount);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabEncounterComponent) {
    Rep.Field("State", &LabEncounterComponent::State);
    Rep.Field("Alive", &LabEncounterComponent::Alive);
    Rep.Field("Total", &LabEncounterComponent::Total);
    Rep.Field("Wave", &LabEncounterComponent::Wave);
    Rep.Field("WaveCount", &LabEncounterComponent::WaveCount);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabEncounterComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Id", &LabEncounterComponent::Id);
    Props.Field("StartOn", &LabEncounterComponent::StartOn);
    Props.Field("DoneChannel", &LabEncounterComponent::DoneChannel);
    Props.Field("SpawnPrefab", &LabEncounterComponent::SpawnPrefab, PF_AssetPrefab);
    Props.Category("Waves");
    Props.Field("Waves", &LabEncounterComponent::Waves);
    Props.Field("WaveDelay", &LabEncounterComponent::WaveDelay);
    Props.Field("LockChannel", &LabEncounterComponent::LockChannel);
    Props.Field("StopOn", &LabEncounterComponent::StopOn);
    Props.Field("SpawnTeam", &LabEncounterComponent::SpawnTeam);
    Props.Field("EnemyHealth", &LabEncounterComponent::EnemyHealth);
    Props.Choice("State", &LabEncounterComponent::State, "Waiting|Fighting|Done");
}
#endif

GAMECORE_DEFINE_COMPONENT(LabEncounterMemberComponent, "LabEncounterMember")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabEncounterMemberComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Encounter", C.Encounter);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabEncounterMemberComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Encounter", &LabEncounterMemberComponent::Encounter);
}
#endif

namespace
{
    /* One wave's kinds: "Watchman:2,Brute:1" -> {Watchman, Watchman, Brute}. */
    std::vector<uint8_t> ParseWave(const char* InWaves, const int InWave, int& OutWaves)
    {
        std::vector<std::string> Waves(1);
        for(const char* C = InWaves; *C; ++C) { if(*C == '|') { Waves.emplace_back(); } else { Waves.back() += *C; } }
        OutWaves = int(Waves.size());
        std::vector<uint8_t> Out;
        if(InWave < 0 || InWave >= OutWaves) { return Out; }
        std::string Kind;
        const std::string Spec = Waves[size_t(InWave)] + ",";
        for(const char C : Spec)
        {
            if(C != ',') { Kind += C; continue; }
            const size_t Colon = Kind.find(':');
            const std::string Name = Kind.substr(0, Colon);
            const int Count = Colon == std::string::npos ? 1 : std::max(0, std::atoi(Kind.c_str() + Colon + 1));
            const uint8_t Archetype = LabBots::ArchetypeByName(Name.c_str());
            if(!Name.empty()) { for(int I = 0; I < Count; ++I) { Out.push_back(Archetype); } }
            Kind.clear();
        }
        return Out;
    }

    void SpawnWave(World& InWorld, const EntityID InEncounter)
    {
        LabEncounterComponent* Encounter = InWorld.GetEntity(InEncounter).GetComponent<LabEncounterComponent>();
        if(!Encounter) { return; }
        const LabEncounterComponent Data = *Encounter;

        /* Spawn points: children named Spawn_*, collected before spawning. */
        std::vector<TransformPose> Points;
        for(EntityID C = InWorld.GetEntity(InEncounter).GetFirstChild(); C.IsSet(); C = InWorld.GetEntity(C).GetNextSibling())
        {
            if(std::strncmp(InWorld.GetEntity(C).GetName(), "Spawn_", 6) == 0) { Points.push_back(InWorld.GetEntity(C).GetWorldPose()); }
        }
        if(Points.empty()) { return; }

        if(Data.Waves[0])
        {
            int Waves = 1;
            const std::vector<uint8_t> Kinds = ParseWave(Data.Waves, Data.Wave, Waves);
            int Index = 0;
            for(const uint8_t Kind : Kinds)
            {
                const TransformPose& At = Points[size_t(Index) % Points.size()];
                ++Index;
                LabBots::Spawn Bot;
                Bot.Archetype = Kind;
                Bot.Tree = LabBots::TreeFor(Kind);
                Bot.Team = Data.SpawnTeam;
                Bot.MaxHealth = Data.EnemyHealth;
                Bot.At.Position = At.Position + EngineCore::Vector3(double(Index / int(Points.size())) * 60.0, 0, 0);
                Bot.At.Rotation = At.Rotation;
                const EntityID Avatar = LabBots::SpawnBot(InWorld, Bot);
                if(!Avatar.IsSet()) { continue; }
                LabEncounterMemberComponent Member;
                std::snprintf(Member.Encounter, sizeof(Member.Encounter), "%s", Data.Id);
                InWorld.GetEntity(Avatar).AddComponent(Member);
            }
            CORE_LOG(std::string("[DemoLab] encounter '") + Data.Id + "' wave " + std::to_string(int(Data.Wave) + 1)
                     + " of " + std::to_string(Waves) + ": " + std::to_string(Kinds.size()) + " enemies")
            return;
        }

        int Index = 0;
        for(const TransformPose& At : Points)
        {
            char Name[48];
            std::snprintf(Name, sizeof(Name), "%s_%d", Data.Id, Index++);
            TransformPose Pose;
            Pose.Position = At.Position;
            Pose.Rotation = At.Rotation;
            PrefabInstance Spawned = InWorld.SpawnPrefab(Data.SpawnPrefab, Pose, Name);
            if(!Spawned.IsValid())
            {
                CORE_LOG_WARN(std::string("[DemoLab] encounter '") + Data.Id + "': cannot spawn " + Data.SpawnPrefab)
                continue;
            }
            LabEncounterMemberComponent Member;
            std::snprintf(Member.Encounter, sizeof(Member.Encounter), "%s", Data.Id);
            Spawned.GetRoot().AddComponent(Member);
        }

        CORE_LOG(std::string("[DemoLab] encounter '") + Data.Id + "' started, " + std::to_string(Points.size()) + " spawned")
    }

    void Start(World& InWorld, const EntityID InEncounter)
    {
        LabEncounterComponent* Encounter = InWorld.GetEntity(InEncounter).GetComponent<LabEncounterComponent>();
        if(!Encounter || Encounter->State != LabEncounters::STATE_WAITING) { return; }
        Encounter->State = LabEncounters::STATE_FIGHTING;
        Encounter->Wave = 0;
        Encounter->WaveTimer = -1.f;
        int Waves = 1;
        if(Encounter->Waves[0]) { ParseWave(Encounter->Waves, 0, Waves); }
        Encounter->WaveCount = uint8_t(std::max(1, Waves));
        const LabEncounterComponent Data = *Encounter;

        if(Data.LockChannel[0]) { LabSignals::Emit(InWorld, Data.LockChannel, true, InEncounter); }
        SpawnWave(InWorld, InEncounter);

        EventArgs Args;
        Args.Args[Args.Count++] = EventArg::Make(Data.Id);
        InWorld.GetEvents().Emit("Encounter.Started", Args);
    }
}

void LabEncounters::StartById(World& InWorld, const char* InId)
{
    if(!HasAuthority() || !InId || !InId[0]) { return; }
    std::vector<EntityID> Matches;
    InWorld.Each<LabEncounterComponent>([&](Entity InEntity, LabEncounterComponent& InEncounter)
    {
        if(std::strcmp(InEncounter.Id, InId) == 0) { Matches.push_back(InEntity.GetID()); }
    });
    for(const EntityID E : Matches) { Start(InWorld, E); }
}

void LabEncounters::OnSignal(World& InWorld, const char* InChannel, const bool bInValue)
{
    if(!HasAuthority() || !bInValue || !InChannel) { return; }
    std::vector<EntityID> Matches;
    std::vector<std::string> Stopped;
    InWorld.Each<LabEncounterComponent>([&](Entity InEntity, LabEncounterComponent& InEncounter)
    {
        if(InEncounter.StartOn[0] && std::strcmp(InEncounter.StartOn, InChannel) == 0) { Matches.push_back(InEntity.GetID()); }
        if(InEncounter.StopOn[0] && std::strcmp(InEncounter.StopOn, InChannel) == 0 && InEncounter.State == STATE_FIGHTING)
        {
            /* The last wave, already cleared: Tick finishes it. */
            InEncounter.Wave = uint8_t(InEncounter.WaveCount > 0 ? InEncounter.WaveCount - 1 : 0);
            Stopped.emplace_back(InEncounter.Id);
        }
    });
    for(const EntityID E : Matches) { Start(InWorld, E); }
    if(Stopped.empty()) { return; }
    std::vector<EntityID> Living;
    InWorld.Each<LabEncounterMemberComponent>([&](Entity InEntity, LabEncounterMemberComponent& InMember)
    {
        for(const std::string& Id : Stopped) { if(Id == InMember.Encounter) { Living.push_back(InEntity.GetID()); } }
    });
    for(const EntityID Member : Living)
    {
        LabDamageEvent Event;
        Event.Victim = Member;
        Event.DamageType = EDamageType::EDT_Explosion;
        Event.Dir = EngineCore::Vector3(0, 1, 0);
        Event.Impulse = 2000.f;
        Event.Point = InWorld.GetEntity(Member).GetWorldPose().Position;
        LabDamage::Kill(InWorld, Member, Event);
    }
    CORE_LOG("[DemoLab] encounter stopped by " + std::string(InChannel))
}

void LabEncounters::Tick(World& InWorld, const float InSeconds)
{
    if(!HasAuthority()) { return; }

    /* Encounters with no start signal fight from the first tick. */
    std::vector<EntityID> Immediate;
    InWorld.Each<LabEncounterComponent>([&](Entity InEntity, LabEncounterComponent& InEncounter)
    {
        if(InEncounter.State == STATE_WAITING && !InEncounter.StartOn[0]) { Immediate.push_back(InEntity.GetID()); }
    });
    for(const EntityID E : Immediate) { Start(InWorld, E); }

    struct Count { std::string Id; int Alive = 0; int Total = 0; };
    std::vector<Count> Counts;
    InWorld.Each<LabEncounterMemberComponent>([&](Entity InEntity, LabEncounterMemberComponent& InMember)
    {
        Count* Found = nullptr;
        for(Count& C : Counts) { if(C.Id == InMember.Encounter) { Found = &C; } }
        if(!Found) { Counts.push_back({ InMember.Encounter, 0, 0 }); Found = &Counts.back(); }
        ++Found->Total;
        const LabHealthComponent* Health = InEntity.GetComponent<LabHealthComponent>();
        if(Health && !Health->bDead) { ++Found->Alive; }
    });

    std::vector<LabEncounterComponent> Finished;
    std::vector<EntityID> NextWaves;
    InWorld.Each<LabEncounterComponent>([&](Entity InEntity, LabEncounterComponent& InEncounter)
    {
        if(InEncounter.State != STATE_FIGHTING) { return; }
        int Alive = 0, Total = 0;
        for(const Count& C : Counts) { if(C.Id == InEncounter.Id) { Alive = C.Alive; Total = C.Total; } }
        InEncounter.Alive = uint8_t(Alive);
        InEncounter.Total = uint8_t(Total);
        if(Alive != 0) { return; }
        if(InEncounter.Wave + 1 < InEncounter.WaveCount)
        {
            /* Cleared a wave: a breath, then the next. */
            if(InEncounter.WaveTimer < 0.f) { InEncounter.WaveTimer = InEncounter.WaveDelay; }
            InEncounter.WaveTimer -= InSeconds;
            if(InEncounter.WaveTimer <= 0.f)
            {
                ++InEncounter.Wave;
                InEncounter.WaveTimer = -1.f;
                NextWaves.push_back(InEntity.GetID());
            }
            return;
        }
        InEncounter.State = STATE_DONE;
        Finished.push_back(InEncounter);
    });
    for(const EntityID E : NextWaves) { SpawnWave(InWorld, E); }

    for(const LabEncounterComponent& Done : Finished)
    {
        CORE_LOG(std::string("[DemoLab] encounter '") + Done.Id + "' cleared")
        EventArgs Args;
        Args.Args[Args.Count++] = EventArg::Make(Done.Id);
        InWorld.GetEvents().Emit("Encounter.Done", Args);
        if(Done.LockChannel[0]) { LabSignals::Emit(InWorld, Done.LockChannel, false); }
        if(Done.DoneChannel[0]) { LabSignals::Emit(InWorld, Done.DoneChannel, true); }
    }
}

bool LabEncounters::GetWave(World& InWorld, int& OutWave, int& OutWaves)
{
    bool bFound = false;
    InWorld.Each<LabEncounterComponent>([&](Entity, LabEncounterComponent& InEncounter)
    {
        if(InEncounter.State == STATE_FIGHTING && !bFound)
        {
            bFound = true;
            OutWave = InEncounter.Wave + 1;
            OutWaves = InEncounter.WaveCount;
        }
    });
    return bFound;
}

void LabEncounters::RestartInterrupted(World& InWorld)
{
    if(!HasAuthority()) { return; }
    std::vector<std::string> Interrupted;
    std::vector<EntityID> Encounters;
    InWorld.Each<LabEncounterComponent>([&](Entity InEntity, LabEncounterComponent& InEncounter)
    {
        if(InEncounter.State != STATE_FIGHTING) { return; }
        InEncounter.State = STATE_WAITING;
        InEncounter.Wave = 0;
        InEncounter.WaveTimer = -1.f;
        Interrupted.push_back(InEncounter.Id);
        Encounters.push_back(InEntity.GetID());
    });
    if(Interrupted.empty()) { return; }
    /* Its members, and their bot players, go. */
    std::vector<EntityID> Doomed;
    InWorld.Each<LabEncounterMemberComponent>([&](Entity InEntity, LabEncounterMemberComponent& InMember)
    {
        for(const std::string& Id : Interrupted) { if(Id == InMember.Encounter) { Doomed.push_back(InEntity.GetID()); } }
    });
    for(const EntityID Member : Doomed)
    {
        if(const LabBotComponent* Bot = InWorld.GetEntity(Member).GetComponent<LabBotComponent>())
        {
            LabBots::ReleaseBotPlayer(InWorld, Bot->BotPlayer);
        }
        InWorld.DestroyEntity(Member);
    }
    for(const EntityID E : Encounters) { Start(InWorld, E); }
    CORE_LOG("[DemoLab] restarted " + std::to_string(Interrupted.size()) + " interrupted encounter(s)")
}

bool LabEncounters::GetActive(World& InWorld, int& OutAlive, int& OutTotal)
{
    bool bFound = false;
    InWorld.Each<LabEncounterComponent>([&](Entity, LabEncounterComponent& InEncounter)
    {
        if(InEncounter.State == STATE_FIGHTING && !bFound)
        {
            bFound = true;
            OutAlive = InEncounter.Alive;
            OutTotal = InEncounter.Total;
        }
    });
    return bFound;
}
