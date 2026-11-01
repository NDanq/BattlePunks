#ifndef DEMOLAB_LABFOOTSTEPS_H
#define DEMOLAB_LABFOOTSTEPS_H

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include <cstdint>

namespace GameCore { class World; }

/*
 * Footsteps by surface (Docs/Design.md): every character on its feet -- the
 * player, Watchmen, Brutes -- plays a step each stride (the distance its
 * Character Mover carries it: longer running, longer for the 1.25x Brute),
 * picked by what is under the foot:
 *
 *   Metal   catwalks, gratings, gantries, steel plate      Step_MetalGrate_01..04
 *   Tile    marble and tiled floors (the atrium, L2's halls) Step_Tile_01..04
 *   Stone   concrete, stone blocks, rock, roads, the default Step_Stone_01..04
 *   Wood    planks, the pier, the archive                   Step_Wood_01..04
 *   Grass   the terrain, meadow and forest floor, dirt      Step_Grass_01..04
 *   Water   puddles, wet floors under a running sprinkler,
 *           the shallows                                     Step_Water_01..04
 *
 * The surface comes from a ray down from the feet: the entity it lands on
 * (a Terrain is Grass), then its material, mesh and name, by keyword
 * (SurfaceOf). Water wins wherever a puddle or a running sprinkler is near.
 * A step is quieter crouched and louder running; the player's running steps
 * are a noise the AI hears (ReportAINoise, loudness 0.25).
 *
 * [DemoLab] FootstepLog=1 writes each of the player's steps to the log.
 */
namespace LabFootsteps {
    enum ESurface : uint8_t { FS_Stone = 0, FS_Metal, FS_Tile, FS_Wood, FS_Grass, FS_Water, FS_Count };

    const char* SurfaceName(ESurface InSurface);

    /* The surface under InFeet (a ray down, ignoring InSelf). */
    ESurface SurfaceAt(GameCore::World& InWorld, const EngineCore::Vector3& InFeet, GameCore::EntityID InSelf);

    /* The surface a level entity is made of, by its terrain, material, mesh and name. */
    ESurface SurfaceOf(GameCore::World& InWorld, GameCore::EntityID InGround);

    /* A level loaded: no one has walked yet. */
    void Reset();

    /* Each frame: strides, steps and their sounds. */
    void Tick(GameCore::World& InWorld, float InSeconds);

    /* Steps the player has taken on each surface since the level started (the tests read it). */
    uint32_t StepsOn(ESurface InSurface);
}

#endif //DEMOLAB_LABFOOTSTEPS_H
