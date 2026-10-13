#include "Public/AICore/Perception.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "Private/AICore/NavInternal.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/PerceptionComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/* PerceptionWorld: sight, hearing, memory. Perception.h has the rules. */
namespace AICore
{
    namespace
    {
        /* The topmost parent: a prefab's parts are one actor. */
        GameCore::EntityID RootOf(GameCore::World& InWorld, GameCore::EntityID InEntity)
        {
            for(int Depth = 0; Depth < 32 && InWorld.IsAlive(InEntity); ++Depth)
            {
                const GameCore::EntityID Parent = InWorld.GetEntity(InEntity).GetParent();
                if(!Parent.IsSet()) { break; }
                InEntity = Parent;
            }
            return InEntity;
        }
    }

    PerceptionWorld::PerceptionWorld(GameCore::World& InWorld) : World(InWorld) {}
    PerceptionWorld::~PerceptionWorld() = default;

    void PerceptionWorld::ReportNoise(const EngineCore::Vector3& InPosition, const float InLoudness, const GameCore::EntityID InInstigator)
    {
        Noise N;
        N.Position = InPosition;
        N.Loudness = std::max(InLoudness, 0.0f);
        N.Instigator = InInstigator;
        Noises.push_back(N);
    }

    const std::vector<PerceivedStimulus>* PerceptionWorld::GetMemory(const GameCore::EntityID InAgent) const
    {
        const auto Found = Agents.find(InAgent.Pack());
        return Found != Agents.end() ? &Found->second.Memory : nullptr;
    }

    GameCore::EntityID PerceptionWorld::GetTarget(const GameCore::EntityID InAgent) const
    {
        const auto Found = Agents.find(InAgent.Pack());
        return Found != Agents.end() ? Found->second.Target : GameCore::EntityID();
    }

    bool PerceptionWorld::CanSee(const GameCore::EntityID InAgent, const GameCore::EntityID InTarget) const
    {
        const auto Found = Agents.find(InAgent.Pack());
        if(Found == Agents.end()) { return false; }
        for(const PerceivedStimulus& S : Found->second.Memory)
        {
            if(S.Entity == InTarget) { return S.bSeen != 0; }
        }
        return false;
    }

    uint32_t PerceptionWorld::TakeSightTestCount()
    {
        const uint32_t Count = SightTests;
        SightTests = 0;
        return Count;
    }

    void PerceptionWorld::SenseSight(const GameCore::EntityID InAgent, Agent& InOutAgent)
    {
        const PerceptionComponent* Self = World.GetEntity(InAgent).GetComponent<PerceptionComponent>();
        if(!Self) { return; }
        const GameCore::TransformPose Pose = World.GetEntity(InAgent).GetWorldPose();
        const EngineCore::Vector3 Eye(Pose.Position.GetX(), Pose.Position.GetY() + Self->EyeHeight, Pose.Position.GetZ());
        const double Yaw = double(Pose.Rotation.GetY()) * 3.14159265358979323846 / 180.0;
        const double ForwardX = std::sin(Yaw), ForwardZ = std::cos(Yaw);
        const double CosFov = std::cos(double(std::clamp(Self->FovHalfAngle, 0.0f, 180.0f)) * 3.14159265358979323846 / 180.0);
        const GameCore::EntityID SelfRoot = RootOf(World, InAgent);

        for(PerceivedStimulus& S : InOutAgent.Memory) { S.bSeen = 0; }

        struct Candidate { GameCore::EntityID Entity; EngineCore::Vector3 Feet; EngineCore::Vector3 Aim; uint8_t Team; };
        std::vector<Candidate> Candidates;
        World.Each<AIStimulusComponent>([&](const GameCore::Entity& InEntity, AIStimulusComponent& InStimulus)
        {
            if(!InStimulus.bEnabled || !IsHostile(Self->Team, InStimulus.Team)) { return; }
            const GameCore::EntityID Id = InEntity.GetID();
            if(Id == InAgent || RootOf(World, Id) == SelfRoot) { return; }
            const EngineCore::Vector3 Feet = InEntity.GetWorldPose().Position;
            Candidates.push_back({ Id, Feet, EngineCore::Vector3(Feet.GetX(), Feet.GetY() + InStimulus.TargetHeight, Feet.GetZ()), InStimulus.Team });
        });

        for(const Candidate& C : Candidates)
        {
            const double DX = double(C.Aim.GetX()) - Eye.GetX(), DY = double(C.Aim.GetY()) - Eye.GetY(), DZ = double(C.Aim.GetZ()) - Eye.GetZ();
            const double Distance = std::sqrt(DX * DX + DY * DY + DZ * DZ);
            if(Distance > double(Self->SightRange)) { continue; }
            const double Flat = std::sqrt(DX * DX + DZ * DZ);
            if(Flat > 1.0 && (DX * ForwardX + DZ * ForwardZ) / Flat < CosFov) { continue; }
            ++SightTests;
            if(Host.LineOfSight && !Host.LineOfSight(World, Eye, C.Aim, InAgent, C.Entity, Host.UserData)) { continue; }

            PerceivedStimulus* Entry = nullptr;
            for(PerceivedStimulus& S : InOutAgent.Memory) { if(S.Entity == C.Entity) { Entry = &S; break; } }
            if(!Entry)
            {
                InOutAgent.Memory.emplace_back();
                Entry = &InOutAgent.Memory.back();
                Entry->Entity = C.Entity;
            }
            Entry->LastKnownPosition = C.Feet;
            Entry->Strength = 1.0f;
            Entry->Age = 0.0f;
            Entry->bSeen = 1;
            Entry->bHeard = 0;
            Entry->Team = C.Team;
        }
    }

