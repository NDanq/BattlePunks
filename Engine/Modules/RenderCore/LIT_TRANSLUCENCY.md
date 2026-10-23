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

## Phase 0: a lab and a baseline

- **TranslucencyLab** in WorldsLab (`Tools/TranslucencyLab/make_translucencylab.py`,
  as LightingLab's): glass panes half in a wall's sun shadow, a water strip
  under a smooth wall, a smoke plume crossing a spot light's cone and its
  shadow and casting onto the floor, sparks through the smoke, all inside a
  GI Volume, under volumetric fog.
- Repeatable particles: a fixed step and seed (see "Validation").
- Baseline captures (each twice, for the noise floor) and the Forward column
  of `stat frame`, on Vulkan and D3D12. Every later phase is measured against
  it.

## Phase 1: one translucent stage

- Translucent Lit and Translucent Unlit become one stage, **Translucent**,
  sorted back to front together; each draw keeps its own shader (forward Lit
  or Unlit). `GetMaterialRenderStage` and the stage table change; the
  GPU-timing column and the `stat` names keep "Forward".
- Costs pipeline switches where lit and unlit interleave; measured.
- Removes the ordering note under RENDERPIPELINE.md's stage table.

## Phase 2: shadows on forward Lit

- Bind the sun's and the local lights' shadow atlases to engine slots 11
  and 12 for every forward Lit draw; `LIGHTING_SHADOWS` for the forward
  template too.
- The insets' table (`Lighting_Insets`, now the deferred pass's own block)
  moves into the scene block, beside the cascades.
- Forward draws filter with a smaller kernel (`[Shadows] ForwardFilter`,
  default 1 tap radius): translucent overdraw multiplies the cost.
- Contact shadows stay deferred-only (no depth of the translucent surface to
  march from). Metal still draws none.

## Phase 3: GI and premultiplied specular

- **DDGI for forward Lit**: include DDGI.glsl in the forward template, and
  keep its atlases bound at engine slots 9 and 10 through the translucent
  stage (today the IBL pass binds them and clears them after);
  `IBL_LightModel` then takes the volume's irradiance where it is, as the IBL
  pass does. Screen-space GI stays G-buffer only.
- **Specular over opacity**: a Translucent Lit draw writes
  `diffuse x opacity + specular + emissive x opacity` and blends
  `ONE, ONE_MINUS_SRC_ALPHA` (premultiplied), so a pane at opacity 0.05 still
  reflects the sky and the sun's highlight. Fresnel already raises its
  specular at grazing angles; `opacity` becomes the body's coverage only.
  A material switch, **Translucency Lighting** (Surface, the new default for
  Lit; Legacy, today's), keeps old content looking as it did.
- Fog's composite already covers translucent surfaces; nothing changes there.

## Phase 4: reflections on glass and water

- A forward Lit draw smoother than the SSR node's Max Roughness marches the
  screen in its own pixel shader: `PPScreenReflections`' march as a function
  (ScreenTrace.glsl), against the opaque depth (material slot 7,
  `Material_SceneDepth`), reading last frame's colour (TAA history or the
  `PPCopy`, engine slot 13). Mirror ray only, a fixed step count; trust fades
  the same way, the probes behind it.
- Only while the stack's SSR node is on; `[Rendering] ForwardReflections`
  turns it off alone.

## Phase 5: the Volume shading model (lit particles, per pixel)

- **SHADING_MODEL_VOLUME** (6), forward only (a Base Pass material with it is
  a compile error, as is Opaque or Masked): light scattered by a thin
  medium, not reflected by a surface.
  - Each light: its irradiance x a Henyey-Greenstein phase of **Phase
    Anisotropy** (a new pin, -0.9 to 0.9; 0.3 for smoke) about the view, no
    N.L, no specular. **Normal Influence** (0-1) wraps by N.L toward a
    shaped sprite's normal, so a flipbook's normal map still reads.
  - Ambient: the probes' and DDGI's irradiance averaged over up and down
    (non-directional), x Base Color.
  - Shadows from Phase 2, per pixel.
- **Sphere normals** for sprites: a material node, *Particle Sphere Normal*,
  bends the quad's normal across its UV like a sphere, for round puffs under
  a Default-model material.
- LightLab's `FlareSmoke` and LightingLab get a lit variant; Explosion's
  smoke goes Volume with its flipbook.

## Phase 6: per-particle lighting (lit particles, cheap)

- An emitter setting, **Lighting**: Per Pixel (the material's stage, Phase 5)
  or **Per Particle**. Per Particle lights each living particle once, at its
  centre, in a third compute dispatch after the sort (`ParticleLight.csh`):
  the Volume model's lights, its sun and local shadows (one tap each), DDGI
  or the probes. It multiplies the light into the instance colour
  (`InInstanceColor`, a vec4 attribute; that it holds values past 1 is
  checked first), so the material draws **Unlit** -- `Base Color x Vertex Color` as
  emissive -- and an overdrawn plume costs what an unlit one does.
- Needs the compute dispatch to read the light block, the shadow atlases and
  DDGI's atlases: the dispatch's bindings grow (checked first on all three
  desktop backends; OpenGL's compute images are the risk).
- Light varies across a large particle only by its neighbours'; documented,
  and Per Pixel is there for hero effects.

## Phase 7: translucent shadow casting

Opaque casters write depth into the shadow atlases; a plume has no single
depth, so it gets a second map per shadowed light: a **Fourier opacity map**
[Jansen and Bavoil 2010] -- along each texel's ray from the light, the optical
depth as a short Fourier series between the translucent casters' near and far
depth in that tile.

- **Casters:** particle emitters with **Cast Shadows** (a new emitter
  setting, off by default) and Translucent materials with **Cast Translucent
  Shadow**. Each draws into its lights' tiles with a depth-only-style
  permutation that writes `extinction = -ln(1 - opacity)` x the series'
  basis at its light depth, additively; no depth write. Opaque Mesh
  particles cast into the ordinary atlases (checked; added if the instanced
  path skips the Shadows sub-stage today).
- **The maps:** a 1 + 3-term series, seven floats in two targets per tile,
  at a quarter of the opaque tile's resolution (smoke is soft). RGBA32F
  where `GDI_RTF_RGBA32F` is offered -- the higher terms ring and lose
  precision in half floats -- else RGBA16F with the series windowed harder.
  Sun cascades and spot lights first; point lights (six faces) behind a
  `[Shadows] TranslucentPointLights` switch.
- **Receivers:** a receiver's light is its opaque shadow x
  `exp(-reconstructed optical depth at its depth)`, clamped to 0-1 against
  ringing: the Lighting stage (opaque surfaces under smoke), forward Lit
  (Phase 2), the Volume model (Phase 5), the per-particle pass (Phase 6), and
  volumetric fog's march (shafts cut by a plume).
- **Cost control:** a tile with no translucent caster in its frustum keeps
  its map cleared and is not sampled (a flag per tile in the scene block);
  `[Shadows] TranslucentShadows` 0 turns it all off.

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

Particle captures are not repeatable today: the simulation runs on the
render thread's wall clock (`GetDeltaSeconds`), so two runs differ.

- `[Particles] FixedStep` (seconds; 0 off) steps every emitter by a fixed
  dt per frame, and `[Particles] Seed` fixes the spawn random stream, so a
  capture N frames in is the same frame every run.
- FrameRecorder and the smoke test use both. The noise floor for every
  later comparison is two baseline captures with them on; expected to be
  near-identical, and a floor worse than the opaque levels' (ShootingRange
  ~36 dB) means the determinism is not done.

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
