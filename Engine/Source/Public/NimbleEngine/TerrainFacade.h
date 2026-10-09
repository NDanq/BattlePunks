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
 *     local onGround, x, y, z = Terrain.MoveOnGround(fx, fy, fz, tx, ty, tz, rideHeight)
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

    /* The nearest walkable terrain hit along InDirection (any length) within
     * InMaxDistance; every visible terrain when !bInWalkableOnly (editor picking). */
    NE_API bool RaycastTerrain(GameCore::World& InWorld, const EngineCore::Vector3& InOrigin,
                               const EngineCore::Vector3& InDirection, float InMaxDistance,
                               TerrainRaycastHit& OutHit, bool bInWalkableOnly = true);

    /*
     * The terrain surface straight below InPosition, looking from InSearchAbove
     * units above it (so a player slightly under the ground is lifted back),
     * down to any depth. False if no walkable terrain is under that point.
     */
    NE_API bool SampleGround(GameCore::World& InWorld, const EngineCore::Vector3& InPosition,
                             float& OutHeight, EngineCore::Vector3* OutNormal = nullptr,
                             float InSearchAbove = 100000.0f);

    /* How MoveOnGround walks. */
    struct GroundMoveSettings
    {
        float RideHeight      = 0.0f;    // the position rides this far above the ground (an eye height; 0 for feet)
        float MaxSlopeDegrees = 50.0f;   // steeper ground than this cannot be walked up
        float StepHeight      = 40.0f;   // a rise this small is walked up whatever its slope (a ledge, a kerb)
    };

    /*
     * A walk from InFrom to InOutTo (positions riding RideHeight above the
     * ground; only the move's X and Z count) over walkable terrain: InOutTo's
     * Y becomes the ground under it plus RideHeight. A move up onto ground
     * steeper than MaxSlopeDegrees, rising more than StepHeight, slides along
     * the slope instead, or stops. Downhill is never blocked. Deterministic,
     * so an avatar simulation can call it on the server and when predicting.
     *
     * False, InOutTo untouched, when there is no walkable terrain under it --
     * so a game's own floor rules still apply off the terrain. OutNormal: the
     * ground's, if not null.
     */
    NE_API bool MoveOnGround(GameCore::World& InWorld, const EngineCore::Vector3& InFrom, EngineCore::Vector3& InOutTo,
                             const GroundMoveSettings& InSettings = GroundMoveSettings(),
                             EngineCore::Vector3* OutNormal = nullptr);

    /* GameCore's TerrainProvider (the physics module's heightfields), from
     * Terrain components. Installed when this image loads; GameSession calls
     * it again, as it does InstallCollisionMeshProvider. */
    NE_API void InstallTerrainProvider();
}

#endif //NIMBLEENGINE_TERRAINFACADE_H
