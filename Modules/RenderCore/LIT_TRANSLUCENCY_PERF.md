# Lit translucency and lit particles: performance report

What LIT_TRANSLUCENCY.md's work costs on the GPU, measured after it all
landed (dev-rendering, c0a53a17 to a1da7d6a: Phases 0-8, the shadows fix,
glass casting, fog through plumes, self-occlusion). Each phase's own
before/after rows are in that doc; this is one measurement of the finished
work, feature by feature, and what to do about it.

## Setup

- NVIDIA GeForce RTX 2080 Ti, the standalone game (WorldsLab.exe, Debug),
  1280 x 720, Vulkan and D3D12 (shader model 6.0).
- **TranslucencyLab** (`Tools/TranslucencyLab`), its four fixed views: *lab*
  (the whole yard), *glass* (three panes at opacity 0.12 casting at 0.3, an
  overhead slab's shadow across them), *plume* (160 lit smoke particles --
  the Volume model, self-occlusion 0.5, casting into the sun's and a spot's
  opacity maps -- with sparks, in a shadowed spot light), *water* (a smooth
  strip under a red wall). A GI Volume, three reflection probes, SSR, thin
  volumetric fog, TAA throughout.
- `stat frame` GPU spans, each a half-second average, logged at frames 480,
  600 and 720 (`WorldsLab.TranslucencyLabStats`, `...StatsCount`) with the
  render clock fixed (`[Rendering] FixedStep`), so every run renders the
  same frames. No frame is read back: a capture's readback stalls the GPU
  and skews the spans.
- **Every figure is a median of six samples** -- two launches of the game,
  three samples each -- and **every cost is an A/B in one build**: the
  feature's switch off against everything on. The GPU's clock cannot be
  locked here, so spans move run to run: the Shadows span by about 0.01 ms,
  Forward by up to 0.06 ms on Vulkan and 0.1-0.3 ms on D3D12 (D3D12's water
  view is the worst), Lighting and Total more. Differences under 0.05 ms on
  Vulkan and 0.1 ms on D3D12 are noise.
- Tools: `Tools/TranslucencyLab/perf.ps1` (a configuration's runs into a
  CSV), `perf.py` (the medians and differences as these tables). The raw
  CSV of this report is `Games/WorldsLab/Saved/TLab/perf.csv` (not tracked).

## What each feature costs

Cost = the span with everything on, less the same span with the feature
off. **Bold** where it is clearly above noise.

| Feature (switch) | Default | Span | View | Vulkan | D3D12 |
|---|---|---|---|---|---|
| Translucent shadows: particles and glass casting, every receiver reading them, fog's shafts, self-shadowing and self-occlusion (`[Shadows] TranslucentShadows`) | on | Shadows | lab / glass / plume / water | **+0.02 / +0.03 / +0.05 / +0.03** | **+0.03 / +0.05 / +0.06 / +0.04** |
| | | Forward | glass / plume / water | +0.04 / **+0.15** / +0.05 | +0.10 / **+0.17** / **+0.13** |
| Forward reflections, the depth copy included (`[Rendering] ForwardReflections`) | on, with the stack's SSR node | Forward | lab / glass / plume / water | **+0.10 / +0.09 / +0.11 / +0.19** | +0.10 / **+0.19** / +0.10 / **+0.34** |
| Per-particle lighting instead of per pixel, the plume's smoke (`SmokePerParticle`; a material setting) | per pixel (Volume) | Forward | plume | **-0.39** (0.72 to 0.34) | **-0.53** (1.34 to 0.82) |
| | | Forward | lab | **-0.08** | **-0.13** |
| A 3 x 3 filter for forward shadows (`[Shadows] ForwardFilter` 1) | 0 (one tap) | Forward | lab / glass / plume | +0.05 / noise / +0.05 | **+0.10 / +0.10 / +0.13** |

Not switchable, so not in the table: forward shadows themselves (Phase 2),
DDGI on forward Lit (Phase 3), the Volume model (Phase 5) and one sorted
translucent stage (Phase 1). Their commit-time numbers (below) put Phase 2
at most of the forward growth and Phases 1, 3 and 5 within noise.

