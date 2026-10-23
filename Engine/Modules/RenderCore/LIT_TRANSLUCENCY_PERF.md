# Lit translucency and lit particles: performance report

What LIT_TRANSLUCENCY.md's work costs on the GPU, measured after it all
landed (dev-rendering, c0a53a17 onward: Phases 0-8, the shadows fix, glass
casting, fog through plumes, self-occlusion, and this report's forward
reflection fixes). Each phase's own before/after rows are in that doc; this
is one measurement of the finished work, feature by feature, and what to do
about it. How to repeat it: `.claude/skills/perf-testing`.

## Setup

- NVIDIA GeForce RTX 2080 Ti, the standalone game (WorldsLab.exe),
  **RelWithDebInfo**, 1280 x 720, Vulkan and D3D12 (shader model 6.0).
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
- **Every figure is a median of six samples** (two launches, three samples
  each), and **every cost is an A/B in one build, interleaved**: each round
  launches every configuration once, view by view, so the GPU clock's drift
  over the 35 minutes hits them all alike. (A first pass on Debug ran the
  configurations one after another; the drift turned some costs negative, and
  it is superseded by this one.)
- Noise: the samples of one configuration spread by about 0.01-0.03 ms in
  the Shadows span and up to 0.1-0.2 ms in Forward and Total on both
  backends. Differences under 0.05 ms are noise; a single odd median
  (D3D12's plume Shadows span, off higher than on) is too.
- Tools: `Tools/TranslucencyLab/perf.ps1` (interleaved launches into a CSV;
  RelWithDebInfo by default), `perf.py` (medians and differences as these
  tables). This report's raw CSV is `Games/WorldsLab/Saved/TLab/perf_rel.csv`
  (not tracked).

## What each feature costs

Cost = the span with everything on, less the same span with the feature
off. **Bold** where it is clearly above noise.

| Feature (switch) | Default | Span | View | Vulkan | D3D12 |
|---|---|---|---|---|---|
| Forward reflections, the half-size depth copy included (`[Rendering] ForwardReflections`) | on, with the stack's SSR node | Forward | lab / glass / plume / water | +0.03 / **+0.07** / +0.05 / **+0.05** | 0.00 / **+0.08** / noise / **+0.09** |
| Translucent shadows: particles and glass casting, every receiver reading them, fog's shafts, self-shadowing and self-occlusion (`[Shadows] TranslucentShadows`) | on | Shadows | lab / glass / plume / water | +0.01 / 0.00 / **+0.04** / +0.02 | +0.01 / +0.02 / noise / noise |
| | | Forward | lab / glass / plume | +0.04 / +0.04 / +0.03 | noise |
| Per-particle lighting instead of per pixel, the plume's smoke (`SmokePerParticle`; a material setting) | per pixel (Volume) | Forward | plume | **-0.29** (0.57 to 0.29) | **-0.35** (0.69 to 0.34) |
| | | Forward | lab | -0.04 | **-0.11** |
| A 3 x 3 filter for forward shadows (`[Shadows] ForwardFilter` 1) | 0 (one tap) | Forward | lab / plume | +0.04 / +0.04 | +0.02 / +0.05 |

Not switchable, so not in the table: forward shadows themselves (Phase 2),
DDGI on forward Lit (Phase 3), the Volume model (Phase 5) and one sorted
translucent stage (Phase 1). Their commit-time numbers (below) put Phase 2
at most of the forward growth and Phases 1, 3 and 5 within noise.

### Forward reflections: within budget after the fixes

The plan's budget was +0.2 ms. The first (Debug, sequential) pass measured
them at up to +0.19 ms on Vulkan and +0.34 ms on D3D12 in the water view,
and this report's first version proposed three fixes, now landed:

- the march reads a **half-size** copy of the opaque depth, each texel the
  nearest of the four under it (`PPDepthHalf.fsh`), so thin edges still stop
  a ray; the copy writes a quarter of the texels and the march reads them a
  quarter as often (the copy also resizes with the view now -- it did not);
- **at most 12 steps** (the SSR node's count, capped; it was 24);
- the trust fades out from **0.5 x Max Roughness** (it was 0.6).

Measured on RelWithDebInfo, interleaved, they now cost at most +0.07 ms on
Vulkan and +0.09 ms on D3D12 -- the water view, the worst. (The before
figures are Debug and sequential, so the drop is not a like-for-like
number; the budget is met either way.) The water's reflections look the
same, with slightly less banding; its region reads 1.5 darker where rays no
longer reach a hit.

## The plume, frame by frame

The heaviest view, everything on, per pixel and per particle, against both
switchable features off:

| | Vulkan Forward | Vulkan Total | D3D12 Forward | D3D12 Total |
|---|---|---|---|---|
| Everything on, per pixel | 0.57 | 1.45 | 0.69 | 1.63 |
| Everything on, **per particle** | **0.29** | **1.08** | **0.34** | **1.39** |
| Translucent shadows and forward reflections off, per pixel | 0.53 | 1.38 | 0.60 | (noise) |

Per particle, the plume -- shadows cast and received, self-shadowed and
self-occluded -- costs half the per-pixel Forward span, the biggest single
number in this report.

## Through the phases

The plume view's Forward span (ms) as each phase's commit measured it: one
sample per backend, on **Debug**, with a capture's readback in the frame --
read the trend, not the digits, and do not set it against the tables above.
The lab's content changed along the way (Phase 3 made the glass Surface,
Phase 5 the smoke Volume, glass casting the panes cast).

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

## What to do about it

1. **Light plumes per particle unless they are a hero effect.** It is the
   biggest number here and the cheapest change: a material setting. Per
   pixel (the Volume model) is worth it for a large, close, slow plume where
   a sphere normal or a flipbook's normal map must read.
2. **Forward reflections are within budget** after the fixes above; nothing
   further is needed at this size. They scale with translucent pixels, so a
   screen of water at 1080p is the case to re-measure.
3. **Translucent shadows are cheap here** -- 0.01-0.04 ms in the Shadows
   span, about as much in Forward where a plume fills the view, nothing at
   all without casters. Self-occlusion (a second lookup per smoke pixel) is
   the part to drop first on a dense plume; it is 0 by default.
4. **`ForwardFilter` 1 costs only 0.02-0.05 ms on RelWithDebInfo** (the Debug
   pass had it at 0.1 ms on D3D12). One tap stays the default for its
   overdraw scaling, but a project with stair-stepped shadow edges on glass
   can afford 1.

## What this does not cover

- Other resolutions (the forward costs scale with translucent pixels), other
  GPUs, OpenGL (the game cannot start on it here) and Metal; Shipping builds.
- The CPU: the render thread's time issuing these draws and the opacity pass.
- Courtyard and the other shipped levels: only TranslucencyLab was timed;
  Courtyard's glass and water now pay forward shadows and reflections.
