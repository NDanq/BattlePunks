# Materials design

Material templates, permutations, uniforms and the shader cache. DESIGN.md's
"Materials" section describes the first cut (one pixel shader per graph, one
`mat4` of parameters); this document is what replaced it.

## Overview

A material graph no longer generates a whole shader. It generates a
**material function** (`<Name>.mtl.glsl`) and a description (`<Name>.material`).
Engine-owned **templates** in `Engine/Content/Shaders` supply `main()` for each
stage and include the function. The cook compiles each template once per
**permutation** the material needs, and packs the results into a per-module
**shader cache** that RenderCore mounts with the module.

```
Glow.material.nmgraph --(NodeGraph)--> Glow.mtl.glsl + Glow.material
                                              |
            MaterialTemplate.vsh/.fsh + defines per permutation key
                                              |
                       cook_material_graphs.py --> loose files (editor)
                                              \--> Shaders/Cache/<Module>.<Platform>.nmsc
```

## Shader-side names

The project avoids Unreal's `F` prefix. GDI's own structs carry `GDI`; engine
shader types carry no prefix:

| Name | What |
|---|---|
| `MaterialInputs` | What input nodes read, built by each stage's template (`Inputs.TexCoord`, `Inputs.Color`, ...). Pixel-only fields exist only under `#if PIXEL_SHADER`. |
| `MaterialSurface` | What the pixel function writes: BaseColor, Metallic, Roughness, Specular, Normal, Emissive, Opacity, OpacityMask, AmbientOcclusion. |
| `DefaultMaterialSurface()` | Its defaults. |
| `CalcMaterial(in MaterialInputs, inout MaterialSurface)` | The generated pixel function. |
| `CalcMaterialOffset(in MaterialInputs)` | The generated vertex function: World Position Offset. |
| `ShadeSurface(...)` | Lighting.glsl's PBR shading. |

## Domains and shading

| Domain | Pass | Blend | Depth write | Notes |
|---|---|---|---|---|
| Opaque | 1 | off | on | |
| Masked | 2 | off | on | `discard` when OpacityMask < Clip Value |
| Translucent | 3 | alpha | off | sorted back to front |
| PostProcess | after the scene | off | off | full-screen, Unlit only, samples Scene Color; no WPO |

Shading is **Lit** (Emissive plus `ShadeSurface`) or **Unlit** (Emissive is the
colour). The legacy Pixel Output node compiles as Unlit + Translucent, which is
how every material drew before (the scene blended everything).

Two Sided does not change culling -- GDI culls nothing on any backend -- it
flips the normal of back faces (`gl_FrontFacing`) so they light correctly.
A cull mode in GDI's draw state is future work.

## Permutations

A permutation key is a `uint32_t`:

| Bits | Field | Values |
|---|---|---|
| 0-2 | Domain | Opaque 0, Masked 1, Translucent 2, PostProcess 3 |
| 3-4 | Shading | Lit 0, Unlit 1 |
| 8-10 | Vertex input | Quad 0, Mesh 1, Fullscreen 2 (vertex stage only) |
| rest | reserved | quality levels, static switches |

Each field becomes 0/1 defines (`MATERIAL_DOMAIN_MASKED`, `MATERIAL_SHADING_LIT`,
`MATERIAL_VERTEX_MESH`), because the cook's preprocessor has no `#elif`.

**Which keys a material builds (decided):** its own Domain and Shading for the
pixel stage, plus an Unlit sibling of a Lit material (for an unlit view mode
and as a fallback). Building every combination for every material would
triple cook time and cache size for permutations nothing draws.

Vertex stage: only a material whose World Position Offset is linked compiles
its own vertex shaders, one per vertex input it can be drawn with (Quad, Mesh).
Every other material shares the engine's `MaterialDefault` vertex shaders, so
most materials add no vertex shaders at all.

## Uniforms