## The plume, frame by frame

The heaviest view, everything on, per pixel and per particle, against both
switchable features off:

| | Vulkan Forward | Vulkan Total | D3D12 Forward | D3D12 Total |
|---|---|---|---|---|
| Everything on, per pixel | 0.72 | 1.74 | 1.34 | 3.33 |
| Everything on, **per particle** | **0.34** | **1.22** | **0.82** | **3.01** |
| Translucent shadows and forward reflections off, per pixel | 0.61 | 1.57 | 1.10 | 2.95 |

Per particle, the plume with shadows cast and received, self-shadowed and
self-occluded, costs less in Forward than it did at Phase 0 -- unshadowed,
Legacy-lit, with no reflections -- on both backends (0.37 and 0.77 ms then,
one sample each).

## Through the phases

The plume view's Forward span (ms) as each phase's commit measured it: one
sample per backend, with a capture's readback in the frame, so read the
trend, not the digits. The lab's content changed along the way (Phase 3 made
the glass Surface, Phase 5 the smoke Volume, glass casting the panes cast).

| Phase | Vulkan | D3D12 | What it added to this view |
|---|---|---|---|
| 0 baseline | 0.37 | 0.98 | |
| 1 one translucent stage | 0.39 | 0.77 | nothing visible (noise) |
| 2 forward shadows (one tap) | 0.52 | 1.06 | every smoke pixel's shadow lookups |
| 3 DDGI, Surface specular | 0.48 | 1.13 | (noise) |
| 4 forward reflections | 0.53 | 1.19 | the smoke skips them (roughness 1) |
| 5 Volume model | 0.55 | 1.02 | (noise: specular lobes traded for the phase) |
| 6 per particle | 0.27 | 0.64 | per-pixel lighting gone (per pixel: 0.57, 1.01) |
| 7 translucent shadows | 0.58 | 1.09 | + Shadows 0.05 |
| 8 self-shadowing | 0.56 | 1.02 | |
| now (this report's medians) | 0.72 | 1.34 | + self-occlusion's second lookup; no readback |

## What to do about it

1. **Light plumes per particle unless they are a hero effect.** It is the
   biggest number here and the cheapest change: a material setting. Per
   pixel (the Volume model) is worth it for a large, close, slow plume where
   a sphere normal or a flipbook's normal map must read.
2. **Forward reflections are the one feature over budget** (+0.2 ms planned;
   D3D12's water view +0.34, Vulkan's +0.19). Cheap fixes, in order: march
   the copied depth at half size (the copy and the march are both
   full-size); cap the forward march at 12 steps (it takes the node's, up to
   24); fade it out from Max Roughness x 0.5 rather than 0.6. Each is a few
   lines in `ForwardReflections.glsl` and the forward stage.
3. **Translucent shadows cost most where a plume fills the screen**: every
   smoke pixel reads the opacity atlas for the sun and the spot, and once
   more toward the sun for self-occlusion. Self-occlusion is the one to
   drop first on a busy plume (it is 0 by default for that reason); per
   particle pays these per vertex instead.
4. **Keep `ForwardFilter` at 0.** A 3 x 3 filter on forward shadows costs
   0.1 ms on D3D12 for edges a one-tap shadow only shows on glass.
5. **Shadows-stage cost is small and bounded**: 0.02-0.06 ms for the opacity
   pass, nothing at all without casters (the pass returns before binding).

## What this does not cover

- Release builds (Debug here; the GI report's ratios suggest about half),
  other resolutions (the forward costs scale with translucent pixels: a
  plume at 1920 x 1080 about 2.2x), other GPUs, OpenGL (the game cannot
  start on it here) and Metal.
- The CPU: the render thread's time issuing these draws and the new
  opacity pass was not measured.
- Courtyard and the other shipped levels: only TranslucencyLab was timed;
  Courtyard's glass and water now pay forward shadows and reflections.
