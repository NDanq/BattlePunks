# Texture streaming

Material textures keep on the GPU only the mip levels their draws need. A
texture drawn close gets its full-resolution levels; one drawn far away, or
not drawn for a while, keeps only its small tail. Levels come from the mip
chain the importer already bakes into the `.nmtexture` (TextureAsset.h), read
from disk by range, on a thread of their own.

On by default: `[Rendering] TextureStreaming=true`.

## What is streamed

A `.nmtexture` sampled by a **material**, with a mip chain, in RGBA8 or a BC
format, larger than `TextureStreamingMinResident` (64 texels on its larger
side). Everything else loads whole, as before:

| Texture | Why it is whole |
|---|---|
| No mips (Mip Gen None), or 64 x 64 and smaller | nothing to leave out |
| RGBE8 (a sky's HDRI) | no mips; sampled point |
| Lightmaps, the sky's environment | read by the lighting passes, not by a draw with a place |
| UI images | the UI keeps the texture pointer between frames |
| Material previews (thumbnails, the material editor) | drawn once and kept; never blurry |
| Render targets (scene captures) | not files |

## How a level is chosen

Each frame, every mesh section queued in a scene reports to each of its
material's textures how many **screen pixels one UV unit spans** where it is
drawn:

```
pixels per UV = pixels per world unit at the proxy  x  world units per UV unit  /  the material's UV scale
```

- **Pixels per world unit** (RenderScene, per proxy): half the viewport's
  height times the projection's y scale, over the distance from the camera to
  the proxy's nearest point (its padded world bounds; at least ZNear). Inside
  the bounds, the distance is ZNear: all of it. An orthographic view uses no
  distance.
- **World units per UV unit** (RenderContext, per mesh section, measured
  once when the GPU mesh is built): the square root of the section's world
  area over its UV0 area, times the draw's largest scale.
- **The material's UV scale** (`TextureUV` in the cooked `.material`, from
  the material compiler): when a Texture Sample's UV is Texture Coordinate
  times constants and Parameters (any nesting of Multiply), its texture's
  UV unit is that much smaller than UV0's. Parameters count at their
  defaults (an entity's override is not seen); a UV the compiler cannot
  follow counts as UV0. A material compiled before this has no lines, and
  counts as UV0 -- recompile its graph to get them.

The texture's wanted level is the one whose texels match those pixels:

```
level = floor(log2(larger side / pixels per UV) + TextureStreamingMipBias), clamped to [0, tail]
```

The largest demand of the frame wins: the nearest draw of the frame, in any
view (scene captures included). A draw whose demand is unknown -- a quad, a
proxy with no bounds, a mesh with no UV area -- asks for all of it.

## What happens then

`RenderContext::UpdateTextureStreaming`, once a frame before the first scene
queues its draws (one frame behind the demand, like everything else here):

1. **Reimports.** `TextureAssetCache::Invalidate` is also a log
   (`ConsumeInvalidations`); a reimported texture starts again from its tail.
2. **Commit** what the streaming thread finished, oldest first, until
   `TextureStreamingUploadMB` of textures were made this frame (at least one).
   Committing joins the loaded levels onto the resident ones as a new
   `TextureData`; `FindTexture` sees the pointer change and builds a new GPU
   texture, and the one it replaces is freed once no frame in flight can
   sample it (`GDIGetRenderingSerial` past `GDIGetFramesInFlight`).
3. **Wanted levels**, from the last frame's demand; a texture not drawn for
   `TextureStreamingDropDelay` seconds wants only its tail. Then the **pool**:
   if every wanted level together is over `TextureStreamingPoolMB`, every one
   is raised by the same number of levels until they fit (`stat streaming`'s
   *bias*).
4. **Drop** levels a texture has but no longer wants -- after DropDelay, or
   at once when the resident set is over the pool. A drop needs no read: the
   lower levels are already resident.
5. **Request** loads for textures that want more than they have, largest
   shortfall first, until `TextureStreamingMaxLoads` are in flight. A load
   reads only the missing levels, `[wanted, resident)`, in one range read
   (`LoadTextureLevels`) -- `IFileProvider::ReadRange`: a seek on disk, a
   slice of a pack (unscrambled from the right offset).

A texture's first use (its material's first draw) reads its layout -- a few
chunk headers -- and its tail, at once, on the render thread: under 25 KB for
a 1024 x 1024 RGBA8 texture.

## Settings

`[Rendering]`, all live:

