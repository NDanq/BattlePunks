# Lighting and global illumination: plan

Status: **Phases 0-3 landed.** Phases land in order on `dev-rendering`, one
commit each; each updates this note (and [RENDERPIPELINE.md](RENDERPIPELINE.md))
with what was built, how it was checked and what it costs.

The renderer lights a frame with one sun, at most 16 point and spot lights
(`RENDER_LOCAL_LIGHT_MAX`), a sky, at most four reflection probes, and baked
lightmaps and probes for static worlds. Nothing moving bounces light; a
dynamic light lights only what it sees directly. This note adds the lighting
a photoreal frame needs at 60 fps: many lights, area lights, energy-conserving
rough metals, better reflections, and diffuse global illumination that moves.

## Where it starts

- **Lights** reach the shaders in the scene block (`Scene_Locals`, six vec4s a
  light, 96 vec4s), every pixel looping over all of them (`ShadeSurfaceSplit`).
  The scene block is 4 KB and copied per draw, so it cannot simply grow.
- **Texture slots:** GDI has eight material texture slots (bindings 4-11). The
  Lighting stage uses all eight (GBuffer0-4, depth, two shadow atlases); the IBL
  pass seven. GI needs several more.
- **Textures from the CPU** are 8-bit only (`EngineCore::Image`: RGBA8, BGRA8,
  Gray8); float targets exist only as render targets.
- **Colour:** display to linear is `pow(x, 2.2)` throughout (`ToLinear`,
  `ToDisplay`, `Tonemap.fsh`), not sRGB's curve.
- **Specular** loses the energy of multiple scattering between microfacets, so
  rough metals are too dark (single-scattering GGX).
- **Reflections:** four probes plus the sky in a 768 x 768 atlas, a probe
  captured whole (six views) when due, at most two a frame. Screen-space
  reflections fade out above Max Roughness and reflect sharply below it.
- **Diffuse indirect light:** baked (lightmaps, probe ambient cubes) or the
  sky's image-based irradiance. Nothing dynamic.

## Phase 0: room to work (landed)

