#ifndef GAMECORE_COMPONENTS_H
#define GAMECORE_COMPONENTS_H

#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

/*
 * The engine's own built-in components, defined the same way a game defines
 * its own -- GAMECORE_COMPONENT_BODY() first, GAMECORE_DEFINE_COMPONENT(...)
 * once in Components.cpp. Nothing here is special-cased by World or
 * ComponentPool. Every one is GAMECORE_API despite looking like plain POD --
 * see DESIGN.md for why StaticType() makes that exception never apply.
 */
namespace GameCore
{
    /* Position, rotation (Euler degrees) and scale. 64 bytes total (one
     * cache line) -- see DESIGN.md. */
    struct GAMECORE_API TransformComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;   // Euler degrees, applied Z * Y * X -- see DESIGN.md
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};

        /* Translation * (Rz * Ry * Rx) * Scale -- what AABB::Transformed
         * uses to turn BoundsComponent::Local into ::World every refit. */
        EngineCore::Matrix4x4 GetLocalMatrix() const;
    };

    /* A leaf in World's SpatialIndex -- see SpatialIndex.h. */
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

#endif //GAMECORE_COMPONENTS_H
