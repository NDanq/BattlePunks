# Materials design

Material templates, permutations, uniforms and the shader cache. DESIGN.md's
"Materials" section describes the first cut (one pixel shader per graph, one
`mat4` of parameters); this document is what replaced it.

## Overview

A material graph no longer generates a whole shader. It generates a
**material function** (`.mtl.glsl`) and a description (`.material`), both
stored in the graph file's `<gen_code>` -- nothing is written beside the graph.
Engine-owned **templates** in `Engine/Content/Shaders` supply `main()` for each
stage and include the function. The cook reads the graph's `<gen_code>`,
compiles each template once per **permutation** the description asks for, and
packs the results into a per-module **shader cache** that RenderCore mounts
with the module.

**Names.** A material is named by its graph's runtime path,
`"Materials/Glow.material.nmgraph"` (the engine's DefaultLit is
`"Engine/Content/Materials/DefaultLit.material.nmgraph"`). Its cooked files
are named by that path without the extension, the *stem*
(`GetMaterialStem`): `Materials/Glow.material`, and
`Shaders/Cooked/<Platform>/Materials/Glow@<key>...`. RenderComponent and
PostProcessComponent save the full path. A bare name (`"Glow"`) means
`Materials/Glow`: an imported mesh's own section names resolve that way
(quietly, if there is no such material). Names hold up to `RENDER_MATERIAL_PATH_MAX` (128) bytes.

```
Glow.material.nmgraph --(NodeGraph, on save)--> <gen_code>: .mtl.glsl + .material
                                              |
       MaterialTemplate_VS/PS.glsl + defines per permutation key
                                              |
                       cook_material_graphs.py --> loose files (Editor targets)
                                              \--> Shaders/Cache/<Module>.<Platform>.nmsc
```

## Shader-side names

The project avoids Unreal's `F` prefix. GDI's own structs carry `GDI`; engine
shader types carry no prefix:

| Name | What |
|---|---|
| `MaterialInputs` | What input nodes read, built by each stage's template (`Inputs.TexCoord`, `Inputs.Color`, ...). Pixel-only fields exist only under `#if PIXEL_SHADER`. |
| `MaterialSurface` | What the pixel function writes: BaseColor, Metallic, Roughness, Specular, Normal, Emissive, Opacity, OpacityMask, AmbientOcclusion. |
| `DefaultMaterialSurface()` | Its defaults; the Normal is the vertex normal. |
| `CalcMaterial(in MaterialInputs, inout MaterialSurface)` | The generated pixel function. |
| `CalcMaterialOffset(in MaterialInputs)` | The generated vertex function: World Position Offset. |
| `ShadeSurface(...)` | Lighting.glsl's PBR shading. |

All in `MaterialCommon.glsl`, `SceneUniforms.glsl`, `Lighting.glsl` and the two
templates, beside the engine's other shaders.

## Domains and shading

| Domain | Stage (Lit / Unlit) | Blend | Depth write | Notes |
|---|---|---|---|---|
| Opaque | Base Pass / Unlit | off | on | |
| Masked | Base Pass / Unlit | off | on | `discard` when OpacityMask < Clip Value |
| Translucent | Translucent Lit / Translucent Unlit | alpha | off | sorted back to front |
| PostProcess | PostProcess | off | off | full-screen, Unlit only, samples Scene Color; no WPO |

The domain and shading are the key to where a material draws:
[RENDERPIPELINE.md](RENDERPIPELINE.md) has the stages, their draw policies and
deferred lighting. Opaque and Masked **Lit** materials write the G-buffer in the
Base Pass and are lit by the Lighting stage's full-screen pass; everything else
writes its colour directly. Plain tinted quads and meshes drawn with Mesh_PS go
to the Unlit stage, in their old blended, depth-written state.

Shading is **Lit** (Emissive plus `ShadeSurface`) or **Unlit** (Emissive is the
colour). The legacy Pixel Output node compiles as Unlit + Translucent, which is
how every material drew before (the scene blended everything).

Two Sided does not change culling -- GDI culls nothing on any backend -- it
flips the normal of back faces (`gl_FrontFacing`) so they light correctly.
A cull mode in GDI's draw state is future work.

## Permutations

A permutation key is a `uint32_t` (`MaterialDescription.h`):

| Bits | Field | Values |
|---|---|---|
| 0-2 | Domain | Opaque 0, Masked 1, Translucent 2, PostProcess 3 |
| 3-4 | Shading | Lit 0, Unlit 1 |
| 8-10 | Vertex input | Quad 0, Mesh 1, Fullscreen 2 (vertex stage only) |
| rest | reserved | quality levels, static switches |

