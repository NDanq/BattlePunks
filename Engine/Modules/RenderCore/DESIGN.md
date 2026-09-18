# RenderCore design notes

The long-form "why" behind decisions in this module -- comparisons to
alternatives, cross-references to other files' reasoning, bugs found and
fixed during development, and worked examples. `README.md` is the short,
usage-facing doc; this is where its claims are justified in detail. Organized
by topic, roughly in the order a reader would encounter them working through
the module.

---

## Why RenderCore exists at all

Before this module, `NimbleEngine::Application` owned GDI directly: a
`GDIContext`, a render thread it started and stopped by hand, and a fixed
"clear the screen" command built once per frame. Three comments in the tree
said what came next -- `IGame` was "simulation only, for now ... drawing
arrives with render proxies", and `Exec()`'s own fence comment said
"overlapping command BUILDING too is the render-proxy work; only this fence
changes when that lands". RenderCore is that work, pulled out into its own
module rather than grown inside `NimbleEngine.cpp`, for the same reason
GameCore is its own module rather than a header inside `NimbleEngine`: the
render stack has its own threading contract, its own registries, and its own
reasons to be SHARED, none of which belong in the engine layer that merely
owns one.

---

## Why RenderCore must be the only image that links GDI

GDI (`Engine/Modules/GDI`) is a STATIC library -- `GDI_SHARED` defaults OFF,
and its export macro (`GDIAPI.h`) has never actually been exercised in this
tree. A STATIC library linked into two DLLs in the same process gives each
DLL its **own** copy of every global GDI owns: `GDynamicGDI`, `GSync`,
`GRenderCommands`, the `appThreadID()` latch `GDIInRenderThread()` reads.
Before this module, `NimbleEngine.dll` was the only image doing this, so the
question never came up. The moment a second image linked GDI -- which
`NimbleEngine` would have had to, to keep drawing without RenderCore --
`InitializeGDI` in one image would leave the other's `GDynamicGDI` null
forever, with no diagnostic anywhere: the second image's every GDI call
would silently no-op through `IMPLEMENT_GDI_FUNCTION_BODY`'s
`if(GDynamicGDI)` guard.

The fix is the same one `Engine/CMakeLists.txt` already applies to
`EngineCore` (`CORE_SHARED` forced ON) and to `GameCore` (always SHARED, its
own registries): pick exactly one image to own the STATIC dependency, and
make every other image reach it only through that one image's own
(dynamically linked) API. `RenderCoreInternal.h` is the chokepoint --
literally the only header in this module allowed to `#include` a GDI header
-- and it lives under `Private/`, so nothing outside `RenderCore.dll`'s own
`.cpp` files can even see a GDI type, let alone link against it directly.
`NimbleEngine` dropped its GDI link entirely once RenderCore existed; there
was never a point where two images linked it at once.

**What this does and does not mean for the garbage collector.** The
collector (`CoreGarbageCollector.h`) tracks `TRef`s through an intrusive
list; it has no idea GDI or RenderCore exist, and nothing about hiding GDI
inside one DLL changes that. The actual GC-relevant rule is narrower and
already stated where it matters: `RenderCommand.h`'s capture rule (below)
keeps a `TRef` from ever crossing onto the render thread, and
`NimbleEngine.cpp`'s `CollectGarbage()` call site is chosen because the
render thread is provably idle there (`WaitForPreviousFrame()` just
returned) *and* because nothing on the render thread ever touches a `TRef`
in the first place -- the second half of that sentence would be true whether
GDI lived in one image or three. Hiding GDI behind RenderCore is purely a
STATIC-library-linkage argument; it buys nothing and costs nothing on the
collector side.

---

## RenderCommand.h -- one frame of lag, not a fence

**Why not the fence the code before this module used.** Under the old
design, `Application::Exec`'s loop waited for the render thread to go
completely idle (`FrameDone.Wait()`) before building *any* new commands, and
building was one designated function (`BuildRenderCommands`) run once per
frame from exactly one place. That is safe, but it forces "wherever the
engine decides to build commands" to be the only place a command can ever
originate -- which is fine for a fixed clear color and nothing else, and
becomes untenable the moment drawing is driven by arbitrary game logic
running throughout `IGame::OnTick`.

