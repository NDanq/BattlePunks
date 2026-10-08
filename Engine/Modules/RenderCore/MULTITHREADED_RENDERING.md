# Multithreaded rendering: plan

Status: **phases 0 (measure and gate), 1 (per-draw cost), 2 (GDI recording contexts), 3 (parallel issue: Base Pass and Shadows) and 4 (frames in flight, Vulkan and D3D12) done; D3D12 recording contexts done; Metal's not done.** Recording draws on several
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
   compared each draw -- spread thin; parallel recording divides it. D3D12's
   render thread keeps its string path; its recording contexts pack the
   blocks (phase 4).
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
   - D3D12: a direct command list per context (below, under phase 4).
     Metal reports "not implemented yet" at the gate until its contexts exist.
   Verified: GDISmokeTest records four chunks on four threads at once on
   Null -- each thread's state its own, the driver's untouched until the
   pass ends, every draw counted; the single-threaded path, refactored onto
   `VKDrawInputs`, is pixel-identical (RenderCore's GPU smoke test). Vulkan's
   secondaries are first exercised by phase 3, whose exit test compares a
   scene rendered with the switch on and off.
   Moved to phase 3: resolving RenderCore's lazy loads before a parallel
   stage, and per-context issue scratch (material overrides, the mesh and
   view objects, `Material::Block`).
3. **Parallel issue in RenderCore** -- done (Vulkan; Null runs the chunks
   serially). What there is (RenderContext.cpp):
   - `IssueInParallel`: given a stage's draws in issue order (Static, then
     Dynamic, then Stationary -- the serial order), the gate's verdict for
     that many draws, it resolves what the draws would otherwise load lazily
     on the render thread (`ResolveForParallel`: meshes, materials and their
     textures, the lightmap, each material's pass), splits the list into
     contiguous chunks of at least 32 draws (at most one per thread, the
     render thread included, and `GDI_PARALLEL_CHUNKS_MAX`), opens a GDI parallel pass and records
     each chunk on the job system (`ParallelFor`); the pass executes them in
     chunk order, so the command stream is the serial one's.
   - Per worker issue scratch (`RenderIssueScratch`, thread-local): the
     material block and overrides, the mesh object, the draw label state --
     what the serial path kept on `RenderContext::Impl`.
   - Base Pass: tried first; anything it cannot take (a draw that is not
     resolvable) falls back to the serial issue, whole.
   - Shadows: per atlas, one rendering; the views' casters form one list,
     a chunk switching viewport and view matrices where its view changes.
     Falls back to the serial per-view loop the same way.
   - The PrePass (it owns the queries), the translucent stages, captures and
     probes stay serial: below the threshold, or not measured to matter.
   - `[Rendering] ParallelRecording` and `ParallelMinDraws` are read again
     when they change (the console, an ini reload), not only at startup.
     `ParallelMinDraws` defaults to 256; `RenderStats::ParallelPasses`
     counts the passes recorded.

   Verified: RenderCore's GPU smoke test renders a floor and 400 boxes under
   a cascaded sun with the switch off, on (two parallel passes a frame) and
   off again: the three images are identical byte for byte. All smoke tests
   pass on Vulkan and Null; the validation layer reports only the two
   messages it did before phase 0 (`vkCmdDraw-None-09600` in the shadows
   test, `vkDestroyDevice-device-05137` at teardown).

   | Level, build | Stages CPU Off -> Auto | Shadows | Base Pass | Frame |
   |---|---|---|---|---|
   | Street, RelWithDebInfo | 5.3-5.5 -> 4.2-4.3 ms | 2.7-2.8 -> 1.9 ms | 2.0-2.1 -> 1.7 ms | editor's 60 fps timer both ways; render 5.7-6.0 -> 4.9-5.0 ms |
   | Street, Debug | 16.0-16.1 -> 5.4-5.9 ms | 7.7 -> 2.3-2.5 ms | 7.0 -> 2.0 ms | 28.5-28.6 -> 17.3-18.2 ms |
   | GpuTest, occlusion on | 4.55 -> 3.58 ms | 3.30 -> 2.22 ms | 0.46 -> 0.48 ms (108 draws: serial) | 10.3 -> 9.6 ms |
   | GpuTest, occlusion off | 5.10 -> 3.94 ms | 3.29 -> 2.27 ms | 1.63 -> 1.40 ms (409) | 11.75 -> 10.75 ms |

   (Arc B390, 15 workers; the street two half-second windows, GpuTest the
   mean of two benchmark runs each. The Debug street's Off column is slower
   than phase 1's table: that build had been rebuilt with phase 2's
   explicit-input draw path.) What it says:
   - Debug gains most: there the per-draw work dominates and divides.
   - In RelWithDebInfo a stage's chunks are short (32-150 draws, tens of
     microseconds each): what remains is serial -- the resolve, the
     fork and join, `vkCmdExecuteCommands` and re-opening the rendering --
     about 1 ms of the 2 the Shadows still take. Fewer, larger chunks, and
     resolving once per frame instead of per stage, are what is left there.
   - GpuTest's frame is now bound by waiting for the GPU (`wait_ms` 8-10
     of 10): the CPU's next win is phase 4's frames in flight.
