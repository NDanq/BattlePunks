#ifndef GAMECORE_COMPONENTS_H
#define GAMECORE_COMPONENTS_H

#include <cstdint>
#include <cstring>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/TickPhase.h"
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

    /* A display/lookup name. World::FindEntityByName is a linear scan over
     * this pool, deliberately -- see DESIGN.md. */
    struct GAMECORE_API NameComponent
    {
        GAMECORE_COMPONENT_BODY()

        char Name[64] = {};
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

    /* The one component whose behaviour is DATA rather than code: a Lua
     * class instance bound to this entity -- see ScriptEngine.h and
     * ScriptSystem.cpp for the full lifecycle. */
    struct GAMECORE_API ScriptComponent
    {
        GAMECORE_COMPONENT_BODY()

        /* A luaL_ref into the Lua registry for this entity's instance table;
         * -2 is LUA_NOREF spelled as a literal -- lua.h never appears in
         * Public/. */
        int32_t InstanceRef = -2;

        char ClassName[48] = {};

        /* Read from the registered Lua class's TickPhase field at AddScript
         * time; TP_PrePhysics if unspecified. */
        uint8_t Phase = TP_PrePhysics;

        uint8_t bStarted = 0;
        uint8_t bErrored = 0;

        /* Only meaningful with a BoundsComponent on the same entity -- see
         * DESIGN.md. */
        uint8_t bCullOutsideActiveRegion = 0;
    };

    /* Defined here, after NameComponent exists. Both return "" rather than
     * null when there is no NameComponent, so a caller need not null-check
     * before formatting a log line. */
    inline const char* ComponentOwner::GetName() const
    {
        const NameComponent* Name = GetSibling<NameComponent>();
        return Name ? Name->Name : "";
    }

    inline const char* Entity::GetName() const
    {
        const NameComponent* Name = GetComponent<NameComponent>();
        return Name ? Name->Name : "";
    }
}

#endif //GAMECORE_COMPONENTS_H
