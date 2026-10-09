#ifndef NIMBLEENGINE_TERRAINFACADE_H
#define NIMBLEENGINE_TERRAINFACADE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * Ground under a game's feet: the World's Terrain components
 * (RenderCore's TerrainComponent, GameCore's Terrain.h) queried directly --
 * no physics needed, so walking on terrain works with JoltPhysics compiled
 * out (the default). With physics on, a terrain with a Static Rigid Body is
 * also a Jolt heightfield on exactly the same triangles.
 *
 * Only terrains with Walkable set count. Each terrain's Transform places it
 * (rotation and scale included). The "Terrain" script library is this:
 *
 *     local hit, y, nx, ny, nz = Terrain.SampleGround(x, y, z)
 *     local hit, entity, x, y, z = Terrain.Raycast(ox, oy, oz, dx, dy, dz, 5000)
 *
 * Game thread (or any thread not racing an editor brush).
 */
namespace NimbleEngine
{
    struct TerrainRaycastHit
    {
        GameCore::EntityID  Entity;
        float               Distance = 0.0f;   // world units along the ray
        EngineCore::Vector3 Point;
        EngineCore::Vector3 Normal;
    };

    /* The nearest walkable terrain hit along InDirection (any length) within InMaxDistance. */
    NE_API bool RaycastTerrain(GameCore::World& InWorld, const EngineCore::Vector3& InOrigin,
                               const EngineCore::Vector3& InDirection, float InMaxDistance,
                               TerrainRaycastHit& OutHit);

    /*
     * The terrain surface straight below InPosition, looking from InSearchAbove
     * units above it (so a player slightly under the ground is lifted back),
     * down to any depth. False if no walkable terrain is under that point.
     */
    NE_API bool SampleGround(GameCore::World& InWorld, const EngineCore::Vector3& InPosition,
                             float& OutHeight, EngineCore::Vector3* OutNormal = nullptr,
                             float InSearchAbove = 100000.0f);

    /* GameCore's TerrainProvider (the physics module's heightfields), from
     * Terrain components. Installed when this image loads; GameSession calls
     * it again, as it does InstallCollisionMeshProvider. */
    NE_API void InstallTerrainProvider();
}

#endif //NIMBLEENGINE_TERRAINFACADE_H
