# Lit translucency and lit particles

A plan, phase by phase, like GLOBAL_ILLUMINATION.md. Nothing here has landed.

## Where it starts

- **Translucent Lit exists** (RENDERPIPELINE.md, "Stages"): forward-shaded
  after the Lighting stage by the same `ShadeLightingSurface`, alpha blended,
  back to front. It gets the clustered light list (LightList.glsl, engine slot
  8 the LTC table), area lights, the shading models, and the probes' IBL
  (`LIGHTING_IBL_ATLAS`, material slot 0).
- **What it lacks against a deferred surface:**
  - no shadows of any kind -- `LIGHTING_SHADOWS` is the deferred pass's only
    (Lighting.glsl's header); the atlases are its material slots 6 and 7;
  - no DDGI and no screen-space GI or reflections (GLOBAL_ILLUMINATION.md,
    "Not yet"): glass and water keep the reflection probes;
  - its specular is blended by its opacity (`Output = vec4(Color, Opacity)`,
    `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`), so clear glass loses its reflections
    and highlights with its body;
  - no velocity: TAA smears it (RENDERPIPELINE.md, "Temporal antialiasing").
- **Particles are already "lit" if their material is.** A sprite emitter
  draws its instanced quad in its material's stage; Translucent Lit works
  today. But it is a surface BRDF on a camera-facing quad -- flat, a specular
  glint on smoke, unshadowed -- at full per-pixel cost on every overdrawn
  layer.
- **Two translucent stages, sorted apart.** Lit smoke and Unlit sparks in one
  system draw in the wrong order wherever they overlap. Lit particles make
  that the common case, not a corner.

Engine texture slots free for forward draws: 11-15 (bindings 15-19); 8 is the
LTC table, 9 and 10 DDGI's atlases (bound only during the IBL pass today).

## Phase 0: a lab and a baseline (landed)

- **TranslucencyLab** in WorldsLab (`Tools/TranslucencyLab/make_translucencylab.py`,
  as LightingLab's): three clear panes (opacity 0.12) before a dark backdrop,
  an overhead slab's sun shadow across the left of them and the ground in
  front; a water strip under a red wall; a lit smoke plume (Steampunk's
  Steam puff) rising through a shadowed spot light's cone, a post in it,
  sparks (Translucent Unlit) through the smoke; a GI Volume, three
  reflection probes, thin volumetric fog, TAA, no auto exposure.
  `WorldsLabTranslucencyLab.cpp` holds the camera at one of four views
  (`WorldsLab.TranslucencyLabView`: 0 lab, 1 glass, 2 plume, 3 water).
- **Repeatable frames:** `[Rendering] FixedStep` (seconds a frame; 0 the
  wall clock) drives the render clock -- `GetDeltaSeconds`,
  `GetTimeSeconds` (particle turbulence's phase read the wall clock) and
  material Time -- and `[Record] StartFrame` has FrameRecorder take frame N
  rather than a wall-clock second. Particle spawns were already seeded per
  emitter and frame.
- **Tools** (`Tools/TranslucencyLab/`): `capture.ps1` (each view's frame N,
  FixedStep 1/60, through a stand-in `ffmpeg` that keeps the frame as a
  .bmp; `-Stats` logs the GPU spans at that frame), `compare.py` (PSNR whole
  and per region, the mean luminance change, `--diff`), `montage.ps1`.
- **Noise floor:** two runs, frame 480, are bit-identical on Vulkan and on
  D3D12 (99 dB in every view and region) -- any difference later is the
  change's.
- **Baseline cost** (GPU ms at frame 480, RTX 2080 Ti, Debug, 1280 x 720):

  | View | Vulkan Forward | Vulkan Total | D3D12 Forward | D3D12 Total |
  |---|---|---|---|---|
  | 0 lab | 0.270 | 1.054 | 0.585 | 2.454 |
  | 1 glass | 0.252 | 1.210 | 0.553 | 2.757 |
  | 2 plume | 0.367 | 1.155 | 0.977 | 3.135 |
  | 3 water | 0.318 | 1.247 | 0.550 | 2.958 |

## Phase 1: one translucent stage (landed)

- Translucent Lit and Translucent Unlit are one stage, **Translucent**
  (`ERenderStage::Translucent`), sorted back to front together; each draw
  keeps its own shader (forward Lit or Unlit). `GetMaterialRenderStage`,
  the stage table and the draw names (`stat` tree) change; the GPU-timing
  span keeps "Forward".
- **Checked:** RenderCore.Smoke on the GPU, 748 checks, none failing on
  Vulkan or D3D12. New: `TestTranslucencyOnGpu` with a red Lit quad and a
  green Unlit one overlapping, each in front in turn -- the nearer wins
  both ways (centre pixel 59 9 11 with red in front, 13 204 13 with green;
  before, green always covered red). Its materials are engine content,
  `Engine/Content/Materials/Testing` (`make_testing_materials.py`), so the
  Engine cache holds them.
- **TranslucencyLab:** bit-identical to Phase 0 on both backends. The
  plume's smoke and sparks share an entity, so their submission order
  already matched depth. Forward GPU time within run-to-run noise (Vulkan
  0.25-0.39 ms, D3D12 0.47-0.77 ms by view).
- **Regression levels** from here on: Courtyard (two runs 81 dB) and
  LightingLab (bit-identical), frame 480, FixedStep. ShootingRange's
  gameplay moves on the wall clock (two runs 29 dB) and is left out.

## Phase 2: shadows on forward Lit (landed)

- The forward template declares the shadow atlases at the engine's slots 11
  and 12 (bindings 15, 16) and defines `LIGHTING_SHADOWS`; RenderContext
  binds the atlases the scene's Lighting stage read to every forward Lit
  draw (`ForwardShadowAtlas`, cleared at `BeginSceneDraws`, so a scene with
  no Lighting stage draws them unshadowed). Sun cascades and shadowed point
  and spot lights, Standard and Modulated.
- **Filter:** `[Shadows] ForwardFilter` (0-2, default **0**, one tap; in
  Project Settings > Shadows), carried in `Scene_ShadowParams.z` (it held
  1 / atlas size, which no shader read); `Shadows.glsl` takes the radius
  from `LIGHTING_SHADOW_RADIUS`. A 3 x 3 filter cost 0.35-0.45 ms in the
  plume view on D3D12 against 0.21 for one tap, the glass looking nearly
  the same (40 dB apart); one tap shows stair steps where a shadow edge
  crosses a pane diagonally.
- **Deviation:** no inset shadows on forward draws. Their table is the
  deferred pass's own block; `InsetShadow` is compiled only where
  `LIGHTING_INSET_SHADOWS` is defined (DeferredLighting.fsh). Contact
  shadows stay deferred-only too. Metal draws no shadows at all, as before.
- **Checked:** RenderCore.Smoke on the GPU, 751 checks, none failing on
  Vulkan or D3D12. New: a white Translucent Lit pane at opacity 0.5, its
  left half behind an opaque blocker from the sun -- left 64, right 124
  with shadows on; 124 and 124 off. (The plan's shadow-edge-against-the-
  floor check was not built.)
- **TranslucencyLab** (against Phase 1): the glass view's shadowed panes
  darker by 11.6 (luminance, 0-255), the lit ones unchanged (79 dB, the
  backdrop's reflection); the plume and water views within 91-98 dB.
  Identical on Vulkan and D3D12. Forward GPU ms (frame 480): Vulkan 0.23-0.52
  by view (Phase 1: 0.21-0.39), D3D12 0.59-1.06 (0.47-0.77); the plume
  view, all its smoke now shadowed, pays the most. Run-to-run GPU timing
  varies by about 15 % on this machine.
- **Regression:** LightingLab unchanged (no translucent surfaces);
  Courtyard 50 dB whole, its glass and water now shadowed.

## Phase 3: GI and premultiplied specular (landed)

- **DDGI for forward Lit**: the forward template includes DDGI.glsl, and
  RenderContext binds the volume's atlases (as the IBL pass bound them,
  `ForwardDDGIAtlas`) at engine slots 9 and 10 for each forward Lit draw;
  `IBL_LightModel` takes the volume's irradiance where it is. Screen-space
  GI stays G-buffer only.
- **Translucency Lighting** (a Material Output property, Translucent Lit
  only): **Surface** writes `(emissive + baked + albedo x diffuse) x
  coverage + specular`, premultiplied, blended (one, one minus source
  alpha); **Legacy** fades everything by Opacity, as before. The generated
  code defines `MATERIAL_TRANSLUCENCY_SURFACE` and the `.material` says
  `TranslucencyLighting Surface`, so no new permutation key.
- **Old content unchanged, no files touched:** a new Material Output node
  gets Surface (the property's Default); a graph without the property is
  Legacy (`PropertyDefinition::Absent`, which the graph editor's Details
  panel shows for a node that lacks it). The plan's "set Legacy on old
  content in the same commit" was not needed.
- **Checked:** RenderCore.Smoke on the GPU, 754 checks, none failing on
  Vulkan or D3D12; NodeGraph.Tests. New: a pane at opacity 0.02, roughness
  0.3, over black, the sun down the view -- centre 255 as Surface, 70 as
  Legacy (backdrop 36: 2 % of the HDR highlight survives the tone map).
  The plan's DDGI check (a quad by a red wall in a volume) was not built:
  a volume lights only after every probe has rendered, over many frames.
- **TranslucencyLab** (glass and water now Surface; the smoke stays Legacy):
  glass brighter by 2.5-5 (its reflection kept), water by 6.6 in its view;
  the plume unchanged (74-81 dB). Identical on Vulkan and D3D12. Forward GPU
  ms: Vulkan 0.29-0.48 by view, D3D12 0.63-1.13 -- within Phase 2's noise.
- **Regression:** Courtyard 84 dB against Phase 2 (its floor is 81),
  LightingLab bit-identical.
- Fog's composite already covered translucent surfaces; nothing changed there.

## Phase 4: reflections on glass and water (landed)

- A forward Lit draw smoother than the SSR node's Max Roughness marches the
  screen in its own pixel shader (`ForwardReflections.glsl`): a mirror ray,
  8-24 steps (the node's, capped), four halvings, against a **copy of the
  opaque depth** (`SceneTargets::DepthCopy`, R32F -- RGBA32F where that is
  all there is -- copied by `PPCopy` before the forward stage: the depth
  itself is bound for the depth test, and sampling it there would be a
  feedback hazard), reading the last frame's colour (`LastColor`) at the hit
  reprojected by the last frame's view. Engine slots 13 and 14. The trust
  fades as PPScreenReflections' does; `IBL_LightModel` blends it over the
  probes' specular.
- **Deviations:** not the planned material slot 7 for the depth (only a
  PostProcess material binds it) but the copy; the hit's own motion is not
  known (no velocity read), so a moving hit reflects where it was; the
  march's jitter shifts each frame (golden ratio) so TAA averages it.
- Its settings and the last view live in the scene block where the unused
  `Scene_Probes[12]` were (`Scene_PrevViewProjection`,
  `Scene_ForwardReflections[2]`, `Scene_Spare[6]`; same size), set for the
  forward stage only.
- Only while the stack's SSR node is on, in an HDR scene with a last frame;
  `[Rendering] ForwardReflections` 0 turns it off alone -- and then the lab
  is bit-identical to Phase 3.
- **Checked:** RenderCore.Smoke on the GPU, 754 checks, none failing, Vulkan
  and D3D12 (no new check: it needs a post stack with SSR and a frame of
  history; the lab's on/off pair stands in). TranslucencyLab: the water
  view's water brighter by 5.6 (the red wall in it), the lab view's by 6.8;
  the glass's lit panes by 1.5; the plume unchanged (99 dB: the smoke's
  roughness 1 never marches). Identical on Vulkan and D3D12. Faint
  horizontal bands remain in the water where the steps cross the wall's
  base.
- **Cost:** the water view's Forward 0.29 -> 0.48 ms on Vulkan (+0.20),
  0.63 -> 0.87 on D3D12 (+0.24, a little over the +0.2 budget, the depth
  copy included); the plume view unchanged.
- **Regression:** LightingLab bit-identical; Courtyard 52 dB against
  Phase 3 -- its stack has the SSR node, so its glass and water reflect now
  (the node is the opt-in, not the material).

## Phase 5: the Volume shading model (lit particles, per pixel) (landed)

- **Volume** (`SHADING_MODEL_VOLUME`, 6; `EMaterialShadingModel::Volume`),
  forward only -- the schema refuses it outside the Translucent domain:
  light scattered by a thin medium, not reflected by a surface.
  - Each light: its radiance x a Henyey-Greenstein phase of **Phase
    Anisotropy** (a new Material Output pin, -0.9 to 0.9) about the light's
    travel and the view, times 4 pi so an even medium is as bright as
    Lambert facing the light; no N.L, no specular, no area-light lobes.
    **Normal Influence** (a new pin, 0-1) wraps it toward the normal
    (half Lambert, times 2 to average 1), so a flipbook's normal map or a
    sphere normal still reads.
  - Ambient: the probes' (or a GI Volume's) irradiance up and down,
    averaged; the hemisphere ambient's middle without IBL.
  - Shadows from Phase 2, per pixel; never metal.
- **Particle Sphere Normal** (`Input.ParticleSphereNormal`, pixel only):
  a world normal bulging out of a quad like a sphere, by UV, along the
  cotangent frame (`Material_SphereNormal`).
- The cook's model list, the pixel key's bits 5-7 and the `.material`'s
  `ShadingModel Volume` take it with no other change.
- **Checked:** RenderCore.Smoke on the GPU, 758 checks, none failing on
  Vulkan or D3D12; NodeGraph.Tests. New: a Volume quad over black, the sun
  behind it (the backdrop casting no shadow) -- 117 scattering forward
  (g 0.6), 60 back (g -0.6), and even across (114 | 117 | 114). (The
  plan's "no pixel above the diffuse bound" check was dropped: a medium
  has no specular lobe to glint.)
- **TranslucencyLab:** the plume's smoke is Volume (g 0.3, Normal
  Influence 0.5, Particle Sphere Normal): softer and slightly darker
  (-0.5 in its region, 43 dB); glass and water unchanged. Identical on
  Vulkan and D3D12. The plume view's Forward 0.55 ms on Vulkan, 1.02 on
  D3D12 -- the same as Phase 4 within noise (the medium drops the specular
  lobes the surface model paid for). The plan's "at most 2x unlit" was not
  measured against an unlit plume.
- **Not done:** LightLab's FlareSmoke and Explosion are in Games/Sandbox,
  which is not tracked; their lit variants were left out.
- **Regression:** LightingLab bit-identical; Courtyard 83 dB (its floor 81).

## Phase 6: per-particle lighting (lit particles, cheap) (landed)

- **Deviation: the vertex stage, not a compute pass, and a material setting,
  not an emitter's.** The check came out against compute: GDI's dispatch
  binds a parameter block and storage buffers only -- no textures, scene or
  light block -- and the particle passes run before the frame's shadow maps
  are drawn, so they would read last frame's. The instanced draw runs after
  the shadows, and its vertex shader can light each instance at its centre.
  The shared vertex shaders have no material block to say whether to, so the
  material says: a Translucent Unlit material's **Particle Lighting** (Off,
  **Per Particle**) and **Particle Phase**.
- **The material:** Per Particle defines `MATERIAL_PARTICLE_LIGHTING` and
  `MATERIAL_PARTICLE_PHASE` in its generated code and writes `HasOffset 1`
  (a zero `CalcMaterialOffset`), so it is cooked with its own vertex shaders;
  the InstancedMesh one includes `ParticleLighting.glsl` and multiplies the
  instance's colour (display space: through linear) by `Particle_Light` at
  the instance's origin -- the sun through its shadow maps (one tap), every
  point and spot light in the list with theirs (no tiles here; area lights
  as points), the probes' or a GI Volume's irradiance up and down averaged,
  each through the Henyey-Greenstein phase (Volume's). Past 1 survives: the
  interpolant is float, the scene HDR. RenderContext binds such a material
  as a forward Lit one (IBL atlas, shadow and DDGI atlases).
- **GDI:** sampled textures and the light block are visible to the vertex
  stage. Vulkan: material samplers' `stageFlags` include the vertex stage,
  and every barrier into or out of a sampled layout names the vertex shader
  stage beside the fragment's. D3D12: the texture tables and the light CBV
  are visible to all stages, and every sampled resource rests in
  `D3D12_STATE_SAMPLED` (pixel and non-pixel shader resource) in place of
  `PIXEL_SHADER_RESOURCE`. OpenGL has no such limit; Metal draws no shadows.
- **Checked:** RenderCore.Smoke on the GPU, 758 checks, none failing on
  Vulkan or D3D12, after the GDI change. TranslucencyLab per pixel is
  bit-identical to Phase 5 (the GDI change shows nowhere). With
  `WorldsLab.TranslucencyLabPerParticle 1` (the plume's smoke is
  `SmokePerParticle`, Unlit, Per Particle): the plume region 1.7 darker
  than per pixel and flatter (no sphere normal), 37 dB. Vulkan and D3D12
  agree to 60 dB here, not bit for bit as elsewhere (vertex-stage math).
  (The plan's GPU smoke check with a particle system was not built; the
  lab's pair stands in.)
- **Cost** (the plume view's Forward, ms): per pixel 0.57 Vulkan, 1.01
  D3D12; per particle **0.27** and **0.64** -- below Phase 0's
  unshadowed, Legacy-lit plume (0.37, 0.77), compute-free.
- **Regression:** LightingLab bit-identical on both backends; Courtyard
  78 dB (its floor about 81).
- Light varies across a large particle only by its neighbours'; Per Pixel
  (the Volume model) is there for hero effects.

## Phase 7: translucent shadow casting (landed)

- **Deviation: opacity shadow maps [Kim and Neumann 2001], not Fourier
  opacity maps.** Four slices of optical depth between a view's casters'
  nearest and farthest depth, one a channel of one RGBA16F target: the same
  depth-resolved transmittance (what Phase 8 needs), no ringing to clamp,
  one render target where a 1 + 3-term series needs two.
- **Casters:** particle emitters with **Cast Shadows** (a new emitter
  setting, Sprite only) and **Shadow Density** (their alpha times it).
  `RenderContext::CastParticleShadows`, after `DrawParticles`, keeps the
  emitter's draw once a frame. Translucent surfaces (glass) do not cast yet.
- **The atlas** (`OpacityAtlas`, `RenderCore.OpacityShadows`): the shadow
  atlases' size wide, half high -- the directional atlas's tiles in the left
  half, the local's in the right, each at half resolution. At the Shadows
  stage's end (`IssueTranslucentShadows`), per light view (cascades, spot
  and point faces; not insets) the casters whose bounds reach it draw into
  its tile, additively, with `OpacityShadowCaster.vsh/.fsh`: each instance's
  quad turned square to the light (not the camera), a soft disc times its
  alpha times Shadow Density as its opacity, `-ln(1 - a)` into the two
  slices either side of its depth between the view's casters' bounds' depth
  span. No depth test: an opaque caster in front already shadows.
- **The spans** ride in the scene block: per cascade in `Scene_Spare[0..1]`,
  per shadowed local light in `Scene_Locals[6i+5].zw` (both free);
  `Scene_Spare[2].x` says the atlas is bound (engine slot 15).
- **Receivers:** `Shadows.glsl`'s `TranslucentLight` -- the slices in front
  of the receiver's depth summed (the one it is in, partly), `exp(-that)` --
  times the opaque shadow in `DirectionalShadow` and `LocalShadow`, so the
  Lighting stage, forward Lit (Phase 2), the Volume model and per-particle
  lighting (`LocalShadow` takes the span) all receive it.
- `[Shadows] TranslucentShadows` 0 turns it off: the lab is then
  bit-identical to Phase 6.
- **Checked:** RenderCore.Smoke on the GPU, 763 checks, none failing on
  Vulkan or D3D12. New: 48 half-opaque particles that draw nothing (an
  opacity-0 material) but cast, under a sun straight down -- the floor under
  them 128 with translucent shadows on, 210 off and 210 at Shadow Density
  0; aside, 210 either way. (The plan's `(1 - a)^N` stack and the opaque
  blocker above the plume were not built: a burst's particles land at
  random.)
- **TranslucencyLab** (the plume's smoke casts): a soft shadow on the floor
  under and beside the plume (the floor region 1.1 darker), and the smoke
  shading itself -- too dark at its core until Phase 8 (the plume region 9
  darker in its view). Per particle receives the same. Vulkan and D3D12 agree
  to 60 dB (as since Phase 6).
- **Cost:** the Shadows span in the plume view 0.08 -> 0.13 ms on Vulkan,
  0.13 -> 0.18 on D3D12; nothing without casters.
- **Not done:** glass casting (a Translucent material's Cast Translucent
  Shadow); point lights' faces share one span; fog's march does not read the
  atlas yet.
- **Regression:** LightingLab bit-identical, Courtyard 80 dB (no casters).

## Phase 8: self-shadowing

- A particle that is also a caster reads its own light's opacity map at its
  own depth, biased toward the light by half its own thickness (the
  emitter's Size along the light), so it is shadowed by what lies in front
  of it and not by itself. Per pixel (Phase 5) and per particle (Phase 6).
- **Ambient self-occlusion:** the same map read at the particle's depth also
  darkens its ambient by `Ambient Occlusion Strength` x the optical depth
  in front of it toward the sun -- a cheap stand-in for the plume shading its
  own sky light; off by default.
- Translucent surfaces (glass that casts) do not shadow themselves.

## Not in this plan

- Particles as lights (a module adding point lights to the clustered list:
  the list is built on the CPU, the particles live on the GPU).
- Refraction (a scene-colour copy before the translucent stage, offset by a
  Refraction pin) -- small once Phase 4's copy is bound; its own phase if
  wanted.
- Order-independent transparency; velocity or a responsive mask for TAA.
- Translucent shadows from the baked lights (the bake sees opaque geometry
  only).

## Validation

Five layers. A phase is done when all five pass for it; each phase's commit
records its results here, as GLOBAL_ILLUMINATION.md's phases do.

### 1. Repeatable particles (Phase 0, before anything else)

Particle captures were not repeatable: the simulation ran on the render
thread's wall clock, so two runs differed.

- `[Rendering] FixedStep` steps the render clock by a fixed dt a frame, and
  `[Record] StartFrame` captures frame N, so a capture is the same frame
  every run (Phase 0: bit-identical on Vulkan and D3D12).
- Every later comparison uses both (`capture.ps1`).

### 2. Analytic GPU checks (RenderCore.Smoke)

Built like `TestShadowsOnGpu`: a tiny scene, a 256 x 256 scene capture,
pixels compared with what the maths predicts. Each needs no reference
image, runs on Vulkan, D3D12 and OpenGL (Null: the bindings and
permutations only), and dumps PPMs under `NIMBLE_SMOKE_DUMP`.

| Phase | Scene | Expect |
|---|---|---|
| 1 | a red Lit quad in front of a green Unlit one, then swapped | the nearer colour wins where they overlap, both ways |
| 2 | a white Translucent Lit quad, half behind an opaque blocker from the sun, over a floor | its shadowed half darker by the sun's share (within 10 %); its shadow edge within 2 px of the floor's |
| 2 | the same with `[Shadows] Enabled` 0 | the two halves match |
| 3 | an opacity-0.02, roughness-0 pane against a dark backdrop, a bright sky behind the camera | Surface: brighter than the backdrop by the sky's Fresnel reflection; Legacy: equal to the backdrop |
| 3 | a Translucent Lit quad in a GI Volume with a red wall beside it | redder with DDGI on than off |
| 4 | a smooth Translucent Lit floor below a red wall, SSR node on | red in the reflection; with the node off, the probes' colour |
| 5 | one Volume particle between camera and sun, anisotropy +0.6 then -0.6 | brighter at +0.6 looking toward the sun, dimmer looking away; no pixel above the diffuse bound (no glint) |
| 5 | Normal Influence 0 | uniform across the quad |
| 6 | one particle, Per Pixel then Per Particle | mean colour within 5 %; instance colour past 1 survives (HDR) |
| 7 | N particles of opacity a stacked between sun and floor | the floor under them at (1 - a)^N of its lit value, within 10 %, for N = 1, 4, 16 |
| 7 | opacity-0 casters; casters with Cast Shadows off | the floor unchanged |
| 7 | an opaque blocker above the plume | the floor as dark as with the blocker alone |
| 8 | two particles in line with the sun | the far one darker by about (1 - a); a lone particle at least 0.9 of its unshadowed light |

### 3. Image regression

- **TranslucencyLab** (Phase 0), four fixed camera bookmarks, before and
  after each phase, captured twice for the noise floor. Each phase names the
  regions it should change (a mask per bookmark); outside them, PSNR stays
  within 1 dB of the floor; inside them, the change is in the expected
  direction (darker under shadow, brighter reflections).
- **Existing levels** -- LightLab yard (its sparks, flare smoke, explosion),
  LightingLab, Courtyard, ShootingRange -- unchanged by Phases 1-8 except
  where content opts in. Phase 3's Surface default is the one expected
  change on old Lit translucent content: those materials get Legacy in the
  same commit, and the levels must then match.
- Published side by side (before, after, difference) for review.

### 4. Cost

`stat frame` on TranslucencyLab and the LightLab yard, Vulkan and D3D12,
1280 x 720, before and after each phase (GLOBAL_ILLUMINATION.md's table).
Budgets to hold, or argue in the phase's notes:

| Phase | Budget |
|---|---|
| 1 | Forward within 3 % |
| 2 | +0.3 ms Forward on TranslucencyLab |
| 3, 4 | +0.2 ms each |
| 5 | a lit plume per pixel at most 2x its unlit cost |
| 6 | a lit plume per particle within 1.2x its unlit cost, compute included |
| 7, 8 | +0.5 ms for the sun's cascades and two spot lights; 0 when nothing casts |

### 5. Backends and switches

- Vulkan and D3D12: everything above. OpenGL: layer 2 and one TranslucencyLab
  capture per phase. Metal: builds (no shadows there, by design).
- Every new switch (`ForwardFilter`, `ForwardReflections`,
  `TranslucentShadows`, `TranslucentPointLights`, Legacy) is captured off
  and on; off must match the phase before it.
- Debug views: `show translucentshadow` (a light's opacity map, optical
  depth as grey) and `show particlelight` (Per Particle light alone), to
  diagnose what layer 3 flags.

## References

- Henyey, Greenstein 1941. "Diffuse radiation in the galaxy." *ApJ* 93.
- Jansen, Bavoil 2010. "Fourier Opacity Mapping." I3D.
- McGuire, Bavoil 2013. "Weighted Blended Order-Independent Transparency." *JCGT* 2(2).
- Wronski 2014. "Volumetric Fog." SIGGRAPH course *Advances in Real-Time Rendering*.
