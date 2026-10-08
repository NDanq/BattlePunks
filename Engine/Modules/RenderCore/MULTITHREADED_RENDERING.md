# Multithreaded rendering: plan

Status: **phases 0 (measure and gate), 1 (per-draw cost) and 2 (GDI recording contexts) done; phase 3 next.** Recording draws on several
threads when the backend can: the Base Pass first, then the Shadows views.
This note is why, what stands in the way, and in what order.

## The request

- Check at launch whether the platform supports parallel draw recording, and
  gate the feature on it.
- If it does, record the RenderProxy draws of the big stages on several
  threads.

## What it would buy

Parallel recording shortens one thing: the **render thread's CPU time spent
recording draws**. It does not touch:

- GPU time (the same work reaches the GPU);
- the game thread, culling, or the editor's 16 ms frame timer.

It pays when `stat unit`'s **Wait** is large -- the game thread waiting on
the render thread -- *and* the render thread's time is draw recording,
typically thousands of draws a frame. Below roughly a thousand, the fork/join
and the per-thread command buffers cost about what they save.

**Measured (October 2026):** a dense street level -- about 3,000 draws a
frame, from ~65 buildings of ~9 material sections each, drawn in the
occluder pre-pass, the Base Pass and four shadow cascades -- on an Arc B390
(Vulkan, Mesa):

| Build | Render thread | Wait | GPU (Total) |
|---|---|---|---|
| RelWithDebInfo | 12.5-21 ms | 11.5-19 ms | 10-12 ms |
| Debug | ~45 ms | ~45 ms | 7-9 ms |

A fence-wait probe in `VKCommandManager::BeginFrame` read ~0.01 ms a frame,
with no mid-frame flushes: the render thread is recording, not waiting on the
GPU. Note the GPU is not far behind: once recording is fast, the GPU and the
single frame in flight (below) are the next limit.

**Baselines (phase 0)**, from `stat frame`'s render-thread columns
(`RenderStats::CpuMs`, `StageDraws`, `QueueCpuMs`), Arc B390, Vulkan:

| Level, build | Render | Shadows CPU (draws) | Base Pass CPU (draws) | PrePass CPU (draws) | Stages CPU | Queue | GPU Total |
|---|---|---|---|---|---|---|---|
| Street, RelWithDebInfo | 12.1-22.9 ms | 7.6-15.7 ms (1,857) | 2.7-4.5 ms (332) | 0.8-1.0 ms (111) | 11.4-21.9 ms | 0.2-0.3 ms | 8.1-9.1 ms |
| Street, Debug | 35.9-37.0 ms | 24.4-24.9 ms (1,857) | 7.6-8.7 ms (332) | 1.6-1.7 ms (111) | 34.6-36.2 ms | 0.5 ms | 8.0-8.5 ms |
| LightLab GpuTest, RelWithDebInfo client, occlusion on | 15.1 ms | 6.9 ms (1,525) | 0.7 ms (109) | 2.0 ms (402) | 9.7 ms | 0.6 ms | 4.6 ms |
| LightLab GpuTest, occlusion off | 16.0 ms | 7.1 ms (1,530) | 2.7 ms (398) | -- | 9.9 ms | 1.0 ms | 4.8 ms |