**The marker, and why a `while(TryDequeue)` loop could not express this
without it.** The old GDI comment on `GRenderCommands` said the quiet part:
"such a loop cannot tell frame N's commands from N-1's". A drain loop that
just pops until the queue is empty has no way to know where one frame's
worth of work ends and the next begins -- if a caller enqueues throughout
its tick rather than from one fenced point, the drain loop would happily eat
frame N+1's commands in the same pass as frame N's, because nothing
distinguishes them.

`RenderThread::EndFrame` fixes this by enqueuing a **marker** -- an ordinary
`RenderCommandHeader` with `Kind = RCK_FrameMarker` and no payload -- as the
very last thing in the queue for "this frame". The drain loop
(`RenderThread.cpp`'s `ThreadMain`) processes commands in order until it
hits a marker, at which point it stops and returns to waiting on the kick
semaphore; anything enqueued *after* `EndFrame()` was called is behind the
marker and untouched until the *next* kick. This is what lets "a game may
enqueue at any point during OnTick" and "the render thread only ever sees
one frame's worth of work per kick" both be true: the marker is the boundary
a bare `while(TryDequeue)` loop had no way to express.

**The bug this shipped with, and how it was caught.** The first
implementation of the drain loop ran the marker's own handling (swap
buffers, bump `FramesRendered`, signal `FrameDone`) but did not `break` out
of the inner `while(TryDequeue)` loop afterward -- so a command enqueued
*after* `EndFrame()` could be dequeued and executed in the very same
`TryWaitFor` wakeup, before the waiting game thread's `WaitForFrame()` call
had even returned. `RenderCoreSmokeTest`'s markers test caught this
immediately: it asserted that a command enqueued after `EndFrame()` had
**not yet run** the instant `WaitForFrame()` returned, and that assertion
failed on the first run. The fix made `RunDrainStep` report whether it had
just processed a marker, and the drain loop's inner loop `break`s on that
signal -- kicks and markers stay 1:1 (`EndFrame`/`Flush` both `Signal()`
exactly once per marker enqueued), so the next `TryWaitFor` wakeup is
exactly what resumes draining whatever sits behind the marker just reached.

**`Flush()` is the same marker mechanism without presenting.** A caller that
needs "everything queued so far has executed" without needing "and this
counts as a rendered frame" (a test, `Renderer::Shutdown`'s drain) enqueues a
`RCK_FlushMarker` instead, which the drain loop treats identically except it
signals `FlushDone` rather than swapping buffers or incrementing
`FramesRendered`.

---

## Why a command's captures must be trivial

`RenderCommandEnqueuer::operator()` `static_assert`s that a lambda's closure
type is trivially copyable and trivially destructible -- the same rule
`CoreJob.h`'s `JobSystem::Dispatch` states for a job payload, for the same
two reasons:

1. **The payload is never destructed**, only freed. A command is allocated
   with `EngineCore::Memory::Allocate` (the raw form -- see below for why),
   the lambda is placement-constructed into it, and the drain loop's
   `RunDrainStep` calls `Memory::Free` directly with no destructor call in
   between. A capture that owns a resource (a `std::string`, a `TUnique`)
   would leak every single enqueue.
2. **A captured `TRef` would be refcount traffic off the collector's
   thread**, with nothing to catch it. `CoreGarbageCollector.h`'s own
   contract is that no other thread may construct, destroy, copy or assign a
   `TRef` while a collection is running -- allowing a `TRef` into a render
   command's capture list would put exactly that kind of traffic on the
   render thread, invisible to whatever locking discipline the collector
   might someday grow.

**The bug that shaped the allocation path.** The first version of
`RenderCommandEnqueuer::operator()` allocated via
`EngineCore::Memory::Alloc<TRenderCommand<T>>()` -- the typed convenience
wrapper that default-constructs the object in place. That fails to compile
the moment a caller's lambda captures anything by reference: a closure type
with a reference member has no default constructor at all, which C++
propagates outward and deletes `TRenderCommand<T>`'s own implicit default
constructor along with it. `RenderCoreSmokeTest`'s very first thread-lifecycle
test (`[&SeenThreadID]`) hit this immediately, with a compiler error pointing
at `CoreMemory.h` rather than at the actual cause. The fix uses the raw form,
`EngineCore::Memory::Allocate`, which hands back **uninitialised** storage
exactly the way `Memory::AllocIn`'s own implementation does before its
placement-new -- the header fields and the lambda are then each
placement-constructed exactly once, in the only valid order: the
`RenderCommandHeader` base first, then `TLambda`'s own copy/move constructor
over the caller's lambda. The general lesson, worth restating because it is
easy to reach for the more convenient wrapper without thinking about it: a
typed `Alloc<T>()` is only safe when `T` is unconditionally default- or
otherwise-constructible with the arguments you intend to pass; a type
erasure boundary like this one, where the caller's real type is a lambda
closure the API author never sees, needs the raw allocation form instead.