- **GDI: 16 material texture slots** (bindings 4-19), the skin blocks moved to
  20 and 21. Materials still author eight (MaterialSchema's slots 1-7); slots
  8-15 are the engine's, for the passes and for forward Lit materials' light
  lists. The D3D12 root signature (48 of its 64 DWORDs), the Vulkan descriptor
  layout and pool, and OpenGL's texture units grow to match.
- **A light block,** binding 22, up to 64 KB (`GDI_LIGHT_BLOCK_MAX_BYTES`, every
  desktop backend's uniform range): `GDISetUniformBlock` keeps it beside the
  material, scene and skin blocks, and each backend uploads it once per change
  and binds it whole to every material draw. The plan's packed-float textures
  were dropped: a texture update from the CPU is a synchronous upload on Vulkan
  (a transient command buffer, waited on), and the scene block is compared
  byte for byte on every set, so a big light list in it would cost every draw.
- **Shader model 6.0** on D3D12, Vulkan 1.3 SPIR-V and MSL 2.3: shader model
  5.0 allows fourteen cbuffer registers (b0-b13), too few for bindings 20-22.
  `cook_shaders.py` now writes HLSL for shader model 6.0 (spirv-cross) and
  compiles it to DXIL with the Windows SDK's `dxc` (signed by the `dxil.dll`
  beside it; HLSL 2018 rules); glslc targets `vulkan1.3` (SPIR-V 1.6, the
  backend's own API version); spirv-cross writes MSL 2.3, whose SIMD-group
  functions match SM 6.0's wave operations. A D3D12 adapter without shader
  model 6.0 is passed over (`D3D12Device::TryAdapter`). OpenGL stays at GLSL
  4.50.
- **Measuring:** the editor's `stats` command prints each stage's GPU time
  when `stat frame` is on, and logs it, so a scripted run (`--command`) can
  measure a change.

Checked: RenderCore.Smoke on the GPU, 743 checks, none failing on Vulkan or
D3D12 (DXIL verified in the staged `.cso`); NodeGraph.Tests; DummyTest's
skinned crash dummy draws posed on both (the skin blocks moved). No pixel
changes.

## Phase 1: true sRGB (landed)

Every `pow(x, 2.2)` / `pow(x, 1 / 2.2)` that turned display colours into light
and back is sRGB's piecewise curve [IEC 61966-2-1]: `SrgbToLinear` /
`LinearToSrgb` in `Common.glsl` (Lighting.glsl's `ToLinear` / `ToDisplay`,
the tone map's output, the background's encoding, the IBL and fog passes,
Skin's albedo, the shading models' colours), and `RenderCore::SrgbToLinear` /
`LinearToSrgb` (RenderTypes.h) on the CPU: the sky's colours, the light bake's
material colours, the texture viewer's HDR preview. `MATERIAL_TEMPLATE_VERSION`
17 (it also covers Phase 0's skin bindings).

**Changes pixels on purpose:** a display 0.5 is linear 0.214 instead of 0.218,
and darks differ most (display 0.04: 0.0031 instead of 0.0008). DefaultLit's
preview reads 187 188 192 (it read 186 187 191). A bake made before this was
measured with the old curve; rebake to match exactly.

Checked: RenderCore.Smoke on the GPU, 743 checks, none failing on Vulkan or
D3D12 -- the clear-colour check now allows one step (display to linear in the
half-float scene colour and back read 119 for 120 on one row); Courtyard and
ShadingModelLab look as before, a shade richer in the darks.

## Phase 2: many lights (landed)

Clustered shading [Olsson 2012; Persson 2013] with Drobot's z-binning
[Drobot 2017], built on the CPU for each view (`BuildLightList`,
RenderContext.cpp) and read from the light block (`LightList.glsl`):

- **The list:** every active point and spot light, up to
  `RENDER_LOCAL_LIGHT_MAX` (256; it was 16), shadow casters first then nearest
  as WorldBridge gathers them, re-sorted by the view depth of their centres;
  six vec4s each (two spare, for Phase 3). Shadow casters keep their shadow
  views (at most four): an entry names its `Scene_Locals` slot, which holds its
  shadow matrix, tint and biases -- the scene block keeps the first
  `RENDER_SCENE_LOCAL_LIGHT_MAX` (16), for those and for volumetric fog.
- **Tiles:** the target in 32 x 18 tiles, each a 256-bit mask of the lights
  whose bounding cube's projection reaches it (all tiles for a sphere through
  the near plane).
- **Z-bins:** 64 slices of view depth, logarithmic from the near plane to the
  furthest light's reach, each the lowest and highest index of the lights whose
  depth reaches it. A pixel walks its tile's mask only between its slice's two
  indices (`LightingLocal` per light, in `ShadeSurfaceSplit`, for the Lighting
  stage and forward Lit materials alike).
- **The block:** 43 KB of the light block's 64; set once per view in
  `RenderSceneStages`, so captures and previews get their own.
- `[Rendering] ClusteredLights` (on): off, every light is in every tile and
  slice -- each pixel weighs them all, as before this phase.

Checked: RenderCore.Smoke on the GPU, 743 checks, none failing on Vulkan or
D3D12, the point and spot shadow checks among them; LightingLab's field of 144
lights all draw (16 did before), and the frame is bit-identical with
ClusteredLights off. No pixel changes with 16 lights or fewer: the same lights,
in another order.

## Phase 3: area lights and energy (landed)

- **Area lights** by linearly transformed cosines [Heitz 2016; Hill 2016]
  (`AreaLights.glsl`): the specular lobe is a clamped cosine under a 3 x 3
  transform, so its integral over the light's polygon is the cosine's over the
  polygon transformed back -- an edge sum after clipping to the horizon (Heitz
  et al.'s case table).
  - **The fit:** `Scripts/fit_ltc.py` fits the transforms to this renderer's own
    direct-light GGX (Smith with Schlick-GGX, k = (r + 1)^2 / 8), not Heitz's
    published tables, so area and point highlights agree: 64 x 64 cells over
    roughness and sqrt(1 - NdotV), Heitz's isotropic three-parameter matrix by
    Nelder-Mead against his MIS error, plus each cell's albedo split by Schlick's
    Fresnel (so specular scales by F0 x + y). About eight minutes in numpy; the
    result is `LTCTables.inl`, uploaded once as packed floats and decoded into a
    64 x 128 half-float target (`LTCDecode.fsh`, `EnsureLTC`) bound at engine
    slot 8 for the Lighting stage and forward Lit draws.
  - **Rect Light:** a new component (`RectLightComponent`; Docs/Components/
    RectLight.md): Width x Height across its entity's +X and +Y, facing +Z, its
    Intensity its surface's brightness, windowed to nothing at Range. Only its
    front lights. The editor draws its outline and facing.
  - **Spheres:** a Point or Spot Light's Source Radius (until now the bake's
    alone) makes it a sphere, integrated as the square of the disc's area facing
    the shading point, as bright as the point it was at that distance. A sphere
    narrower than 0.05 radians from the pixel lights as a point, which it nearly
    is, so a small bulb costs what it did.
  - Diffuse: the cosine's own integral over the polygon (identity transform).
  - **Changes pixels on purpose** where a light has an area: a Rect Light lights
    as a rectangle, and a bulb with a Source Radius wide enough from a surface
    widens its highlight to the sphere's.
  - Not yet: a Rect Light's shadow (the plan's centre spot shadow), Barn Door;
    area lights light the shading models with Default's lobes (Cloth keeps its
    point sheen); volumetric fog lights a rect as a point.
