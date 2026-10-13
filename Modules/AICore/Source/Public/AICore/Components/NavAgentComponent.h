#ifndef AICORE_NAVAGENTCOMPONENT_H
#define AICORE_NAVAGENTCOMPONENT_H

#include <cstdint>

#include "Public/AICore/AICoreAPI.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/Math/Vector.h"

namespace AICore
{
    /* Where an agent is with its move. */
    enum ENavAgentStatus : uint8_t
    {
        NAS_Idle    = 0,   // no target
        NAS_Moving  = 1,
        NAS_Arrived = 2,   // within AcceptanceRadius of the target
        NAS_Failed  = 3,   // no path, or off the navmesh
    };

    /*
     * "Nav Agent". Walks its entity across the navmesh as one of the level's
     * crowd (DetourCrowd): a path to its target, kept up as it goes, and
     * local avoidance of the other agents.
     *
     * MOVEMENT STAYS WITH THE GAME. Each frame (the "AI.Nav" system,
     * TP_PrePhysics) the agent reads where its entity is, and writes
     * DesiredVelocity -- where it wants to go, in world units a second. An
     * avatar turns that into input (NimbleEngine/AIFacade.h, SteerInput), so
     * bots move as players do and stay on the same prediction path. With
     * bDriveTransform on, the agent moves its entity itself instead: for
     * things with no character controller.
     *
     * A target is set from a behavior tree (MoveTo), Lua (AI.MoveTo) or C++
     * (NavWorld::MoveTo).
     */
    struct AICORE_API NavAgentComponent
    {
        GAMECORE_COMPONENT_BODY()

        float   Radius           = 35.0f;
        float   Height           = 180.0f;
        float   MaxSpeed         = 400.0f;
        float   MaxAcceleration  = 2000.0f;
        float   AcceptanceRadius = 60.0f;
        float   SeparationWeight = 2.0f;
        /* The yaw its mesh adds to its facing: 180 for a mesh modelled facing
         * -Z. Facing is yaw 0 down +Z; the entity's Rotation.Y is facing plus
         * this. */
        float   MeshYaw          = 0.0f;
        float   TurnRate         = 540.0f;   // degrees a second, turning a driven agent
        uint8_t AvoidanceQuality = 2;   // 0 (none) .. 3 (best)
        uint8_t bDriveTransform  = 0;

        // === Engine-written: never saved ====================================
        uint8_t Status     = NAS_Idle;  // ENavAgentStatus
        uint8_t bHasTarget = 0;
        uint8_t bHasFacing = 0;         // FaceTarget is set (a tree's FaceTarget task)
        uint8_t Reserved0  = 0;
        int16_t CrowdIndex = -1;
        int16_t Reserved1  = 0;
        EngineCore::Vector3 Target;
        EngineCore::Vector3 DesiredVelocity;
        EngineCore::Vector3 FaceTarget;  // where the game should turn it to look
        float   RemainingDistance = 0.0f;
    };
}

namespace GameCore { class Entity; }

namespace AICore
{
    /* Where InEntity faces, in degrees (0 down +Z): its world yaw, less its
     * Nav Agent's MeshYaw if it has one. What sight cones and FaceTarget use. */
    AICORE_API float GetFacingYaw(const GameCore::Entity& InEntity);
}

#endif //AICORE_NAVAGENTCOMPONENT_H
