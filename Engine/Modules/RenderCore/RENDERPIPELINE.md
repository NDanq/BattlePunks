# The render pipeline: stages, draw policies, deferred lighting

How a frame is drawn: which stage each draw belongs to, how the material
decides that, and what each stage does. Materials themselves (templates,
permutations, blocks, caches) are in [MATERIALS.md](MATERIALS.md); the thread
model and proxies in [DESIGN.md](DESIGN.md).

## Stages

A frame runs these stages, in this order (`ERenderStage`, RenderPipeline.h):

| Stage | Draws | Into | State |
|---|---|---|---|
| **PrePass** | nothing yet | -- | culls proxies against the view (padded by World Position Offset); what survives submits its draws to the stages below. Occlusion queries and a depth prepass belong here later |
| **Base Pass** | Opaque and Masked, **Lit** | the G-buffer, and depth | depth test and write; no blending. Writes no colour to the scene |
| **Lighting** | one full-screen pass | scene colour | reads the G-buffer and depth; lit pixels only |
| &nbsp;&nbsp;Shadows | nothing yet | -- | a sub-stage of Lighting, run first: shadow casters for the directional light, later |
| **Unlit** | Opaque and Masked, **Unlit**; plain quads; tinted meshes | scene colour, with the Base Pass depth | depth test and write; plain quads keep their alpha blend |
| **Translucent Lit** | Translucent, **Lit** | scene colour | depth test, no write, alpha blend; back to front; forward-shaded |
| **Translucent Unlit** | Translucent, **Unlit** | scene colour | as above |
| **PostProcess** | PostProcess materials (Post Process components) | ping-pong scene colour targets | full screen; Priority order |

Translucent Lit runs after Lighting, not before it: a deferred resolve writes
every lit pixel, so anything drawn earlier would be overwritten, and a
translucent surface cannot be in the G-buffer (one surface per pixel). It is
shaded forward, by the same `ShadeSurface` the resolve uses.

The two translucent stages are sorted separately, each back to front. A
Translucent Unlit surface behind a Translucent Lit one therefore draws over it;
use one shading model for surfaces that overlap, or move them apart.

## The material is the key

A draw's stage comes from its material's Domain and Shading only
(`GetMaterialRenderStage`):

| Domain | Lit | Unlit |
|---|---|---|
| Opaque, Masked | Base Pass | Unlit |
| Translucent | Translucent Lit | Translucent Unlit |
| PostProcess | PostProcess | PostProcess |

Draws with no usable material -- a plain tinted quad, or a mesh drawn with
`Mesh.fsh` because DefaultLit is missing -- go to **Unlit**. A PostProcess
material on a mesh or quad is still refused (warned) and drawn as the tint.

## Draw policies

Each stage has a **`DrawPolicy`** (Private/RenderCore/DrawPolicy.h): the stage,
how it sorts, and the array of draws queued for it this frame.

```cpp
class DrawPolicy
{
    ERenderStage             Stage;
    EDrawSort                Sort;      // Submission, BackToFront
    std::vector<DrawCommand> Draws;
};
```

A `DrawCommand` is what the old `DrawItem` was: a quad, a material quad, a mesh
section or a full-screen pass, with its transform, tint, material, mesh and
index range, and its view depth for sorting. `RenderContext` owns one policy
per stage. Between `BeginSceneDraws` and the end of the frame, `DrawQuad`,
`DrawQuadWithMaterial`, `DrawMesh` and the post-process submission append to
the policy their material names; the stages then run in order, each sorting its
array and issuing it. Outside a scene (tests) draws are issued at once, as
before.

`RenderContext::GetStageDrawCount(Stage)` reports how many draws each stage
issued in the last frame -- the tests' view of the routing.

## Multiple materials per component

`RenderComponent` has **`Materials[8]`**, a slot per mesh section
(`RENDER_MATERIAL_SLOTS`). Section *i* draws with:

1. slot *i*, if it names a material that loads;
2. else the section's own material from the mesh (`MeshSection::Material`);
3. else DefaultLit; else `Mesh.fsh` with the tint.

A quad (no mesh) uses slot 0. Sections past the eighth use step 2 onwards.
Every section can land in a different stage: a mesh with a Lit body and an
Unlit, Translucent visor draws the body in the Base Pass and the visor in
Translucent Unlit.

Culling bounds are padded by the largest Max Offset among the materials a
proxy actually draws with.

