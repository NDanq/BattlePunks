# Lighting and global illumination: plan

Status: **Phase 0 landed.** Phases land in order on `dev-rendering`, one
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

## Phase 1: true sRGB

`ToLinear` / `ToDisplay` and every `pow(x, 2.2)` / `pow(x, 1 / 2.2)` that turns
display colours into light become sRGB's piecewise curve [IEC 61966-2-1].
**Changes pixels on purpose:** a display 0.5 is linear 0.214 instead of 0.218,
darks differ most (display 0.04: 0.0031 instead of 0.0008). The bake measures
materials through the same shaders, so it follows.

## Phase 2: many lights

Clustered shading [Olsson 2012; Persson 2013] with Drobot's z-binning
[Drobot 2017], built on the CPU each frame:

- **The list:** every active point and spot light (and Phase 3's area lights),
  up to `RENDER_LIGHT_LIST_MAX` (1024), sorted by view depth, packed into a
  light texture (one row per light). Shadow casters keep their shadow views (at
  most four). The first 16 also stay in `Scene_Locals`, for volumetric fog.
- **Tiles:** the screen in 32 x 18 tiles, each a bit mask of the lights whose
  bounds reach it (32 words: 1024 bits), in a tile texture.
- **Z-bins:** 64 slices of view depth, each the lowest and highest index of the
  depth-sorted lights reaching it. A pixel walks its tile's mask only between
  its bin's two indices.
- **Shading:** `ShadeSurfaceSplit`'s loop walks that list instead of
  `Scene_Locals`, in the Lighting stage and forward Lit materials (engine slots
  8-9). With 16 lights or fewer a frame lights exactly as before.

## Phase 3: area lights and energy

- **Area lights** by linearly transformed cosines [Heitz 2016; Hill 2016]:
  the GGX lobe's LTC fit (`fit_ltc.py`, Heitz's isotropic parameterisation,
  64 x 64 over roughness and view angle) and its magnitude and Fresnel terms,
  integrated over the light's polygon clipped to the horizon.
  - **Rect Light:** a new component, Width x Height, emitting from one face,
    with Barn Door later.
  - **Sphere:** a Point or Spot Light's Source Radius (the bake's already) makes
    it a sphere, integrated as the disc facing the shading point, approximated
    by a square of the same area.
  - Diffuse: the polygon's form factor (the clamped-cosine LTC, identity).
  - Shadows: a Rect Light's shadow is its centre's spot shadow.
- **Energy compensation** for rough metals [Kulla 2017; Fdez-Aguera 2019;
  Turquin 2019]: the specular lobes times `1 + F0 (1 / E - 1)`, E the
  split-sum's directional albedo (Karis's fit at F0 = 1). **Changes pixels on
  purpose:** rough metals brighten, up to about 50 % at roughness 1; smooth
  surfaces and dielectrics hardly move.

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
says), and editor screenshots of **LightingLab** (WorldsLab) on both
backends. Each phase records its GPU cost here, measured with `stat frame`.

## Cost

GPU milliseconds per stage, from the editor viewport (about 1234 x 857) with
`stat frame`, on an NVIDIA GeForce RTX 2080 Ti; each the half-second average
`stats` logs. A report at the end compares them.

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
