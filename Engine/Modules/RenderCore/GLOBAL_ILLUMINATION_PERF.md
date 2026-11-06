# Lighting and GI: performance report

What the six phases of GLOBAL_ILLUMINATION.md cost on the GPU, measured
after they all landed (dev-rendering, 219202a6 to 48013150). The raw rows and
how each was taken are in that doc's **Cost** section; this is the summary
and what to do about it.

## Setup

- NVIDIA GeForce RTX 2080 Ti, the editor's viewport (about 1234 x 857), debug
  build, Vulkan and D3D12 (shader model 6.0).
- `stat frame` GPU timers per stage; `stats` logs their half-second averages.
- **Every figure is an in-session A/B.** The GPU idles in low power states
  under the editor and its clock cannot be locked without administrator
  rights: the same build gave Lighting 0.36, 1.03 and 1.84 ms in three runs.
  So each feature was switched off and on in one editor session (the new
  `config` console command), four to six times each way, and the stage times
  averaged on each side. Differences under about 0.15 ms are noise.
- Level: WorldsLab's **LightingLab**, made for this (`Tools/LightingLab`): a
  Cornell-box room with two rect lights and a 40-unit bulb, ten gold spheres
  rough 0.1 to 1, a field of 144 point lights, nine reflection probes in the
  room and one over the field capturing every frame, a GI Volume of 324
  probes, and a post process stack with SSR (Max Roughness 0.8) and SSGI.

## What each feature costs

| Phase | Feature (switch) | Default | Where | Vulkan | D3D12 |
|---|---|---|---|---|---|
| 1 | True sRGB curve (none) | always | everywhere | below noise | below noise |
| 2 | Clustered light list (`ClusteredLights`), 144 lights | on | Lighting | **-0.51 ms** | **-0.45 ms** |
| 3 | Area lights by LTC (`AreaLights`), a view filled by 3 | on | Lighting | +0.26 ms | +0.61 ms |
| 3 | Energy compensation (`EnergyCompensation`) | on | Lighting | noise | noise |
| 4 | Stochastic SSR + resolve (`StochasticReflections`) | on | Lighting | +0.19 ms | +0.24 ms |
| 4 | Probe captures over frames (`ProbeFacesPerFrame` 6 to 1), an Every Frame probe | 1 | Captures | **1.50 to 0.27 ms** | **1.91 to 0.33 ms** |
| 5 | Screen-space GI, 4 rays, half size (`ScreenSpaceGI`, stack node) | in stacks with the node | Lighting | +0.69 ms | +0.68 ms |
| 6 | GI Volume probe updates, 1 a frame (`GIProbesPerFrame`) | 1 | Captures | +1.44 ms | +2.38 ms |
| 6 | GI Volume lookup (`DynamicGI`, probes held) | on | Lighting | +0.14 ms | not measured (see below) |

Sixteen reflection probes in place of four (Phase 4) were not measured on
their own: a pixel walks the probes nearest first and stops once their
weights cover it, so the walk only lengthens where many overlap.

## A frame with everything on

LightingLab's room, every feature on, over the same frame with all of them
off, adding the rows above:

| | Vulkan | D3D12 |
|---|---|---|
| Area lights, stochastic SSR, SSGI, DDGI lookup (Lighting) | +1.3 ms | +1.7 ms |
| One GI Volume probe and one reflection-probe face a frame (Captures) | +1.7 ms | +2.7 ms |
| **Total** | **about +3.0 ms** | **about +4.4 ms** |

At 60 Hz that is 18 to 26 % of the frame on this GPU at this size; most of
it, the captures, is a dial (`GIProbesPerFrame`, `ProbeFacesPerFrame`) rather
than a fixed cost. Where the clustered list is in play (many local lights)
it pays back about half a millisecond of that.

What changes the picture is mostly scale: SSGI and SSR scale with pixels
(about 2.2x at 1920 x 1080 against this viewport); the captures do not.

## Where the time goes, and what to do

