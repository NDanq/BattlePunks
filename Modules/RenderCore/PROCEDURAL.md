# Procedural content

Meshes placed by rules instead of by hand: lamp posts every 18 m along a
road, a meadow of grass inside an outline, pines on the gentle slopes of a
hill, moss on top of a boulder. Two components do it:

| Component | Module | What it is |
|---|---|---|
| **Spline** | GameCore `Components/SplineComponent.h`, curve math `Spline.h` | A curve through up to 64 points in its entity's space; open or closed, Catmull-Rom or linear, with a Width. Drawn and edited in the editor; sampled by games. |
| **Procedural Spawner** | RenderCore `Components/ProceduralSpawnerComponent.h` | Rules: where candidate points come from, what they are dropped onto, which survive, what each becomes. Its instances are made from the rules, never saved, and drawn as instanced meshes. |

`Engine/Docs/Guides/ProceduralGuide.md` is the walkthrough (Demo Lab's L1
Foothills, with pictures). This page is the reference.

```
 game thread, once a frame (WorldBridge)                  worker thread
 ┌──────────────────────────────────────────────┐        ┌───────────────────────────┐
 │ ProceduralSystem, per spawner:                │        │ GenerateProcedural        │
 │   signature of what it reads                  │ inputs │   candidates (source)     │
 │   rules, place, Spline, Exclusions' splines,  ├───────►│   Density, noise          │
 │   terrains' revisions, static meshes under it │        │   drop onto surfaces      │
 │   changed and nothing running? snapshot  ─────┘        │   slope, height, layer,   │
 │   finished? publish, count back to component ◄─────────┤   exclusions              │
 └──────────────────────┬───────────────────────┘ result  │   pick, transform, cell   │
                        │ ProceduralResultStore           └───────────────────────────┘
                        ▼ (world, entity) -> result
 render thread: the spawner's proxy, in Simulate, swaps in a new result:
   one instance buffer per cell and mesh entry; per view, cells culled,
   thinned by distance (stable), Far Mesh past LOD Distance, gone past Cull Distance
```

## Splines

A Spline component's points are in its entity's space, so moving, turning or
scaling the entity moves the curve. **Curve** (centripetal Catmull-Rom)
passes through every point and never overshoots between close ones;
**Linear** joins them straight. **Closed** joins the last to the first (three
points or more).

**Points in use** are those up to the last one not at the entity's origin:
the Details panel's array rule. The first point may sit on the origin; a
method that would leave the last one there nudges it a hundredth of a unit
up. `GetPointCount`, `AddPoint`, `InsertPoint`, `SetPoint`, `RemovePoint`
and `ClearPoints` keep the list packed.

**Width** is a band around the curve, half each side. A Spline Path spawner
fills it in lanes; an Exclusion clears it.

`SplineCurve` (Spline.h) is the math, usable without a component: built
from points in any space, it keeps an arc-length table (16 samples a
segment), so `SampleAtDistance` steps evenly along the curve whatever the
spacing of its points. It also answers `NearestTo` / `NearestToXZ` (the
nearest point on the curve, and how far along it), `ContainsXZ` (inside a
closed curve, seen from above), `GetBounds` and `Transformed` (through a
matrix).

Lua and C++ queries on the component are in **world space**:

```lua
local road = World.FindEntity("Road"):GetComponent("Spline")
local length = road:GetLength()
local x, y, z = road:GetLocationAtDistance(length * 0.5)     -- a Vector3 is three numbers
local dx, dy, dz = road:GetDirectionAtDistance(length * 0.5) -- unit length
local along = road:FindDistanceClosestTo(px, py, pz)         -- distance along the curve
local inside = World.FindEntity("Foliage_Meadow"):GetComponent("Spline"):IsInside(px, py, pz)   -- closed splines
```

## The spawner's rules

### Where points come from (Source)

| Source | Candidates | Needs |
|---|---|---|
| **Spline Path** | One every **Spacing** along the entity's Spline. With a spline **Width** wider than Spacing, lanes Spacing apart fill it. **Path Offset** moves the lanes' centre to the curve's right (negative: left); **Both Sides** mirrors them to the left too. An open path keeps one point exactly on each end. | a Spline, two points |
| **Spline Area** | A grid **Spacing** apart, in the entity's X/Z, kept inside the closed Spline (seen from above) | a closed Spline, three points |
| **Box** | A grid **Spacing** apart over **Box Extent** (half sizes, entity space) | — |
| **Mesh Surface** | Over the triangles of the entity's own Render mesh: one per Spacing x Spacing of area, standing on the surface (interpolated vertex normals) | a Render mesh |

**Jitter** (0..1) moves each grid point up to a cell's width; 0 is a neat
grid. Spacing is in world units, whatever the entity's scale.

### What they land on (Project)