| Setting | Default | |
|---|---|---|
| TextureStreaming | true | off: every texture whole, as before; turning it off forgets every streamed level |
| TextureStreamingPoolMB | 512 | the levels draws may want before every wanted level is raised alike |
| TextureStreamingMipBias | 0 | added to every wanted level: 1 is half the resolution, -1 double |
| TextureStreamingMinResident | 64 | the larger side of the levels a texture always keeps; a texture no larger is not streamed |
| TextureStreamingUploadMB | 8 | textures made on the render thread in one frame, by their bytes; 0: no limit |
| TextureStreamingDropDelay | 5 | seconds a texture keeps levels it no longer wants while the pool has room |
| TextureStreamingMaxLoads | 8 | loads on the streaming thread at once |
| TextureMemoryCacheMB | 32 | freed texture blocks the CPU "Textures" pool keeps for reuse (below) |

`stat streaming` (the editor's console and the game's; `[Stats] Streaming`)
shows the textures streamed, the bytes resident, wanted and in every level,
the pool and its bias, loads in flight and done, drops, the last frame's
commits, the render thread's time in the update and in making the
textures, and texture memory on both sides (below).
`RenderStats::TextureStreaming` has the same numbers.

`RenderContext::FlushTextureStreaming` loads and commits every level the last
update wanted, ignoring the upload budget -- for a test or a one-off render.

## Texture memory

**CPU.** Every texture file's levels and blocks are allocated in a Core
memory pool, `"Textures"` (`GetTextureMemoryPool`, TextureAsset.h): a
`BucketMemoryPool` (CoreMemoryPool.h) registered as RenderCore loads. Pixels
live in `EngineCore::PooledBytes` -- `Image`'s storage, and
`TextureBlockLevel::Bytes` -- which allocate through `Memory::Allocate` in
the pool they name. The pool rounds requests to size classes (2^k and
1.5 x 2^k, from 4 KB to 64 MB) and keeps freed blocks, up to
`TextureMemoryCacheMB`, for the next request of the same class: a streamed
level freed is the next one's memory. Its stats are the CPU cost of every
texture loaded (`stat streaming`'s *CPU* line).

**GPU (Vulkan).** On `VKDevice` (VKMemoryHeap.h):

- `VKImageHeap`: images under 512 KB -- most textures by count: a streamed
  texture's tail, UI images -- share 4 MB device-local blocks (first fit,
  coalescing frees; one empty block per memory type kept); larger images get
  their own allocation. Freed images go back through the deferred-destruction
  queue, after the frames that sampled them.
- `VKStagingRing`: a 16 MB persistently mapped upload buffer. An upload into a
  fresh image stages there instead of in a buffer made and freed for it, and
  the ring reclaims each frame's part when that frame's fence has passed. A
  full ring, or an upload into an image in use, stages on its own as before.

**GPU (D3D12).** The same two, on `D3D12Device` (D3D12MemoryHeap.h):

- `D3D12TextureHeap`: textures under 512 KB are placed
  (`CreatePlacedResource`) in 4 MB `ID3D12Heap` blocks with
  `ALLOW_ONLY_NON_RT_DS_TEXTURES` (what resource heap tier 1 needs), at 4 KB
  alignment where the runtime grants it; larger ones are committed. A
  placed texture's range goes back to its block only after the resource is
  released and the frames that sampled it have finished (`DeferRelease`).
