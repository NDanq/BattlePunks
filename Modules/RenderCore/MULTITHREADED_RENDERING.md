# Multithreaded rendering: plan (future work)

Status: **not started.** Recording the Base Pass's draws on several threads,
when the backend can. This note is why, what it would take, and in what order.

## The request

- Check at launch whether the driver supports multithreaded rendering.
- If it does, record the Base Pass's RenderProxy draws on several threads.

## What it would buy

Parallel recording shortens one thing: the **render thread's CPU time spent
recording draws**. It does not touch:

- GPU time (the same work reaches the GPU);
- the game thread, culling, or the editor's 16 ms frame timer.

It pays when `stat unit`'s **Render** is the longest bar *and* that time is
draw recording -- typically thousands of draws a frame. Below roughly a
thousand, the fork/join and the per-thread command buffers cost about what
they save. As of October 2026 a RelWithDebInfo editor holds 60 fps (the frame
timer's cap) with a ~3 ms GPU frame, so there may be nothing to win yet: measure
first (phase 1).

"Driver support" is a per-backend constant, not a runtime probe: Vulkan, D3D12
and Metal can always record in parallel; OpenGL cannot. What launch decides is
GDI's capability flag, the core count, and a config switch.

## Why it is expensive here

`RenderContext::Impl::Issue` drives GDI's global immediate-mode API
(`GDISetMaterialTexture`, `GDISetUniformBlock`, `Bind`, `GDIDrawObject`), and
each backend keeps one `CurrentStateData` -- one command buffer, one set of
bound state -- plus driver-wide bound shaders. Parallel recording needs:

1. **A per-thread GDI recording context** in place of the globals: its own
   bound state, uniform ring slice, descriptor pool and command buffer --
   Vulkan secondary command buffers inside the primary's rendering, D3D12
   extra command lists, Metal `MTLParallelRenderCommandEncoder`.
2. **No shared mutable state on the draw path.** Shader uniforms are written
   onto the shared `GDIShader` resources (`VKShaderResource::GetScalars`);
   the pipeline cache and material caches need concurrent lookups.
3. **A single-threaded fallback** for OpenGL, and whenever the flag is off.
4. **Deterministic order and debugging:** secondaries executed in a fixed
   order so captures and the pixel-for-pixel smoke tests are stable; draw
   events (RENDERPIPELINE.md, "Draw events") per worker.

## Cheaper first: the cost of one draw

`VulkanDynamicGDI::BindDrawState`, per draw:

- scans the pixel shader's uniforms **by name** (`StartsWith` over a
  string-keyed map) to pick the descriptor layout group -- fixed per shader
  pair;
- allocates and writes a **new descriptor set** (`vkAllocateDescriptorSets` +
  `vkUpdateDescriptorSets`);
- repacks the ViewParameters/Transform blocks through **string lookups**.

Caching the layout group and pipeline per shader pair, reusing descriptor sets
for unchanged bindings, and pre-packing static draw lists' GPU state (they are
already captured once: RENDERPIPELINE.md, "Static draw lists") cut every
draw's cost on the one thread, with no threading risk, on every backend.

## Phases

1. **Measure** (~an hour). RelWithDebInfo; `stat unit` (Render), `stat frame`
   and the per-stage draw counts on LightLab and a stress level (thousands of
   static and dynamic meshes). Add draw counts to the `stat unit` panel if
   reading them is awkward. Exit: Render ms and Base Pass draw count per level.
2. **Per-draw cost on one thread** (days, low risk). Layout/pipeline cache per
   shader pair; descriptor reuse; pre-packed static draws. Exit: Base Pass
   recording time per draw, before and after; smoke tests unchanged.
3. **Parallel recording** (weeks) -- only if Render is still the bottleneck:
   1. GDI: a recording-context interface and `GDISupportsParallelRecording()`;
      Vulkan first (secondary command buffers, per-thread descriptor pools
      and UBO ring slices, a concurrent pipeline cache).
   2. RenderCore: a job pool; the Base Pass's Static then Dynamic lists split
      into contiguous chunks per worker, executed in chunk order; a config
      switch (`[Rendering] ParallelBasePass`) and the single-threaded path as
      the fallback.
   3. D3D12 (command lists), then Metal (parallel encoder). OpenGL stays
      single-threaded.
   Exit: identical smoke-test pixels with the switch on and off; Render ms
   down on the stress level; clean under the Vulkan validation layer.

## Risks

- Threading bugs that show as flicker or a device loss, not a crash: keep the
  switch, and a validation-layer smoke run, from the first commit.
- More memory: a command buffer, descriptor pool and UBO slice per worker.
- Draw order: workers must not reorder within a chunk; Base Pass order only
  matters for coplanar ties (it neither blends nor depends on order).