GDI used to pack a shader's custom uniforms back to back in hash-map order,
with no std140 alignment and a 256-byte cap -- reliable for exactly one
uniform, hence the old single `mat4 MaterialParams`. Materials now use **raw
uniform blocks**: the material compiler lays the block out by std140 rules and
records each member's byte offset, and the runtime fills a byte buffer that GDI
uploads verbatim.

| Binding | Block | Filled | Layout source |
|---|---|---|---|
| 0, 1 | View, Transform | GDI, per draw | Common.glsl (unchanged) |
| 2 | `Material_Block`: `Material_Tint`, `Material_ClipValue`, then user parameters | RenderContext, per draw | MaterialSchema (offsets in `.material`) |
| 3 | `Scene_Block`: camera, time, light, ambient | RenderContext, per frame | SceneUniforms.glsl, mirrored by `SceneUniforms` in C++ with static_asserts |
| 4-11 | Material textures (slot 0-7); slot 0 is Scene Color for PostProcess | RenderContext | fixed |

Both blocks are visible to both stages, so World Position Offset can read
parameters and time. Parameters are float, vec2, vec3 or vec4; the block's
size is capped at `MATERIAL_BLOCK_MAX_BYTES` (1024), a compile error past it.

GDI's side (`GDISetUniformBlock`, `GDISetMaterialTexture`, and a shader
resource flag that selects the material binding layout) is described in
"GDI changes" below. Shaders written against the old name-based uniforms
(Basic, Mesh, Screen, Odessa's) are unaffected.

## Stages and node restrictions

World Position Offset is the only vertex-stage root; every other Material
Output pin and every Discard node is pixel-stage. The compiler walks back from
each root and tags nodes with the stages that reach them. A node reached from
both is emitted in both functions (temporaries numbered per stage); parameters
are collected once.

`NodeDefinition::StageMask` and `PinDefinition::Stage` are schema-defined bit
masks (Material: Vertex 1, Pixel 2; Script leaves them at all). Enforced:

- **Compiler** (authoritative, covers hand-edited files): a pixel-only node
  that reaches World Position Offset is an error on that node.
- **Editor links:** `Schema::CanLink` is called from `GraphDocument::PlanLink`;
  the material schema refuses a link that would carry a pixel-only value into
  a vertex root, even through intermediate nodes.
- **Editor palette:** dragging from a vertex path greys out pixel-only nodes.

| Input node | Vertex | Pixel |
|---|---|---|
| Texture Coordinate, Vertex Color, Vertex Normal | yes | yes |
| World Position (before the offset in the vertex stage) | yes | yes |
| Parameter, Instance Tint, Viewport Size, Mouse, Time | yes | yes |
| Math, Vector, Logic | yes | yes |
| Pixel Coordinate, Pixel Depth, Clip Position | no | yes |
| Scene Texture (PostProcess only) | no | yes |

**Time** reads `Scene_Block.Time`: seconds since the renderer started (real
time, so editor previews animate), plus a Delta output.

## World Position Offset

`MaterialTemplate.vsh` computes the world position, adds
`CalcMaterialOffset(Inputs)` when `MATERIAL_HAS_OFFSET`, then applies view and
projection. Normals are not recomputed after the offset. The Material Output
node's **Max Offset** (world units, set by hand -- decided) pads the proxy's
culling bounds so displaced geometry does not pop at screen edges. The
editor's CPU mesh picking ignores the offset.

## Interpolants

`Interp_Block` (Common.glsl) gains, after `Normal` so older pixel shaders still
read a valid prefix:

```glsl
vec4  Color;          // vertex colour; white when the mesh has none
vec3  WorldPosition;
float ViewDepth;      // linear, positive in front of the camera
```

`Position` stays clip space (the old "Interpolated Position" node is renamed
Clip Position; its type id is unchanged so saved graphs load).

Vertex colour is a mesh attribute at location 9 (`GDIMeshDesc::Colors`, RGBA
float); a mesh without colours gets white from `GDIMakeIndexedRenderData`, so
every backend's vertex layout is fixed. `.nmmesh` stores it as an optional
chunk; the glTF importer reads `COLOR_0`.

## The `.material` file, version 2

```
NimbleMaterial 2
Domain Opaque
Shading Lit
TwoSided 0
ClipValue 0.5
MaxOffset 0
HasOffset 0
BlockSize 48
Param Color vec3 32 0.2 0.8 1 0
Param Intensity float 44 1 0 0 0
```

Offsets are bytes into `Material_Block`. The first 32 bytes are the engine's
(`Material_Tint` vec4 at 0, `Material_ClipValue` float at 16).

## The shader cache

One file per module per backend: `Shaders/Cache/<Module>.<Platform>.nmsc`,
an `EngineCore::Archive` chunked file like `.nmmesh`:

- header: magic, format version, backend, `MATERIAL_TEMPLATE_VERSION`;
- one record per material: name, domain, shading, flags, uniform layout and
  defaults -- so a shipped game needs no loose `.material` files;
- one record per entry: material index, permutation key, stage, code hash,
  offset, size;
- code, 16-byte aligned, identical code stored once.

Written by the Python cook (`-cache`), read by `ShaderCache.cpp`.

**Loading without changing GDI's load path.** Every backend reads shader bytes
through `EngineCore::IFileProvider`. NimbleEngine and the editor install a
`LayeredFileProvider` (Core); a mounted cache adds an in-memory layer that
serves the paths GDI already asks for. Lookup order: editor loose overrides
(editor builds) -> mounted caches, newest first -> disk.

**Mounting.** RenderCore mounts `Engine` at render-thread startup.
`GameModuleHost::Load` mounts `<ModuleName>` once `OnStartup` succeeds; `Unload`
unmounts it first. The host does it, not the module DLL, so nothing points
into an unloaded image (CoreModule.h, RELOAD HAZARDS). Mount and unmount run
on the render thread; unmounted materials' shaders go to the retired list.

**Name clashes (decided):** a later mount overrides an earlier one -- game
materials override engine materials of the same name -- with a warning.

A cache whose template version differs is refused and logged; the editor then
falls back to cooking loose files.

## Lighting

`DirectionalLightComponent` (colour, intensity; direction from the entity's
rotation) and `SkyLightComponent` (sky and ground ambient). Without either the
scene uses a default light matching the old `Mesh.fsh` (from above, -Y).
`Lighting.glsl`: GGX distribution, Smith-Schlick geometry, Schlick Fresnel,
gamma-encoded output (no HDR target yet).

## PostProcess

The scene renders to its back buffer target; `PostProcessComponent`s
(material, priority) then draw full-screen passes, ping-ponging between two
targets with Scene Color in texture slot 0, and the last result is copied back
to the back buffer for the present.

## GDI changes

- `GDIDynamicDriver::SetUniformBlock(Binding, Data, Size)` (2 or 3) and
  `SetMaterialTexture(Slot, GDITextureResource*)`: draw state, copied.
- `GDIShaderResource::SetMaterialLayout(true)`: a draw whose vertex or pixel
  shader has it uses the material layout instead of the name-based one.
- `UBO_BLOCK_BEGIN/END` and `MATERIAL_SAMPLER2D` macros: real std140 blocks
  and bound samplers on OpenGL too.
- Vulkan: descriptor layout group `DLG_Material` (UBO 0-3, samplers 4-11).
- D3D12: root parameters for b3 and t4-t11/s4-s11.
- OpenGL: a UBO per binding, `glBindBufferBase`; texture units 4-11.
- Vertex colour at location 9 on Vulkan, D3D12 and OpenGL.
- Metal: not done. The engine does not cook Metal, and Metal has no indexed
  meshes yet.

## Later

Texture parameters and Texture Sample nodes (the slots exist); tangents for
normal maps; an RGBA16F scene target and tonemapping (needs a render-target
format in GDI); material instances; a cull mode; more light types; Scene
Depth for PostProcess (needs depth targets as textures).