**A command enqueued from inside another already-executing command runs
inline.** `IsInRenderThread()` inside `RenderCommandEnqueuer::operator()`
is what makes this possible: if the caller is already on the render thread
(true precisely when a command's own lambda body enqueues another command),
the new lambda runs immediately, synchronously, and never touches the queue
at all. This preserves the exact behaviour GDI's own `GRenderCommands` queue
documented ("commands enqueued from inside a command's Execute() run in the
same frame") without the drain loop needing any special case to recognise
it -- from the drain loop's point of view, those inline commands simply
never existed.

---

## Extraction -- why it sends whole components, with no dirty tracking

`WorldBridge::ExtractAndRender` walks every registered proxy type's
`ComponentPool` once per frame and copies, per live component, `{the
`RenderProxyHandle`, a `RenderTransform` read from the sibling
`TransformComponent`, the component's own raw bytes}` into one
`Memory::Allocate`d blob, then enqueues exactly one command per type that
walks the blob calling `SetTransform` + `ApplyComponent` on each proxy.

**Why not dirty flags.** GameCore's own `World::RefreshSpatialIndex` makes
the identical call for the identical reason, stated in GameCore's own
DESIGN.md: "a Transform is public POD with no setter to hook, and comparing
against the fat box is cheaper than the bookkeeping would be." The same
argument applies here, with less room to optimise around it: a
`TransformComponent`'s `Position`/`Rotation`/`Scale` fields are public and
mutated directly by any system that wants to (Sandbox's own `OnTick` does
exactly this to `Marker`), so there is no write to intercept. Extraction
would need either a version counter bumped on every write (which needs
either non-public fields with setters, contradicting GameCore's own
POD-component philosophy, or a comparison against the previous frame's
value, which costs as much as just resending it) or accept sending
everything every frame. The second option is what this module does, and it
is cheap: a record is `sizeof(RenderProxyHandle) + sizeof(RenderTransform) +
sizeof(TComponent)` bytes, rounded up to the component's own alignment, and
the whole walk is one linear pass over a `ComponentPool`'s dense array --
exactly the access pattern `ComponentPool` is built for.

**Why every proxy type gets parameter updates for free.** Because the whole
component's bytes are in the record, not just the transform, a proxy type's
`ApplyComponent` override is the *entire* extraction contract a proxy author
has to implement -- there is no separate "did the width change" path to
wire up. `PrimitiveProxy::ApplyComponent` copies `Width`/`Height`/`Tint`/
`bVisible` unconditionally, every frame; the cost of that unconditional copy
is a handful of bytes, not worth avoiding.

**The upgrade path, if a profile ever shows the walk.** The same one
GameCore's own DESIGN.md names for its refit pass: a per-component version
byte, bumped by whatever write path exists once one is added, checked before
building a record for that component. Nothing about the current design
forecloses that -- it is purely an optimisation to `WorldBridge`'s own
extraction loop, invisible to `RenderProxy::ApplyComponent`'s contract.

**One blob per type, freed by the command that consumes it.** The extraction
command captures `{void* BlobPtr, size_t BlobStride, size_t BlobCount}` --
three trivially copyable values, satisfying `RenderCommand.h`'s own capture
rule -- walks the blob, and calls `Memory::Free(BlobPtr)` once at the end.
Every record inside is trivially copyable and trivially destructible (a
`RenderProxyHandle`, a `RenderTransform`, and a POD component), so `Free`
alone is correct with no destructor to run -- the same reasoning
`RenderCommand.h`'s own header comment states for a command's payload.

---

## Proxy lifecycle -- reading and writing a handle at a fixed offset

`RENDERCORE_PROXY_BODY()` places a `RenderProxyHandle` as the **second**
member of a render component, immediately after `GAMECORE_COMPONENT_BODY()`'s
own `ComponentOwner`. `WorldBridge`'s lifecycle handlers reach it with
`reinterpret_cast<RenderProxyHandle*>(Bytes + sizeof(ComponentOwner))` --
exactly the trick `ComponentOwner` itself is built on (`World::AddComponentRaw`
writes it at offset 0 of every component "blindly", without knowing the
component's real type). `RENDERCORE_DEFINE_PROXY_COMPONENT`'s own
`static_assert(offsetof(TypeName, Proxy) == sizeof(ComponentOwner))` turns a
struct that gets the member order wrong into a compile error rather than a
silent wrong-offset write -- the identical discipline
`GAMECORE_DEFINE_COMPONENT`'s own `offsetof(TypeName, Owner) == 0` assert
applies one layer down.

**Why `OnComponentAdded`'s handler can call `GetComponentRaw` safely.**
`World::AddComponentRaw` (`World.cpp`) writes the `ComponentOwner` header
immediately after copying the caller's init bytes, and *then* broadcasts
`OnComponentAdded` -- so by the time `WorldBridge::OnComponentAdded` runs,
`GetComponentRaw` returns a fully-formed component with a valid owner
header already in place. The proxy handle write happens after the factory
constructs the proxy, directly into those same bytes.

**Why `OnComponentRemoved`'s handler can still read the handle.**
`World::RemoveComponentRaw`, `TickPhase`'s deferred-removal flush, and
`World::Impl::DestroyEntityImmediate` all broadcast `OnComponentRemoved`
**before** the owning `ComponentPool::Remove` actually erases the bytes
(`World.cpp`, several call sites) -- so the handle written there by
`OnComponentAdded` is still readable when `WorldBridge::OnComponentRemoved`
runs, letting it enqueue `RemoveProxy` against the right pointer before the
bytes it lives in are swap-removed out from under it.

**A proxy is a promise to remove**, restated from `Delegate.h`'s "a binding
is a promise to unbind": `RENDERCORE_DEFINE_PROXY_COMPONENT`'s generated
factory allocates through `EngineCore::Memory::Alloc`, which tags the
allocation with the **calling image's** `GImageID` -- the same trap
`CoreMemory.h`'s own `Allocate` documents (`GImageID` read in the wrong
place always resolves to `IMAGE_Host`). A proxy therefore carries its
defining module's identity, and `Memory::OnImageUnloading` would destroy it
out from under the render thread the instant that image unloads if nothing
had already removed it. `RegisterProxyType`/`UnregisterProxyType` are the
seam a module manages that promise through; `UnregisterProxyType` itself
only clears the factory table slot (it has no reach into whichever
`RenderScene` a `Renderer` happens to own, and there may be more than one
bound `World` or none) -- the actual sweep of live proxies is what removing
every component of that type from a bound `World` already does, through the
ordinary `OnComponentRemoved` path.

---

## Culling -- why an empty bounds means "no basis to cull", on either side

`RenderScene::Render` decides whether to draw a proxy by comparing its
`WorldBounds` against the current view's `Bounds` (`GameCore::AABB::Overlaps`).
Both sides of that comparison can be **empty** -- `GameCore::AABB`'s own
default-constructed state (`Min = FLT_MAX`, `Max = -FLT_MAX`) -- and
`AABB::Overlaps` returns `false` the instant *either* box is empty. That is
the right default for "no view has been set yet" (a hand-built scene in a
test, before `RenderView::MakeOrthographic` has ever run): a caller with no
real view still wants everything drawn, not nothing.

It is exactly the **wrong** default for a proxy's own bounds, and this
module shipped that bug once. `RenderProxy`'s default `LocalBounds`/
`WorldBounds` are the same empty `AABB()` state -- a proxy that never calls
`SetLocalBounds` (which `PrimitiveProxy::ApplyComponent` always does, but a
game's own proxy type is not required to) has an empty `WorldBounds`, and
under `AABB::Overlaps`'s own semantics that reads as "never overlaps
anything" -- such a proxy would be permanently culled against any real view,
regardless of `IsVisible()`, with no error and no obvious symptom beyond "my
proxy's `Draw()` never runs." `RenderCoreSmokeTest`'s test-defined proxy type
(step 4's `TestProxy`, which deliberately declares no bounds to prove
exactly this path works) caught it on the very first run.

The fix treats an empty *proxy* bounds the same permissive way an empty
*view* bounds already was: no basis to cull, not "never overlaps" --

```cpp
const bool bViewHasBounds  = !View.Bounds.IsEmpty();
const bool bProxyHasBounds = !Proxy->GetWorldBounds().IsEmpty();
const bool bVisible = !bViewHasBounds || !bProxyHasBounds
                    || Proxy->GetWorldBounds().Overlaps(View.Bounds);
```

explicitly overriding `AABB::Overlaps`'s own empty-handling rather than
calling it directly on an unconditional basis. A proxy type that wants real
culling calls `SetLocalBounds`; one that does not (a full-screen effect, a
debug overlay, `TestProxy` itself) draws unconditionally, which is the
correct default for "undeclared", not "everywhere and nowhere".

---

## RenderContext -- why DrawQuad reuses one mesh and rebuilds one GDIView

`RenderContext::DrawQuad` owns exactly one `GDIView` (and the `GDICamera` it
carries) and one `GDIViewObject` (and the unit quad mesh
`GDIViewObject`'s own constructor allocates), constructed lazily on first
use and reused for every draw call, of every proxy, for the life of the
render thread. The `GDICamera`'s `Projection`/`ViewMat` are overwritten from
the current `RenderView` on every `DrawQuad` call rather than recomputed
from a width/height the way `GDICamera::UpdateDimensions` normally would --
`RenderView` already carries the finished matrices
(`RenderView::MakeOrthographic` builds them once, on the game thread, from
`GDICamera::UpdateDimensions`'s own formula), so there is nothing left for a
second `GDICamera` recomputation to add.

**Why one mesh serves every primitive size.** `GDICreateQuadMesh(Width,
Height)` allocates a *new* mesh sized for that call's dimensions; calling it
once per `DrawQuad` invocation would allocate (and leak, since nothing frees
the old one) a mesh per draw call. `GDIViewObject`'s own constructor already
creates a half-unit quad through the vertex-data overload
(`GDICreateQuadMesh(HalfQuadVertData, QuadTexCoordData, ...)`, see
`GDIView.cpp`); `DrawQuad` scales that single mesh per call through
`GDITransform::Scale`, exactly the mechanism `GDITransform::GetModel()`
already composes into its own model matrix. One allocation for the life of
the render thread, however many proxies draw through it.

**Why the tint is a per-draw uniform write, not a per-proxy shader.**
`Basic_PS::InitUniformBindings` (`TestbedShaders.h`) registers `"TestColor"`
once, as a shared uniform on the one `Basic_PS` instance every backend
already registers in `InitializeGDI`. `DrawQuad` overwrites that uniform,
per call, with the current proxy's tint -- correct because GDI's draw calls
are strictly sequential on one thread (the render thread), so there is no
concurrent access to race against, only a "did I remember to set it before
this particular draw" ordering question, which an unconditional write before
every `GDIDrawObject` call answers by construction.

---

## What arrived deliberately incomplete

- **No Lua bindings.** `ScriptEngine`'s proxy metatables (`ScriptBindings.cpp`)
  are private to GameCore; extending them for `PrimitiveComponent` needs an
  extension point that does not exist yet. A game that wants this from a
  script currently binds it itself.
- **No frame-arena allocator for commands or extraction blobs.** Both
  currently go through `EngineCore::Memory::Allocate`/`Free` -- correct, and
  consistent with `CoreQueue.h`'s own stated stance ("the contention here is
  a few items per frame ... a lock-free ring would be more code, more
  subtle, and no faster at this volume"), but a profile that shows
  allocation pressure has a clear upgrade path: `RenderCommand.h`'s header
  comment and the extraction section above both name it explicitly, so the
  next person to reach for it does not have to rediscover the tradeoff.
- **`UnregisterProxyType` does not sweep live proxies itself** -- see the
  Proxy lifecycle section above for why that is `WorldBridge`'s job, driven
  through the ordinary `OnComponentRemoved` path, rather than something this
  registry can do on its own.
