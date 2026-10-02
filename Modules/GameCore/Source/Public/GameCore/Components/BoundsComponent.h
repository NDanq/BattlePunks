#ifndef GAMECORE_BOUNDSCOMPONENT_H
#define GAMECORE_BOUNDSCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /* "Bounds". A leaf in World's SpatialIndex -- see SpatialIndex.h. */
    struct GAMECORE_API BoundsComponent
    {
        GAMECORE_COMPONENT_BODY()

        AABB Local;    // entity space; the refit pass transforms this
        AABB World;    // world space, as of the last refit -- read only

        /* This entity's leaf in World::GetSpatialIndex(), or -1 until the
         * first refit inserts it. */
        int32_t ProxyID = -1;

        /* Never re-fit after insertion -- for level geometry that never moves. */
        uint8_t bStatic = 0;
    };
}

#endif //GAMECORE_BOUNDSCOMPONENT_H
