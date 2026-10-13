#ifndef AICORE_PERCEPTION_H
#define AICORE_PERCEPTION_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * What AI agents see and hear.
 *
 * SIGHT. A "TP_PostPhysics" system ("AI.Perception") checks each Perception
 * component every SightInterval seconds, staggered so a crowd does not all
 * look in one frame: every enabled AI Stimulus of a hostile team (teams
 * differ, neither 0) within SightRange and FovHalfAngle of its facing (the
 * entity's yaw), from its eye (EyeHeight up) to the stimulus' TargetHeight,
 * then a line-of-sight test through the host (AIHost::LineOfSight; the
 * engine's runs a physics raycast). AICore never links physics.
 *
 * HEARING. ReportNoise(World, where, loudness, instigator): every Perception
 * within HearingRange * loudness hears it on the next pass -- a gunshot is
 * loudness 1, a footstep 0.2.
 *
 * MEMORY. Each agent remembers what it perceived -- the entity, where it was
 * last seen or heard, how strongly and how long ago -- for ForgetSeconds.
 * After each pass it writes its brain's blackboard, for whichever of these
 * keys the tree declares:
 *
 *   TargetActor     Entity   the nearest hostile seen, else the freshest remembered
 *   TargetVisible   Bool     TargetActor is in sight now
 *   TargetDistance  Float    to TargetActor (or its last known position)
 *   LastKnownPos    Vector   where TargetActor was last seen or heard
 *   HeardNoise      Bool     a noise in the last ForgetSeconds
 *   NoisePos        Vector   the latest noise heard
 */
namespace AICore
{
    struct AIHost
    {
        /* Whether InViewer can see InTarget along InFrom -> InTo (the eye to
         * the target's chest). Null: everything in range and cone is seen. */
        bool (*LineOfSight)(GameCore::World& InWorld, const EngineCore::Vector3& InFrom, const EngineCore::Vector3& InTo,
                            GameCore::EntityID InViewer, GameCore::EntityID InTarget, void* InUserData) = nullptr;
        /* The ground under InAt (the highest surface below it, within a
         * couple of metres): where an agent that moves its own transform
         * (Nav Agent, Drive Transform) stands. The navmesh's own heights are
         * approximate -- its polygons carry no detail mesh -- so without one
         * a driven agent walks on them. False: no ground found. */
        bool (*GroundHeight)(GameCore::World& InWorld, const EngineCore::Vector3& InAt, float& OutHeight, void* InUserData) = nullptr;
        void* UserData = nullptr;
    };

    struct PerceivedStimulus
    {
        GameCore::EntityID  Entity;              // unset: a noise with no instigator
        EngineCore::Vector3 LastKnownPosition;
        float   Strength = 0.0f;                 // 1 seen, a noise's loudness heard
        float   Age      = 0.0f;                 // seconds since last perceived
        uint8_t bSeen    = 0;                    // in sight at the last pass
        uint8_t bHeard   = 0;                    // heard since it was last seen
        uint8_t Team     = 0;
    };

    class AICORE_API PerceptionWorld
    {
    public:
        explicit PerceptionWorld(GameCore::World& InWorld);
        ~PerceptionWorld();
        PerceptionWorld(const PerceptionWorld&)            = delete;
        PerceptionWorld& operator=(const PerceptionWorld&) = delete;

        void SetHost(const AIHost& InHost) { Host = InHost; }
        const AIHost& GetHost() const { return Host; }

        /* A noise, heard on the next pass. */
        void ReportNoise(const EngineCore::Vector3& InPosition, float InLoudness, GameCore::EntityID InInstigator);

        /* What InAgent remembers; null if it has no Perception. */
        const std::vector<PerceivedStimulus>* GetMemory(GameCore::EntityID InAgent) const;
        /* Its pick (TargetActor), unset if none. */
        GameCore::EntityID GetTarget(GameCore::EntityID InAgent) const;
        bool               CanSee(GameCore::EntityID InAgent, GameCore::EntityID InTarget) const;

        /* Sight tests run since the last call (the stats). */
        uint32_t TakeSightTestCount();

        /* The system's pass. */
        void Tick(float InDeltaSeconds);

        /* Whether InA and InB are hostile teams. */
        static bool IsHostile(uint8_t InA, uint8_t InB) { return InA != 0 && InB != 0 && InA != InB; }

    private:
        struct Agent
        {
            std::vector<PerceivedStimulus> Memory;
            float NextSight = 0.0f;
            GameCore::EntityID Target;
            uint8_t bTargetVisible = 0;
            EngineCore::Vector3 NoisePosition;
            float NoiseAge = 1e9f;
        };
        struct Noise
        {
            EngineCore::Vector3 Position;
            float Loudness = 1.0f;
            GameCore::EntityID Instigator;
        };

        void SenseSight(GameCore::EntityID InAgent, Agent& InOutAgent);
        void Hear(GameCore::EntityID InAgent, Agent& InOutAgent, const Noise& InNoise);
        void Choose(GameCore::EntityID InAgent, Agent& InOutAgent);
        void WriteBlackboard(GameCore::EntityID InAgent, const Agent& InState);

        GameCore::World& World;
        AIHost Host;
        std::unordered_map<uint64_t, Agent> Agents;   // by EntityID::Pack()
        std::vector<Noise> Noises;
        uint32_t SightTests = 0;
        float    Stagger = 0.0f;
    };
}

#endif //AICORE_PERCEPTION_H
