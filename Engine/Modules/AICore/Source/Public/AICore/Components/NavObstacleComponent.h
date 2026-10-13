#ifndef AICORE_NAVOBSTACLECOMPONENT_H
#define AICORE_NAVOBSTACLECOMPONENT_H

#include <cstdint>

#include "Public/AICore/AICoreAPI.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/Math/Vector.h"

namespace AICore
{
    enum ENavObstacleShape : uint8_t
    {
        NOS_Box      = 0,   // HalfExtents, turned with the entity's yaw
        NOS_Cylinder = 1,   // HalfExtents.X the radius, .Y half the height
    };

    /*
     * "Nav Obstacle". Cuts the navmesh at runtime while enabled -- a closed
     * door, a dropped crate -- through DetourTileCache: the tiles it touches
     * rebuild over the next frames, and agents re-plan round it. Follows the
     * entity when it moves more than a few centimetres.
     */
    struct AICORE_API NavObstacleComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 HalfExtents{ 50.0, 100.0, 50.0 };
        uint8_t Shape    = NOS_Box;   // ENavObstacleShape
        uint8_t bEnabled = 1;

        // === Engine-written: never saved ====================================
        uint32_t Handle = 0;          // the tile cache's obstacle, 0 for none
        EngineCore::Vector3 PlacedAt; // where it was cut
        float    PlacedYaw = 0.0f;
    };
}

#endif //AICORE_NAVOBSTACLECOMPONENT_H
