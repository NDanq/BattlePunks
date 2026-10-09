# Terrain

Heightmap terrain: a square grid of heights and paint weights. You sculpt
and paint it in the editor, it is saved beside its level, it is drawn in
chunks with LOD, and you can walk on it, with or without the physics module.

```
.nmterrain (beside the level) ──load──► TerrainAssetCache (one shared copy) ◄──brushes── Editor (Terrain mode)
                                              │            │             │
                                       Terrain proxy   Jolt heightfield   SampleGround / MoveOnGround
                                       (chunk meshes)  (Rigid Body)       (gameplay, picking)
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| `TerrainData` | GameCore `Terrain.h` | The samples, in terrain space: `Resolution`² heights `Spacing` apart, and four 0-255 paint weights each. Sampling, normals, raycasts, per-chunk revisions. |
| `.nmterrain` | `LoadTerrainFile` / `SaveTerrainFile` | The file: heights as 16-bit samples over the terrain's own height range, weights as bytes, layer names. Saves are atomic (a temporary file, then a rename). |
| `TerrainAssetCache` | GameCore `Terrain.h` | Loaded terrains by runtime path. It holds one mutable copy, which every reader shares and the editor's brushes write. |
| `ApplyTerrainBrush` | GameCore `TerrainBrush.h` | One brush dab: Raise (a negative Rate lowers), Smooth, Flatten, Noise, Set Height, Paint, Smooth Paint. Rates are per second. |
| Terrain component | RenderCore `Components/TerrainComponent.h` | `"Terrain"`: the file, a material, Visible, Cast Shadows, Occluder, Walkable, UV Tile Size, LOD Distance. |
| Rigid Body, shape **Height field** | GameCore `RigidBodyComponent.h` | Physics' collision for the terrain. Static or Kinematic only. |
| `SampleGround`, `RaycastTerrain`, `MoveOnGround` | NimbleEngine `TerrainFacade.h` | Ground queries for game code, plus the `Terrain` script library. |
| Terrain mode | Editor `Private/Editor/Terrain/` | The dock, the viewport brush, strokes as undo steps, heightmap import and export, and the `terrain` console command. |

## Space and units

Sample (X, Z) sits at (X × Spacing, height, Z × Spacing) from the entity's
origin, so the terrain covers [0, Size] on X and Z with +Y up. The entity's
Transform places it, rotation and scale included. Units are the world's, which
is centimetres in the shipped content, so the default Spacing of 100 is one
metre.

**One triangulation everywhere.** Every cell is split along its (X, Z) to
(X+1, Z+1) diagonal, which is how Jolt's `HeightFieldShape` splits it. The
render mesh, `SampleHeight`, `Raycast` and the physics body therefore all agree
on the same surface. What you see is exactly where a player stands and where a
ball rolls.

## Making one

**World > Terrain...** opens the Terrain dock, tabbed with Details. Its
**Create** tab makes a flat terrain centred on the origin:

- **Name:** the entity's name, which is also the file's name.
- **Size:** 65 to 2049 samples a side.
- **Spacing:** the distance between samples.
- **Height:** the starting height of every sample.
- **Collision:** whether to add a Static Rigid Body (a heightfield).

The file goes in `Content/Levels/<Level>/Terrain/<Name>.nmterrain`, beside the
level's `Lighting/` folder, so the level must be saved first. The entity is
Static, and making it is one undo step.

The **Heightmap** box imports a greyscale image into the selected terrain,
mapping black and white to the heights you set. The image is resampled to fit,
and 16-bit PNG works best. Export writes the heights as a 16-bit PNG, lowest
black and highest white.

## Sculpting and painting

With the dock's **Sculpt** or **Paint** tab in front, a left drag on a
terrain uses the brush. The camera keeps its buttons (right, middle, and
Alt+left), and a click off the terrain still selects.

| Tool | What it does |
|---|---|
| Raise / Lower | Moves the ground up or down by Rate units a second at full strength. Shift flips them. |
| Smooth | Moves each sample toward the average of its 3 × 3 neighbours. |
| Flatten | Moves the ground toward the height where the stroke began. |
| Noise | Adds two octaves of value noise, Noise Scale wide, by Rate. |
| Set Height | Puts the ground at Target Height (world Y). |
| Paint | Raises the chosen layer's weight and lowers the others, so the weights still sum to 255. Shift smooths instead. |
| Smooth Paint | Blurs the layers' edges together. |

- **Brush settings:** Radius (world units), Strength, and Falloff (Smooth,
  Linear, Sphere or Tip).
- **The ring:** the outer ring lies on the ground at the brush's radius. The
  inner ring marks where its strength is half.
- **Resizing:** `[` and `]`, or Ctrl+wheel.
- **Cancelling:** Esc or a right click cancels the stroke in progress and puts
  every sample back.

**Undo.** A stroke is one undo step. It records only the chunks it touched,
copied before the first dab, so undo restores them exactly.

**Saving.** Edited terrain counts as unsaved like any other edit (the title's
`*`), and **Save** writes it beside the level. If you open or make another
level without saving, the unsaved edits are dropped and the file loads as it
was.

**Layers.** The four paint layers are the terrain material's (below). Their
display names live in the file. A new terrain names them Grass, Earth, Rock
and Snow.

## Materials

A chunk's vertices carry:
- the paint weights as **Vertex Color**: R, G, B and A are layers 0, 1, 2 and 3;
- **UV0** tiled once every UV Tile Size units.

In a material graph, the **Terrain Layer Blend** node (category Terrain) mixes
four inputs by those weights. Use one per channel you want blended: base
colour, roughness, normal, and so on. The engine's own
`Engine/Content/Materials/Terrain.material.nmgraph` is four flat colours and
roughnesses, and it is what a Terrain with no Material draws with. A textured
terrain material samples four textures at UV0 and blends them with the node.

The vertex weights are one per sample, so paint blends across a whole cell.
Up close, borders follow the grid. Breaking them up with a noise texture in
the material helps.

## Drawing

The proxy builds a mesh for every 64 × 64-cell chunk at three LODs: every
sample, every second, and every fourth. Each mesh has skirts that hide the
cracks between chunks at different LODs. The meshes are stored in
`MeshAssetCache` under `@terrain/...` paths and drawn through the ordinary
`DrawMesh`, `DrawMeshShadow` and `DrawMeshOccluder` paths. They therefore get
the G-buffer, cascaded shadows, occlusion and parallel recording like any
mesh.

- **Culling:** each chunk is culled against the view on its own.
- **LOD:** within LOD Distance (default one and a half chunk widths) a chunk
  draws in full, within twice that at half resolution, and beyond that at a
  quarter. Its shadow uses the same LOD.
- **Edits:** a brush stroke rebuilds only the chunks whose samples moved, in
  the next frame. That works whatever the entity's mobility, because the
  proxy polls the data's revisions in `Simulate`.

Measured on this Linux/Vulkan machine: a 2049 × 2049 terrain (1024 chunks,
about 2 km a side) next to a second terrain keeps the editor viewport at its
60 fps cap.

## Walking on it

Walking works without physics. `NIMBLE_WITH_JOLTPHYSICS` is off by default,
and the ground queries read the terrain data directly:

```cpp
#include "Public/NimbleEngine/TerrainFacade.h"