(The GpuTest rows are means over its benchmark path, `gpu_test_perf.py`'s
`occlusion` suite; the street's are two half-second windows from the
`Player`'s start.) What it says:

- **Shadows is the biggest single stage**: the casters drawn once per cascade
  and local tile, 4-8 us of render thread a draw. It is a first target for
  parallel recording, with the Base Pass -- not a second one.
- **Per draw, the Base Pass is dearer** (~8 us in RelWithDebInfo): material
  binds, more descriptors. Phase 1's per-draw work pays there most.
- **Queueing is cheap** (0.2-1.0 ms): culling and building the draw lists is
  not worth threading.
- The render thread's time outside the stages (GpuTest: ~5 ms of 15) is
  probes, captures, UI and the frame's bookkeeping: not draw recording.

## The gate

Decided once, at device creation, then switched per frame:

1. **Backend capability**, `GDISupportsParallelRecording()`:

   | Backend | Supported | Needs |
   |---|---|---|
   | Vulkan | when dynamic rendering is on (always, at API 1.3) | secondary command buffers (core); `VkCommandBufferInheritanceRenderingInfo` |
   | D3D12 | always | direct command lists (bundles cannot set targets, barriers or queries) |
   | Metal | always | `MTLParallelRenderCommandEncoder` |
   | OpenGL | never | -- |
   | Null | yes, run serially | lets headless tests exercise the chunked path |

   On Vulkan, `inheritedQueries` and `inheritedConditionalRendering` are
   enabled where the device has them. Neither is required: every occlusion
   query and every draw predicate brackets one draw, so it is recorded inside
   one secondary. They are reported, for later use.
2. **Config**, `[Rendering] ParallelRecording = Auto | On | Off`. Off is the
   single-threaded path, kept as the fallback and the pixel reference.
3. **Worth it**: Auto goes parallel only for a stage over
   `[Rendering] ParallelMinDraws` draws, with spare cores (the job system's
   workers).

The verdict, and why, is logged once at startup.

## Why it is expensive here

Every draw goes through one `RenderContext::Impl` and the global `GDynamicGDI`
(one `CurrentStateData`: one command buffer, one set of bound state), and both
are written on every draw.

**RenderCore** (`RenderContext.cpp`):

- `Impl` state: `BoundVertex`/`BoundPixel`, `MeshObject`/`ViewObject` and
  their `GDITransform` (`Place`), `PlaceProjection`/`PlaceView` (per shadow
  view), `OverrideBlock`, the `Scene` block.
- `BindMaterial` writes the tint and baked-lighting bytes **into the cached
  `Material::Block`**, shared by every draw of that material.
- `GDIMesh::SetDrawRange` on a mesh shared by every draw of it.
- Lazy caches that load or create on a miss, mid-issue: `FindMaterial`,
  `FindMesh`, `FindTexture`, render targets, default and post shaders.
- A captured static list is a vector of pre-resolved `DrawCommand`s copied
  back into the stage policies each frame -- immutable, safe to read
  concurrently, but not a unit of work: it is issued with everything else.

**GDI** (all backends' shared layer, `GDI.h`/`GDI.cpp`):

- `GDIDynamicDriver` holds the bound shaders, `MaterialBlocks[]` with serials,
  `MaterialTextures[]`, the instance buffer and indirect arguments.
- `DrawObject` → `UpdateCoreUniforms` writes ~14 ViewParameters/Transform
  values **by string into the shared `GDIShader` resources**
  (`VKShaderResource::Scalars`), then every user uniform the same way.
- `GetShader<>()` inserts into `LoadedShaderMap` on a miss.
- `GDIDrawEvent` bumps the plain global `GDebugGroupID`.
- `GDIInRenderThread()` checks fire on any other thread.

**Vulkan** (`VulkanDynamicGDI.cpp`, `VKMesh.cpp`):

- `VKMesh` keeps the `CurrentStateData` pointer it was made with and draws on
  its `CurrentCmd`; it reads the instance buffer from the global driver.
- `BindDrawState`, per draw: picks the descriptor layout group by
  `StartsWith` scans (fixed per shader pair); hashes ~25 fields into a
  pipeline key and looks it up (created on a miss, no lock, matched on the
  64-bit hash alone); `vkAllocateDescriptorSets` and `vkUpdateDescriptorSets`;
  re-uploads the view block (per view) and the transform block through string
  lookups, 512+ bytes of ring; binds the pipeline and set with no redundancy
  check.
- One `VKDescriptorAllocator` (pools of 64 sets, one bump-pointer UBO ring), no
  lock. A `VkDescriptorPool` must be externally synchronised anyway.
- One command pool, one primary buffer, one fence: **one frame in flight.**
- Mid-recording interruptions: the instanced-quad ring overflow calls
  `FlushForReadback` (submit, wait, reopen); compute dispatches and the
  occlusion round's reset and resolve close and reopen the rendering.

**D3D12** mirrors it (`D3D12CurrentStateData`, one list and allocator, the
same string packing and unlocked pipeline map and constant-buffer ring), but
textures own their SRV slots for life, so there is no per-draw descriptor
work.

**Threads**: EngineCore's `JobSystem` (`CoreJob.h`: `ParallelFor`, groups,
waits that work) exists, but **no host creates one** -- `GetJobSystem()` is
null in the launcher and the editor, so animation and texture compression run
serially today.

## What has to hold for a parallel chunk

- **Order**: draws in issue order within a chunk, chunks executed in order.
  The Base Pass neither blends nor depends on order (coplanar ties aside);
  back-to-front translucent stages do.
- **After the occlusion resolve**: chunks run after `IssuePrePass` resolved
  the queries; predicates are per draw.
- **Targets on the primary**: barriers, clears, `vkCmdBeginRendering` (with
  `VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`, so no inline clears
  inside it), stage timestamps and stage draw events stay on the render
  thread. Each secondary sets its own viewport and scissor and balances its
  own labels.
- **Shadows**: views are independent (separate tiles, depth only), one
  rendering per atlas; a view, or a run of views, is a chunk.

## Phases

0. **Measure and gate** (1-2 days).
   - Per-stage draw counts and per-stage render-thread CPU time, published in
     `RenderStats` and shown by `stat frame`.
   - A `JobSystem` created and registered by the launcher and the editor.
   - `GDISupportsParallelRecording()` and the device's inheritance features;
     `[Rendering] ParallelRecording`, `ParallelMinDraws`; the verdict logged.
   - Baselines, Debug and RelWithDebInfo: LightLab's GpuTest benchmark and a
     dense street level.
   Exit: Render ms, per-stage draws and per-stage CPU ms per level; the gate
   reports correctly on Vulkan and Null.
1. **Per-draw cost on one thread** -- done (Vulkan). What changed:
   - `VulkanDynamicGDI::DrawObject` packs the ViewParameters and Transform
     blocks once, as plain floats on each shader resource
     (`VKShaderResource::CoreBlocks`), instead of seven named `SetUniform`s
     per shader; the scalars remain for anything set by name.
   - The draw layouts' uniform buffers are `UNIFORM_BUFFER_DYNAMIC`:
     descriptor sets are cached by their contents (buffers, ranges, images)
     for the ring's epoch and bound with each draw's offsets -- 97-98% of
     material draws reuse a set.
   - The view block is uploaded once per view, the material block once per
     change; the last pipeline lookup, the layout group (per shader) and the
     bound pipeline, set and offsets are remembered, so unchanged state is
     not looked up or bound again.
   - Conditional rendering (occlusion predication) is held open across
     consecutive draws with the same predicate -- an entity's sections --
     instead of begun and ended round every draw, which cost this driver
     ~2.7 us a draw. It is closed before clears, queries and the end of the
     rendering.

   | Level, build | Stages CPU before -> after | Shadows | Base Pass | Frame |
   |---|---|---|---|---|
   | Street, RelWithDebInfo | 11.4-21.9 -> 5.2-5.4 ms | 7.6-15.7 -> 2.7 ms | 2.7-4.5 -> 2.0-2.1 ms | wait 11-21 -> 0 ms (the editor's 60 fps timer) |
   | Street, Debug | 34.6-36.2 -> 8.1-8.5 ms | 24.4-24.9 -> 3.7 ms | 7.6-8.7 -> 3.6-3.9 ms | 46.5 -> 18-19 ms |
   | GpuTest, occlusion on | 9.7 -> 4.4 ms | 6.9 -> 3.3 ms | 0.7 -> 0.4 ms | 15.6 -> 10.0 ms |
   | GpuTest, occlusion off | 9.9 -> 5.0 ms | 7.1 -> 3.3 ms | 2.7 -> 1.6 ms | 16.6 -> 11.9 ms |

   Smoke tests pixel-identical; the Vulkan validation layer reports nothing
   new. Still per Base Pass draw (~6 us): material binding -- the pipeline
   lookup on each material switch, texture lookups, the 4 KB scene block
   compared each draw -- spread thin; parallel recording divides it. D3D12
   keeps its string path (it is not compiled on Linux): its turn comes with
   phase 4.
2. **GDI recording contexts** -- done (Vulkan, Null). What there is:
   - `GDIRecordingContext` (GDI.h): one thread's draw state -- bound shaders,
     the material, scene and skin blocks with their serials, the material
     textures, instance buffer and indirect arguments, the occlusion
     predicate, the viewport, and the draw's own index range
     (`GDISetDrawRange`: a mesh is shared, its range cannot be).
   - `GDIBeginParallelPass(Chunks, OutContexts)` / `GDIEndParallelPass()`
     on the render thread, inside the bound targets; between them, each
     thread recording a chunk calls `GDISetCurrentRecording(Context)`. While
     a context is current (thread-local), the draw path's `GDI*` setters --
     shaders, blocks, textures, depth, blend, stencil, colour mask,
     viewport, scissor, instance buffer, indirect arguments, predicate,
     draw range, `GDIDrawObject`, draw events -- write into it, so RenderCore's
     issue code runs unchanged on a worker. `GDIInRenderThread()` is true on
     a recording thread. `GDebugGroupID` is atomic.
   - Vulkan (`VKRecordingContext.h/.cpp`): each context keeps a command pool
     (a secondary per pass, reset each frame), its own descriptor allocator
     (pools and uniform-ring chunks), draw caches (`VKDrawCaches`), blend and
     depth states, viewport and core blocks. A pass reopens the primary's
     rendering with `VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`;
     each secondary continues it (`VkCommandBufferInheritanceRenderingInfo`);
     `EndParallelPass` executes them in chunk order and reopens the rendering
     for inline commands, forgetting what the primary had bound. The draw
     path takes explicit inputs (`VKDrawInputs`): `BindDrawState` and
     `VKMesh::Record` read nothing of the driver's but the device, the
     pipeline cache -- now read under a shared lock, created under an
     exclusive one, and matched on the whole key, not just its hash -- and
     the null texture. The render thread's own draws are refused while a
     pass is open.
   - Null: contexts that count their draws, folded into its stats at the
     pass's end in chunk order; the chunks run wherever the caller runs them.
   - D3D12 and Metal report "not implemented yet" at the gate until their
     contexts exist (phase 4).
   Verified: GDISmokeTest records four chunks on four threads at once on
   Null -- each thread's state its own, the driver's untouched until the
   pass ends, every draw counted; the single-threaded path, refactored onto
   `VKDrawInputs`, is pixel-identical (RenderCore's GPU smoke test). Vulkan's
   secondaries are first exercised by phase 3, whose exit test compares a
   scene rendered with the switch on and off.
   Moved to phase 3: resolving RenderCore's lazy loads before a parallel
   stage, and per-context issue scratch (material overrides, the mesh and
   view objects, `Material::Block`).
3. **Parallel issue in RenderCore** (~1 week).
   - Base Pass: the primary clears, begins rendering for secondaries; the
     Static then Dynamic issue order split into contiguous chunks; each job
     records a secondary; the primary executes them in order.
   - Then Shadows (a chunk per view or run of views, a rendering per atlas).
   - The PrePass (it owns the queries) and the translucent stages stay
     serial unless measurement says otherwise. Captures and probes take the
     same path.
   Exit: identical smoke-test pixels with the switch on and off; Render ms
   down on both levels; clean under the Vulkan validation layer.
4. **D3D12, then frames in flight.** D3D12 direct command lists (barriers on
   the primary list). Separately, more than one frame in flight -- a command
   pool, fence, descriptor pool and ring per frame -- so the CPU records a
   frame while the GPU draws the last.

## Risks

- Threading bugs that show as flicker or a device loss, not a crash: keep the
  switch, and a validation-layer smoke run, from the first commit.
- More memory: a command buffer, descriptor pool and UBO slice per worker.
- Determinism: shared scratch, or ties in sorting, can make the switch's two
  sides differ by a pixel; the smoke test compares them exactly.
- Debug builds: per-draw overhead dominates there; judge gains in
  RelWithDebInfo, but keep Debug usable.
