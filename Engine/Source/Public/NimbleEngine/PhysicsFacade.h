#ifndef NIMBLEENGINE_PHYSICSFACADE_H
#define NIMBLEENGINE_PHYSICSFACADE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * What a game asks of physics, without JoltPhysics' headers. Always compiled:
 * with the module compiled out (NIMBLE_WITH_JOLTPHYSICS=OFF, the default) or
 * a World without physics, IsPhysicsAvailable() is false and PhysicsRaycast
 * tests the World's Bounds instead (World::Raycast), with no normal, and
 * walkable terrain (TerrainFacade.h), with one -- so game code needs no #if.
 *
 * Bodies themselves are GameCore's components: a RigidBodyComponent to
 * simulate an entity, its AddImpulse / AddForce / SetLinearVelocity to push
 * it, and the World events ContactBegin / ContactEnd / SensorEnter /
 * SensorExit (Self, Other) for what touched it (Engine/Modules/JoltPhysics/README.md).
 *
 * The "Physics" script library is this, for Lua and script graphs:
 *
 *     local hit, entity, x, y, z = Physics.Raycast(ox, oy, oz, dx, dy, dz, 5000)
 *     local hit, nx, ny, nz = Physics.RaycastNormal(ox, oy, oz, dx, dy, dz, 5000)
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    struct PhysicsRaycastHit
    {
        GameCore::EntityID  Entity;
        float               Distance = 0.0f;   // world units
        EngineCore::Vector3 Point;
        EngineCore::Vector3 Normal;            // zero without physics
    };

    /* False when JoltPhysics is compiled out. */
    NE_API bool IsPhysicsAvailable();

    /* InWorld is simulated (a running level's, or one a tool attached). */
    NE_API bool IsWorldSimulated(const GameCore::World& InWorld);

    /*
     * A tool's own World simulated from now on, as GameSession does a
     * level's: the editor's prefab Simulate. Its bodies step in TP_Physics
     * whenever the World ticks, until StopSimulating -- which must come
     * before the World is destroyed. False (and nothing done) without the
     * module, or if InWorld is already simulated.
     */
    NE_API bool StartSimulating(GameCore::World& InWorld);
    NE_API void StopSimulating(GameCore::World& InWorld);

    /* The nearest hit along InDirection (any length) within InMaxDistance:
     * bodies whose Layer is in InMask (sensors skipped, and InIgnore's own
     * bodies -- its ragdoll's parts too -- when set) when InWorld is
     * simulated, else Bounds. */
    NE_API bool PhysicsRaycast(GameCore::World& InWorld, const EngineCore::Vector3& InOrigin,
                               const EngineCore::Vector3& InDirection, float InMaxDistance,
                               PhysicsRaycastHit& OutHit, int32_t InMask = -1,
                               GameCore::EntityID InIgnore = GameCore::EntityID());
}

#endif //NIMBLEENGINE_PHYSICSFACADE_H
