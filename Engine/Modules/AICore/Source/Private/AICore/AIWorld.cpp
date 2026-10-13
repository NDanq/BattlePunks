#include "Public/AICore/AIWorld.h"

#include <cstdio>
#include <unordered_set>

#include "Public/AICore/BehaviorTreeFile.h"
#include "Public/AICore/Components/AIBrainComponent.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/* AIWorld: a World's navigation, perception and brains. AIWorld.h has the
 * systems and their order. */
namespace AICore
{
    namespace
    {
        std::unordered_map<const GameCore::World*, AIWorld*>& Systems()
        {
            static std::unordered_map<const GameCore::World*, AIWorld*> Map;
            return Map;
        }
    }

    AIWorld* AIWorld::Attach(GameCore::World& InWorld)
    {
        AIWorld*& System = Systems()[&InWorld];
        if(!System)
        {
            System = new AIWorld(InWorld);
            InWorld.RegisterSystem("AI.Brains", GameCore::TP_PrePhysics, &AIWorld::BrainSystem, System);
            InWorld.RegisterSystem("AI.Nav", GameCore::TP_PrePhysics, &AIWorld::NavSystem, System);
            InWorld.RegisterSystem("AI.Perception", GameCore::TP_PostPhysics, &AIWorld::PerceptionSystem, System);
        }
        ++System->Attachments;
        return System;
    }

    void AIWorld::Detach(GameCore::World& InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(&InWorld);
        if(Found == Map.end()) { return; }
        AIWorld* System = Found->second;
        if(--System->Attachments > 0) { return; }
        InWorld.UnregisterAllSystems(System);
        Map.erase(Found);
        delete System;
    }

    AIWorld* AIWorld::Find(const GameCore::World* InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(InWorld);
        return Found != Map.end() ? Found->second : nullptr;
    }

    AIWorld::AIWorld(GameCore::World& InWorld) : World(InWorld), Nav(InWorld), Perception(InWorld) {}

    AIWorld::~AIWorld()
    {
        /* Brains first: their tasks let go of agents in the crowd. */
        Brains.clear();
    }

    void AIWorld::BrainSystem(GameCore::World&, const float InDeltaSeconds, void* InUserData)
    {
        static_cast<AIWorld*>(InUserData)->TickBrains(InDeltaSeconds);
    }

    void AIWorld::NavSystem(GameCore::World&, const float InDeltaSeconds, void* InUserData)
    {
        static_cast<AIWorld*>(InUserData)->TickNav(InDeltaSeconds);
    }

    void AIWorld::PerceptionSystem(GameCore::World&, const float InDeltaSeconds, void* InUserData)
    {
        static_cast<AIWorld*>(InUserData)->TickPerception(InDeltaSeconds);
    }

    // === Brains ===============================================================

    bool AIWorld::StartBrain(const GameCore::EntityID InEntity, const char* InTree)
    {
        AIBrainComponent* Component = World.GetEntity(InEntity).GetComponent<AIBrainComponent>();
        if(!Component) { return false; }
        const std::string Path = InTree && InTree[0] ? InTree : Component->Tree;
        if(Path.empty()) { return false; }
        std::shared_ptr<const BehaviorTreeAsset> Asset = BehaviorTreeAssetCache::Find(Path);
        if(!Asset) { return false; }

        Brain& B = Brains[InEntity.Pack()];
        B.Instance.reset();
        B.Instance = std::make_unique<BTInstance>(Asset, World, InEntity, this, (NextSeed++) * 2654435761u + 1u);
        B.Instance->GetBlackboard().ApplyAssignments(Component->Blackboard);
        B.Generation = BehaviorTreeAssetCache::GetGeneration();
        B.Accumulated = 0.0f;
        B.bStartedOnce = true;
        B.bStopped = false;
        if(InTree && InTree[0] && std::string(Component->Tree) != Path)
        {
            std::snprintf(Component->Tree, sizeof(Component->Tree), "%s", Path.c_str());
        }
        Component->bRunning = 1;
        return true;
    }

    void AIWorld::StopBrain(const GameCore::EntityID InEntity)
    {
        const auto Found = Brains.find(InEntity.Pack());
        if(Found == Brains.end()) { return; }
        Found->second.Instance.reset();
        Found->second.bStopped = true;
        if(AIBrainComponent* Component = World.GetEntity(InEntity).GetComponent<AIBrainComponent>()) { Component->bRunning = 0; }
    }

    bool AIWorld::IsBrainRunning(const GameCore::EntityID InEntity) const
    {
        const auto Found = Brains.find(InEntity.Pack());
        return Found != Brains.end() && Found->second.Instance != nullptr;
    }

    BTInstance* AIWorld::FindBrain(const GameCore::EntityID InEntity)
    {
        const auto Found = Brains.find(InEntity.Pack());
        return Found != Brains.end() ? Found->second.Instance.get() : nullptr;
    }

    Blackboard* AIWorld::GetBlackboard(const GameCore::EntityID InEntity)
    {
        BTInstance* Instance = FindBrain(InEntity);
        return Instance ? &Instance->GetBlackboard() : nullptr;
    }

    void AIWorld::TickBrains(const float InDeltaSeconds)
    {
        /* By id first: a tree's tasks may create and destroy entities. */
        std::vector<GameCore::EntityID> Ids;
        World.Each<AIBrainComponent>([&Ids](const GameCore::Entity& InEntity, AIBrainComponent&) { Ids.push_back(InEntity.GetID()); });

        const uint64_t Generation = BehaviorTreeAssetCache::GetGeneration();
        std::unordered_set<uint64_t> Seen;
        for(const GameCore::EntityID Id : Ids)
        {
            AIBrainComponent* Component = World.GetEntity(Id).GetComponent<AIBrainComponent>();
            if(!Component) { continue; }
            Seen.insert(Id.Pack());
            Brain& B = Brains[Id.Pack()];
            if(!B.Instance)
            {
                if(B.bStopped || B.bStartedOnce || !Component->bAutoStart || Component->Tree[0] == '\0') { continue; }
                B.bStartedOnce = true;
                if(!StartBrain(Id)) { continue; }
            }
            else if(B.Generation != Generation)
            {
                /* The tree was saved (or the cache cleared): start afresh. */
                if(!StartBrain(Id)) { continue; }
            }

            Brain& Running = Brains[Id.Pack()];
            Running.Accumulated += InDeltaSeconds;
            if(Running.Accumulated < Component->TickInterval) { continue; }
            const float Step = Running.Accumulated;
            Running.Accumulated = 0.0f;
            Running.Instance->Tick(Step);
        }

        for(auto It = Brains.begin(); It != Brains.end();)
        {
            if(Seen.count(It->first)) { ++It; }
            else { It = Brains.erase(It); }
        }
    }

    void AIWorld::TickNav(const float InDeltaSeconds)
    {
        Nav.Tick(InDeltaSeconds);
    }

    void AIWorld::TickPerception(const float InDeltaSeconds)
    {
        Perception.Tick(InDeltaSeconds);
    }

    void ReportNoise(GameCore::World& InWorld, const EngineCore::Vector3& InPosition, const float InLoudness, const GameCore::EntityID InInstigator)
    {
        if(AIWorld* AI = AIWorld::Find(&InWorld)) { AI->GetPerception().ReportNoise(InPosition, InLoudness, InInstigator); }
    }
}
