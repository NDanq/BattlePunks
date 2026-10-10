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

`stat streaming` (the editor's console and the game's; `[Stats] Streaming`)
shows the textures streamed, the bytes resident, wanted and in every level,
the pool and its bias, loads in flight and done, drops, the last frame's
commits, and the render thread's time in the update and in making the
textures. `RenderStats::TextureStreaming` has the same numbers.

`RenderContext::FlushTextureStreaming` loads and commits every level the last
update wanted, ignoring the upload budget -- for a test or a one-off render.

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
  the frames in flight may be reading it. D3D12 still waits every time.
- **UploadMB 8**: about 1 ms a megabyte remains on this machine (copying the
  levels, the staging buffer, the allocations), so 32 MB a frame was a 20 ms
  frame at a cut. At 8 the most in one frame was 8 ms, and a cut's textures
  arrive over a quarter of a second.

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
- **D3D12** still drains the device after every upload (D3D12TransientOps.h).
- **Prefetching** from where the camera is going (a sequence's camera
  track) -- what a cut wants arrives a few frames after it.