Each field becomes 0/1 defines (`MATERIAL_DOMAIN_MASKED`, `MATERIAL_SHADING_LIT`,
`MATERIAL_VERTEX_MESH`), because the cook's preprocessor has no `#elif`. Files
are named `<Name>@<key as 8 hex digits>.vsh/.fsh`.

**Which keys a material builds:** its own Domain and Shading for the pixel
stage -- one pixel shader per material. (An Unlit sibling of a Lit material,
for an unlit view mode, was considered and left out until something draws
with it.)

Vertex stage: only a material whose World Position Offset is linked compiles
its own vertex shaders, one per vertex input it can be drawn with (Quad,
Mesh). Every other material shares the engine's `MaterialDefault` vertex
shaders (Quad, Mesh, Fullscreen), so most materials add no vertex shaders.

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
| 2 | `<Name>_Block`: `Material_Tint`, `Material_ClipValue`, `Material_TwoSided`, two reserved floats, then `Param_<Name>` for each parameter | RenderContext, per draw | MaterialSchema (offsets in `.material`) |
| 3 | `Scene_Block`: camera, time, light, ambient, and the inverse view-projection (the Lighting stage's) | RenderContext, per frame | SceneUniforms.glsl, mirrored by `SceneUniforms` in RenderContext.cpp with static_asserts |
| 4-11 | Material textures (slot 0-7); slot 0 is Scene Color for PostProcess | RenderContext | fixed |

Both blocks are visible to both stages, so World Position Offset can read
parameters and time. Parameters are float, vec2, vec3 or vec4, in name order;
the block is capped at 1024 bytes (a compile error past it). A material
without an offset never declares its block in a vertex stage: the shared
vertex shaders include `MaterialDefault.mtl.glsl`, which declares nothing, so
OpenGL never links two different blocks under one name.

Shaders written against the old name-based uniforms (Basic, Mesh, Screen,
Odessa's) are unaffected.

## Stages and node restrictions

World Position Offset is the only vertex-stage root; every other Material
Output pin and every Discard node is pixel-stage. The compiler compiles each
stage from its own roots, so a node reached from both is emitted in both
functions (temporaries numbered per stage); parameters are collected once.

`NodeDefinition::StageMask` and `PinDefinition::Stage` are schema-defined bit
masks (Material: Vertex 1, Pixel 2; Script leaves them at all).
`Schema::GetRequiredStages` / `GetAvailableStages` / `CanLinkStages` reason
about them generically. Enforced:

- **Compiler** (authoritative, covers hand-edited files): a pixel-only node
  that reaches World Position Offset is an error on that node.
- **Editor links:** `GraphDocument::PlanLink` refuses a link that would carry
  a pixel-only value into a vertex root, even through intermediate nodes.
- **Editor palette:** dragging from an input that leads to the vertex stage
  shows pixel-only nodes greyed out, with the reason as their tooltip.
- **Editor nodes:** a stage-limited node's header says so ("Pixel").

| Input node | Vertex | Pixel |
|---|---|---|
| Texture Coordinate, Vertex Color, Vertex Normal | yes | yes |
| World Position (before the offset in the vertex stage) | yes | yes |
| Parameter, Instance Tint, Viewport Size, Mouse, Time | yes | yes |
| Math, Vector, Logic | yes | yes |
| Pixel Coordinate, Pixel Depth, Clip Position | no | yes |
| Scene Texture (PostProcess only) | no | yes |
| Texture Object, Texture Sample (level 0 in the vertex stage) | yes | yes |

**Time** reads `Scene_Time`: seconds since the renderer started (real time,
so editor previews animate), and the last frame's length.

## World Position Offset

`MaterialTemplate_VS.glsl` computes the world position, adds
`CalcMaterialOffset(Inputs)` when `MATERIAL_HAS_OFFSET`, then applies view and
projection. Normals are not recomputed after the offset. The Material Output
node's **Max Offset** (world units, set by hand) pads the proxy's culling
bounds (`RenderProxy::GetBoundsPadding`) so displaced geometry does not pop at
screen edges. The editor's CPU mesh picking ignores the offset.

## Interpolants

`Interp_Block` (Common.glsl) gained, after `Normal` so older pixel shaders still
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
`MCOL` chunk (older files load white); the glTF importer reads `COLOR_0`.

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
Texture 1 Default Default Content/Textures/Brick.nmtexture
Texture 2 Nearest Clamp Content/Textures/Height Map.nmtexture
```

Offsets are bytes into the material block; the first 32 are the engine's.
`Texture <slot> <filter> <wrap> <path>`: a material texture slot (1-7), the
node's sampler settings (`Default` keeps the asset's own), and the texture's
runtime path -- the rest of the line, so it may hold spaces (not `#`).
`ParseMaterialDescription` refuses version 1 (recompile the graph) and skips
lines it does not know.

## Textures

A material graph's **Texture Object** node (NodeGraph's README) names a
`.nmtexture` and, optionally, its own Filter and Wrap; any number of **Texture
Sample** nodes read it, each at its own UV. The renderer, per draw:

- binds slots 1-7 from the material's `Texture` lines, clearing the rest; slot
  0 stays the caller's (a PostProcess pass's scene);