- **Energy compensation** for rough metals [Kulla 2017; Fdez-Aguera 2019;
  Turquin 2019] (`SM_EnergyCompensation`): the specular lobes -- point, area and
  image-based -- times `1 + F0 (1 / E - 1)`, E the split-sum's directional
  albedo (Karis's fit at F0 = 1). **Changes pixels on purpose:** rough metals
  brighten, most at roughness 1; smooth surfaces and dielectrics hardly move.
- `[Rendering] AreaLights` and `[Rendering] EnergyCompensation` (on): off,
  every light lights as a point and specular is single scattering, as before
  this phase -- flags in the light block's header.

Checked: RenderCore.Smoke on the GPU, 743 checks, none failing on Vulkan or
D3D12; the LTC target in the editor's Render Target view (the fitted matrices
and amplitudes); LightingLab's smooth gold spheres reflect the ceiling
softbox as a rectangle and the warm wall panel as a tall streak (round dots
with AreaLights off); its rough spheres brighten with EnergyCompensation (5 %
of the view's pixels, up to 73 of 255).

## Phase 4: reflections

- **More probes:** the IBL atlas grows to 1536 x 1280, 20 blocks: 16 probes,
  the sky, the sheen table (and spare). The 16 probes nearest whose influence
  the view reaches; a pixel blends them nearest first, as now.
- **Captures over frames:** a probe captures one face a frame (six frames a
  probe) and prefilters after its sixth, so a capture costs a sixth of what it
  did per frame; Every Frame probes keep capturing whole.
- **Rough screen-space reflections** [Stachowiak 2015]: the reflected ray drawn
  from GGX's distribution of visible normals [Heitz 2018] per pixel and frame,
  the hit's colour accumulated over frames (reprojected, clipped to its
  neighbourhood as TAA's is [Karis 2014]), so rough surfaces reflect blurred
  rather than fade to the probes. Max Roughness rises to 0.8.

## Phase 5: screen-space diffuse GI

A cheap near-field bounce [Ritschel 2009; Mara 2016]: at half size, a few
cosine-weighted rays a pixel marched through the depth buffer (SSR's march),
each hit's colour read from the last frame's lit scene -- so light bounces
again each frame -- accumulated over frames and filtered up depth-aware. The
IBL pass blends its irradiance over the probes' and DDGI's by how many rays
hit. A stack node, **Screen-Space GI** (Intensity, Rays, Radius). Baked pixels
keep their lightmap.

## Phase 6: probe GI (DDGI)

Irradiance probes with visibility [Majercik 2019; McGuire 2017; Majercik 2021],
updated by rasterising rather than ray tracing:

- **A GI Volume** component (with a Bounds box and Probe Spacing): a grid of
  probes, at most 32 x 8 x 32.
- **Updates:** a few probes a frame render six small views (the scene capture
  path, lit by the live lights, the sky and the volume's own last irradiance,
  so light bounces again each update), turned into an 8 x 8 octahedral
  irradiance and a 16 x 16 mean and mean-square distance per probe, blended
  over the old by hysteresis.
- **Lookup:** the IBL pass's diffuse from the eight probes round a pixel,
  weighted trilinearly, by facing, and by Chebyshev visibility from the
  distance moments [Donnelly 2006], biased off the surface.
- **With baking:** baked pixels keep their lightmap. Inside a volume, dynamic
  draws take DDGI instead of the probes' ambient cube, so moving objects and
  dynamic lights bounce; outside, nothing changes.

## Checking

Every phase: NodeGraph.Tests and RenderCore.Smoke (Null; GPU on Vulkan and
D3D12, the Sandbox materials recooked from git history as SHADING_MODELS.md
says), and editor screenshots of **LightingLab** (WorldsLab; written by
`Tools/LightingLab/make_lightinglab.py`) on both backends: a Cornell box open
to a low sun, gold spheres from roughness 0.1 to 1, and a field of 144
coloured point lights. Each phase records its GPU cost here, measured with
`stat frame`.

## Cost

GPU milliseconds per stage, from the editor viewport (about 1234 x 857) with
`stat frame`, on an NVIDIA GeForce RTX 2080 Ti; each the half-second average
`stats` logs. A report at the end compares them.

**Clocks.** The editor's viewport is a light load, so the GPU sits in low power
states and its clock moves between runs (435 MHz idle of 2100; clocks cannot
be locked without administrator rights). The same build measured three times
on ShadingModelLab gave a Lighting stage of 0.36, 1.03 and 1.84 ms. So runs
are compared only within one session from here on: the editor's new `config`
command switches a phase's feature off and on, alternately, and the stage
times on each side are averaged. The baseline rows below are one such run each
and only show the order of things.

Phase 1 adds a few ALU operations per colour conversion and has no switch; its
cost is below this noise.

A/B rows: one session, the switch alternated four times each way, means of
`stats` taken 4.5 s after each switch (the editor's console timers tick about
once a second, so commands are kept 1.5 s apart).

| Phase | Level, view | Backend | Switch | Lighting off | Lighting on | Total off | Total on |
|---|---|---|---|---|---|---|---|
| 2 | LightingLab, the field (144 lights) | Vulkan | ClusteredLights | 1.642 | 1.135 | 2.226 | 1.794 |
| 2 | LightingLab, the field (144 lights) | D3D12 | ClusteredLights | 1.712 | 1.264 | 2.219 | 1.845 |
| 3 | LightingLab, the room (2 rects, a 40-unit bulb) | Vulkan | AreaLights | 1.155 | 1.410 | 1.918 | 2.133 |
| 3 | LightingLab, the room (2 rects, a 40-unit bulb) | D3D12 | AreaLights | 1.057 | 1.671 | 1.614 | 2.280 |
| 3 | LightingLab, the gold spheres | Vulkan | EnergyCompensation | 1.700 | 1.847 | 2.473 | 2.718 |
| 3 | LightingLab, the gold spheres | D3D12 | EnergyCompensation | 1.562 | 1.806 | 2.197 | 2.441 |

Energy compensation is a handful of ALU operations per lobe; its rows move
Base Pass, which it does not touch, by about as much, so read them as within
the noise. Area lights cost two LTC integrals (eight edges each, after
clipping) per area light per pixel it reaches: 0.25-0.6 ms here, for a view
filled by three.

| Phase | Level | Backend | BasePass | AO | Shadows | Lighting | Forward | Post | Total |
|---|---|---|---|---|---|---|---|---|---|
| 0 (baseline) | ShadingModelLab | Vulkan | 0.140 | -- | 0.048 | 0.363 | 0.010 | 0.026 | 0.665 |
| 0 (baseline) | ShadingModelLab | D3D12 | 0.081 | -- | 0.026 | 0.281 | 0.004 | 0.018 | 0.470 |
| 0 (baseline) | Courtyard | Vulkan | 0.508 | 0.260 | 0.203 | 0.758 | 0.527 | 0.371 | 2.756 |
| 0 (baseline) | Courtyard | D3D12 | 0.513 | 0.208 | 0.296 | 0.789 | 0.673 | 0.357 | 2.941 |

## References

- IEC 61966-2-1:1999. *Multimedia systems and equipment -- Colour measurement and management -- Default RGB colour space -- sRGB.*
- Olsson, Billeter, Assarsson 2012. "Clustered Deferred and Forward Shading." HPG.
- Persson, Olsson 2013. "Practical Clustered Deferred and Forward Shading." SIGGRAPH course *Advances in Real-Time Rendering*.
- Drobot 2017. "Improved Culling for Tiled and Clustered Rendering." SIGGRAPH *Advances in Real-Time Rendering*.
- Heitz, Dupuy, Hill, Neubelt 2016. "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines." *ACM TOG* 35(4) (SIGGRAPH).
- Hill, Heitz 2016. "Real-Time Area Lighting: a Journey from Research to Production." SIGGRAPH course *Advances in Real-Time Rendering*.
- Kulla, Conty 2017. "Revisiting Physically Based Shading at Imageworks." SIGGRAPH PBS course.
- Fdez-Aguera 2019. "A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting." *JCGT* 8(1).
- Turquin 2019. "Practical Multiple Scattering Compensation for Microfacet Models." Ubisoft technical report.
- Karis 2013. "Real Shading in Unreal Engine 4." SIGGRAPH PBS course.
- Karis 2014. "High Quality Temporal Supersampling." SIGGRAPH *Advances in Real-Time Rendering*.
- Stachowiak 2015. "Stochastic Screen-Space Reflections." SIGGRAPH *Advances in Real-Time Rendering*.
- Heitz 2018. "Sampling the GGX Distribution of Visible Normals." *JCGT* 7(4).
- Lagarde, Zanuttini 2012. "Local Image-based Lighting With Parallax-corrected Cubemap." SIGGRAPH talk.
- Ritschel, Grosch, Seidel 2009. "Approximating Dynamic Global Illumination in Image Space." I3D.
- Mara, McGuire, Nowrouzezahrai, Luebke 2016. "Deep G-Buffers for Stable Global Illumination Approximation." HPG.
- McGuire, Mara, Nowrouzezahrai, Luebke 2017. "Real-Time Global Illumination using Precomputed Light Field Probes." I3D.
- Majercik, Guertin, Nowrouzezahrai, McGuire 2019. "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields." *JCGT* 8(2).
- Majercik, Marrs, Spjut, McGuire 2021. "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production." *JCGT* 10(2).
- Donnelly, Lauritzen 2006. "Variance Shadow Maps." I3D.
