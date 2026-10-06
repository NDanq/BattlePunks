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
| GBuffer1 | Normal (world, ×0.5+0.5) | | | 1 = lit pixel; 0.75 = lit, its diffuse ambient baked |
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
a back face's normal, before it is stored. A draw with baked lighting adds it
to the emissive it stores and marks the pixel 0.75 (see [Baked
lighting](#baked-lighting)).

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

**Orientation.** Every render target is stored top-down on every backend
(`GDI.h`): row 0 is the top of the image, NDC y = +1, texture v = 0 and
`gl_FragCoord.y` = 0.5 -- the order `ToImage` returns and RenderDoc shows.
Vulkan gets there with a negative viewport height, OpenGL with
`glClipControl(GL_UPPER_LEFT)`; D3D12 and Metal are top-down natively. A pass
that turns a target's UV into NDC or back uses `TargetUvToNdc` /
`NdcToTargetUv` (`Common.glsl`), since v runs down where NDC y runs up.
Front faces are GL's on every backend, which Two Sided relies on.

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
- **the sky**: the first active Sky Light's hemisphere ambient -- or, in an
  HDR scene, its image-based lighting (see [Image-based lighting](#image-based-lighting)).

Every light shares one BRDF (`EvaluateLight`). Translucent Lit surfaces get the
same lights forward, but unshadowed: their texture slots are their material's.

### Shadows

Each light has a **Shadows** section: Cast Shadows, a Technique, a Modulated
Tint and two biases. A Render component's **Cast Shadows** says whether its
mesh casts, and **Cast Inset Shadow** gives it a sun shadow map of its own (see
[Inset shadows](#inset-shadows)). Every scene's quality is the `[Shadows]` config section, a
project setting (the game's DefaultEngine.ini; Edit > Project Settings >
Shadows edits it and applies it at once): Enabled, Resolution,
Filter, MaxShadowedLights, DistanceScale, MaxInsetShadows and InsetFilter.

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

### Inset shadows

A Render component's **Cast Inset Shadow** (Shadows, beside Cast Shadows;
`bCastInsetShadow`, the component's version 4, off by default) gives its mesh a
sun shadow map of its own -- Unreal's per-object, or inset, shadow. Two things
come of it:

- **Sharpness.** The map is fitted round the one caster, not the camera's
  frustum: LightLab's lattice (90 units across) spends well under a unit a
  texel, where the far cascade there spends four or more.
- **A shadow from a baked sun.** A static sun is in the bake, not the live
  lighting, so a dynamic entity casts nothing from it (see
  [Baked lighting](#baked-lighting)). Its inset shadow is drawn regardless.

**Which sun.** `WorldBridge::BuildLighting` records it in `RenderLighting`'s
`InsetLight*` fields: the live Directional Light, else -- drawing with a bake --
the first static one, if it has Cast Shadows on.

**Which casters.** `RenderScene` offers each proxy whose
`RenderProxy::CastsInsetShadow` is true (a Render component with a mesh, Cast
Shadows and Cast Inset Shadow) to `RenderContext::PrepareShadows`, largest on
screen first -- its bounding sphere's radius over its distance. A visible one
always; a culled one when its shadow can reach the view: its sphere's box,
swept along the sun across the view's whole extent, overlaps the view's
bounds. The sphere is
the mesh's local box, scaled, so it does not change as the entity turns.
`[Shadows] MaxInsetShadows` (0-4; Project Settings > Shadows > Inset shadows)
caps them, and `[Shadows] InsetFilter` (Inset shadow filter: Hard, PCF 3x3 --
the default -- or 5x5) is their edge filter, apart from Filter: their maps are
already sharp, so a smaller one usually looks the same and costs less.

**The views** (`ShadowSetup.cpp`): each an orthographic view along the sun
holding the caster's sphere -- its depth across the sphere alone -- moved in
whole texels so a moving caster's shadow does not crawl. They take the local
atlas's tiles the point and spot lights left, one each (Resolution / 6 texels a
side), so they need no texture slot of their own -- the Lighting pass has none
spare -- and their matrices are `Scene_LocalShadowMatrices`' after the
lights'. With no tile left, a caster casts into the sun's own views as
before -- under a baked sun, none.

**Drawing.** An inset view draws its caster and nothing else (draw event
"Inset n"). A caster that got one is left out of the sun's own views, so its
shadow is not drawn twice; it still casts into point and spot lights' views.

**Lighting** (`Shadows.glsl`'s `InsetShadow`, fed by `Lighting_Block`: the count,
the sun's biases, its direction and tint, and each view's matrix, texel and
tile). A receiver is shadowed by an inset view only inside its tile -- the
prism behind its caster. One past the far plane is behind the caster, so its
depth is held just inside the view; one before the near plane is between the
caster and the sun, and lit. The darkest of them is the inset shadow:

- **live sun:** taken with the cascade's (the darker of the two), then as the
  sun's technique says -- Standard takes away its light, Modulated tints.
- **baked sun:** there is no live light to take away, so it is Modulated by the
  sun's Shadow Tint over the whole pixel, the baked light in its emissive too
  (`DeferredLighting.fsh`). Faces turned from the sun are left alone.

**Only the pixels it can reach.** Each inset view also carries the screen
rectangle of that prism (`ShadowView::ScreenNdc`, `InsetScreenNdc`): its
caster's sphere -- grown by the largest normal offset and the filter's reach --
swept along the sun. The sweep runs only while it overlaps the box round
everything the view draws (`RenderScene` passes it to `PrepareShadows`), so it
stops at the floor; its faces are then clipped to the view in clip space, so
what is behind the eye or off screen drops out, and the rest is projected.
`Lighting_InsetRects` holds it in the Lighting target's pixels, and every
other pixel skips that inset before its matrix. A prism that meets nothing
drawn covers no pixel; one round the eye, the whole screen.

Limits: under a baked sun the tint darkens shade the bake already has, so a
dynamic entity's shadow in a static one's is darker still; the sun only, not
point or spot lights.

Verified: the smoke test checks the views (after the lights' tiles, never
overlapping them, capped, texel-snapped, holding the sphere, their screen
rectangles holding the caster), the component's version 4, and on Vulkan that
an inset caster shadows the floor where the sun's own view does not reach it,
that MaxInsetShadows 0 turns that off, and that a caster above and behind the
camera, out of the view's bounds, still shadows the floor it sees. LightLab's
lattice (`InsetLattice`, from `Games/LightLab/Tools/make_inset_lattice_mesh.py`)
shows it, turning and drifting (`LightLab.TurnLattice`): in the LightLab level,
and in the BakedLighting level's window beam, Baked and Live.

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

### Background

A scene clears to one colour (`Renderer::SetClearColor`), or to a vertical
gradient (`Renderer::SetClearGradient(Scene, Top, Bottom)`). For a gradient,
`RenderSceneStages` draws `ClearGradient.fsh` full screen right after the
clear: top colour at the top of the view, bottom at the bottom, display
space, with a half-step of dither so a slow ramp does not band in the 8-bit
target. Its depth is untouched and the targets are bound again for the
stages. The Lighting stage writes lit pixels only, and everything else draws
over it, so the gradient shows wherever the clear colour would. The editor
sets it on every viewport from Editor Settings (`Editor.ViewportGradient`,
`ViewportTopColor`, `ViewportBottomColor`); a game keeps its clear colour.

### HDR and tone mapping

Where the backend can make a half-float render target
(`GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F)`), a scene's colour is
**RGBA16F holding linear light** (`SceneTargets::bHDR`). The format, per backend:

| Backend | Format |
|---|---|
| Vulkan | `R16G16B16A16_SFLOAT` |
| D3D12 | `R16G16B16A16_FLOAT` |
| Metal | `RGBA16Float` |
| OpenGL | `GL_RGBA16F` |

Null has none. Only Vulkan is built and tested on Linux. The D3D12, Metal and
OpenGL code is written to match and awaits its platforms. So does the
shaders' cross-compilation: HLSL through `fxc` at shader model 5.0, and MSL.

- **Writing it:** `Scene_Output.x` tells the shaders. Lit output skips its
  `ToDisplay` (`ToSceneColor` in `Lighting.glsl`). An Unlit material's emissive,
  authored in display space, is linearised. Translucent blending happens in
  linear light.
- **Tone mapping:** a `Tonemap.fsh` pass runs first in the post chain. It turns
  the scene colour into display space in `PostProcess` (8-bit): exposure, the
  operator, then 2.2 gamma. Every effect and PostProcess material after it sees
  display colours as before. PostProcess-domain materials stay display space in,
  display space out.
- **The stack's Tone Mapping node:** Exposure in stops (each +1 doubles the
  light) and Tone Mapper (**ACES**, Narkowicz's fit; **Reinhard**; or
  **None**). Volumes blend the exposure, and switch the operator once half in.
  Without the node: exposure 0 and None, a clamp. That is exactly what an
  8-bit scene showed, so a stack without it looks unchanged.
- **The background:** a clear colour or the editor's gradient is drawn by
  `ClearGradient.fsh` even when plain. It is un-tone-mapped and un-exposed
  (`SceneEncodeDisplay`, `ToneMapping.glsl`), so it comes out of any tone map as
  authored.
- **Captures and previews:** these run with no stack, so they are clamped.
  A reflection is therefore tone mapped once, by the view that shows it.
- **Without one:** a backend with no half-float target (Null) keeps the old
  display-space path; the node does nothing there.

Verified (`NIMBLE_SMOKE_GPU=Vulkan`): an Unlit quad of display 0.9 reads 229 with
no tone map, 202 with ACES, 229 with ACES at +1 EV and 176 with Reinhard, and
the gradient background stays within 1 of authored under each.

### Image-based lighting

In an HDR scene, Lit surfaces take their ambient light and reflections from
images instead of the Sky Light's hemisphere. The images are the sky and up to
four **Reflection Probes**.

**The atlas** (`IBLCommon.glsl`) is one RGBA16F 768 x 768 target per scene
(`IBLResources`, owned by `RenderScene`). It holds five blocks of 384 x 256:
four probes and the sky. Each block is an octahedral map:
- six roughness levels, from 256 texels (roughness 0, a copy) down to 8
  (roughness 1), each a GGX-weighted average, importance sampled;
- a 32-texel diffuse irradiance, cosine weighted.

`IBLPrefilter.fsh` writes a block in one pass over the atlas from a **cube
strip** (6S x S, the scene-capture face order). It places texels by
`gl_FragCoord`, never a viewport offset, so the layout is the same on every
backend.

**The sky block** is rebuilt whenever the sky changes (`SkyHashOf`):
- the Sky Light's **Environment** HDRI if it has one: an equirectangular RGBE8
  texture, times Environment Intensity, turned Environment Rotation;
- otherwise its sky and ground colours (`IBLSky.fsh`).

With Draw Background on, an HDRI is also drawn behind the scene along each view
ray (`Sky.fsh`).

**Reflection Probes:**
- **Selection:** `WorldBridge::BuildReflectionProbes` sends the four enabled
  probes nearest the camera whose influence the view reaches, each marked due
  by its Frequency (Once by default, Interval, Every Frame, Manual).
- **Capture:** `RenderScene` keeps which block holds which probe. It captures a
  probe that is due, or new to a block, at most two a frame. A capture is six
  90-degree HDR views, shadows included unless Capture Shadows is off, lit by
  the sky alone so no probe sees another. The sky is drawn behind. The linear
  scene colour of each view is copied into the strip, then prefiltered into the
  probe's block.

**Lighting** (`IBL_Light`):
- **`IBLLighting.fsh` pass:** added (one, one) over the Lighting stage, from
  the G-buffer, depth, AO and the atlas. The Lighting stage then drops its
  hemisphere term (`Scene_IBL.x`).
- **Forward Lit materials:** these do the same from the atlas bound at texture
  slot 0.
- **Probe weights:** each probe weighs a surface fully inside 80% of its
  Radius, or of its Box Extent, fading to nothing at the edge. The nearest
  first, then the sky fills the rest.
- **Box projection:** a box probe's reflections are box-projected (parallax
  corrected).
- **The terms:**
  - specular = prefiltered radiance at the roughness x Karis's fitted split-sum
    BRDF x Lagarde's specular occlusion;
  - diffuse = irradiance x albedo x (1 - metallic) x AO.

A backend with no half-float target keeps the hemisphere ambient. A material
preview is lit by its studio light alone.

Verified (`NIMBLE_SMOKE_GPU=Vulkan`, in a room of six coloured walls):
- **Mirror ball with a probe:** a mirror metal ball reflects, from the probe,
  the wall behind the camera at its centre and the ceiling, floor and sides
  where they belong.
- **Probe off:** the ball reflects the sky instead.
- **Rough white ball:** its top takes the green ceiling's irradiance.
- **HDRI:** a two-tone HDRI is drawn behind the scene and reflected the right
  way up.

### Baked lighting

A world can bake its static lighting: lightmaps for its static meshes, light
probes for everything else. An entity's Mobility (GameCore's `Mobility.h`)
decides what is baked:

- **Static** meshes and lights are baked whole.
- **Stationary** lights (and a Stationary Sky Light) stay live -- their direct
  light and shadows are drawn every frame -- and only the light they bounce is
  baked. A Stationary mesh is treated as Dynamic.
- **Dynamic** entities are left to the renderer, lit by the probes.

**Turning it on.** World > World Settings > Baked lighting
(`GameCore::WorldLightingSettings`, saved with the world: a `<Settings>`
element in a text world, a `WSET` chunk in a cooked one). Its fields: lightmap
density (texels per unit), bounces, samples per texel, probe spacing and the
lightmap atlas size.

**Lightmap UVs.** A mesh needs a second UV set, UV1 (`MeshData::UV1`, the
`.nmmesh`'s optional `MUV1` chunk, vertex location 10): charts in 0-1 that do
not overlap. The importer reads glTF's `TEXCOORD_1`, or makes them with xatlas
(the editor's `LightmapUVs.h`; an import option, on by default); the engine's
primitives have them (`generate_primitive_meshes.py`). A static mesh without
UV1 is still in the bake -- it shadows and bounces light -- but is lit by the
probes, as a dynamic one is.

**Baking.** Build > Bake Lightmaps (`lighting bake` in the console) is a CPU
path tracer in the editor (`Editor/.../Lighting/`), on every core, behind a
progress dialog that can cancel:

1. **Measure** (`BakeLightmaps.h`). The viewport's renderer draws each static
   material on a quad twice -- its base colour, then its emissive -- through a
   material preview's channel (`EMaterialPreviewChannel`, `Scene_View` 2 and 3),
   and averages each in linear light. So the bake reflects and glows as the
   materials draw, textures included. Without a GPU (the Null backend), a
   material bakes by its `BaseColor` parameter, else 0.5 grey.
2. **Gather** (`LightBakeWorld.h`). Every static Render component's mesh, in
   world space, each section's albedo and emissive as measured (the entity's
   `BaseColor` and `Metallic` overrides win). Static and Stationary
   directional, point and spot lights and the Sky Light (its colours, or its
   HDRI). The world's hash (`HashWorld`): settings, and each static or
   Stationary entity's GUID, mobility, place, components, mesh and material
   description -- no triangles touched, so it is cheap to check again.
3. **Layout** (`LightBaker.h`). Each lightmapped mesh gets a square of
   sqrt(area) x density texels, shelf-packed into atlases with a texel of
   border; its UV1 maps into it.
4. **Lighting.** A texel whose short rays along its surface find the back of
   one is inside or under something (the floor under a wall): it is left out
   and filled from its neighbours, so walls leak no dark line. Then per texel:
   every Static light, through shadow rays jittered over
   its size for soft shadows -- a Directional Light's **Source Angle** (degrees,
   0.75 by default), a point or spot light's **Source Radius** (units, 4), each
   under Baked Lighting in its Details; then cosine-weighted paths that reach the sky or
   bounce off up to Bounces surfaces, each lit by every light, Stationary ones
   too, and each glowing with its emissive. Values match the live renderer's: a
   surface's direct diffuse equals what `EvaluateLight` gives it. Each texel
   also keeps the light's dominant direction (luminance weighted) and how
   one-sided it is, for directional lightmaps.
5. **Filtering.** The bounced light (and its direction) is smoothed across texels of the same
   mesh with similar positions and normals; lit texels grow into the empty
   ones round them.
6. **Probes.** A grid over the static geometry at Probe Spacing, each an
   ambient cube (+X -X +Y -Y +Z -Z) from rays over the whole sphere plus each
   Static light's direct part. A probe that sees mostly backfaces is inside something
   and left out of the blend.

The result goes beside the world, `Levels/<World>/Lighting/`:
`Lightmap_<n>.nmtexture` (at most `RENDER_BAKED_ATLASES_MAX`, 8) and
`Lighting.nmlighting` (`BakedLightingAsset.h`: the hash, whether the sky was
baked, whether the atlases are directional, each entity's atlas and UV1
scale/offset, the probes). A directional atlas is twice as tall: the light
above (RGBE8), its direction below (RGBA8: the direction times its
one-sidedness, x 0.5 + 0.5, and in alpha the half-Lambert at the texel's own
normal). The world's
settings name it (`BakedData`): one undo step, saved with the world. The
status bar says "Lighting needs to be rebuilt" once a static entity or light no
longer matches the bake's hash.

Every value is *diffuse radiance* -- irradiance over pi, what a white
Lambertian surface sends back -- so a surface adds value x albedo, as the
hemisphere ambient is added.

**Drawing** (`WorldBridge`, `BakedLighting.glsl`). While the world has a bake
and `[Rendering] BakedLighting` is on:
- **Static lights** are left out of the frame's lighting and shadow maps --
  they are in the bake (the default sun too, if a static sun was the only
  one).
- **Each Render component's draw** carries `RenderBakedDraw` from the
  extraction record to its proxy and the draw command:
  - a static entity the bake has: its lightmap atlas and UV1 scale/offset;
  - anything else: the probes' trilinear blend at its position, an ambient
    cube.
- **The material block's engine bytes** (`MaterialDescription.h`, 32-159)
  hold that per draw: `Material_LightmapScaleOffset`, `Material_BakedLighting`
  (x: 0 none, 1 lightmap, 2 ambient cube) and `Material_AmbientCube[6]`. The
  lightmap is bound at material texture slot 0, which a Base Pass material
  does not otherwise use; RGBE, so `Baked_SampleLightmap` filters it by hand.
  A directional one (`Scene_Baked.z`) is shaded for the surface's normal --
  its normal map's: `Baked_SampleDirectional` scales the light by the
  half-Lambert of the normal against the stored direction, over the same at
  the texel's own normal, so a flat surface shows the bake as it is.
- **The Base Pass** adds baked light x diffuse albedo x AO to the emissive it
  stores, and writes GBuffer1.a = 0.75. The Lighting stage and the IBL pass
  then leave out their diffuse ambient there (specular reflections stay) --
  unless the bake did not have the sky (a dynamic Sky Light), whose live
  ambient then still applies (`Scene_Baked.y`).
- **Forward Lit** (translucent) draws add the ambient cube the same way.

**Live and Baked.** `[Rendering] BakedLighting` 0 draws as if there were no
bake: every light live, nothing baked. The editor's Show > Lighting > Baked /
Live and `lighting baked | live` set it; LightLab's L key toggles it.

The trade-offs of fully baked Static lights: they light dynamic entities only
through the probes -- diffuse, no specular highlight, no sharp shadow -- and
dynamic entities cast no shadow from them, unless they have Cast Inset Shadow
(a static sun's, Modulated: see [Inset shadows](#inset-shadows)). A Stationary
light keeps all of that, live, at its live cost; only its bounce is baked.

Verified: the editor's lighting tests bake small scenes whose answers are
known -- an open texel under a straight-down sun equals radiance / pi to 5%,
the texel under a box is in its shadow, a probe's downward face sees the
floor's bounce, a white sky lights an open floor fully and a red wall tints
the floor beside it. LightLab's BakedLighting level, on Vulkan, in both views.

### View modes

What a scene's view shows (`Renderer::SetViewMode`, `ERenderViewMode`; the
editor's Show > View Mode):

- **Lit:** the picture.
- **Unlit:** base colour plus emissive, no light. `Scene_View.x` tells the
  Lighting stage to write that and the IBL pass to add nothing; a Base Pass
  draw adds no baked light, and a forward Lit material shades as Unlit.
- **Render Target:** after the stages, one of the frame's targets is drawn
  over the final colour (`RenderContext::ShowTarget`, `ViewTarget.fsh`),
  stretched to the view. A colour target shows as it is; a shadow atlas's
  depth in grey; the scene's depth as distance from the eye. Each frame lists
  them by name (`RenderContext::ListViewableTargets`), which
  `Renderer::GetViewableTargets` hands to the game thread: the scene's stage
  targets, the shadow atlases, the image-based lighting atlas and cube strip,
  and every render target asset captured.

The screenshot (`Renderer::RequestSceneCapture`) reads what the mode shows.

### Scene capture

A **Scene Capture Probe** component (`Components/SceneCaptureProbeComponent.h`)
renders the scene from its entity into a render target asset, which any
material then samples like a texture.

**Render targets** (`RenderTargetAsset.h`) are two asset kinds:

| File | Kind | Texture it samples as |
|---|---|---|
| `*.nmrt2d` | Tex2D | Width x Height |
| `*.nmrt3d` | Cube | a 6S x S strip of faces +X -X +Y -Y +Z -Z |

- **What the file holds:** size, filter, wrap and clear colour, plus an
  optional baked image (chunks `NRTA` / `RTHD` / `RTPX`).
- **Transient targets:** made at runtime by
  `RenderTargetAssetCache::CreateTransient`, named `transient://<name>` and
  never written to disk.
- **Strip format:** GDI has no cube textures, so a cube is a strip. Each face
  is drawn with a horizontal viewport offset only, its up at the strip's top
  row. Targets are RGBA8.
- **What a material samples:** `RenderContext::FindTexture` sends a
  render-target path to the GPU target if it has been captured, else to the
  bake, else to the null texture.

**Probe settings:**
- Target: a `.nmrt2d` or `.nmrt3d`.
- Mode: **Planar Mirror** (2D) or **Omnidirectional** (cube).
- Resolution: a 2D target keeps its aspect.
- Frequency: **Every Frame**, **Interval** (seconds), **Once** (on start) or
  **Manual** (`CaptureNow`, from a script or the editor).
- Near and far clip.
- Extent X / Z: the mirror's rectangle in local X/Z.
- Hide Owner: the probe's entity subtree stays out of its own capture.
- Capture Shadows: off by default. Off, a capture prepares no shadow maps
  and lights its views unshadowed (`SetView` leaves the scene's shadows off,
  and `QueueDraws` skips `PrepareShadows`). On, every view redraws every
  shadow map; a cube does that six times. On LightLab's level in a Debug
  build, an every-frame cube probe costs the render thread 5 ms off and
  30 ms on.
- Enabled.

**Game thread:** `WorldBridge::BuildCaptures` applies the frequencies and
skips a mirror whose rectangle is out of view. It sends at most
`RENDER_CAPTURES_MAX` (8) requests with the frame (`SceneCaptureList`, POD).

**Render thread:** `RenderScene::Render` runs the captures before the main
view. For each request, and each of its views (`BuildCaptureViews`), it:
1. sets the view;
2. queues draws, culled to the view and without the excluded entities;
3. runs every stage (shadows only with Capture Shadows) into the target's own stage targets;
4. copies the result into the target face (`CaptureCopy.fsh`).

The post process stack is the main view's alone.

**Omnidirectional** captures six 90-degree views from the probe. A material
samples them with `Material_SampleCubeStrip(tex, dir)`, which picks the face
and insets half a texel.

**Planar mirror:**
- **The camera:** the main camera reflected in the probe's plane (local +Y,
  through its position), built as a proper LookAt with the main view's
  projection. Nothing is captured while the camera is behind the plane.
- **Clipping:** the projection's near plane is replaced by the mirror plane
  (an oblique near plane, after Lengyel). Whatever lies behind the mirror,
  such as the floor it rests on, is clipped by the rasteriser rather than
  drawn between the reflected camera and the scene.
  - Every backend clips GL-style, with z in [-w, w]; GDI remaps the range for
    Vulkan, D3D12 and Metal. So the plane maps to z = -w.
  - Shaders rebuild positions through `Scene_InvViewProjection`, so lighting,
    AO and DoF follow the oblique matrix.
- **Sampling:** a material samples the mirror at the screen position flipped
  in X (`Material_MirrorUV`).
- **Limit:** perspective views only.

**Editor:**
- **World > Bake Scene Captures:** fires every probe, reads the targets back
  (`Renderer::RequestRenderTargetReadback`) and saves each image into its
  file as the bake.
- **Fallback:** a game shows the bake until its first live capture, or for
  good with Frequency Manual.

Verified (`NIMBLE_SMOKE_GPU=Vulkan`):
- a cube in a room of six coloured walls reads back each face's wall;
- a Reflect sphere shows the wall each part of it faces;
- a mirror floor shows an off-centre box on the correct side;
- the mirror sees past a slab lying under it.

### Static draw lists

An entity with **Static** mobility (GameCore's `Mobility.h`) keeps its draws
from frame to frame instead of rebuilding them:

- **Extraction (game thread):** `WorldBridge::ExtractAndRender` builds a
  static component's record (transform, mobility, baked lighting and component bytes) as
  usual, but sends it only if it differs from the last one it sent for that
  proxy. An editor edit, a tool's move or a mobility change goes through; an
  unchanged static entity costs no copy, no command and no `ApplyComponent`.
  A reimported mesh (`MeshAssetCache::GetGeneration`) sends every static
  record once more, so bounds follow the new mesh.
- **Proxies:** each applied record bumps the proxy's change serial
  (`RenderProxy::MarkChanged`) and sets its mobility (`SetStatic`).
- **Drawing (render thread):** for a static proxy, `RenderScene::Render`
  captures what its `Draw` queues (`RenderContext::BeginCapture` /
  `EndCapture`), and separately what its `DrawShadow` queues, the first frame
  each is needed. Its cull padding is cached too. Every later frame the
  captured commands are replayed (`SubmitCaptured`): depths recomputed for the
  view, shadow commands aimed at the current shadow view. Culling still runs
  per frame on the proxy's bounds.
- **Invalidation:** a list is rebuilt when the proxy's change serial moves,
  or when the context's content generation moves -- a material reloaded or
  retired, a GPU mesh rebuilt, or a mesh reimported -- since the commands
  hold `Material` and `GDIMesh` pointers.
- **Stats:** `RenderStats::StaticProxiesDrawn` (last frame) and
  `StaticListBuilds` (lifetime; flat while nothing static changes).

Verified: the GPU smoke check draws a lit, shadowed scene static and dynamic
pixel for pixel alike, and a static box moved by a tool lands exactly where a
dynamic one does.

### UI

A scene's UI -- HUDs and menus, Engine/Modules/UI/README.md -- is drawn over
its final colour after the post process stack, and after the Render Target
view mode's picture, before the editor's readback (so a capture shows it) and
under "stat unit".

- **What it draws.** `Renderer::SubmitUIDrawList(Scene, List)` gives a scene a
  `UIDrawList` (`UIDrawList.h`, GDI-free): textured, coloured triangles in
  pixels from the top-left, in commands of one texture and one scissor each.
  The scene **keeps** it and draws it every frame until the next submit; an
  empty list clears it. A host submits only when its UI changed.
- **Textures.** A command names one by key: `""` white, an asset path
  (`TextureAssetCache`, as materials sample), or `ui:...`, an image given with
  `Renderer::SetUIImage` (font atlases). Resolved once per list.
- **The pass.** `RenderContext::DrawUI` (`UIRenderer.h`): `UI.vsh` / `UI.fsh`,
  alpha blended, no depth; texture times vertex colour. Positions go through
  `ViewParameters.Projection` -- an orthographic one, the top-left pixel at
  NDC (-1, +1) -- so the list lands as the scene does. One draw per command, a scissor change
  only between commands that clip differently (`GDISetScissorRect`).
- **Its mesh.** One dynamic mesh per scene (`GDICreateDynamicMesh`), rewritten
  only when the list changes: a static HUD uploads nothing per frame. It grows
  by half when a list outgrows it.
- **Cost.** A document is typically 1-3 commands: batches merge on matching
  texture and scissor, and a font atlas keeps a white texel so flat fills join
  its text's draw.

Verified on a GPU (RenderCore's smoke test with `NIMBLE_SMOKE_GPU=Vulkan`):
orientation (the list's top-left is the image's), each scissor edge, alpha
blending, a kept list drawn again without a resubmit, and an empty list clearing.

### Stat unit

**Stats.Unit** (config var, off by default) draws frame, game thread,
render thread, wait, present and idle times, and FPS, in a panel at the top right of every viewport.
- **Toggling it:** in the editor console, `stat unit` toggles it and
  `stat none` hides it. In a game, set `[Stats] Unit=true` in
  `Config/DefaultEngine.ini`.
- **Frame** is the game thread's time from one `EndFrame` to the next.
- **Game Thread** runs from `Renderer::BeginGameFrame`, which a host calls
  before its pump and tick, to `EndFrame`. The time spent blocked in
  `WaitForPreviousFrame` and `PresentToWindow` is taken out.
- **Render Thread** is the render thread's time running a frame's commands,
  summed over every wakeup the frame took. The swap and the wait for work are
  left out. A wait on the GPU inside GDI, such as a fence at
  `BeginRendering`, is counted.
- **Wait**, **Present** and **Idle** are the rest of Frame. Wait is the game
  thread in `WaitForPreviousFrame`, Present is the game thread in
  `PresentToWindow` (the swapchain's vsync), and Idle is the time from
  `EndFrame` to the next `BeginGameFrame` (the host's loop, the editor's frame
  timer). So Frame = Game + Wait + Present + Idle. A large Wait means the
  render thread is the bottleneck, a large Present means vsync, and a large
  Idle means the host's pacing.
- **Averaging:** each figure is averaged over half a second. `Renderer` then
  publishes the averages to the render thread (`StatUnitSnapshot`,
  `RenderThreadInternal.h`) and into `RenderStats`.
- **Drawing:** `RenderScene::Render` draws the panel last, over the debug
  lines and after a scene capture's readback, so captures never show it.
  `RenderContext::DrawStatUnit` is one full-screen pass (`StatsOverlay.fsh`,
  alpha blended). The text, 4 characters to a uint, goes in the material
  block. The font (`OverlayFont.h`: 5 x 7 glyphs, ASCII 32-126) is a texture
  made on first use. The panel is placed from the quad's UV, which runs from
  the target's top-left as its layout does.

### Other modules' stats

`Renderer::SetStatText(Name, Text)` adds a section of its own below the
thread and GPU times, on the game thread. It is how another module shows its
numbers. `GameSession` uses it for `stat physics` (`[Stats] Physics`), with
the level's bodies, contacts and step times. Sections show in name order
from the next `EndFrame`, and empty text removes one. The panel holds 960
characters (its widest line times its lines), so a section should be a few
short lines.

### Stat frame

**Stats.Frame** (config var, off by default) adds each stage's **GPU** time to
the same panel. Toggle it with `stat frame` in the editor console, or set
`[Stats] Frame=true` in a game's ini.

**The spans** (`ERenderGpuTimer`) are PrePass, BasePass, AO, Shadows,
Lighting, Forward (Unlit and the translucent stages), PostProcess, and Total
(from the scene's clear to the end of the post process). The last line is
**Captures**: every Scene Capture Probe's views that frame, all stages
included. Captures run before the scene's stages and are not part of Total.
`RenderScene::Render` times them with `RenderContext::WriteGpuTimestamp`
around the capture loop, so a frame with no captures shows 0.00, not `-`.

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

### Draw events

Each scene's frame is annotated for GPU capture tools (RenderDoc, PIX, Xcode)
with nested draw events, `SCOPED_DRAW_EVENT(Name)` (GDI.h: a `GDIDrawEvent` to
the end of the scope, compiled out of shipping builds). A frame reads:

```
Scene
  Simulate
    Particles <system>/<emitter>   its two compute dispatches
  MaterialPreview            (editor; the stages below, per preview)
  SceneCapture <target>
    Face +X .. Face -Z | Mirror
      (the stages below)
  ImageBasedLighting
    SkyLight                 IBLSky, IBLPrefilter
    ReflectionProbe <block>
      Face +X .. Face -Z
  Background                 Sky | ClearGradient
  BasePass
    Static                   draws replayed from static draw lists
    Dynamic
  AmbientOcclusion           AmbientOcclusion, AmbientBlur
  Shadows
    ClearAtlases
    Sun | Sun Cascade <n> | Spot <light> | Point <light> +X..-Z
      Static
      Dynamic
  Lighting
    DeferredLighting
    IBLLighting
  Unlit, TranslucentLit, TranslucentUnlit
  PostProcess
    Tonemap
    Materials (Before Effects)
    DepthOfField             DofPrepare, DofBlur, DofComposite
    MotionBlur
    Bloom                    BloomDownsample, BloomUpsample, BloomComposite
    Materials (After Effects)
    Fxaa
  ViewTarget, DebugLines, StatUnit
```

**Every draw is named** for what it draws and with what (`RenderContext`'s
`Issue`, from the mesh's file and section and the material's file, extensions
dropped):

```
BasePass
  Static
    Sphere [Sphere] | PBR                      a mesh's section, with its material
    Cylinder [Cylinder] | FlareBody
  Dynamic
    Particles Debris/Chunks (up to 128) | DefaultLit  a particle emitter: its system and emitter, its capacity
TranslucentUnlit
  Particles Sparks/Sparks (up to 512) | ParticleStreak
  Quad | Glow                                  a material quad
PostProcess
  Materials (After Effects)
    Fullscreen | Negative
```

each a region holding its one `vkCmdDrawIndexed`. A mesh section with no material
reads `| (plain tint)`. The names are built only where the backend records regions
(`GDIHasDrawEvents`: Vulkan with `VK_EXT_debug_utils` -- always under RenderDoc --
D3D12, OpenGL, Metal); `[Rendering] DrawLabels=false` turns the per-draw ones off.

**GPU objects are named** after their assets (`GDISetDebugName`), so RenderDoc's
Resource Inspector and a draw's pipeline state read what they are: a mesh's
buffers `Engine/Content/Meshes/Box.nmmesh Positions` (`TexCoords`, `Normals`,
`Colors`, `UV1`, `Indices`), a texture its path, a particle emitter's buffers
`Particles Content/Particles/Laser.nmparticle:Glow State` (`Instances`, or a
strip's `Triangles A`/`B`), every shader module its cooked file
(`Shaders/Cooked/Vulkan/Content/Materials/Glow@00000000.fsh.spv`), and render
targets `RT:<name>` as before. Vulkan names them through
`vkSetDebugUtilsObjectNameEXT`, D3D12 through `ID3D12Object::SetName` (PIX and
the debug layer); Metal and OpenGL keep the name and do not pass it on yet.

A stage or group with nothing to draw has no event. Every full-screen pass is
named after its shader (`DrawPost`). Static draws are issued before dynamic ones
only in the Base Pass and Shadows, which neither blend nor depend on order;
every other stage keeps its order.

| Backend | As | Notes |
|---|---|---|
| Vulkan | `vkCmdBeginDebugUtilsLabelEXT` | `VK_EXT_debug_utils` is enabled on the instance in non-shipping builds whenever offered (a capture tool's layer offers it); render targets are named too |
| D3D12 | `ID3D12GraphicsCommandList::BeginEvent` | PIX's string encoding |
| OpenGL | `glPushDebugGroup` | |
| Metal | `-[MTLCommandBuffer pushDebugGroup:]` | on the command buffer, so an event spans encoders; closed before a commit |

A mid-frame readback (an editor capture) submits the frame and starts another
command buffer: events opened before it end with it.

### Particles

A **Particle System** component (`ParticleSystemComponent.h`) runs a particle
system asset from its entity and simulates it on the GPU, with compute shaders,
where the backend has them (Vulkan; Null runs the calls and nothing else).
Elsewhere it draws nothing and says so once.

- **The asset** (`ParticleSystemAsset.h`, `*.nmparticle`) is up to 8 emitters,
  each with its own mesh, material, offset and settings (spawn rate, lifetime,
  velocity, forces, size and colour over life). Shape, Velocity, Forces,
  Rotation, Collision and Flipbook are optional **modules** (`EParticleModule`, `Modules Shape Forces`
  in the file): one left out is at rest whatever its settings hold -- a point,
  no initial speed, no forces, no spin (`ToSettings`). A file written before
  modules has those its settings use (`InferModules`; a beam, none). It is text in every build --
  a line per setting under each `Emitter <name>` line, unknown keys skipped --
  so a game reads it as staged, with no cook. The editor's Particle Editor makes
  and edits them (Editor README, "Particle Editor").
- **Each emitter has a type** (`EParticleType`, `ParticleTypes.h`):

  | Type | Drawn as | Turned by |
  |---|---|---|
  | **Sprite** | its mesh (empty: `Quad.nmmesh`) a particle, sorted back to front | **Billboard**: Face Camera (square to the view), Vertical (about world +Y toward the eye), Velocity Aligned (along its motion, stretched by speed x Velocity Stretch), Fixed (the quad's own axes); plus Rotation Rate in its plane |
  | **Mesh** | its mesh (empty: `Box.nmmesh`) a particle, sized by Size | **Mesh Orientation**: Fixed (spinning about +Y), Random Spin (about an axis of its own), Align to Velocity (+Z along its motion) |
  | **Ribbon** | one strip through its particles in the order they were born: a trail | widened square to the eye |
  | **Beam** | one strip from the emitter to Beam End (or the component's Beam Target), through Beam Segments points jittered up to Beam Noise, none at its ends | widened square to the eye |

  Random Rotation starts each sprite or mesh at a random angle, spinning
  either way. A particle's random bits are fixed at birth (`Particle.Extra`),
  so its rotation is a function of its age: no extra state.
- **Shape**: where each is born, in the entity's own axes (the shape turns
  with the entity; Velocity and Acceleration stay world space, so gravity is
  down): **Sphere** (within Radius, the default), **Hemisphere** (its +Y
  half), **Box** (within Box Size), **Cone** (a disc of Radius facing +Y,
  each sent out within Cone Angle of +Y), **Ring** (a circle of Radius about
  +Y, Ring Width across). **On Surface** keeps them to its surface -- the
  shell, the faces, the rim, the circle -- and **Outward Speed** adds speed
  out of the shape where each is born (from the centre, through the face,
  along the cone). `ParticleSimulate.csh`'s `ShapePoint`.
- **Over life**: size and colour (alpha too) each go **Start/End** -- size
  straight, colour by Color Curve, faded in and out, as before -- or through
  up to 8 **keys**: a size **Curve**, a colour **Gradient**, straight between
  keys and held past the first and last. Either way the CPU bakes them into
  32 samples, birth to death (`PARTICLE_LUT_SIZE`), in the dispatch's
  parameter block (`Pa_ColorLut`, `Pa_SizeLut`; 1008 of the 1024 bytes a
  block may hold), which every pass samples (`SizeAt`, `ColorAt`): one path
  for every type, so a ribbon's or beam's gradient runs along it. The
  component's Tint multiplies the samples.
- **Collision** (an optional module): a horizontal plane Height above the
  entity. A particle that reaches it from above bounces off -- keeping Bounce of
  its speed into the plane and losing Friction of its speed along it -- or, with
  Response Kill, dies there (`ParticleSimulate.csh`). Any type but a beam.
- **Flipbook** (an optional module, sprites and meshes): the material's texture
  as Columns x Rows frames, left to right then top to bottom; Frames of them
  (0: all), at Frame Rate a second, looping, or -- at 0 -- Cycles times over a
  life, each from a frame of its own with Random Start. The sort writes each
  instance's frame into its transform's bottom row (frame, columns, rows),
  spare in an affine transform; the material template's InstancedMesh vertex
  shader reads it, clears it and maps the texture coordinate into the frame's
  cell (`MATERIAL_TEMPLATE_VERSION` 12), so any material's UVs, and so its
  texture samples, play the frame.
- **Culling**: past an emitter's **Cull Distance** from the view (0: never) it
  is neither simulated nor drawn, and its GPU buffers are let go; back in range
  it starts afresh. With **Pause Offscreen** (on by default) it stops simulating
  once no view has drawn it for half a second -- its particles kept as they
  were -- and goes on when one does. A view culls by its Bounds: a game
  camera's frustum box; the editor's viewport views have none and cull
  nothing, so there an emitter pauses only when its scene is not drawn at all
  (a hidden Particle Editor tab). `RenderStats::ParticleSimulations` counts the
  emitters simulated.
- **Spawn Mode**: Continuous (Spawn Rate a second) or **Burst**: Burst Count at
  once, again every Burst Interval (0: one burst), Burst Cycles times (0: no
  end). Emitting off and on again starts the bursts over -- how a script
  replays a one-shot effect. Any type but a beam bursts.
- **Spawns move with the emitter.** A frame's spawns are born at evenly spread
  moments inside it, the first oldest, each where the emitter was then (between
  last frame's position and this one's): a fast emitter leaves a line, not
  clumps, and a ribbon's points come in order.
- **The component** names one (`System`; empty runs one emitter at its
  defaults) and says how this instance runs it: Emitting, a Tint on every
  colour, Spawn Rate Scale (bursts too), Time Scale (0 freezes it), a Seed added
  to each emitter's, and a Beam Target its beams end at instead (Use Beam
  Target), for a script aiming a laser. Version 1 held one emitter's settings
  inline; those are read past.
- **`ParticleSystemCache`** holds systems by path. The proxy looks its system up
  every frame and notices a new version by its pointer, so a save (the editor
  invalidates the path) or a `Store` (the editor's unsaved edits, a system made
  by code) reaches every component on the next frame. Each emitter keeps its GPU
  particles through such a change unless its Max Particles changes.

- **Each emitter's particles** live in world space, in a ring of up to
  `PARTICLE_MAX_PER_SYSTEM` (2048), 48 bytes each: position and age, velocity
  and lifetime, its random bits (`ParticleCommon.glsl`). A zeroed slot has never
  been used.
- **Once a frame**, before any target is bound (`RenderProxy::Simulate`,
  against the scene's own view), two dispatches (`RenderContext::SimulateParticles`)
  for a Sprite or Mesh emitter:
  - `ParticleSimulate.csh`, a thread a particle: this frame's spawns -- the
    rate times the frame's delta, the fraction carried, the CPU's count round
    the ring (`ParticleSimulation::BuildParams`) -- are born in the emitter's
    sphere, each at a random moment inside the frame; the rest move on by
    acceleration, a swirl (Turbulence) and drag.
  - `ParticleSort.csh`, one workgroup: a bitonic sort in shared memory, two
    particles a thread, farthest first along the view; then each written as
    an instance (GDI's layout: a transform and a colour) -- turned to the
    camera if Face Camera, sized and coloured by age, faded in and out. Dead
    and unused slots collapse to a point and sort last.
  A **Ribbon** emitter runs `ParticleSimulate.csh`, then `ParticleRibbon.csh`
  instead of the sort; a **Beam** places its points with `ParticleBeam.csh`
  (point i of N at i/N of the way, its age i/N of a life, so size and colour
  over life run along the beam), then `ParticleRibbon.csh`. The ribbon pass
  takes the points in order -- a ribbon's from the oldest slot round the ring --
  widens each across the view to its size, and writes each segment between two
  living points as **two triangles**, one instance each of
  `RibbonSegmentA.nmmesh` and `RibbonSegmentB.nmmesh` (a unit triangle, with
  UVs that make the pair a quad) into two instance buffers. An affine transform
  maps a triangle's three corners exactly, so neighbouring segments share their
  edges with no gaps or overlaps -- through the same instanced-material path
  as sprites, with no new GDI. U runs across the strip, V along each segment.
  Strips are not sorted; keep a ribbon's Lifetime Variance at 0, or it breaks
  where a point dies early.
- **Drawn** from every view that sees it (`Draw`): the mesh (an empty Mesh is
  `Engine/Content/Meshes/Quad.nmmesh`) instanced, one draw per section, with the
  material's **InstancedMesh** vertex permutation (MATERIALS.md), in the stage
  its material names, sorted with the other draws at the emitter. The
  particle's colour reaches the material as its **Vertex Color**.
- **Only the living are drawn.** Each emitter has a small indirect-arguments
  buffer (`ParticleEmitterGpu::Args`): an entry per section of its mesh (laid
  out by `DrawParticles` when it first draws it, or the mesh changes), or a
  strip's two. The sort counts the living as it sorts them first and writes the
  count into every entry's instance count; the ribbon pass packs the living
  segments to the front in order (a prefix sum, one workgroup) and writes
  theirs. The draws are then `GDISetIndirectArgs` draws
  (`vkCmdDrawIndexedIndirect`, D3D12's `ExecuteIndirect`): an emitter that
  holds 512 with 20 alive vertex-shades 20, with no readback. A backend
  without indirect draws draws the whole ring, the dead collapsed to points as
  before.
- **Time** is the render thread's clock (`RenderContext::GetDeltaSeconds`),
  clamped to a tenth of a second a frame, so the editor's viewport animates
  while the World stands still.
- **Never static**: a particle proxy refuses static draw lists
  (`RenderProxy::AllowsStaticDraws`), its draws changing on their own.
- **Culling** pads the entity by how far a particle can get in its life.

Particles are sorted within a system, and the systems with every other
translucent draw by their emitters; two plumes that cross can blend out of
order. Opaque particles write depth as any mesh does.

LightLab's yard runs a sample of each (`SpawnParticleDemos`; entities
`SparksDemo`, `DebrisDemo`, `LaserDemo`, `TrailDemo`, `ExplosionDemo`), from its
`Content/Particles/`: `Sparks` (Velocity Aligned sprites in repeating bursts
from a Cone, under gravity, bouncing on the floor, through a white-hot to red Gradient and a
shrinking size Curve), `Debris` (Random Spin box meshes coming to rest on the
floor by Collision, and dust thrown out
of a Ring that grows by a Curve, bursting), `Trail` (a ribbon with a glowing head: turn
on Move Emitter; in the yard, AnimateLights flies it round), `Laser` (two
jittering beams, a wide glow and a thin core; in the yard, strung between two
pillars by its Beam Target) -- the strips and sparks with
`ParticleStreak.material.nmgraph`, soft across U and three times as bright, so
Bloom spreads them. `Explosion` is the Flipbook sample: a burst of
fireballs, each playing the 4 x 4 sprite sheet
`Content/Textures/ExplosionFlipbook.nmtexture` once over its life (white-hot,
then orange, then grey smoke) through `ExplosionFlipbook.material.nmgraph`,
with embers bursting out of it. LightLab's `Tools/make_flipbook_texture.py`
writes the sheet (procedural fire, every cell's border transparent so filtering
and mips never show the next frame).

LightLab's yard has a flare (`SpawnFlare`): a red stick, a flame ball whose
emissive (`FlareFlame`) flickers past 10 for Bloom, a red point light, and
smoke (`Content/Particles/FlareSmoke.nmparticle` with the `FlareSmoke` material,
Translucent Unlit: soft round puffs) born red and black well before it fades
(Color Curve 0.45).

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

UI drawing (### UI) needed two more:

- **Scissor rectangles.** `GDISetScissorRect(X, Y, W, H)`, in pixels of the
  viewport from its top-left, clipped to it; `GDIClearScissorRect()` goes back
  to the whole viewport, as `GDISetViewport` does. Those are the target's own
  rows (every backend stores row 0 at the image's top), so a backend's
  `SetScissorRect` takes them as they are.
  `Clear` ignores it.
- **Dynamic meshes.** `GDICreateDynamicMesh(MaxVertices, MaxIndices)`: the
  `GDIMeshDesc` layout (normals and UV1 optional), written in place by
  `GDIUpdateDynamicMesh(Mesh, Desc)`. Within one BeginRendering block each
  update appends behind the last -- draws recorded earlier still read their
  own data at submit -- and the next block starts again at 0 (every backend
  waits for the previous submission first; `GDIGetRenderingSerial` counts
  blocks). False if it no longer fits: the caller makes a bigger one.

| Backend | Scissor | Dynamic mesh |
|---|---|---|
| Vulkan | `vkCmdSetScissor`, kept in the state and set on each new command buffer | host-visible, coherent, persistently mapped buffers; `vkCmdDrawIndexed` at the update's base vertex and index |
| D3D12 | `RSSetScissorRects`, as Vulkan | upload-heap buffers, persistently mapped; `DrawIndexedInstanced` at the base |
| OpenGL | `glScissor`, the test on only while it is not the whole viewport, off around `glClear` | stream buffers written with `glBufferSubData`; `glDrawElementsBaseVertex` |
| Metal | the encoder's scissor, clamped as the viewport's | none (null): Metal has no indexed meshes yet, so no UI |
| Null | recorded (`NullGDIStats`) | counted, nothing stored |

Verification: Vulkan draws the UI GPU checks; D3D12 and Metal are written
against their backends' existing patterns but were not compiled here (Linux);
OpenGL compiles (the host still cannot create a GL context); the Null backend's
are covered by GDI's smoke test.

Particles (### Particles) needed three more, where `GDISupportsCompute()` says
the backend has them -- Vulkan, D3D12 and Null so far:

- **GPU buffers.** `GDICreateBuffer(Bytes, Usage, Data)`: storage for compute
  (`GDI_BU_Storage`), per-instance vertex data (`GDI_BU_Instance`), or both;
  filled once at creation (or zeroed), then only shaders write it.
  `GDIReadBuffer` reads one back, waiting for the GPU, for tests and tools.
- **Compute shaders.** A `.csh` is cooked like the other stages (`cs_5_0` for
  D3D12) and loaded as `ST_Compute`. `GDIDispatchCompute(Shader, Params,
  Bytes, Buffers, Count, X, Y, Z)`: the parameters at binding 0 (up to 1 KB),
  the storage buffers at 1.. (up to 4), declared with `ComputeCommon.glsl`'s
  macros. Inside a frame it is recorded in order with the draws, the open
  render pass closed around it and reopened with its contents; outside one it
  runs at once.
- **Instanced meshes.** `GDISetInstanceBuffer(Buffer, Count)`: the indexed
  draws after it draw Count instances, each a column-major mat4 at locations
  2-5 and a colour at 7 (`GDI_INSTANCE_STRIDE`, 80 bytes), until it is set
  again.

| Backend | Compute | Instanced meshes |
|---|---|---|
| Vulkan | a compute pipeline per shader and buffer count (`VKPipelineCache::GetOrCreateCompute`), its parameters in the frame's UBO ring; full memory barriers either side | vertex layout 3: the mesh's five streams and the instance buffer at binding 5; `vkCmdDrawIndexed` with the count |
| D3D12 | a compute PSO per shader on one compute root signature (the parameters at b0, root UAVs at u1-u4: spirv-cross makes each buffer a `RWByteAddressBuffer`), its parameters in the frame's constant-buffer ring; UAV barriers either side. No render pass to close: the draws after it keep their targets. Buffers track their state per command list, since a buffer decays to COMMON after each | vertex layout 3: the instance buffer at slot 5; `DrawIndexedInstanced` with the count, or `ExecuteIndirect` |
| Null | buffers kept on the CPU; dispatches counted (`NullGDIStats`) | counted |
| Metal, OpenGL | not yet: `GDISupportsCompute()` is false, so nothing is made and particles draw nothing | ignored: one instance |

A compute pipeline is keyed by its shader module, so releasing a compute
shader drops its pipelines from the cache (`ForgetComputeModule`); they are
destroyed at shutdown, past any frame that used them.

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
- Scene capture (`NIMBLE_SMOKE_GPU`): see Scene capture above.
- Particles (Null backend): the component's settings clamped, its Lua and save
  fields; spawns counted round the ring with the fraction carried, a hitch
  clamped, the sort sized to a power of two. GDI's smoke test: buffers,
  dispatches and instance buffers accepted and refused.
- GPU particles (`NIMBLE_SMOKE_GPU=Vulkan`): eleven simulations of 0.1 s read
  back -- 55 particles, each 100 units along +Z a second of its age; the
  instances sorted farthest first, the unused slots last; and a plume drawn
  side on through the instanced path (`ParticleProbe`), red where it is born
  and dark where it has aged.
- GPU (`NIMBLE_SMOKE_GPU=Vulkan|D3D12`): previews of DefaultLit and Wave go
  through the G-buffer and keep their forward-shaded pixels (186 188 192; 40
  103 172); Glow (Translucent Unlit) is unchanged; Negative (PostProcess) inverts
  the preview's clear colour. Clean under the Vulkan validation layer.

## Later

Post process: bloom from the HDR scene colour (it still thresholds display
colours after the tone map); auto exposure; a velocity target for per-object
motion blur; temporal AA; colour grading. Running HDR and image-based lighting
on D3D12, Metal and OpenGL (written, not yet built there). Reflection probe blending
across more than four; cube-face capture time-slicing; SH irradiance.

Hardware shadow comparison, depth bias and front-face culling in GDI (then
drop most of the receiver bias); Masked casters clipped in the Shadows
sub-stage; shadows for Translucent Lit; a tiled or clustered light list past 16
lights; occlusion queries and a depth prepass in the PrePass; per-section
bounds for culling; RGBA16F G-buffer and scene colour.

Particles: compute and instancing on Metal and OpenGL; GPU emission
counts (spawn by events, bursts); collisions with depth; soft particles
(fading where they meet the scene); lit particles' shadows; sorting across
systems; sub-UV animation.

Recording the Base Pass on several threads, after cheaper per-draw work first:
[MULTITHREADED_RENDERING.md](MULTITHREADED_RENDERING.md).