- loads each file through `TextureAssetCache` (shared, thread-safe, like
  `MeshAssetCache`) and makes one GPU texture per path and *resolved* sampler
  -- the node's setting, else the asset's -- rebuilt when the cache's copy
  changes. The editor invalidates it when it stages a texture, so a reimport
  shows next frame;
- a texture that cannot be loaded is warned once and its slot reads the
  backend's transparent black.

Texels are read as stored: a colour texture is display space, like every
colour a material authors, so nothing converts it (`sRGB` on a texture marks
colour data for later, for an sRGB format). The image's top row is v = 0 --
glTF's convention, so imported meshes need no flip -- and a quad shows it
upright. No mipmaps on Vulkan and D3D12 yet (OpenGL generates them): Linear
filters between texels at the full-size level.

The editor previews a material on the engine's meshes (Sphere, Box, Plane) or a
quad, and any mesh with a material per section, through
`Renderer::RequestMeshPreview`: an orbiting perspective camera framing the
mesh's bounds, a fixed studio light, the same stages as the viewport.

`Games/Sandbox/Materials/Textured` samples `Content/Textures/Quadrants.nmtexture`
(red, green / blue, white) with its Linear filter overridden to Nearest; the GPU
smoke check reads its four quarters and its centre.

## The shader cache

One file per module per backend: `Shaders/Cache/<Module>.<Platform>.nmsc`,
`EngineCore::Archive` chunks (`ShaderCache.h`):

| Chunk | Payload |
|---|---|
| `NMSC` | string Module, string Platform, u32 `MATERIAL_TEMPLATE_VERSION` |
| `BLOB` | u64 FNV-1a hash, u32 size, the bytes -- one per distinct content |
| `FILE` | string path, u64 hash -- one per file |

The files are exactly what a loose cook writes, at their runtime paths:
`Shaders/Cooked/<Platform>/<stem>@<key>...` and `<stem>.material` --
`Materials/Glow` for a game's graph, `Engine/Content/Materials/DefaultLit` for
the engine's. So a shipped game needs no loose material files,
and nothing that loads a shader or a material knows which it read.

Written by `cook_material_graphs.py -cache`, read by `ReadShaderCache`, which
checks every blob's hash.

**Mounting.** Every backend reads shader bytes through
`EngineCore::IFileProvider`. NimbleEngine and the editor install a
`LayeredFileProvider` (Core) over the disk; a mounted cache is a
`MemoryFileProvider` layer on it, newest first. Editor builds ask the disk
first instead, so a material the editor just cooked beats the last build's
cache.

- `Renderer::Initialize` mounts `Engine` (DefaultLit and the shared vertex
  shaders, from `Engine/Content/Materials`).
- `GameModuleHost::Load` mounts `<ModuleName>` once the module has started;
  `Unload` unmounts it first. The host does it, not the module DLL, so
  nothing points into an unloaded image (CoreModule.h, RELOAD HAZARDS). Both
  the game launcher and Play-in-Editor go through it.
- Mount and unmount run on the render thread; every material is retired and
  reloads through the new layers.

A later mount shadows an earlier one: game materials override engine
materials of the same name. A cache cooked for another platform or template
version is refused and logged, and its materials fall back to loose files.

**Building them.** `nimble_stage_materials(TARGET <game> DIR <folder> MODULE
<name>)` cooks a game's caches; `NimbleStageShaders` cooks the Engine cache.
Editor targets also stage loose files; Client and Server targets stage only
the caches.

## Lighting