    void PerceptionWorld::Hear(const GameCore::EntityID InAgent, Agent& InOutAgent, const Noise& InNoise)
    {
        const PerceptionComponent* Self = World.GetEntity(InAgent).GetComponent<PerceptionComponent>();
        if(!Self || InNoise.Instigator == InAgent) { return; }
        const EngineCore::Vector3 At = World.GetEntity(InAgent).GetWorldPose().Position;
        if(NavDetail::Distance3D(At, InNoise.Position) > Self->HearingRange * InNoise.Loudness) { return; }

        InOutAgent.NoisePosition = InNoise.Position;
        InOutAgent.NoiseAge = 0.0f;
        if(!InNoise.Instigator.IsSet() || !World.IsAlive(InNoise.Instigator)) { return; }
        const AIStimulusComponent* Source = World.GetEntity(InNoise.Instigator).GetComponent<AIStimulusComponent>();
        if(!Source || !IsHostile(Self->Team, Source->Team)) { return; }

        for(PerceivedStimulus& S : InOutAgent.Memory)
        {
            if(S.Entity == InNoise.Instigator)
            {
                if(!S.bSeen) { S.LastKnownPosition = InNoise.Position; S.Age = 0.0f; }
                S.bHeard = 1;
                S.Strength = std::max(S.Strength, InNoise.Loudness);
                return;
            }
        }
        PerceivedStimulus Entry;
        Entry.Entity = InNoise.Instigator;
        Entry.LastKnownPosition = InNoise.Position;
        Entry.Strength = InNoise.Loudness;
        Entry.Age = 0.0f;
        Entry.bHeard = 1;
        Entry.Team = Source->Team;
        InOutAgent.Memory.push_back(Entry);
    }

    void PerceptionWorld::Choose(const GameCore::EntityID InAgent, Agent& InOutAgent)
    {
        const EngineCore::Vector3 At = World.GetEntity(InAgent).GetWorldPose().Position;
        const PerceivedStimulus* Best = nullptr;
        float BestDistance = 1e30f;
        for(const PerceivedStimulus& S : InOutAgent.Memory)
        {
            if(!S.bSeen) { continue; }
            const float D = NavDetail::Distance3D(At, S.LastKnownPosition);
            if(D < BestDistance) { BestDistance = D; Best = &S; }
        }
        if(!Best)
        {
            float Freshest = 1e30f;
            for(const PerceivedStimulus& S : InOutAgent.Memory)
            {
                if(S.Age < Freshest) { Freshest = S.Age; Best = &S; }
            }
        }
        InOutAgent.Target = Best ? Best->Entity : GameCore::EntityID();
        InOutAgent.bTargetVisible = Best && Best->bSeen ? 1 : 0;
    }