/* In a movement system or an avatar simulation: */
NimbleEngine::GroundMoveSettings Walk;
Walk.RideHeight = 170.0f;        // eye height above the ground
EngineCore::Vector3 Next = Position + Move * (Speed * DeltaSeconds);
if(NimbleEngine::MoveOnGround(World, Position, Next, Walk))
{
    Position = Next;             // on the ground, slid along slopes too steep to climb
}
```

- **`SampleGround(World, Position, Height, &Normal)`:** the surface straight
  below a point. It looks from well above, so a point a little under the
  ground is found too.
- **`MoveOnGround`:**
  - It snaps the move's end to the ground plus RideHeight.
  - It stops or slides a move up a slope steeper than MaxSlopeDegrees.
  - A rise smaller than StepHeight is always taken, and downhill is never
    blocked.
  - It is deterministic, so a server and a predicting client agree.
  - It returns false, leaving the move alone, off the terrain.
- **`RaycastTerrain`:** the nearest hit along any ray, with its normal.
  `PhysicsRaycast` without physics now also hits walkable terrain.
- **Lua and script graphs:** `Terrain.SampleGround(at)`,
  `Terrain.Raycast(from, dir, len)` and
  `Terrain.MoveOnGround(from, to, rideHeight)`.

Only terrains with **Walkable** set count. Editor picking uses every visible
terrain.

With physics on, a Static **Rigid Body** with shape **Height field** (or Auto,
on an entity with a Terrain) is a Jolt heightfield built from the same
samples. Bodies, vehicles' wheels and `PhysicsRaycast` hit it. An edit
rebuilds it, because the terrain's revision is part of the body's signature.

## Content

- **Staging:** every game's CMakeLists stages `Content/**/*.nmterrain` as-is,
  in every build, as it does baked lighting.
- **Cooking:** the cook fails a world whose Terrain names a file that does not
  exist.
- **Networking:** terrain is level data that every machine loads, so it is
  not replicated. Deforming terrain at run time is not supported.

## Console

| Command | What it does |
|---|---|
| `terrain create [name] [resolution] [spacing]` | Makes a terrain for the open level |
| `terrain select [name]` | Selects a terrain |
| `terrain stats` | Prints the edited terrain's size, height range, chunks and revision |
| `terrain sculpt <Raise\|Lower\|Smooth\|Flatten\|Noise\|SetHeight> <x> <z> <radius> [strength] [seconds] [height]` | Runs one stroke at a world X/Z |
| `terrain paint <layer> <x> <z> <radius> [strength] [seconds]` | Paints one stroke |
| `terrain import <image> <low> <high>` / `terrain export <image>` | Heightmap import and export |
| `terrain tool <name\|off>` / `terrain show` | Drives the dock |
| `camera [x y z yaw pitch]` | Places, or prints, the viewport camera, for scripted shots |

## Foliage

A terrain can carry foliage: grass, rocks, trees. The Terrain component's
**Foliage** names a `.nmfoliage` beside the `.nmterrain`. The terrain's proxy
draws it as instanced meshes and culls and thins it with the terrain's own
chunks and LOD. You paint it on the Terrain dock's **Foliage** tab. See
`FOLIAGE.md`.

## Not yet

- Holes.
- More than four layers. A second set of weights in UV1 would allow eight.
- Per-entity texture overrides of the material.
- Splitting large terrains into streamed tiles.
- Runtime deformation over the network.
- The light baker ignores terrain, so terrain is lit live.

## Tests

| Suite | Checks |
|---|---|
| `NimbleEngineTerrainTests` | Data, the file, the cache, brushes, chunk meshes and LODs, ground queries, walking |
| `NimbleEngineTerrainTests` (with Jolt) | The same tests with the physics module compiled in |
| `NimbleEnginePhysicsTests` `[heightfield]` (with Jolt) | Rays land where `SampleHeight` says, a ball rolls downhill and rests, an edit rebuilds the body |
| `NimbleEditorTerrainTests` (offscreen) | Create, strokes, undo and cancel, save and discard, heightmap round trip, the console, the dock |
