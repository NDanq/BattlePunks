# DX12 Handoff: Windows Compile

2026-10-10

> **Temporary file.** A hand-off plan for the Windows/MSVC build and the D3D12 port of
> texture streaming's upload and memory work. **Delete this file in the commit that
> finishes the D3D12 implementation** (every "Done means" box below ticked); the lasting
> description belongs in `RenderCore/TEXTURE_STREAMING.md`.

## Summary

Windows Compile owns two things on the `Dev-Rendering` branch: make it build and pass under MSVC, then bring the D3D12 backend up to what Vulkan now does for texture uploads and texture memory. Everything below was written and tested on Linux (Vulkan, Intel Arc B390); nothing has been compiled on Windows yet.

What landed, both commits local to `Dev-Rendering`, not pushed:

| Commit | What | Touches D3D12? |
| --- | --- | --- |
| `347fd5b` | Texture memory: Core `BucketMemoryPool` + `PooledBytes` (now `Image`'s pixel storage), RenderCore "Textures" pool, Vulkan `VKImageHeap` + `VKStagingRing`, `GDIGetTextureMemoryStats` | New `GDIDynamicDriver::GetTextureMemoryStats` virtual; D3D12 inherits the default (returns false) |
| `8932b55` | Texture streaming: mips on the GPU by distance, range reads (`IFileProvider::ReadRange`), `TextureUV` material lines, Vulkan uploads into fresh images without `WaitIdle` | Backend-agnostic except the upload change, which is Vulkan-only |

Done means:

- [ ] `Dev-Rendering` builds on Windows (MSVC, `/std:c++17`) for the editor and a client preset, D3D12 and Vulkan backends
- [ ] RenderCore smoke tests pass on Windows, CPU and D3D12 GPU paths, with no new failures
- [ ] D3D12 uploads into fresh textures no longer drain the device
- [ ] D3D12 has an image heap and staging ring matching Vulkan's, and reports `GDIGetTextureMemoryStats`
- [ ] The WorldsLab tour numbers on D3D12 are recorded beside the Vulkan ones
- [ ] This file is deleted, and `TEXTURE_STREAMING.md` says what D3D12 now does

## Step 1: Windows build

Get `Dev-Rendering` at `347fd5b` building and green under MSVC before touching D3D12, so any later failure is yours, not ours. The MSVC lint is clean and a hidden-visibility Linux build links every new export, but those only approximate MSVC.

1. Check out `Dev-Rendering` (it holds main up to `1a0dadd` plus the two commits). It is not on `origin` yet: ask for a push or take a bundle.
2. Configure a game with the editor preset and both backends (`GDI_WITH_D3D12=ON`, `GDI_WITH_VULKAN=ON`), plus `-DRENDERCORE_BUILD_SMOKETEST=ON`.
3. Build. Where it breaks, these are the most likely places, in order:
   1. **Core `Image`** now stores pixels in `EngineCore::PooledBytes` (`CoreBytes.h`), a new `CORE_API` class with copy and move operations defined out of line. Every module that includes `CoreImage.h` recompiles.
   2. **`BucketMemoryPool`** (`CoreMemoryPool.h`) is `CORE_API` with a `std::mutex` member and deleted copies (MSVC rule 6).
   3. **`TextureStreamer`** is `RENDERCORE_API` with `std::unique_ptr<Impl>`; its destructor is in the .cpp (rule 7).
   4. **`appFileReadRange`** has a new Windows body (`CorePlatformWindows.cpp`, `SetFilePointerEx`) that has never been compiled.
   5. **`TextureBlockLevel::Bytes`** has a default member initializer that calls the exported `GetTextureMemoryPool()`.
   6. **Vulkan headers on Windows**: `VKMemoryHeap.cpp` uses `(std::min)`, but check any new `min`/`max` against `windows.h` macros.
4. Run `python .claude/skills/msvc-compat/scripts/msvc_lint.py` if you have the local msvc-compat skill (it is not in the repo). It reported 0 errors on Linux.
5. Run the tests below (Verification). Expect exactly these existing failures, which are not part of this work:
   - RenderCore smoke: *PostProcessComponent saves as version 2 (Stack, BlendDistance)*
   - `EngineCoreSmokeTest` does not compile (a `ConfigVar` constructor mismatch in `Core/Tests/SmokeMain.cpp`)
   - `RenderCoreAnimationTests` fails to link with hidden visibility (`RenderComponentProxy` vtable); on MSVC this likely shows as LNK2019

Fix MSVC-only breaks in their own commit on `Dev-Rendering`, named for the error code (C2280, LNK2019, ...), so the Linux side can learn the rule.

## Step 2: D3D12 work items

Four items, in this order. Each mirrors a Vulkan change that is already measured, so copy the behaviour, not the code. Today every D3D12 upload ends in `Device->WaitIdle()` (`D3D12TransientOps.h`, `EndAndSubmitTransientCmd`). That is the stall that cost Vulkan 10–35 ms per camera cut before the fix.

1. **Uploads into fresh textures stop draining the GPU.**
   - Rule: a texture nothing has sampled yet uploads without a wait; one already in use (`Update` at the same size) keeps the `WaitIdle`. Vulkan tells them apart by `CurrentLayout == UNDEFINED`. On D3D12, a texture just made by `CreateCommittedResource` starts in `COPY_DEST`; track a `bFresh` flag rather than infer it from state.
   - Submit the transient list with `ExecuteCommandLists` and no wait. It goes ahead of the frame being recorded on the same queue, and the frame's fence `Signal` runs after all earlier work, so the copy is done before anything samples the texture.
   - Keep the `TransientCmd` (allocator + list) and the staging buffer alive until `IsFrameComplete(GetRecordingFenceValue())`. `DeferredResources` holds only `ID3D12Resource`; add a queue for allocators and lists, drained in `DrainDeferredDestruction`.
   - Read-backs (`ReadbackTexture`) stay synchronous.
2. **A staging ring instead of a buffer per upload.**
   - One 16 MB `D3D12_HEAP_TYPE_UPLOAD` buffer, mapped once and kept mapped. Each fresh-texture upload takes a range tagged with `GetRecordingFenceValue()`; `DrainDeferredDestruction` hands back ranges whose frame is complete.
   - Pass the ring offset as `BaseOffset` to `GetCopyableFootprints`, and align each start to `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT` (512). Vulkan only needed 16.
   - Ring full, or an upload bigger than it: fall back to today's per-upload buffer.
3. **Small textures share heap blocks.**
   - Textures whose `GetResourceAllocationInfo` size is under 512 KB go into 4 MB `ID3D12Heap` blocks via `CreatePlacedResource`. That covers most textures by count: every streamed texture's tail and UI images. Use `D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES` (resource heap tier 1). Try `Desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT` (4 KB) and keep it if the allocation info agrees.
   - Larger textures stay `CreateCommittedResource`. On Vulkan, large images in shared blocks stranded memory: GPU memory rose from 825 to 963 MB on average, because nothing can move a texture to compact a block. Do not repeat that.
   - First fit with coalescing frees; keep one empty block per heap kind; free the rest. A placed resource goes through `DeferRelease`, and its range returns to the block only after that frame completes.
4. **Report texture memory.**
   - Override `GDIDynamicDriver::GetTextureMemoryStats` in `D3D12DynamicGDI`: blocks, their used bytes, committed (dedicated) bytes, and the ring. `stat streaming`'s GPU line and the WorldsLab report read it as they do on Vulkan.

While in `D3D12Texture.cpp`, also skip the RGBA8 conversion copy when the source is already RGBA8. Vulkan saved one full copy of every mip chain that way.

## Code map

Read each Vulkan file first; the D3D12 change lands in the file beside it. Paths are under `Engine/Modules/GDI/Source/`.

| Item | Vulkan reference | D3D12 target |
| --- | --- | --- |
| 1. Async fresh uploads | `Private/GDI/Vulkan/VKTexture.cpp` (`FinishUpload`, `UploadRegions`), `VKTransientOps.h` (`EndAndSubmitTransientCmdAsync`), `VulkanDynamicGDI.cpp` (`DeferDestroyCommandPool`) | `Private/GDI/D3D12/D3D12TransientOps.h` (`UploadSubresources`, `EndAndSubmitTransientCmd`), `D3D12Texture.cpp`, `D3D12DynamicGDI.cpp/.h` (a deferred queue for allocators and lists) |
| 2. Staging ring | `Public/GDI/Vulkan/VKMemoryHeap.h` + `Private/GDI/Vulkan/VKMemoryHeap.cpp` (`VKStagingRing`) | New `D3D12MemoryHeap.h/.cpp`, owned by `D3D12Device`; reclaimed in `D3D12DynamicGDI::DrainDeferredDestruction` |
| 3. Small-texture heap | `VKMemoryHeap.cpp` (`VKImageHeap`), `VKTexture.cpp` (`AllocateImage`, `Release`, `DestroyImage`), `VulkanDynamicGDI.cpp` (`DeferDestroyImage` with a `VKAllocation`) | `D3D12MemoryHeap.cpp`, `D3D12Texture.cpp` (resource creation and release), `D3D12DynamicGDI.cpp` (`DeferRelease` returning the range) |
| 4. Memory stats | `VulkanDynamicGDI::GetTextureMemoryStats` | `D3D12DynamicGDI::GetTextureMemoryStats`; the struct and virtual are in `Public/GDI/GDI.h` |
| Build | `Engine/Modules/GDI/CMakeLists.txt` lists Vulkan sources by hand | Add `D3D12MemoryHeap.cpp/.h` to the D3D12 source list |

Nothing above GDI needs to change. RenderCore's texture streaming, the "Textures" CPU pool and `stat streaming` are backend-agnostic, and `RenderContext` frees replaced textures after `GDIGetFramesInFlight()` frames on every backend.

## Verification

Run the same checks after Step 1 and again after each D3D12 item, and record the D3D12 tour numbers beside the Vulkan ones below.

**Tests** (from the build's `Binaries/<Platform>_<Config>` folder):

- `RenderCoreSmokeTest`: CPU path. Linux baseline: 506 checks, 1 failure (the known PostProcess one).
- `RenderCoreSmokeTest` with `NIMBLE_SMOKE_GPU=D3D12`, then `=Vulkan`. Linux Vulkan baseline: 676 checks, the same single failure. The GPU tests need the game's shader caches, so run from a Sandbox build, as on Linux.
- `NodeGraphTests` (1597 checks, 0 failures; the golden `Textured.material` now ends in `TextureUV 2 4`).
- `NimbleEditorImportTests`, `NimbleEditorDocumentTests`, `NimbleEditorDetailsTests`, `NimbleEditorLightingTests`: all passed on Linux.
- `GDISmokeTest` needs `-DGDI_BUILD_SMOKETEST=ON`; worth running once the D3D12 heap lands.

**Tour benchmark.** The numbers come from WorldsLab's SteampunkStreet, with a local-only camera tour: `--play --command "WorldsLab.PerfInterval 1" --command "WorldsLab.PerfReport 1" --command "WorldsLab.PerfTour 1"`, 75 seconds, read from the `[WorldsLab] perf` lines in the editor log. WorldsLab is not in the repo (`.git/info/exclude`), so it has to be copied to the Windows machine. GPU memory on Linux was the process's DRM resident memory; on Windows use `stat streaming`'s GPU line plus Task Manager's dedicated and shared GPU memory, and say which.

| Measure (75 s tour) | Vulkan, Linux (Arc B390) | D3D12, Windows | Vulkan, Windows |
| --- | --- | --- | --- |
| Streamed textures resident, mean | 56 MB of 118 MB |  |  |
| GPU memory, mean / end of run | 838 / 787 MB |  |  |
| Texture creation per frame, mean | 0.8–1.1 ms |  |  |
| Worst texture creation in one frame | 5.7–6.3 ms |  |  |
| Frame, mean | 16.1 ms (vsync) |  |  |

For D3D12, run the tour before item 1 as well: that is the stalling baseline. On Vulkan, the stalling version peaked at 10–35 ms of creation in one frame (98 ms at load).

## Risks and open questions

- **Resource state after an async upload.** The texture must already be in `PIXEL_SHADER_RESOURCE` when its transient list finishes, and the frame's list must not transition it again. Watch the debug layer for state mismatches on the first frame a texture is drawn.
- **Placed resources and the debug layer.** A placed texture released while a frame still reads it is a GPU fault, not a validation message. The range must go back to its block only after `IsFrameComplete`, never at `Release`.
- **Heap tier.** `ALLOW_ONLY_NON_RT_DS_TEXTURES` is for resource heap tier 1. If every target is tier 2, a plain heap works, but render targets must still stay out of these blocks.
- **The CPU pool on Windows.** On Linux the "Textures" pool's 32 MB cache cut page faults by only ~5%, because glibc already reuses large blocks. MSVC's CRT maps every large block afresh, so it should help more there. Measure it once: `Rendering.TextureMemoryCacheMB 0` against the default.
- **Not done anywhere yet:** prefetching from a sequence's camera, partial texture updates (min-LOD clamp instead of recreating), per-entity UV-scale overrides. Out of scope here.

**Open questions**

- [ ] Push `Dev-Rendering` to `origin`, or hand Windows Compile a bundle?
- [ ] Should WorldsLab be copied to the Windows machine for the benchmark, or use LightLab's `BakedLighting` level there and accept numbers that don't compare with the Linux ones?
