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
| &nbsp;&nbsp;Shadows | shadow casters: Opaque and Masked mesh sections whose component casts | the two shadow atlases' depth | a sub-stage of Lighting, run first; depth test and write, no colour. See [Shadows](#shadows) |
| **Unlit** | Opaque and Masked, **Unlit**; plain quads; tinted meshes | scene colour, with the Base Pass depth | depth test and write; plain quads keep their alpha blend |
| **Translucent Lit** | Translucent, **Lit** | scene colour | depth test, no write, alpha blend; back to front; forward-shaded |
| **Translucent Unlit** | Translucent, **Unlit** | scene colour | as above |
| **PostProcess** | the post process stack: its PostProcess materials and effects (Post Process volumes) | two full-size targets in turn, and smaller ones | full screen; a fixed order. See [Post process](#post-process) |

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
| GBuffer3 | Emissive ÷ scale | | | log2(scale) / 12 |

Colours are stored display space, as authored, exactly what `ShadeSurface`
already takes. Emissive may be far past 1, so GBuffer3 stores it divided by its
brightest channel, when that is over 1, with that scale in alpha as log2 over
12 octaves, up to 4096 (`EncodeGBufferEmissive` / `DecodeGBufferEmissive` in
`Common.glsl`). The Lighting stage reads it back whole, and Bloom uses it too. Eight-bit normals band a little on smooth, glossy surfaces;
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
- textures 9-10: the directional and local shadow atlases (see [Shadows](#shadows));
- texture 11: this frame's screen-space ambient occlusion, half size (see
  [Ambient occlusion](#ambient-occlusion));
- binding 2, `Lighting_Block`: x is 1 when texture 11 holds this frame's
  occlusion, 0 otherwise;
- binding 3, the scene block, which gains `Scene_InvViewProjection`, the
  point and spot lights and the shadow matrices.

For each pixel whose GBuffer1 alpha is set it rebuilds the world position from
depth -- every backend's depth is GL's window depth, `z_ndc = 2d - 1`, because
Vulkan and D3D12 remap the projection to it (`PackViewParamsUBO`) -- and writes
`ToDisplay(ToLinear(Emissive) + ShadeSurface(...))`, what the forward template
wrote. Other pixels are discarded, so the clear colour shows.

The result matches the forward path for the same material: the GPU smoke check's
DefaultLit, Glow and Wave pixels are unchanged.

### Lights

`Lighting.glsl`'s `ShadeSurface` lights a surface with:

- **the sun**: the first active Directional Light, shining along its entity's
  forward (+Z turned by the Transform's Rotation, `LightForward` in
  `Components/LightShadow.h`). With none, the old fixed light.
- **point and spot lights**: up to `RENDER_LOCAL_LIGHT_MAX` (16) a frame,
  gathered by `WorldBridge::BuildLighting` -- shadow casters first, then nearest
  the camera; more are warned once and dropped. Falloff is inverse-square-like,
  windowed to reach zero at Radius (point) or Range (spot); a spot fades from
  its inner to its outer cone half-angle.
- **the sky**: the first active Sky Light's hemisphere ambient.

Every light shares one BRDF (`EvaluateLight`). Translucent Lit surfaces get the
same lights forward, but unshadowed: their texture slots are their material's.

### Shadows

Each light has a **Shadows** section: Cast Shadows, a Technique, a Modulated
Tint and two biases. A Render component's **Cast Shadows** says whether its
mesh casts. Every scene's quality is the `[Shadows]` config section, a
project setting (the game's DefaultEngine.ini; Edit > Project Settings >
Shadows edits it and applies it at once): Enabled, Resolution,
Filter, MaxShadowedLights and DistanceScale.

| Technique | Lights | What it draws |
|---|---|---|
| **Standard** | all | the shadow takes away that light's own contribution |
| **Modulated** | all | that light is unshadowed; the shadow multiplies everything lit there toward its tint (no extra pass) |
| **Cascaded** | directional | Standard, in 1-4 cascades along the view, split by Cascade Split Lambda (uniform to logarithmic) |

`ShadowSetup.cpp` works out a frame's shadow views (pure math, tested on the
Null backend) into two atlases, render targets of `Resolution` texels a side:

- **directional**: one orthographic view fitted around the camera's frustum out
  to Shadow Distance, or one per cascade in its 2x2 quarters. Each is fitted to
  a bounding sphere and moved in whole texels, so it does not shimmer.
- **local**: 6 x 4 tiles. A spot light takes one (a perspective view as wide as
  its outer cone, out to Range), a point light six (its cube faces). At most
  `MaxShadowedLights` (and 4) cast, in gather order.

`RenderScene::Render` then asks every visible proxy whose bounds each view can
see for its casters (`RenderProxy::DrawShadow`) -- including proxies the camera
culled. The Shadows sub-stage clears both atlases and draws each caster into
its view's tile with the material's own vertex shader (so World Position Offset
matches) and `ShadowDepth.fsh`; the colour target is masked off.

The Lighting pass samples the atlases' depth with `texelFetch` and compares by
hand (`Shadows.glsl`), Filter's 1, 9 or 25 taps kept inside the tile. GDI has
no comparison samplers, depth bias or cull mode yet, so the bias is the
receiver's: Depth Bias in the shadow map's depth, Normal Bias in world units
along the surface normal (more where the light grazes it).

Limits: Masked casters cast their whole shape (their clip is not run in the
Shadows sub-stage); a quad casts nothing; Translucent Lit surfaces are
unshadowed; Metal, which cannot sample a depth target, draws no shadows (warned
once). A material preview never draws shadows.

The scene block grew past the material block's 1024 bytes (3600 bytes:
`GDI_SCENE_BLOCK_MAX_BYTES` is 4096). GDI now re-uploads a uniform block only
when its bytes change, so the scene block is uploaded once a frame, not per draw.

### Post process

The PostProcess stage runs a **post process stack**: which effects run and
their settings, and PostProcess materials to run before or after them.

**The asset** is a `*.pp.nmgraph` (NodeGraph's PostProcess schema). Its nodes
are effects, each with its own settings and no pins. The editor compiles it to
`.ppstack` text in its `<gen_code>`, and RenderCore reads that text
(`PostProcessStack.h`: `ParsePostProcessStack`, `PostProcessStackCache`). A
game build therefore needs neither NodeGraph nor XML. Saving a stack in the
editor invalidates the cache, so the viewport updates on the next frame. A path
to a PostProcess material graph also works: it is read as a stack of that one
material.

**Volumes.** A **Post Process component** names a stack (Stack), with Priority,
Blend Distance and Enabled.
- **The volume:** its sibling **Bounds** World box. With no Bounds the stack
  applies everywhere.
- **Weight:** 1 inside the box, fading to 0 over Blend Distance outside it.
  With a Blend Distance of 0 the edge is hard.
- **Where the camera is:** the eye for a perspective view; the view's centre
  for the 2D view.
- **Blending:** `WorldBridge::BuildPostProcess` blends every volume with weight
  above 0, Priority low first (`BlendPostProcess`):
  - **numbers** lerp toward each stack by its weight, starting from the effect
    off, so Intensity and Max Blur fade in;
  - **switches** (Near Blur, Quality, Antialiasing itself) take the stack's value
    at weight 0.5 or more;
  - **an effect a stack has no node for** is left alone;
  - **material passes** join at weight 0.5 or more.

**The passes**, always in this order (Ambient Occlusion, the one effect that
is not in this chain, runs earlier: see [below](#ambient-occlusion)):

| # | Pass | Shaders | Targets |
|---|---|---|---|
| 1 | Materials, Before Effects | the material's | full size |
| 2 | Depth of Field | `PPDofPrepare` (circle of confusion from depth: far past the focus range, or nearer with Near Blur; a half-size copy, CoC in alpha), `PPDofBlur` (a 16-tap disc, each tap counted as far as its own CoC reaches), `PPDofComposite` (sharp to blurred by CoC) | `Half[0..1]`, then full |
| 3 | Motion Blur | `PPMotionBlur`: each pixel's world position, from depth, reprojected with **last frame's** view-projection gives its screen motion; taps along it, jittered, limited to Max Length | full |
| 4 | Bloom | `PPBloomDownsample` (13 taps; the first step adds back the emissive the scene clipped, from GBuffer3, and keeps what passes the threshold, with a soft knee) down `Bloom[0..Levels-1]` (1/2 to 1/64); `PPBloomUpsample` (each level's own plus a 9-tap tent of the one below, times Radius) back up `BloomUp`; `PPBloomComposite` adds the top level times Intensity and Tint | chain, then full |
| 5 | Materials, After Effects | the material's | full |
| 6 | Antialiasing | `PPFxaa` (FXAA 3.11 quality: luma edges, an end search of 4, 8 or 12 steps by Quality, plus sub-pixel smoothing) | full |

**How the passes run:**
- **Targets:** they alternate between `SceneTargets::PostProcess` and
  `PostProcessB`. The scene colour is only ever read, so its depth can be
  sampled throughout: post targets bind with no depth.
- **Small targets:** the half-size and bloom targets are made on first use and
  resize with the viewport.
- **Materials:** a PostProcess material samples the scene so far in material
  slot 0 and, through the **Scene Depth** node, the scene depth in slot 7. Its
  own textures are slots 1-6.
- **The previous view-projection** is kept per scene (`RenderScene`). There is
  none on the first frame or after a resize, and then Motion Blur skips.
- **Debug lines** (gizmos) draw after the stack, so they stay sharp.
- **Previews:** a material preview never runs the stack.

**Limits:**
- **Bloom:** the scene is RGBA8 display colour with no HDR, so its brightness
  stops at 1. Bloom recovers what an **Opaque or Masked Lit** surface's emissive
  lost to that clip from GBuffer3, so emissive 50 glows far wider than
  emissive 1. Its chain stores brightness up to 64 as `sqrt(v / 64)`
  (`EncodeBloom`), so the faint tail keeps its precision; that encoding does
  not add, so upsampling writes a second chain (`BloomUp`) instead of blending.
  Unlit and Translucent emissive is drawn forward, straight into the scene
  colour, so it still stops at 1. A white lit surface past the threshold glows
  all over, but only by its clipped 0-1. What reaches the G-buffer is bloomed
  even if a translucent surface later covers it.
- **Motion Blur:** the camera's motion only; there is no per-object velocity.
- **Antialiasing:** FXAA only; there is no temporal AA.
- **Memory:** each small target also allocates a depth it never uses (GDI makes
  every target with one).

### Ambient occlusion

A stack's **Ambient Occlusion** node is blended with the other effects, but its
passes run mid-frame, between the Base Pass and Shadows, so that only the
**ambient** light is darkened: direct light and shadows are untouched.
- **`PPAmbientOcclusion`** runs at half size into `Half[0]`. For each lit pixel
  it rebuilds the world position from the scene depth and reads the normal from
  GBuffer1. It then tests Samples points spread over the hemisphere round that
  normal, out to Radius world units, turned per pixel by interleaved gradient
  noise. Each point is projected with this frame's view-projection, which the
  pass block carries ahead of its settings, as Motion Blur's does. A point
  counts as occluded when the scene's surface at that pixel is nearer the eye
  than the point is, fading out once that surface is more than Radius away, so
  a distant wall does not darken a silhouette. The result is
  `pow(1 - occluded fraction, Power)`. Intensity fades it in up to 1 and
  deepens it past 1.
- **`PPAmbientBlur`** (when Blur is on) averages 4 x 4 half-size texels into
  `Half[1]`, weighting each tap by how close its distance is to the centre's,
  so occlusion does not bleed across edges.
- **The Lighting stage** samples the result bilinearly and multiplies it into
  `Surface.AmbientOcclusion`, the material's own AO. `ShadeSurface` applies
  that to the indirect term only.
- **Scope:** Opaque and Masked Lit surfaces, the G-buffer's. Forward
  (Translucent, Unlit) surfaces get none. With no G-buffer, or on a backend
  that cannot sample depth, it is skipped. `Half[0..1]` are free again by the
  time Depth of Field uses them.

### Stat unit

**Stats.Unit** (config var, off by default) draws frame, game thread and
render thread times, and FPS, in a panel at the top right of every viewport.
- **Toggling it:** in the editor console, `stat unit` toggles it and
  `stat none` hides it. In a game, set `[Stats] Unit=true` in
  `Config/DefaultEngine.ini`.
- **Frame** is the game thread's time from one `EndFrame` to the next.
- **Game Thread** runs from `Renderer::BeginGameFrame`, which a host calls
  before its pump and tick, to `EndFrame`. The time spent blocked in
  `WaitForPreviousFrame` and `PresentToWindow` is taken out.
- **Render Thread** is the render thread's time running a frame's commands,
  summed over every wakeup the frame took. The swap and the wait for work are
  left out.
- **Averaging:** each figure is averaged over half a second. `Renderer` then
  publishes the averages to the render thread (`StatUnitSnapshot`,
  `RenderThreadInternal.h`) and into `RenderStats`.
- **Drawing:** `RenderScene::Render` draws the panel last, over the debug
  lines and after a scene capture's readback, so captures never show it.
  `RenderContext::DrawStatUnit` is one full-screen pass (`StatsOverlay.fsh`,
  alpha blended). The text, 4 characters to a uint, goes in the material
  block. The font (`OverlayFont.h`: 5 x 7 glyphs, ASCII 32-126) is a texture
  made on first use. The panel is placed from the quad's UV rather than
  `gl_FragCoord`, whose origin differs between backends.

### Stat frame

**Stats.Frame** (config var, off by default) adds each stage's **GPU** time to
the same panel. Toggle it with `stat frame` in the editor console, or set
`[Stats] Frame=true` in a game's ini.

**The spans** (`ERenderGpuTimer`) are PrePass, BasePass, AO, Shadows,
Lighting, Forward (Unlit and the translucent stages), PostProcess, and Total
(from the scene's clear to the end of the post process).

**How a scene is timed:**
- `RenderSceneStages(..., bInTimed)` writes a GPU timestamp before and after
  each span (`GDIWriteGPUTimestamp`, indices `2t` and `2t + 1`).
- It does this only for the scene's own frame, and only while Stats.Frame is
  on, so a preview in the same block does not overwrite them and timing costs
  nothing when it is off.
- A span that does not run (no G-buffer, so no AO, Shadows or Lighting) shows
  `-`.

**Getting the results back:**
- A GDI block (`GDIBeginRendering` to `GDIEndRendering`, one scene's render)
  is complete by the next block's start, because the backends keep one frame
  in flight.
- So `RenderScene::Render`, right after `GDIBeginRendering`, takes the
  previous block's times (`RenderContext::BeginTimedBlock`) and gives them to
  the scene that wrote them. With several viewports, that is usually another
  scene.
- Each scene averages its times over half a second. The stats-reporting scene
  also publishes them to `RenderStats::GpuMs`.

**GDI timestamps** (`GDIDynamicDriver::WriteGPUTimestamp` /
`GetPreviousGPUTimestamps`), up to `GDI_GPU_TIMESTAMP_MAX` (64) per block:

| Backend | How |
|---|---|
| Vulkan | a 64-query `VK_QUERY_TYPE_TIMESTAMP` pool. It is reset at each `BeginRendering`, after the previous block's results are read (no wait: its fence was just waited). Writes use `vkCmdWriteTimestamp2` (ALL_COMMANDS), and ticks are scaled by `timestampPeriod` and the queue's `timestampValidBits`. Off if the queue cannot time. |
| D3D12 | a timestamp query heap, `EndQuery`. Every submit, including a mid-block `FlushForReadback`, resolves the queries into a readback buffer, which is read at the next `BeginRendering`. Ticks are divided by the queue's `GetTimestampFrequency`. |
| OpenGL | `glQueryCounter(GL_TIMESTAMP)`, read with `GL_QUERY_RESULT` at the next `BeginRendering`. That waits for the previous block, as the other backends' fences do, and only while timing. |
| Metal, Null | none: the panel says the GPU is not timed. |

Verified on Vulkan, including under the Khronos validation layer. The D3D12
and OpenGL code builds only on Windows, and has not been compiled or run yet.

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
- Serialization: `Material0`-`Material7` are written as graph paths and read
  back unchanged.
- Culling: a mesh past the view's edge is kept while a slot's World Position
  Offset reaches it, and culled without.
- Shadow setup (Null backend): cascade splits rise and end at the far edge;
  cascades fill the directional atlas's quarters; local tiles fit and never
  overlap; each view maps what it should see into its own tile; a point light's
  faces look along +X -X +Y -Y +Z -Z; MaxShadowedLights caps the casters.
- Serialization: Render (version 2: CastShadows, on for a version 1 save),
  Camera (version 2: Projection, FovY), Directional Light (version 2: no
  Direction; shadows), Point and Spot Light (version 1).
- GPU shadows (`NIMBLE_SMOKE_GPU`): a box over a floor, captured with shadows
  on and off (`Renderer::RequestSceneCapture`) and compared pixel by pixel. A
  Standard, Cascaded, spot and point shadow each darken the floor and brighten
  nothing; Cast Shadows off darkens nothing; a Modulated shadow leans to its
  tint. `NIMBLE_SMOKE_DUMP=<dir>` writes each capture as a PPM.
- GPU (`NIMBLE_SMOKE_GPU=Vulkan|D3D12`): previews of DefaultLit and Wave go
  through the G-buffer and keep their forward-shaded pixels (186 188 192; 40
  103 172); Glow (Translucent Unlit) is unchanged; Negative (PostProcess) inverts
  the preview's clear colour. Clean under the Vulkan validation layer.

## Later

Post process: an HDR scene colour (RGBA16F) so bloom thresholds real
brightness; a velocity target for per-object motion blur; temporal AA; colour
grading and exposure as stack effects.

Hardware shadow comparison, depth bias and front-face culling in GDI (then
drop most of the receiver bias); Masked casters clipped in the Shadows
sub-stage; shadows for Translucent Lit; a tiled or clustered light list past 16
lights; occlusion queries and a depth prepass in the PrePass; per-section
bounds for culling; RGBA16F G-buffer and scene colour.