1. **GI Volume updates are the largest cost**, and they are mostly fixed per
   view, not per pixel: each probe is six full passes through the
   scene-capture path at 16 x 16 -- shadow maps, light list, G-buffer,
   lighting, IBL -- so 16 x 16 costs nearly what a larger face would. The
   ways down, in order of payoff:
   - share the main view's shadow maps with the probe faces instead of
     drawing shadows per face (the bulk of a face's fixed cost);
   - render the six faces as one pass (a layered target or a 96 x 16
     viewport strip) so the per-pass setup is paid once a probe;
   - update probes by importance (near the camera, near changed lights)
     rather than round the grid, so fewer a frame suffice.
   Until then, `GIProbesPerFrame 1` is the setting; 0 holds a settled
   volume for free (the lookup's 0.14 ms only). Phase 7 (below, "Captures")
   did the first and more: kept capture shadow maps, simple views, a time
   budget, and volumes that stop once converged and relight on a change.
2. **D3D12 costs more than Vulkan for the same shaders**: area lights 2.3x,
   GI Volume updates 1.7x. The area-light gap points at DXC's code for the
   LTC edge integral (`acos` and the horizon clip) and is worth a look in
   PIX; the update gap is the capture path's per-pass overhead, which item 1
   shrinks on both.
3. **SSGI at 0.7 ms** is reasonable for four rays of twelve steps at half size.
   Two rays plus a small spatial filter (it has none yet, and four rays
   leave a faint grain on flat floors) would cost less and look better.
4. **Reflection probe slicing works as planned**: a capture's cost spread
   over six frames, an Every Frame probe now about 0.3 ms a frame rather
   than 1.5 to 1.9.
5. **The clustered list is a clear win** with many lights, and the only path
   past the old 16-light cap.

## Captures (Phase 7)

GLOBAL_ILLUMINATION.md, Phase 7: the capture budget, shared and kept capture
shadow maps, simple capture views, static captures and relighting.

**Setup.** Intel Arc B390 (Vulkan), RelWithDebInfo DemoLab editor, viewport
1967 x 1278 (the editor's window; the captures do not scale with it),
`stat frame` and the `stats` console command, `[Scalability] PowerProfile
PowerMax`, High tier. Demo Lab's dressed L1 (the WIP branch's content,
1125-probe GI Volume, three Once reflection probes) from the dock, and L2's
core room (264-probe volume, six more volumes, three probes). Every config
launched once per round, interleaved, two rounds, three samples a launch
(10, 13, 16 s); medians. The GPU is shared with other work on this machine:
the spread within a config is about +-1 ms on the legacy rows, +-0.2 ms on
the new ones.

**One GI probe a frame** (`CaptureBudgetMs` 0, so 6 faces of one probe a
frame; L1, Captures span):

| Config | Captures | Main view (Total) |
|---|---|---|
| Legacy (budget 0, every Phase 7 switch off) | **43.0 ms** | 22.3 ms |
| Legacy, `[Shadows] Enabled` off (no shadow maps anywhere) | 21.9 ms | 18.0 ms |
| + `SharedCaptureShadows` (kept from frame to frame) | 21.7 ms | 22.3 ms |
| + `SimpleCaptures` alone (per-view shadows still) | 14.0 ms | 22.3 ms |
| + both | 8.6 ms | 22.4 ms |
| **Defaults** (`CaptureBudgetMs` 2, `StaticCaptures`, `GIRelight`) | **1.45 ms** | 23.0 ms |

So a legacy probe was half shadow maps (the sun's cascades and the local
lights' cubes drawn again for each of six faces, about 3.5 ms a face) and half
draws (every proxy to 20000 units). The kept capture shadow maps remove the
first half entirely; the simple views (2-pixel cull, static only, 8 spacings,
no translucency or custom passes) cut the second by about 60 %; the budget
then holds the frame to about 1.5 ms while the volume fills (about 1.3 faces
a frame here, a face 1 ms GPU and 0.4 ms render thread). The main view's
Total does not change (Total 22-23 ms either way; the GPU's clock moves with
the load).

**Convergence.** L1's 1125-probe volume fills in about two minutes (six probes
a second; probes that see only sky and terrain are cheap, those by the cliffs
and the gatehouse about 2 ms a face) and stops; Captures is then 0.005 ms. L2's 264-probe room: the first fill takes about 50 s
(its faces are dense: 150-600 draws, 1.5-3 ms of render thread, so one a
frame), Captures 0.6-2 ms meanwhile, then 0.005 ms once it and the
background fill of the rooms round it are done. Legacy: about 28 ms every
frame, forever (one probe a frame, round and round).

**A lighting change** (forced with `config Rendering CaptureSunStepDegrees`,
which changes the lighting's signature and nothing else): L2's room relit
and converged (two rounds, 528 relit probes) in **0.13 s**, within the
budget; a relit probe costs 0.010-0.018 ms GPU. L1 (1125 probes, filled after
133 s): 0.26 s. Before, the same change
reached every probe after one round at one probe a frame: 264 frames at about
28 ms of captures each.

**Look.** L2's room, converged, viewport screenshots (1967 x 1278): relit
against drawn every round (`GIRelight` off) 51.6 dB PSNR, 0.02 % of pixels off
by more than 8 of 255; the new defaults against legacy 47.4 dB, 0.3 %. The
volume itself matters: against `DynamicGI` off, 30.4 dB, 18.5 %.

**Not covered.** D3D12 (not built here); 1440p (the captures do not scale
with the view); L3 (its dressed branch replaced DDGI with SSGI); WorldsLab's
LightingLab (another build tree); a game build's frame pacing.

## What these numbers do not cover

- **CPU time.** Not measured. The clustered list is built on the CPU per view
  (`BuildLightList`), and every GI Volume probe queues the scene's draws six
  times; in a debug build both are visible on the render thread. A release
  build profile is the next step.
- **Locked clocks.** With administrator rights, `nvidia-smi -lgc` would pin the
  clock and allow absolute numbers between sessions; until then only the
  in-session differences above mean anything.
- **D3D12's DDGI lookup.** That run held the probes before the volume had
  finished its first round (1.5 % of pixels changed, against 49 % with the
  volume lit), so it measured nothing; the lookup is the same shader on both
  backends and Vulkan's 0.14 ms is the estimate.
- **Other GPUs and resolutions.** One GPU, one viewport size.
