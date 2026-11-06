# Importing meshes with meshlets

A **meshlet** is a small piece of a mesh: at most 64 vertices and 124
triangles, never spanning two sections, with a bounding sphere and a normal
cone. With meshlet culling on, the GPU tests a mesh's meshlets one by one
against the view, and against what hid them last frame, and draws only the
pieces that can be seen. This guide imports a mesh with meshlets, turns the
culling on, checks that it works, and says when it is worth it.

Demo Lab's L2 workshop statue is the worked example: a 1,346,780-triangle
marble scan, imported with meshlets and culled with occlusion on
(`Games/DemoLab`, once unpacked: [Demo Lab](../../../Games/DemoLab/Docs/Guide.md)).

Reference: [RENDERPIPELINE.md](../../Modules/RenderCore/RENDERPIPELINE.md),
"Meshlets" (how the culling works, the backends) and
[MESHLETS_PERF.md](../../Modules/RenderCore/MESHLETS_PERF.md) (what it costs).

## Contents

1. [What gets meshlets](#1-what-gets-meshlets)
2. [Import with the wizard](#2-import-with-the-wizard)
3. [Import from the command line](#3-import-from-the-command-line)
4. [Turn culling on](#4-turn-culling-on)
5. [Check it works](#5-check-it-works)
6. [Older meshes and reimporting](#6-older-meshes-and-reimporting)
7. [When it is worth it](#7-when-it-is-worth-it)
8. [Pitfalls](#8-pitfalls)

---

## 1. What gets meshlets

| Mesh | Meshlets |
|---|---|
| A static mesh imported from glTF / GLB with **Build meshlets** on (the default) | built at import, saved in the `.nmmesh` (its `MLET` chunk) |
| A static mesh of 248 triangles or more without them (an older import, a primitive, a mesh made in code) | built in memory the first time a culling view draws it, while `[Rendering] Meshlets` is on |
| A skinned mesh | none: its bounds are the bind pose's, not the animated pose's |
| A smaller mesh | none: two meshlets' worth is the least that gains anything |

Each section is split on its own, so a mesh with several materials keeps one
draw per section, as before. The vertex buffers and materials are unchanged:
World Position Offset, velocity and every shading model work as they did (a
material's Max Offset pads each meshlet's sphere).

The data costs about **6.4 bytes a triangle** on disk: the statue's `MLET`
chunk is 8.7 MB of its 62.9 MB.

## 2. Import with the wizard

Drop a `.gltf` or `.glb` on the Content Browser, or use Tools > Import
(Ctrl+I). On the wizard's **Mesh options** page:

- **Build meshlets (static meshes)**: on by default. Leave it on.

Everything else is as for any mesh (scale, convert axes, normals, lightmap
UVs, combine). Meshlets are built last, after the tangents, so they describe
the indices the file will hold. If the build fails for a mesh, the import log
says so (`<Mesh>: no meshlets -- it draws by section`) and the mesh is
written without them. A skinned mesh in the same file gets none, with no
warning.

The checkbox starts from Edit > Editor Settings > Import > **Build meshlets**
(`[Editor] ImportMeshlets`), and the wizard saves your choice back there.

## 3. Import from the command line

For art scripts, the editor imports one file the way the wizard would, with no
window, then exits:

```
NimbleEditor --import <file.glb> [--import-to <folder>] [--import-settings key=value ...]
```

- `--import-to`: the folder, relative to the project (default
  `Content/Meshes`). The asset is named after the file.
- `--import-settings`: an option changed from the wizard's defaults, as the
  asset records it. Repeat it for more than one. The keys are `Scale`,
  `ConvertCoordinates`, `GenerateNormals`, `CombineMeshes`, `ImportTextures`
  (off by default here), `GenerateLightmapUVs`, `ImportAnimations`,
  `AnimationSampleRate`, `BakeMaterialColors` and `BuildMeshlets`.
- It prints what it wrote and any warnings, and exits 1 if the import failed.

Demo Lab's statue is imported this way: no lightmap UVs (a 1.35M-triangle
mesh has no use for them), meshlets built.

```
NimbleEditor --import Saved/ArtCache/L2_Statue.glb --import-to Content/Meshes/L2_Laboratory --import-settings GenerateLightmapUVs=0
```

Run it from the project's `Binaries/<Platform>_<Config>` folder, as the editor
always runs. A relative `<file>` means relative to where you launched it.

## 4. Turn culling on

Meshlets in a file do nothing until culling is on. The switches are in
`[Rendering]`, in the project's `Config/DefaultEngine.ini`, or at the console
(`Rendering.Meshlets true`):

| Key | Default | Does |
|---|---|---|
| `Meshlets` | off | culls meshlets against the view's frustum |
| `MeshletOcclusion` | off | also culls meshlets behind what the view drew the frame before |
| `MeshletCones` | off | also culls meshlets all of whose triangles face away (only for closed, one-sided meshes: the Base Pass draws back faces) |
| `MeshletFreezeCulling` | off | keeps culling against the view as it was when turned on, to look at what was culled |
| `MeshletStats` | off | reads back what the cull kept (stalls the GPU: debugging only) |

Demo Lab's own settings:

```ini
[Rendering]
Meshlets=true
MeshletOcclusion=true
```

Culling runs on **Vulkan and D3D12**, for a scene's own views: the camera's,
not reflection or scene captures, previews or shadow views, which draw every
section whole. On Metal and OpenGL meshes draw whole, as before.

## 5. Check it works

**Look at what was culled.** Put the camera in front of the mesh, type
`Rendering.MeshletFreezeCulling true` in the console, then fly round it. The
culling stays as it was from the first spot, so the back of the mesh, and
anything that was off screen or hidden, is missing. Type
`Rendering.MeshletFreezeCulling false` to return.

**Compare the pictures.** Culling never changes the image. A screenshot with
`Rendering.Meshlets false` and one with it on, from the same place, should
match. With occlusion, something coming out from behind a wall can show one
frame late, as with the engine's other occlusion culling.

**Count in code.** `RenderStats` (from `Renderer::GetStats`) carries the last
view's `MeshletSectionsLastFrame`, `MeshletsSubmittedLastFrame` and
`MeshletTrianglesLastFrame`. With `[Rendering] MeshletStats` on,
`MeshletTrianglesDrawnLastFrame` is what the cull kept; it is -1 otherwise.

**Time it.** `stat frame` shows the Base Pass span, which includes the cull.
Compare it with `Rendering.Meshlets` off and on from the same view. Demo Lab's
`Tools/Perf/meshlet_perf.ps1` does this properly: interleaved launches, both
backends, medians.

## 6. Older meshes and reimporting

A mesh imported before meshlets existed has no `MLET` chunk, and its recorded
import settings have no `BuildMeshlets` key, which reads as **off**. So:

- **Reimport** (the Content Browser's context menu) rebuilds it the same way,
  still without meshlets.
- To add them, import the source again with **Build meshlets** on and replace
  the asset, or run `--import` with `--import-settings BuildMeshlets=1`.
- Until then, with culling on, a mesh of 248 triangles or more builds its
  meshlets in memory when first drawn. That works, but it costs load time and
  is redone every session.

The `MLET` chunk records which indices it was built from. If the mesh's
indices change without the meshlets being rebuilt, the meshlets are dropped
on load with a warning (`meshlets: built from other indices; drawing the
mesh by section`) and the mesh draws as before. A damaged chunk is dropped
the same way. Neither stops the mesh loading.

## 7. When it is worth it

Meshlet culling saves GPU time when a mesh is **dense and often partly out of
view or hidden**: a large scan, a cliff, a building you walk round. It costs
GPU time for each culled mesh in view: a compute dispatch per mesh, then
indirect draws from the indices it kept.

On an RTX 2080 Ti at 1280 x 720, the Demo Lab statue, culled along with every
other mesh in the room, cost more than it saved: about 1-2 ms more GPU time
with occlusion and 2-3.7 ms with frustum culling alone. That GPU draws 1.35M
triangles quickly, so there was little to save
([MESHLETS_PERF.md](../../Modules/RenderCore/MESHLETS_PERF.md)). Measure
your own scene before turning it on for a shipping game. The engine default
stays off.

- Use `MeshletOcclusion` with `Meshlets`: on the statue it cost less than the
  frustum alone, since what it culls more than pays for its depth pyramid.
- Turning **Build meshlets** off at import does **not** exclude a mesh from
  culling. A large enough mesh builds them at load anyway (section 1). Culling
  applies to all meshes or none, through `[Rendering] Meshlets`.

## 8. Pitfalls

- **A one-sided open mesh seen from behind** loses triangles with
  `MeshletCones` on. Leave cones off unless every culled mesh is closed.
- **A material that moves vertices further than its output node's Max
  Offset** can be culled while still on screen, as with the CPU's culling.
  Set Max Offset to the largest offset the material makes
  ([MATERIALS.md](../../Modules/RenderCore/MATERIALS.md)).
- **Skinned and auto-instanced meshes** are never culled by meshlet: they draw
  whole. Splitting a character into static parts does not help either; its
  bones move them.
- **`MeshletStats` stalls the GPU** every frame. Turn it off before timing
  anything.
- **The D3D12 Captures span** counts some render-thread time, so it reads
  higher than Vulkan's. Compare GPU spans on the same backend
  ([MESHLETS_PERF.md](../../Modules/RenderCore/MESHLETS_PERF.md)).