**Fields.** The slots are `Material0` to `Material7` in saved worlds and Lua
(GameCore's `Bind.Element(name, &T::Array, index)`), and one **Materials** list
in the editor's Details (`Props.Array`): entries are added as needed, each with
the material picker. Lua keeps
`Material` as another name for slot 0. A component saved before slots
(serializer version 1, one `Material` field) loads it into slot 0 -- which
is a change for a multi-section mesh: that field used to override every
section, and now covers only the first.

The editor's "Apply to Selected Entity" (material graph window) sets slot 0.

## Deferred lighting

### The G-buffer

Four RGBA8 colour targets beside the scene colour, all the viewport's size, and
the scene colour target's depth:

| Target | R | G | B | A |
|---|---|---|---|---|
| GBuffer0 | Base Color | | | Ambient Occlusion |
| GBuffer1 | Normal (world, ×0.5+0.5) | | | 1 = lit pixel |
| GBuffer2 | Metallic | Roughness | Specular | -- |
| GBuffer3 | Emissive | | | -- |

Colours are stored display space, as authored, exactly what `ShadeSurface`
already takes. Eight-bit normals band a little on smooth, glossy surfaces;
RGBA16F targets need a render-target format in GDI (MATERIALS.md, "Later").

### Base Pass

The Opaque and Masked Lit permutations of `MaterialTemplate_PS.glsl` write the
four G-buffer outputs instead of a lit colour (`MATERIAL_GBUFFER`). The
permutation key does not change: those two domain/shading pairs only ever draw
in the Base Pass. Masked still discards below Clip Value; Two Sided still flips
a back face's normal, before it is stored.

### Lighting

`Engine/Content/Shaders/DeferredLighting.fsh`, with the shared full-screen
vertex shader (`MaterialDefault`, Fullscreen input) and the material binding
layout:

- textures 4-8: GBuffer0-3 and the scene depth, read with `texelFetch`;
- binding 3, the scene block, which gains `Scene_InvViewProjection`.

For each pixel whose GBuffer1 alpha is set it rebuilds the world position from
depth -- every backend's depth is GL's window depth, `z_ndc = 2d - 1`, because
Vulkan and D3D12 remap the projection to it (`PackViewParamsUBO`) -- and writes
`ToDisplay(ToLinear(Emissive) + ShadeSurface(...))`, what the forward template
wrote. Other pixels are discarded, so the clear colour shows.

The result matches the forward path for the same material: the GPU smoke check's
DefaultLit, Glow and Wave pixels are unchanged.

### Previews

A material preview runs the same stages into its own small set of targets, so an
Opaque Lit material previews through the G-buffer like the viewport draws it.

## GDI

Deferred shading needs two things GDI did not have:

- **Several colour targets in one pass.** `GDISetRenderTargets(Colors, Count,
  DepthFrom)` binds up to `GDI_MAX_COLOR_TARGETS` (4) colour targets and,
  separately, the depth attachment of `DepthFrom` (or none).
  `GDISetRenderTarget(RT)` is `GDISetRenderTargets(&RT, 1, RT)`.
  Every target must be the same size. `GDIClear` clears every bound colour
  target. Blend and colour-mask state apply to all of them. Pipelines are keyed
  by the colour count and whether depth is bound.
- **Depth as a texture.** `GDIRenderTarget::ToDepthTexture2D()` wraps the
  target's depth for sampling (point-filtered). A target whose depth is not the
  bound depth attachment is kept sampleable, as colour targets already are.

| Backend | Several targets | Depth texture |
|---|---|---|
| Vulkan | dynamic rendering with N colour attachments; pipelines' `colorAttachmentCount` | D32_SFLOAT with SAMPLED usage; DEPTH_ATTACHMENT <-> SHADER_READ_ONLY |
| D3D12 | `OMSetRenderTargets` with N RTVs; PSO `NumRenderTargets` | R32_TYPELESS resource, D32_FLOAT DSV, R32_FLOAT SRV; DEPTH_WRITE <-> PIXEL_SHADER_RESOURCE |
| OpenGL | a shared framebuffer, colour textures attached per bind, `glDrawBuffers` | a depth texture instead of a renderbuffer |
| Metal | first target only (logged once) | none |

Verification: Vulkan and D3D12 render the GPU smoke check; OpenGL compiles (the
host still cannot create a GL context); Metal is unchanged and untested. The
Null backend accepts any binding and draws nothing, as before.

The G-buffer template needed `MATERIAL_SHADING_LIT && (MATERIAL_DOMAIN_OPAQUE ||
MATERIAL_DOMAIN_MASKED)`, which `cook_shaders.py`'s own preprocessor read as
false -- it had no parentheses, and put `!` above `&&` -- so every permutation
compiled, as the forward variant. It now has both.

## Tests

- Routing (Null backend, materials from memory): each domain/shading pair lands
  in its stage; a two-section mesh with two slots draws in two stages; an empty
  slot falls back to the section's material; a quad uses slot 0; the bounds
  padding takes the largest offset of the slots in use.
- Serialization: version 1's `Material` loads into slot 0, and only there;
  version 2 writes `Material0`-`Material7` and reads them back.
- Culling: a mesh past the view's edge is kept while a slot's World Position
  Offset reaches it, and culled without.
- GPU (`NIMBLE_SMOKE_GPU=Vulkan|D3D12`): previews of DefaultLit and Wave go
  through the G-buffer and keep their forward-shaded pixels (186 188 192; 40
  103 172); Glow (Translucent Unlit) is unchanged; Negative (PostProcess) inverts
  the preview's clear colour. Clean under the Vulkan validation layer.

## Later

Shadows (a shadow map for the directional light, in the Shadows sub-stage);
occlusion queries and a depth prepass in the PrePass; per-section bounds for
culling; RGBA16F G-buffer and scene colour; more light types, as further passes
over the G-buffer.