- `D3D12StagingRing`: a 16 MB upload-heap buffer, mapped once. Ranges start
  at 512 bytes (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`), not Vulkan's 16.

`GDIGetTextureMemoryStats` reports both backends' (blocks, their used bytes,
dedicated images, the ring); `stat streaming`'s *GPU* line shows them.

**Measured** (the same path as below; GPU memory is the process's DRM GTT):

| | Before the pools | Large images in 64 MB blocks | Final (small images in 4 MB blocks) |
|---|---|---|---|
| GPU memory, mean | 825 MB | 963 MB | 838 MB |
| ... at the end, streaming idle | 777 MB | 971 MB | 787 MB |
| Worst texture creation in a frame | 7.1-7.9 ms | 5.7 ms | 5.7-6.3 ms |
| Texture creation per frame, mean | 1.5-1.9 ms | 1.1 ms | 0.8-1.1 ms |

Large images in shared blocks stranded memory: once streaming dropped them,
partly used blocks held three times what they still served, and nothing can
move an image to compact them -- hence large images on their own. What the
final layout costs is the 16 MB ring. The CPU pool's cache changed little on
Linux: glibc already reuses large blocks once some are freed (page faults
over the run 120.7K with the cache, 126.5K without); a CRT that maps every
large block afresh, as MSVC's does, has more to gain. Not measured there.

## Cost, measured

WorldsLab's SteampunkStreet on an Arc B390 (Vulkan, Mesa), RelWithDebInfo, the
same 75-second camera path each run (the level's flyover cinematic, then a
walk down the street and out). 42 textures stream; all their levels are
118 MB.

| | Streaming off | On (pool 512) | On (pool 64) |
|---|---|---|---|
| Streamed textures resident, mean | 118 MB | 56 MB | 30 MB |
| ... peak | 118 MB | 110 MB | 64 MB |
| Process GPU memory (GTT), mean | 897 MB | 825 MB | 792 MB |
| First frame | 384-1214 ms | 210-250 ms | 210-250 ms |
| Frame (vsync), mean | 16.2 ms | 16.1 ms | 16.1 ms |
| Worst frame each second, 90th percentile | 26.3 ms | 24.5 ms | 23.7 ms |
| Update, per frame | -- | 0.01-0.07 ms | up to 1.4 ms |

The peak is near everything: the flyover passes close to every material
at some point and DropDelay keeps what it saw. The pool bias did its job at
64 MB (one level).

**Making the textures was the one real cost, and it was the GDI's.** Every
Vulkan texture upload ended in `Device->WaitIdle()` -- the render thread
draining the GPU -- and a commit makes a whole texture, so camera cuts that
wanted many textures at once cost 10-35 ms (98 ms at load) of the render
thread in one frame. Two changes:

- **Vulkan uploads into a fresh image no longer wait** (VKTexture.cpp,
  `FinishUpload`): submitted without a fence and freed with the frame being
  recorded, whose fence covers every earlier submission on the queue. An
  upload into an image already in use (`Update` at the same size) still waits:
  the frames in flight may be reading it. D3D12 does the same (D3D12Texture's
  `bFresh`; the list, its allocator and its staging are kept until the frame
  being recorded has finished).
- **UploadMB 8**: about 1 ms a megabyte remains on this machine (copying the
  levels, the staging buffer, the allocations), so 32 MB a frame was a 20 ms
  frame at a cut. At 8 the most in one frame was 8 ms, and a cut's textures
  arrive over a quarter of a second.

**D3D12, measured.** WorldsLab is not in the repository, so on Windows
(RTX 2080 Ti, RelWithDebInfo client, 120 Hz vsync) the numbers come from
LightLab's BakedLighting room instead: a few textures stream (the bricks',
the chrome's, the lightmap), 21 MB in all, so
they do not compare with the table above. Each run is the same 75-second
camera path: a dolly up to the brick box, cuts away and back, an orbit, a
cut to the chrome ball. Two runs each, before the D3D12 change (every upload
waits) and after; GPU memory is Task Manager's, dedicated and shared, and
`stat streaming`'s GPU line.

| | D3D12, waiting | D3D12 | Vulkan |
|---|---|---|---|
| Streamed textures resident, mean | 17.2 of 21.3 MB | 17.2 MB | 17.2 MB |
| Texture creation per frame, mean | 0.020-0.022 ms | 0.008 ms | 0.005 ms |
| Worst texture creation in one frame | 9.4-12.7 ms | 4.2-5.3 ms | 2.9 ms |
| Frame, mean | 8.35-8.37 ms | 8.35 ms | 8.35 ms |
| Task Manager, dedicated, mean / end | 275 / 279 MB | 279 / 283 MB | 292 / 296 MB |
| Task Manager, shared | 35 MB | 52 MB | 33 MB |
| `stat streaming` GPU, mean / end | (not reported) | 45 / 50 MB | 45 / 49 MB |

Each second of the tour with a creation spike over 0.5 ms waiting (twelve:
load, the cuts, the dollies' mip changes) was 2-6x lower without the wait.
The extra 17 MB of shared memory is the staging ring (an upload heap lives in
system memory) and the extra 4 MB dedicated the one kept heap block.

## Not done

- **Per-entity material overrides** of a UV scale parameter are not seen.
- **CPU copies.** The resident levels are kept on the CPU too (to join and
  drop without a read), and making a GPU texture copies them once more
  (`GDICreateTextureResourceMips` takes one array of levels). On an
  integrated GPU that memory is shared. Keeping no CPU copy means reading a
  drop's levels again and a GDI call that takes levels in place.
- **Partial updates.** A commit makes a whole new texture rather than adding
  levels to the one it has; GDI has no texture with a resident-level range
  (`minLod` / `SetResourceMinLODClamp`).
- **Prefetching** from where the camera is going (a sequence's camera
  track) -- what a cut wants arrives a few frames after it.
