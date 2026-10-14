# Foliage

Grass, bushes, rocks and trees placed on a terrain: many copies of a few
meshes, painted in the editor and drawn as instanced meshes. Foliage belongs
to its terrain. It is saved beside the terrain, stands on the terrain's
ground, and is culled and thinned by the terrain's own chunks and LOD. Read
`TERRAIN.md` first.

```
Terrain component ── Terrain ──► .nmterrain    the ground
                  └─ Foliage ──► .nmfoliage    types + instances, bucketed by terrain chunk
                                     │
                    terrain proxy, per chunk:  ground mesh (LOD 0/1/2)
                                               + per foliage type: one instance buffer,
                                                 drawn as the chunk's LOD allows
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| `FoliageType` | GameCore `Foliage.h` | A kind of foliage: up to three LOD meshes, an optional material, how it is placed (spacing, scale, slope limit, ground alignment, sink), and how it is drawn (cull distance, per-LOD density, colour variation, shadows). |
| `FoliageInstance` | GameCore `Foliage.h` | One copy: X and Z on the terrain, yaw, scale and a seed. 22 bytes in the file. |
| `FoliageData` | GameCore `Foliage.h` | The types and the instances, kept in one bucket per terrain chunk, each bucket with its own revision. |
| `.nmfoliage` | `Load/SaveFoliageFile` | The file, saved atomically. `FoliageAssetCache` shares one mutable copy, as `TerrainAssetCache` does. |
| `ApplyFoliageBrush` | GameCore `Foliage.h` | One paint or erase dab. |
| `GetFoliageTransform` | GameCore `Foliage.h` | Where an instance stands: on the terrain, yawed, scaled, tilted toward the ground and sunk. |
| Terrain component, **Foliage** and **Foliage Visible** | RenderCore `TerrainComponent.h` | Names the foliage file. The terrain's proxy draws it. |
| `RenderContext::CreateInstanceBuffer` | RenderCore `RenderContext.h` | CPU-filled instance buffers for instanced `DrawMesh`, `DrawMeshShadow` and `DrawMeshOccluder` (`MaterialParameterOverrides::Instances`). |
| Foliage tab, `foliage` console command | Editor `Private/Editor/Terrain/` | Types, painting and erasing, undo, saving. |

## Instances stand on the ground

An instance stores where it is on the terrain, not how high. Each time its
chunk is built, it is placed at the terrain's height there:

- tilted toward the ground's normal by its type's **Align to Ground** (0 is
  upright, 1 follows the ground);
- pushed into the ground by **Sink** × its scale.

Sculpt under a tree and the tree follows the ground: a stroke changes the
terrain chunk's revision, and the chunk's foliage is rebuilt with it.

## Culling and LOD: the terrain's

Each terrain chunk (64 × 64 cells) owns one bucket of instances. The proxy
builds one instance buffer per chunk and type, and draws it while the chunk is
in view. The chunk's box is raised by the tallest foliage on it, so a tree
standing at a chunk's edge is not culled early. Each frame, the chunk's
distance from the camera picks its terrain LOD (see TERRAIN.md), and that LOD
drives the foliage too:

| Chunk LOD | Instances drawn | Mesh |
|---|---|---|
| 0 | All of them | LOD 0 Mesh |
| 1 | LOD 1 Density × the count (default 0.5) | LOD 1 Mesh, else LOD 0 |
| 2 | LOD 2 Density × the count (default 0.25) | LOD 2 Mesh, else LOD 1, else LOD 0 |

**Thinning is stable.** Within a chunk, a type's instances are sorted by a
hash of their seeds, and a coarser LOD draws the first N of them. The same
instances survive at every distance; they never swap or flicker, and no buffer
is rebuilt as the camera moves.

**Cull Distance** hides a type entirely on chunks farther than that from the
camera. It uses the same chunk distance as the LOD, and 0 means never.
Typical settings:

| Type | Typical settings |
|---|---|
| Grass | Cull Distance 5000–8000, LOD 1 Density 0.3–0.5, LOD 2 Density 0, Cast Shadows off |
| Bushes | Cull Distance 10000–15000, densities 0.5 / 0.25, Cast Shadows off |
| Rocks | Cull Distance 0, LOD 2 Density 0.5, Align to Ground 1, Sink a little |
| Trees | Cull Distance 0, densities 1 / 1 (the silhouette must not thin), LOD 1 and LOD 2 meshes with fewer triangles |

**Shadows** draw at the same counts and meshes, for types with Cast Shadows
set. Leaf cards (Masked materials) cast their whole quad, because the shadow
pass does not run a material's clip. Turn Cast Shadows off for card foliage
such as grass and bushes, and leave it on for solid meshes such as trunks,
rocks and pine cones.

## Painting

1. Select a terrain. The level must be saved, because the foliage file goes
   beside the terrain's.
2. Open **World > Terrain...**, go to the **Foliage** tab, and click **Add
   Mesh...** to pick an `.nmmesh` in the project. The first one makes
   `<Terrain>.nmfoliage` and names it in the Terrain component's **Foliage**.
3. Set the type: Spacing first (the distance kept between two of this type),
   then Scale, Max Slope and Align to Ground.
4. Tick the types to paint, and left drag on the terrain.

| Tool | What it does |
|---|---|
| **Paint** | Fills the brush toward each ticked type's Spacing, more toward the centre (it uses the brush's falloff). It skips ground steeper than the type's Max Slope and spots nearer than its Spacing to another of the same type. Holding still keeps filling; a second at full strength fills the brush. |
| **Erase** | Removes the ticked types under the brush, faster toward the centre. |

<kbd>Shift</kbd> swaps Paint and Erase. The brush's radius, strength and
falloff are the shared Brush settings below the tabs (<kbd>[</kbd>
<kbd>]</kbd>, Ctrl+wheel). <kbd>Esc</kbd> or a right click cancels a stroke.

- **Undo:** each stroke is one undo step ("Paint Foliage Ground"). Type edits
  are steps too, and quick edits of one type merge into a single step.
  Removing a type removes its instances, and undo brings both back.
- **Saving and staging:** foliage is written with the level, staged beside
  it, and dropped unsaved with it, exactly as terrain is.

## Meshes and materials

- **Pivot:** a foliage mesh's pivot is its base, at the origin, with +Y up.
  The mesh's box top sets how far its chunk's culling box is raised.
- **Materials:** every section draws with the type's **Material** if one is
  set, otherwise with the mesh's own section materials. Instances use the
  material's InstancedMesh vertex shader, which every material cooks.
  - World Position Offset (wind) works per instance.
  - Vertex Color in the material is multiplied by the instance's colour.
    **Color Variation** darkens each instance by up to that much.
- **Masked leaf cards:** give their vertices normals that lean upward and
  outward rather than face-on. They then light like a soft clump instead of a
  flat card, and they don't wash out white from above.
- **Card UVs:** use a card's own 0–1 UVs. World-tiled UVs make a leaf
  texture's alpha clip most of the card away.

`Games/WorldsLab/Tools/Terrain/make_foliage_meshes.py` (local) builds a
grass tuft, a bush, a pine with two LOD meshes, and a rock this way.

## Console

| Command | What it does |
|---|---|
| `foliage add <mesh> [name] [spacing]` | Adds a type, making the foliage file on first use |
| `foliage set <type> <field> <value>` | Name, Mesh, Mesh1, Mesh2, Material, Spacing, ScaleMin, ScaleMax, AlignToNormal, MaxSlope, Sink, CullDistance, LOD1Density, LOD2Density, ColorVariation, CastShadows, RandomYaw |
| `foliage remove <type>` | Removes a type and its instances |
| `foliage paint <x> <z> <radius> [strength] [seconds] [type,...]` | One stroke at a world X/Z |
| `foliage erase <x> <z> <radius> [strength] [seconds] [type,...]` | One erase stroke |
| `foliage clear [type]` | Every instance, or every one of a type |
| `foliage stats` | Instances in total and per type |

A type can be named by its index or by its name. `terrain tool
Foliage` (or `FoliagePaint`, `FoliageErase`) opens the tab.

## Numbers

Measured on this Linux/Vulkan machine, in the editor, on the 257² Hills test
level: about 12,800 instances of grass, bushes, rocks and pines with shadows,
seen from inside the woods: the editor viewport reports 63 fps. Grass
spacing 110 over a 120 m radius paints in a few seconds.

**Backends:** instanced draws need `GDISupportsCompute()`, which Vulkan,
D3D12 and Null have. Elsewhere foliage is not drawn, and the proxy says so
once.

## Not yet

- **Collision:** a tree does not stop a player. `MoveOnGround` sees the
  ground only.
- **Per-instance culling:** instances are culled by chunk only. A chunk only
  partly in view draws all of its instances.
- **No fading:** there is no cross-fade between LODs and no dithered cull. A
  type pops at its cull distance.
- **Gameplay queries:** nothing in game code asks "what foliage is here?".
- **Run-time changes:** foliage isn't added or removed while the game runs.
- **Impostors:** distant trees don't use billboard impostors.
- **Procedural fill:** painted foliage has no "fill from these rules" generator; a
  Procedural Spawner does that instead, with its own instances
  (`Engine/Modules/RenderCore/PROCEDURAL.md`).