Every source but Mesh Surface may drop its points **straight down** in the
world onto **Terrain** (every visible Terrain component), **Static Meshes**
(the Render meshes of Static entities whose boxes overlap the source's area;
skinned meshes and the spawner's own mesh never count), or both — the higher
surface wins. A point with nothing under it is dropped. **None** leaves them
where the source put them: on the box's middle plane, the spline's mean
height, or the path itself.

How far the drop searches: a **Box** searches its own height (a box limits
what it lands on vertically too); spline sources search **Project Range**
above and below the spline.

### Which survive

In this order, cheapest first:

| Filter | Keeps |
|---|---|
| **Density** | a random fraction (0..1) |
| **Noise Scale / Noise Threshold** | where smooth value noise (Noise Scale world units a patch; 0 = off) is above the threshold — the share left bare |
| **Max Slope** | surfaces no steeper (degrees from up) |
| **Min / Max Height** | world Y within the range |
| **Terrain Layer / Layer Threshold** | points that landed on a terrain whose paint layer (0..3) weighs at least the threshold. Any: no test. Only terrain hits pass a layer test |
| **Exclusions** | points outside every named entity's Spline: not inside a closed one, not within half its Width |

### What each one becomes

- **Meshes**: up to 8 entries, picked by **Weight**. Each has a **Mesh**, an
  optional **Far Mesh** (drawn at LOD 1 and 2), an optional **Material**
  for every section, and a **Scale** multiplier.
- Scale between **Scale Min** and **Scale Max**, times the entry's Scale.
- Yaw: the curve's heading with **Align to Path** (Spline Path; the mesh's
  +Z along the curve), else random with **Random Yaw**, else none.
- Tilted toward the surface normal by **Align to Normal** (0 upright, 1
  along it); pushed in by **Sink** times its scale.
- Darkened by up to **Color Variation** (the material's Vertex Color).

### Deterministic

Each candidate's randomness is a hash of where it came from (its grid cell,
its step and lane along a path, its triangle) and the **Seed** — never a
running random stream. The same rules and surfaces always give the same
instances, and an edit changes only what is near it: moving one spline point
moves the instances along the segments that point shapes, not the rest.

## When instances are made

`ProceduralSystem` (Private, driven by `WorldBridge::ExtractAndRender` on
the game thread) keeps, per spawner, a 64-bit signature of everything its
placement reads:

- its rule fields (not the drawing ones: Cell Size aside, changing LOD
  Distance, LOD densities or Cull Distance never regenerates);
- its entity's world matrix, its Spline, each Exclusion's Spline and matrix;
- with terrain projection, every visible terrain's path, matrix and data
  revision — so an editor brush stroke regenerates spawners on that terrain;
- with mesh projection, the static meshes under its area (looked for again
  every 30 frames, or at once when anything else moved) and their matrices;
- `MeshAssetCache`'s generation (a reimport), and `RegenerateAllProcedural`.

When the signature moves and no generation is running, it snapshots those
inputs (shared terrain data, mesh data, curves, a copy of the component) and
starts a worker thread. One generation per spawner runs at a time; an edit
made meanwhile starts the next one when it finishes. A finished result is
published to `ProceduralResultStore` and the component's **Instance Count**
and **Generate ms** are written back. The first generation of each spawner
is logged:

```
[RenderCore] Procedural Spawner 'Meadow': 45732 instances in 46 cells from 70801 points, 55.7 ms (Procedural pool 3.65 MB)
```

A problem (no Spline, no mesh that loads, Spacing far too small, Max
Instances reached) is logged once, as a warning, when it starts.

Terrains are locked per query, so a brush stroke on the game thread waits
for one ray at most, never for a whole generation.

## Memory