The first active `DirectionalLightComponent` (colour, intensity; it shines
along its entity's forward) and `SkyLightComponent` (sky and ground ambient),
and up to 16 `PointLightComponent`s and `SpotLightComponent`s, built by
WorldBridge into `RenderLighting`. Without a directional light the scene uses a
default light matching the old `Mesh.fsh` (from above, -Y). Shadows, and how
the lights are gathered and capped: RENDERPIPELINE.md's "Lights" and "Shadows". `Lighting.glsl`: GGX
distribution, Smith-Schlick geometry, Schlick Fresnel; BaseColor, Emissive
and light colours are authored in display space, lit linear, and encoded back
(no HDR target yet). Meshes without a material draw with the engine's
`DefaultLit` (Tint times vertex colour, Lit). Opaque and Masked Lit surfaces
are shaded once per pixel by `DeferredLighting.fsh` from the G-buffer;
Translucent Lit ones by their own pixel shader. Both call `ShadeSurface`, so
the same surface lights the same either way (RENDERPIPELINE.md).

## PostProcess

`PostProcessComponent`s (material, priority; at most 8 run, low priority
first) are the PostProcess stage's draws: each samples the previous result as
material texture 0 and writes the other of two targets (the scene colour and a
second one made on first use). The last target written stays bound, which is
what the present shows. A material of another domain is warned about and
skipped. A preview of a PostProcess material runs it over the preview's clear
colour.

## GDI changes

- `GDIDynamicDriver::SetUniformBlock(Binding, Data, Size)` (2 or 3) and
  `SetMaterialTexture(Slot, GDITextureResource*)`: draw state, copied.
- `GDIShaderResource::SetMaterialLayout(true)`: a draw whose vertex or pixel
  shader has it uses the material layout instead of the name-based one.
- `UBO_BLOCK_BEGIN/END` and `MATERIAL_SAMPLER2D` macros (engine copies of
  OpenGLCommon/VulkanCommon.glsl): real std140 blocks and bound samplers on
  OpenGL too.
- Vulkan: descriptor layout group `DLG_Material` (UBO 0-3, samplers 4-11).
- D3D12: root parameters for b3 and t4-t11/s4-s11.
- OpenGL: a UBO per binding, `glBindBufferBase`; texture units 4-11.
- Vertex colour at location 9 on Vulkan, D3D12 and OpenGL.
- Metal: not done. The engine does not cook Metal, and Metal has no indexed
  meshes yet.
- Several colour targets in one pass and depth as a texture, for the
  G-buffer: RENDERPIPELINE.md, "GDI".
- `cook_shaders.py`'s `#if` evaluator handles parentheses and C precedence
  (`A && (B || C)`, `!A && B`); it used to read both wrongly, and silently.

## Status and verification

| Backend | Compiles | Verified drawing |
|---|---|---|
| Vulkan | yes | yes -- RenderCore smoke `NIMBLE_SMOKE_GPU=Vulkan` renders DefaultLit and Wave (WPO) through the G-buffer, Glow and Negative (PostProcess) through the caches and checks their pixels (clean under the validation layer); Sandbox (Editor and Client targets) loads every material and its cache |
| D3D12 | yes (every permutation through spirv-cross and fxc; registers match the root signature) | yes -- the same smoke check with `NIMBLE_SMOKE_GPU=D3D12` gives Vulkan's pixels; Sandbox and the editor viewport (`--rhi 4`) run on it |
| OpenGL | yes (preprocessed; the backend's multiple targets and depth textures compile) | no -- the host does not create a WGL context yet |
| Metal | -- | -- |

Tests: NodeGraph (codegen goldens including Wave's two stages, std140
offsets, stage errors, `CanLinkStages`, PostProcess and domain rules),
`NodeGraph.MaterialGLSL` (every permutation through glslc), RenderCore smoke
(`.material` v2, block layout, permutation names, cache reader, layered
providers, vertex colours, stage routing and material slots, the RenderComponent
serializer's versions, `Texture` lines; the opt-in GPU check, which draws
Textured upright and with its node's filter), Editor.Import (`COLOR_0`), and
NodeGraph's texture codegen (Texture Objects and their slots, several samples
of one, Size, the vertex stage, the errors, upgrading version 1 Texture
Samples; `Textured.mtl.glsl` through glslc).

## Later

Texture parameters (a texture an instance or a component can swap); mipmaps
(and a Mip Level input); separate U and V wrap; tangents for normal maps; an RGBA16F scene target and tonemapping (needs a render-target
format in GDI); material instances; a cull mode; more light types; Scene
Depth for PostProcess (depth targets can be textures now; a node and a slot
remain); an Unlit view mode (and its permutation); OpenGL bring-up through the
window path.