    void PerceptionWorld::WriteBlackboard(const GameCore::EntityID InAgent, const Agent& InAgentState)
    {
        AIWorld* AI = AIWorld::Find(&World);
        Blackboard* Board = AI ? AI->GetBlackboard(InAgent) : nullptr;
        if(!Board) { return; }

        const PerceivedStimulus* Target = nullptr;
        for(const PerceivedStimulus& S : InAgentState.Memory) { if(S.Entity == InAgentState.Target) { Target = &S; break; } }
        if(Target)
        {
            Board->SetEntity("TargetActor", Target->Entity);
            Board->SetBool("TargetVisible", Target->bSeen != 0);
            Board->SetVector("LastKnownPos", Target->LastKnownPosition);
            const EngineCore::Vector3 At = World.GetEntity(InAgent).GetWorldPose().Position;
            const EngineCore::Vector3 Where = Target->bSeen && World.IsAlive(Target->Entity)
                ? World.GetEntity(Target->Entity).GetWorldPose().Position : Target->LastKnownPosition;
            /* To a metre: a distance that changes every frame would wake
             * every observing decorator every frame. */
            Board->SetFloat("TargetDistance", std::round(NavDetail::Distance3D(At, Where) / 100.0f) * 100.0f);
        }
        else
        {
            Board->Clear("TargetActor");
            Board->SetBool("TargetVisible", false);
            Board->Clear("TargetDistance");
        }
        const PerceptionComponent* Self = World.GetEntity(InAgent).GetComponent<PerceptionComponent>();
        const bool bHeard = Self && InAgentState.NoiseAge <= Self->ForgetSeconds;
        Board->SetBool("HeardNoise", bHeard);
        if(bHeard) { Board->SetVector("NoisePos", InAgentState.NoisePosition); }
    }

    void PerceptionWorld::Tick(const float InDeltaSeconds)
    {
        std::vector<GameCore::EntityID> Ids;
        World.Each<PerceptionComponent>([&Ids](const GameCore::Entity& InEntity, PerceptionComponent&) { Ids.push_back(InEntity.GetID()); });

        std::unordered_set<uint64_t> Seen;
        for(const GameCore::EntityID Id : Ids)
        {
            const PerceptionComponent* Self = World.GetEntity(Id).GetComponent<PerceptionComponent>();
            if(!Self) { continue; }
            Seen.insert(Id.Pack());
            auto Inserted = Agents.try_emplace(Id.Pack());
            Agent& State = Inserted.first->second;
            if(Inserted.second)
            {
                /* Staggered: a crowd that spawns together does not all look
                 * in the same frame. */
                Stagger = std::fmod(Stagger + 0.037f, std::max(Self->SightInterval, 0.05f));
                State.NextSight = Stagger;
            }

            /* Age what it remembers; forget the old, the gone and the hidden. */
            State.NoiseAge += InDeltaSeconds;
            for(PerceivedStimulus& S : State.Memory) { S.Age += InDeltaSeconds; }
            State.Memory.erase(std::remove_if(State.Memory.begin(), State.Memory.end(), [&](const PerceivedStimulus& InS)
            {
                if(InS.Age > Self->ForgetSeconds) { return true; }
                if(!World.IsAlive(InS.Entity)) { return true; }
                const AIStimulusComponent* Stimulus = World.GetEntity(InS.Entity).GetComponent<AIStimulusComponent>();
                return !Stimulus || !Stimulus->bEnabled;
            }), State.Memory.end());

            if(!Self->bEnabled)
            {
                State.Memory.clear();
            }
            else
            {
                State.NextSight -= InDeltaSeconds;
                if(State.NextSight <= 0.0f)
                {
                    State.NextSight += std::max(Self->SightInterval, 0.02f);
                    if(State.NextSight <= 0.0f) { State.NextSight = Self->SightInterval; }
                    SenseSight(Id, State);
                }
                for(const Noise& N : Noises) { Hear(Id, State, N); }
            }
            Choose(Id, State);
            WriteBlackboard(Id, State);
        }
        Noises.clear();

        for(auto It = Agents.begin(); It != Agents.end();)
        {
            if(Seen.count(It->first)) { ++It; }
            else { It = Agents.erase(It); }
        }
    }
}