What a generation keeps comes from Core's **"Procedural" pool** (a
`HeapMemoryPool`, `GetProceduralMemoryPool()`): the result, its cells and
instance arrays (`ProceduralVector`, a `std::vector` over
`ProceduralAllocator`), the shared handle's control block, and the snapshot
of a generation's inputs (`std::allocate_shared` with the same allocator).
So it shows in Core's pool stats and leak report; `pcg stats` and each
spawner's first log line print its size. The generator's scratch
(candidates, the mesh grid) is freed before it returns and stays on the
plain heap. A meadow of ~47,000 instances holds 3.65 MB there, 80 bytes
each (measured on WorldsLab's former Meadow level).

The handle is a `std::shared_ptr`, **not a `TRef`**. A result is shared by
the worker that builds it, the game thread that publishes it and the render
thread that draws it, and `CollectGarbage` requires that no thread but the
game thread touches a TRef while it runs (it is called at the frame fence
on exactly that promise: NimbleEngine.cpp, RenderCommand.h). Results have
no cycles, so reference counting already frees every byte; the collector
would have nothing to add. The asset caches (`MeshAssetCache`,
`TerrainAssetCache`) make the same choice for the same reason.

## Drawing

Instances are bucketed into square **cells** of **Cell Size** (world
units, on the entity's X/Z); within a cell, each mesh entry's instances are
sorted by a hash, so the first N are the ones a coarser LOD keeps. The
proxy creates one instance buffer per cell and entry when a result arrives,
and per view:

| Distance from the view to the cell's box | Drawn |
|---|---|
| under **LOD Distance** (0: 1.5 x Cell Size) | every instance, Mesh |
| under twice that | **LOD 1 Density** of them, Far Mesh if set |
| further | **LOD 2 Density** of them, Far Mesh if set |
| past **Cull Distance** (0: never) | nothing |

Cells are frustum-culled one by one. Shadows draw the same cells, counts and
meshes, when **Cast Shadows** is set (turn it off for card foliage, as for
terrain foliage). Instanced draws need `SupportsInstancing()` (Vulkan,
D3D12, Null); elsewhere a spawner draws nothing and says so once. Instances
use each material's InstancedMesh vertex shader; World Position Offset
(wind) works per instance.

## Editor

| | |
|---|---|
| World > **Add Spline** | an entity with a two-point Spline in front of the camera |
| World > **Add Procedural Spawner** | an entity with a Box spawner |
| World > **Edit Spline Points** | the selected entity's points take the left button (a Spline is added if it has none) |

While editing points: drag a point over the ground and meshes under the
cursor (Shift: up and down), **Ctrl + click** adds one (after the selected
point, else at the end), **Delete** removes the selected one, **Esc** cancels
a drag, then leaves point editing. Each change is one undo step.

The viewport draws every Spline (the selected one bright, with its points
and its Width band) and the selected Box spawner's box.

Console, for scripts and tests (points in world space):

| Command | |
|---|---|
| `spline new <name> <x y z>` | an entity with an empty Spline, selected |
| `spline add <x y z>`, `insert <i> <x y z>`, `set <i> <x y z>`, `remove <i>`, `clear` | the selected entity's points (one undo step each) |
| `spline closed <on\|off>`, `type <curve\|linear>`, `width <w>`, `info` | |
| `spline edit <on\|off>` | point editing |
| `pcg new <name> <x y z>` | an entity with a spawner, selected |
| `pcg set <field> <value>` | Source (path, area, box, surface), Project (none, terrain, meshes, all), Spacing, Jitter, ProjectRange, PathOffset, BothSides, Density, NoiseScale, NoiseThreshold, MaxSlope, MinHeight, MaxHeight, Layer (any, 0..3), LayerThreshold, ScaleMin, ScaleMax, AlignToNormal, AlignToPath, RandomYaw, Sink, ColorVariation, CastShadows, CellSize, LODDistance, LOD1Density, LOD2Density, CullDistance, MaxInstances, Seed, BoxExtent x y z, Visible |
| `pcg mesh <i> <mesh\|none> [weight] [scale] [far mesh] [material]` | an entry (`-` for an empty far mesh or material) |
| `pcg exclude <i> <entity\|none>` | an Exclusion by entity name |
| `pcg regenerate` | every spawner, now |
| `pcg stats` | each spawner's count and time |

## Numbers

Linux, Vulkan, RelWithDebInfo, WorldsLab's Meadow level (257² terrain). The
level was removed when Demo Lab replaced WorldsLab (its generator is in git
history: `Games/WorldsLab/Tools/Procedural/make_meadow_level.py` at 61af1eec);
Demo Lab's L1 (`Games/DemoLab/Tools/Levels/dress_l1.py`) has the same kinds
of spawner and has not been timed here yet:

| Spawner | Instances | Generation (worker thread) |
|---|---|---|
| Meadow (Spline Area, 55-unit grass, terrain + layer + noise + exclusion) | 45,700 from 70,800 points | 57 ms |
| Woods, Rocks, Road lamps, Boulder moss | 30 – 600 each | under 1 ms each |

## Files

| File | |
|---|---|
| GameCore `Spline.h`, `Spline.cpp` | SplineCurve |
| GameCore `Components/SplineComponent.*` | the component, serializer, Lua, properties |
| RenderCore `Components/ProceduralSpawnerComponent.*` | the component, its proxy, serializer, Lua, properties |
| RenderCore `ProceduralGeneration.h`, `Private/.../ProceduralGeneration.cpp` | `GenerateProcedural`: the placement, a plain function |
| RenderCore `Private/.../ProceduralSystem.*` | signatures, snapshots, worker threads, the result store |
| Editor `Private/Editor/Procedural/ProceduralEditing.*` | the overlay, point editing, `spline` and `pcg` |
| RenderCore `Tests/ProceduralTests.cpp` | `RenderCore.Procedural`: curves, the component, every source, filter and surface, Lua, and the pool draining to zero |

## Not yet

- **Collision:** instances have none; a player walks through a lamp post.
- **Per-instance culling and fading:** cells are culled whole, and a cell
  pops at its cull distance.
- **Run-time edits are whole regenerations:** fine for a level's dressing,
  not for things that change every frame.
- **Rules from graphs:** a spawner is one fixed pipeline; there is no node
  graph of operations (sample, filter, transform, merge) as UE's PCG has.
- **Spawner on spawner:** instances are not surfaces for another spawner,
  nor exclusions.
- **Spline per-point roll, width and tangents:** a point is a position only.
