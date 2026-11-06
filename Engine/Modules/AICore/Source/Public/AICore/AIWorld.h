#ifndef AICORE_AIWORLD_H
#define AICORE_AIWORLD_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/BehaviorTree.h"
#include "Public/AICore/NavWorld.h"
#include "Public/AICore/Perception.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * A World's AI: its navigation (NavWorld), perception (PerceptionWorld) and
 * the brains its AI Brain components run. NimbleEngine's GameSession
 * attaches one to each level's World; a test or tool attaches its own.
 *
 * THE SYSTEMS, in tick order:
 *   AI.Brains       TP_PrePhysics: each running brain's tree, every
 *                   TickInterval seconds; a brain whose tree file changed
 *                   (BehaviorTreeAssetCache's generation) restarts
 *   AI.Nav          TP_PrePhysics, after: the crowd (NavWorld::Tick)
 *   AI.Perception   TP_PostPhysics: sight and hearing, then blackboards
 *
 * Game thread only.
 */
namespace AICore
{
    class AICORE_API AIWorld
    {
    public:
        /* InWorld's, made on the first Attach; each Attach needs a Detach. */
        static AIWorld* Attach(GameCore::World& InWorld);
        static void     Detach(GameCore::World& InWorld);

        /* Null if InWorld has none. */
        static AIWorld* Find(const GameCore::World* InWorld);

        GameCore::World& GetWorld() { return World; }
        NavWorld&        GetNav() { return Nav; }
        PerceptionWorld& GetPerception() { return Perception; }

        void SetHost(const AIHost& InHost) { Perception.SetHost(InHost); Nav.SetHost(InHost); }
        void SetTreeHost(const BTHost& InHost) { TreeHost = InHost; }
        const BTHost& GetTreeHost() const { return TreeHost; }

        // === Brains ========================================================

        /* InEntity's brain runs InTree (null: its component's). False if it
         * has no AI Brain, or the tree cannot be loaded. */
        bool StartBrain(GameCore::EntityID InEntity, const char* InTree = nullptr);
        void StopBrain(GameCore::EntityID InEntity);
        bool IsBrainRunning(GameCore::EntityID InEntity) const;

        /* The running brain's; null if it has none. */
        BTInstance* FindBrain(GameCore::EntityID InEntity);
        Blackboard* GetBlackboard(GameCore::EntityID InEntity);

        /* Visit every running brain (the debugger's list). */
        template<typename F>
        void ForEachBrain(F&& InFn)
        {
            for(auto& Pair : Brains) { if(Pair.second.Instance) { InFn(GameCore::EntityID::Unpack(Pair.first), *Pair.second.Instance); } }
        }

        /* The systems' work, by hand (tests): brains, nav, perception. */
        void TickBrains(float InDeltaSeconds);
        void TickNav(float InDeltaSeconds);
        void TickPerception(float InDeltaSeconds);

    private:
        explicit AIWorld(GameCore::World& InWorld);
        ~AIWorld();
        AIWorld(const AIWorld&)            = delete;
        AIWorld& operator=(const AIWorld&) = delete;

        static void BrainSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);
        static void NavSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);
        static void PerceptionSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);

        struct Brain
        {
            std::unique_ptr<BTInstance> Instance;
            uint64_t Generation = 0;
            float    Accumulated = 0.0f;
            bool     bStartedOnce = false;
            bool     bStopped = false;   // told to stop: no auto start again
        };

        GameCore::World& World;
        int32_t          Attachments = 0;
        NavWorld         Nav;
        PerceptionWorld  Perception;
        BTHost           TreeHost;
        uint32_t         NextSeed = 1;
        std::unordered_map<uint64_t, Brain> Brains;   // by EntityID::Pack()
    };

    /* A noise, for every AIWorld-attached World: what a gunshot calls. */
    AICORE_API void ReportNoise(GameCore::World& InWorld, const EngineCore::Vector3& InPosition, float InLoudness,
                                GameCore::EntityID InInstigator);
}

#endif //AICORE_AIWORLD_H
