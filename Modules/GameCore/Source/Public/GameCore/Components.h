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
 * ComponentPool; a game's component registers into the exact same table these
 * four do.
 */
namespace GameCore
{
    /*
     * Position, rotation (Euler degrees) and scale. GAMECORE_API because
     * GetLocalMatrix() is an out-of-line member -- the one thing in this file
     * that is not header-only POD, so it is the one struct here annotated (see
     * GameCoreAPI.h's rule: annotate types with out-of-line members, not
     * header-only ones).
     *
     * At 64 bytes with the 16-byte ComponentOwner header (three Vector3 at 16
     * bytes each under WITH_SIMD, per Math/Vector.h's ABI guard), this is
     * exactly one cache line.
     */
    struct GAMECORE_API TransformComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;   // Euler degrees, applied Z * Y * X
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};

        /* Translation * (Rz * Ry * Rx) * Scale, via Matrix4x4's own
         * InitTranslation/InitRotation/InitScale -- reused, not reimplemented.
         * This is what AABB::Transformed uses to turn BoundsComponent::Local
         * into ::World every refit (see SpatialSystem.cpp). */
        EngineCore::Matrix4x4 GetLocalMatrix() const;
    };

    /*
     * A display/lookup name.
     *
     * GAMECORE_API DESPITE LOOKING LIKE HEADER-ONLY POD -- every component
     * using GAMECORE_COMPONENT_BODY() gets an out-of-line StaticType(), always
     * (that out-of-line function, defined once by GAMECORE_DEFINE_COMPONENT in
     * Components.cpp, is the whole registration mechanism -- see
     * ComponentType.h). GameCoreAPI.h's "do not annotate a header-only type"
     * rule is about a type with NOTHING out-of-line; StaticType() means that
     * exception never actually applies to a component struct, whatever its
     * data members look like. Without this, MSVC's dllexport model does not
     * export StaticType() from GameCore.dll and every OTHER translation unit
     * that names NameComponent::StaticType() fails to link against it -- which
     * is exactly what happened here before this was added; TransformComponent
     * happened to need GAMECORE_API anyway for GetLocalMatrix() and so never
     * exposed the gap.
     *
     * World::FindEntityByName is a LINEAR SCAN over this pool, deliberately --
     * the same call CoreConfig.h's ConfigVar list and EventBus's name table
     * make: an engine this size has, at most, a few thousand named entities,
     * and adding a hash index here is solving a problem that has not been
     * measured to exist. If it ever is, the index goes beside the pool, not
     * inside this struct.
     */
    struct GAMECORE_API NameComponent
    {
        GAMECORE_COMPONENT_BODY()

        char Name[64] = {};
    };

    /*
     * A leaf in World's SpatialIndex. See SpatialIndex.h's "what is in the tree
     * and what is not" and SpatialSystem.cpp's refit pass for how Local becomes
     * World every frame. GAMECORE_API for the reason given on NameComponent
     * above -- StaticType() is out-of-line regardless of how plain the rest of
     * the struct looks.
     */
    struct GAMECORE_API BoundsComponent
    {
        GAMECORE_COMPONENT_BODY()

        AABB Local;    // entity space; the refit pass transforms this
        AABB World;    // world space, as of the last refit -- READ ONLY, the pass owns writing this

        /* This entity's leaf in World::GetSpatialIndex(), or -1 until the
         * first refit inserts it (a Bounds added mid-frame may not have its
         * Transform yet -- see SpatialSystem.cpp's proxy-lifecycle note). */
        int32_t ProxyID = -1;

        /* Never re-fit after insertion -- for level geometry that never moves.
         * The refit pass skips a static Bounds entirely, Move() is never
         * called for it again after its first Insert. */
        uint8_t bStatic = 0;
    };

    /*
     * The one component whose behaviour is DATA rather than code: a Lua class
     * instance bound to this entity. See ScriptEngine.h and ScriptSystem.cpp
     * for the full lifecycle (Script.Register, AddScript, the per-phase tick,
     * the removal observer). GAMECORE_API for the same StaticType() reason
     * given on NameComponent above.
     */
    struct GAMECORE_API ScriptComponent
    {
        GAMECORE_COMPONENT_BODY()

        /* A luaL_ref into the Lua registry for this entity's instance table;
         * -2 is LUA_NOREF, spelled as a literal rather than pulling lua.h into
         * a public header (see GameCoreInternal.h's note on why lua.h never
         * appears in Public/). */
        int32_t InstanceRef = -2;

        char ClassName[48] = {};

        /* Read from the registered Lua class table's TickPhase field once, at
         * AddScript time; TP_PrePhysics if the class did not specify one. */
        uint8_t Phase = TP_PrePhysics;

        uint8_t bStarted = 0;
        uint8_t bErrored = 0;

        /* Read from the class table's CullOutsideActiveRegion field. Only
         * meaningful if this entity also has a BoundsComponent -- an entity
         * with no Bounds is never in the tree and so is never "outside" any
         * region; the per-phase tick ticks it regardless of this flag. */
        uint8_t bCullOutsideActiveRegion = 0;
    };

    // ============================================================================
    // ComponentOwner::GetName() and Entity::GetName() -- defined here, after
    // NameComponent exists. Both read a sibling/self NameComponent and return
    // "" rather than null when there is none, so a caller need not null-check
    // before formatting a log line -- the common case for this accessor.
    // ============================================================================

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
