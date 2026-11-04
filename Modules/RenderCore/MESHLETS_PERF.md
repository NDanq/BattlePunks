# Meshlet culling: performance report

What GPU meshlet culling (RENDERPIPELINE.md, "Meshlets": `[Rendering] Meshlets`,
`MeshletOcclusion`) costs, measured on DemoLab's L2 workshop statue, a
1,346,780-triangle marble scan (`feat-meshlets`, after M4). The answer, in
short: on this GPU and this scene **it costs more than it saves**, on both
backends. DemoLab keeps it on as its showcase (frames still hold 16 ms); the
engine default stays off. How to repeat it:
`.claude/skills/perf-testing`.

## Setup

- NVIDIA GeForce RTX 2080 Ti, the editor (NimbleEditor.exe), **RelWithDebInfo**,
  1280 x 720 at full resolution (dynamic resolution off), Vulkan and D3D12.
- `Content/Levels/L2_Laboratory.nmworld`, two fixed cameras on the statue:
  *front* (`camera -1650 260 -40 0 6`, the whole statue in view, square on) and
  *side* (`camera -1250 260 300 -75 4`, from the east door, its far half off
  screen).
- Three configurations: *off* (`Meshlets` off: every section drawn whole),
  *frustum* (`Meshlets` on, `MeshletOcclusion` off) and *occlusion* (both on,
  DemoLab's own settings).
- `stat frame` GPU spans, half-second averages, three samples a view (`stats`),
  render clock fixed (`[Rendering] FixedStep`), no readback (`MeshletStats` off).
  The cull's dispatch is inside the Base Pass span.
- Medians of two launches a configuration (front 6 samples, side 4), the
  configurations interleaved, backend by backend, each round.
- Noise: up to about 0.1 ms in Base Pass, 0.3-0.5 ms in Total and Captures.
- Tools: `Games/DemoLab/Tools/Perf/meshlet_perf.ps1` (launches, CSV); raw CSV
  `Games/DemoLab/Saved/Perf/meshlets.csv` (not tracked).

## What it costs

GPU ms, median; cost against *off* in brackets. **Bold** where clearly above
noise.

| Backend | View | Span | off | frustum | occlusion |
|---|---|---|---|---|---|
| Vulkan | front | Base Pass | 1.57 | 5.18 **(+3.61)** | 3.63 **(+2.06)** |
| | | Total | 5.51 | 9.20 **(+3.69)** | 7.67 **(+2.17)** |
| | side | Base Pass | 1.58 | 4.77 **(+3.19)** | 4.13 **(+2.55)** |
| | | Total | 5.90 | 9.01 **(+3.11)** | 8.35 **(+2.45)** |
| D3D12 | front | Base Pass | 1.42 | 3.54 **(+2.12)** | 2.51 **(+1.09)** |
| | | Total | 5.92 | 7.65 **(+1.74)** | 6.95 **(+1.03)** |
| | | Captures | 4.32 | 5.74 **(+1.42)** | 5.73 **(+1.41)** |
| | side | Base Pass | 1.44 | 3.26 **(+1.82)** | 2.91 **(+1.47)** |
| | | Total | 6.49 | 8.24 **(+1.75)** | 8.06 **(+1.56)** |
| | | Captures | 4.29 | 5.57 **(+1.28)** | 5.65 **(+1.36)** |

Every other span (AO, Shadows, Lighting, Forward, Post Process, Particles)
moves by noise only. Render thread, Base Pass: Vulkan 1.90 / 2.43 / 2.56 ms,
D3D12 1.79 / 2.85 / 3.09 ms (off / frustum / occlusion); every frame holds at
16 ms.

**Why it loses here:** the Base Pass draws the whole room, statue included,
in 1.4-1.6 ms with meshlets off. 1.35M triangles is little for this GPU, so
there is little to save, while the cull runs for *every* static mesh of 248
triangles or more in view, built meshlets on load when it has none
(`MeshletGpuMesh::Create`): one dispatch per mesh, each between two full UAV
barriers, then indirect draws from the compacted indices. Occlusion pays for
itself partly (it culls the statue's back half and what is behind it) but not
enough.

**D3D12's Captures span is not GPU work alone.** A parallel pass submits the
frame's list in two halves, so a span across one counts the GPU's wait for
the render thread too. With parallel recording off (`[Rendering]
ParallelRecording Off`) D3D12's Captures read 0.66-0.86 ms in every
configuration, as Vulkan's do. Dynamic resolution subtracts Captures from its
budget (`RenderScene.cpp`, `UpdateDynamicResolution`), so on D3D12 it reacts
to render-thread time as if it were GPU time.

## Fixed during this measurement

D3D12 drew a meshlet view's whole Base Pass serially (a workaround for
buffer states that implicit promotion already covers). That cost the render
thread about 10 ms (Base Pass 11.7 ms against 1.98 ms off), held frames at
21-22 ms, and swelled D3D12's Captures span to 12.7-18.2 ms through the
effect above, so dynamic resolution fell to 50 % in front of the statue
while Vulkan held 97 %. The Base Pass now records in parallel chunks on D3D12
too: no D3D12 debug-layer error over 90 s of L2 (Debug client, meshlets,
occlusion, `ParallelRecording On`), and the figures above are after the fix.

## What to do about it

1. **The engine default stays off** until 2-3 below land. DemoLab keeps
   `[Rendering] Meshlets` and `MeshletOcclusion` on (its `Config`), to show the
   feature on the statue; its 1.7-3.7 ms fits L2's 16 ms frame on this GPU.
2. **Cull only meshes imported with meshlets** (the `.nmmesh`'s `MLET` chunk),
   not every mesh large enough to build them on load: the room's kit pieces
   gain nothing from it and pay a dispatch each.
3. **One dispatch for every mesh**, the records naming their mesh's data,
   instead of one per mesh between full barriers (or no barrier between the
   cull's own dispatches: they write disjoint regions and slots).
4. **Time Captures without the parallel pass's wait on D3D12** (or leave
   Captures out of dynamic resolution's fixed cost there): a separate,
   pre-existing issue, which meshlets made visible.

## What it does not cover

- One GPU, one scene. A GPU with less geometry throughput, or a scene of many
  dense meshes mostly off screen or hidden, is where culling should win; this
  is not measured.
- The CPU side beyond the Base Pass's recording time, and frame time beyond
  the 16 ms cap.
- `MeshletCones` (off by default) and `MeshletFreezeCulling`.
- Shadow views and captures, which do not cull meshlets.
