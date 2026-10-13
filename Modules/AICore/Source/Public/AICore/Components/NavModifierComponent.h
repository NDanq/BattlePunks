#ifndef AICORE_NAVMODIFIERCOMPONENT_H
#define AICORE_NAVMODIFIERCOMPONENT_H

#include <cstdint>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/NavTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/Math/Vector.h"

namespace AICore
{
    /* What a Nav Modifier makes of the navmesh inside it. */
    enum ENavModifierArea : uint8_t
    {
        NMA_Blocked  = 0,   // a hole: nothing walks there
        NMA_HighCost = 1,   // walkable, Cost times dearer: paths go round
        NMA_Custom   = 2,   // CustomArea (2..62), at Cost
    };

    /*
     * "Nav Modifier". A box (HalfExtents about the entity, turned with its
     * yaw) that marks the navmesh under it when it is built. Moving one needs
     * a rebuild of the tiles under it (the editor does it with Rebuild On
     * Change); for something that moves at runtime use a Nav Obstacle.
     */
    struct AICORE_API NavModifierComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 HalfExtents{ 200.0, 100.0, 200.0 };
        uint8_t Area       = NMA_HighCost;   // ENavModifierArea
        uint8_t CustomArea = NAV_AREA_CUSTOM_MIN;
        float   Cost       = NAV_HIGH_COST_DEFAULT;

        /* The area id it marks with (NAV_AREA_*). */
        uint8_t GetAreaId() const;

        const char* GetAreaName() const;
        void        SetAreaName(const char* InName);
    };
}

#endif //AICORE_NAVMODIFIERCOMPONENT_H