4. **Frames in flight** -- done (Vulkan, then D3D12). **D3D12 parallel
   recording -- done.**

   Frames in flight: `[Rendering] FramesInFlight` (1-3, default 2, read
   when the device is made) lets the CPU record frame N+1 while the GPU
   draws frame N. Before it, the render thread submitted a frame and then,
   at the next `BeginRendering`, waited on its fence for most of the GPU's
   frame (GpuTest: 3-5 ms a frame of a ~4.6 ms GPU frame). What became per
   frame slot, each reused only after that slot's fence is waited:
   - `VKCommandManager`: a command pool, primary buffer and fence per slot;
     `EndRendering` advances; `FlushForReadback` stays on its slot (its wait
     covers every earlier submission, so it still drains everything).
   - The descriptor allocators -- pools and uniform ring -- the driver's and
     each recording context's, and the contexts' secondary pools. Allocator
     epochs come from one counter, so a draw cache keyed by an epoch never
     takes another allocator's sets or offsets.
   - Timestamps: a query range per slot, read when the slot comes round.
   - Dynamic meshes: a region of the buffers per slot (`GDIMesh::UpdateDynamic`
     writes the recording slot's).
   - Deferred destruction: each handle tagged with the frame that may use it,
     destroyed once that frame has finished.
   - "Done" for readbacks and occlusion answers is the timeline semaphore's
     value (`IsSubmissionDone`), not "the previous frame".
   RenderCore: the main view keeps an occlusion query set per frame in
   flight (`GDIGetFramesInFlight`), used in turn, each proxy remembering its
   query index per set; answers arrive that many frames late. "stat frame"
   gives each block's GPU times to the scene that timed that block, found by
   its rendering serial. Material previews and target readbacks are
   delivered up to `FramesInFlight` frames after they are drawn.

   Verified: every smoke test passes with 1 and with 2 frames in flight
   (RenderCore's on Vulkan 634/0, on Null 464/0; GDI 75/75); occlusion
   culling hides the same boxes with the same pixels; the validation layer
   reports only the two pre-existing messages.

   | LightLab GpuTest, RelWithDebInfo, Auto parallel recording | 1 in flight | 2 in flight |
   |---|---|---|
   | occlusion on: frame / render / wait | 9.7 / 9.1 / 8.6 ms (one run of two: 19.8 ms, an outlier) | 5.3-5.4 / 4.7-4.8 / 4.3-4.4 ms |
   | occlusion off: frame / render / wait | 10.5-10.9 / 9.8-10.2 / 9.2-9.6 ms | 6.3-6.5 / 5.7-5.8 / 5.2-5.4 ms |
   | stages CPU | 3.6-4.0 ms | 3.5-3.8 ms |
   | occluded proxies a frame | 303 | 302-303 |

   The street in the editor barely moves (Debug 21 -> 19 ms a frame,
   RelWithDebInfo pinned at the editor's ~16 ms): there the frame is the
   present, ~10.7 ms blocked on the swap, not the render thread.

   D3D12 parallel recording (`D3D12RecordingContext.cpp`): each context
   records a direct command list of its own -- bundles cannot clear, query or
   set targets -- on an allocator it keeps, rewound at `BeginRendering` after
   the frame fence's wait. A list inherits nothing, so each binds the heaps,
   root signature, targets, viewport and scissor as the frame list has them,
   then its pipelines, blocks and predicates draw by draw. `EndParallelPass`
   splits the frame list round the chunks: its first half executes, then the
   chunks in one more `ExecuteCommandLists` call, and it reopens on the same
   allocator (`D3D12CommandManager::Resume`) with its targets and PIX events
   as they were; the frame fence behind `EndRendering`'s submit covers all of
   it. The separate calls matter: a buffer decays to COMMON between them, so
   the instance and indirect-argument buffers a chunk reads are promoted
   implicitly, with no barrier from a thread that cannot track their state.
   - The draw path takes explicit inputs (`D3D12DrawInputs`) as Vulkan's
     does: `D3D12Mesh::Record` and `BindDrawState` read nothing mutable the
     render thread owns. The core blocks come packed (`GDICorePack`, shared
     with Vulkan, byte for byte what the string path writes); the scene and
     skin uploads' cache is per context (`D3D12BlockUploads`).
   - Shared and safe: the constant-buffer ring's `AllocateCB` claims with an
     atomic; the pipeline cache is read under a shared lock and matched on
     the whole key.

   Verified on Windows (RTX 2080 Ti): RenderCore's GPU smoke test on D3D12
   634/0, the parallel capture byte-identical to the serial ones, the D3D12
   debug layer silent; GpuTest 300 frames under the debug layer with
   `ParallelRecording=On`, silent.

   | LightLab GpuTest, D3D12, RelWithDebInfo, two runs each | Off | Auto |
   |---|---|---|
   | frame / render | 11.3-11.6 / 10.3-10.6 ms | 7.4 / 6.4 ms |
   | stages CPU | 8.4-8.5 ms | 4.9 ms |
   | Shadows CPU (~1,500 draws) | 5.7-5.9 ms | 2.2 ms |
   | Base Pass CPU (~110 draws: under ParallelMinDraws, serial) | 0.7 ms | 0.7 ms |

   D3D12 frames in flight, as Vulkan's, per frame slot, each reused once its
   slot's fence value is reached:
   - `D3D12CommandManager`: a command allocator per slot (the one list resets
     on any of them as soon as it has been executed) and the fence value the
     slot's last submit signalled; `BeginFrame` waits only that. `EndRendering`
     advances; `FlushForReadback` stays on its slot, so its wait covers every
     earlier submission.
   - The constant-buffer ring: a 4 MB region per slot in one buffer (the SRV
     and sampler heaps are shared), its epoch bumped at each rewind.
   - Each recording context's allocator; GPU timestamps' query range and
     readback range; dynamic meshes' buffer region.
   - Deferred destruction: each resource, query heap and SRV slot tagged with
     the fence value of the frame being recorded, freed once it completes.
   - Occlusion answers: readable once the fence value of the frame that
     resolved them completes, not "from the next block".
   - `[Rendering] FramesInFlight` moved from the Vulkan backend into GDI
     (`GDIGetConfiguredFramesInFlight`), which both read.

   Verified: RenderCore's GPU smoke test on D3D12 634/0 with 1, 2 and 3 frames
   in flight, the parallel capture byte-identical each time, the debug layer
   silent; GpuTest under the debug layer, silent; Null 464/0, GDI 75/75.

   | LightLab GpuTest, D3D12, RelWithDebInfo, Auto, two runs each | 1 in flight | 2 in flight |
   |---|---|---|
   | frame / render | 6.9-7.0 / 6.0-6.1 ms | 6.6 / 5.7 ms |
   | render thread outside its stages and queue | 1.0 ms | 0.6 ms |
   | GPU | 1.3-1.4 ms | 1.3 ms |
   | occluded proxies a frame | 300 | 300 |

   A small win here because this machine's GPU (RTX 2080 Ti) draws GpuTest in
   about 1.3 ms: the render thread was waiting on the fence for only ~0.4 ms
   of its frame, and is now bound by its own recording. A frame that keeps the
   GPU busier gains as Vulkan's did.

   D3D12 phase 1, the per-draw savings, as Vulkan's (`D3D12DrawCaches`, one
   for the frame list and one per recording context):
   - `DrawObject` packs the ViewParameters and Transform blocks once, as floats
     on each shader resource (`D3D12ShaderResource::CoreBlocks`, by
     `GDICorePack`), instead of seven named `SetUniform`s per shader per draw.
   - The view block is uploaded once per view (while its bytes are unchanged),
     the material block (b2) once per change by serial, as the scene and skin
     blocks already were; only the transform is every draw's.
   - The last pipeline key is remembered: a draw with the same key skips the
     hash, the cache's lock and the map.
   - What the list has bound -- pipeline, each root CBV and table -- is
     remembered and not set again; forgotten whenever a list is (re)opened, and
     the pipeline after an in-frame compute dispatch. A material texture slot
     holding the same resource as last time is skipped whole.
   - Texture lookups are `static_cast`s (every texture this backend makes is a
     `D3D12TextureResource`), and a shader's texture 0 is found when set, not
     per draw.
   Verified: RenderCore's GPU smoke test on D3D12 634/0, the debug layer
   silent; its 24 dumped captures (`NIMBLE_SMOKE_DUMP`) byte-identical to
   the build before.

   And a fix outside GDI that parallel recording had been paying for:
   `JobSystem::Wait` yielded 64 times, then slept a millisecond at a time, so
   the render thread waiting on a pass's chunks woke a millisecond or more
   after they were done (profiled: chunks finished in 0.6 ms, `ParallelFor`
   returned at 1.6). It now yields for up to `WAIT_YIELD_US` (1 ms) of wall
   time before sleeping. Every backend's parallel passes gain from it.

   | LightLab GpuTest, D3D12, RelWithDebInfo, two runs each | before phase 1 | phase 1 | phase 1 + Wait |
   |---|---|---|---|
   | serial: stages / per draw | 8.2-8.4 ms / 4.0-4.1 us | 2.4 ms / 1.16 us | 2.4 ms |
   | serial: Shadows (~1,500 draws) | 5.6-5.7 ms | 1.55 ms | 1.56-1.58 ms |
   | Auto: stages / Shadows | 4.9 / 2.2 ms | 2.8 / 2.0 ms | 1.5 / 0.75 ms |
   | Auto: render thread | 6.3-6.4 ms | 4.3 ms | 3.0 ms |
   | frame (wall) | 7.4 ms | 5.4 ms | 5.0 ms |

   The frame no longer follows the render thread: on D3D12 it holds at
   exactly 5.0 ms of wall time (Vulkan, the same run: 3.8 ms), with the
   game thread at 1 ms and the GPU under 1. Something on D3D12's side --
   the present, most likely -- paces it; not yet found.

   Metal: its recording contexts were not written (no macOS here); the gate
   says "not implemented yet", and Metal records serially.

## Risks

- Threading bugs that show as flicker or a device loss, not a crash: keep the
  switch, and a validation-layer smoke run, from the first commit.
- More memory: a command buffer, descriptor pool and UBO slice per worker.
- Determinism: shared scratch, or ties in sorting, can make the switch's two
  sides differ by a pixel; the smoke test compares them exactly.
- Debug builds: per-draw overhead dominates there; judge gains in
  RelWithDebInfo, but keep Debug usable.
