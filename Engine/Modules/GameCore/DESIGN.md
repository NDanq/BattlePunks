# GameCore design notes

The long-form "why" behind decisions in this module -- comparisons to
alternatives, cross-references to other files' reasoning, bugs found and
fixed during development, and worked examples. `README.md` is the short,
usage-facing doc; this is where its claims are justified in detail. Organized
by source file, in roughly the order a reader would encounter them.

---

## Delegate.h / Delegate.cpp

**Why not `std::function`.** A `std::function` captured by value allocates in
the image that built the lambda and is destroyed by whatever runs the
multicast's destructor -- the exact cross-heap free `CoreConfig.h`'s CONFIG
HAZARDS section documents for the reason `ConfigValue` is a fixed union
rather than a `std::string`. `GameWindowDelegates.h` makes the same call for
window callbacks and `CoreLog.h`'s `LogHandler` for the same reason again: a
bare function pointer plus a `void*` has no owner and no allocation, so it has
exactly one way to dangle -- its image unloading -- and that case already has
a name (the reload hazard below).

**Why the storage is erased (`DelegateList`) rather than living in the
template.** This is `CoreMemory.h`'s "templates compute, EngineCore
allocates" argument, applied to delegates instead of allocations: a template
instantiates in every image that calls it, so if the storage lived in
`TMulticastDelegate<TArgs...>` itself, `NimbleEngine.dll` and a hot module
would each get their own private list and nobody would see the other's
bindings. `DelegateList` is the erased, `GAMECORE_API`, out-of-line
implementation; `TMulticastDelegate` and `TDelegate` are header-only and do
nothing but cast a typed function pointer to `DelegateList::RawFn` and back.

**The reload hazard.** A function pointer into a hot module left in a
`DelegateList` after that module unloads is `CoreModule.h`'s DANGLING
CALLBACKS hazard, verbatim: the next `Broadcast()` jumps into unmapped
memory, and the crash lands in whatever fired the broadcast, nowhere near the
module that forgot to unbind. `RemoveAll(UserData)` exists so "whatever
`OnStartup` bound, `OnShutdown` unbinds" is one call; `TScopedBinding` exists
so a C++ owner cannot forget at all. `World` logs a warning for any of its
own built-in delegates still holding entries at destruction -- the same
"cannot enforce it, but can detect and warn" posture `GameWindowSystem::Shutdown`
takes for `GameWindow`'s own delegate table.

**Broadcast re-entrancy, and the bug that shaped it.** Add/Remove called from
inside a callback that `Broadcast` is currently running are deferred to when
that `Broadcast` finishes -- a callback that unbinds itself must not shift
the indices a broadcast in progress is walking. Broadcasting the *same*
`DelegateList` again from inside one of its own callbacks is refused outright
(a different `TMulticastDelegate` may still be broadcast from inside a
callback with no issue; this is specifically "this list, again, while its own
walk is still on the stack").

The refusal is an explicit `if(!BeginBroadcast()) return;`, not a
`CORE_CHECK`. An earlier version called `CORE_CHECK` and then fell straight
through into starting the broadcast anyway -- `CORE_CHECK` only logs and
returns when a handler is installed (`CoreLog.h`'s documented Fatal
semantics: "`LogDispatch` does exactly what the handler does and then
RETURNS, even for Fatal"), so the "refusal" never refused anything. A
callback that broadcast its own delegate recursed until the stack
overflowed, logging one warning per frame on the way down. This was caught by
`GameCoreSmokeTest`'s re-entrancy case and fixed by making `BeginBroadcast`
return `bool` and having every caller (`TMulticastDelegate::Broadcast`,
`EventBus::Emit`) branch on it. The general lesson: `CORE_CHECK` is a
diagnostic, never a gate -- anywhere code must not proceed past a failed
condition needs an explicit `if`/`return`, with `CORE_CHECK` (or a plain log
call) only supplying the message.

**`DelegateList::RawFn`'s cast safety.** `RawFn` is a function-pointer-shaped
placeholder with no real signature. Casting a function pointer through an
unrelated function pointer type and back is well-defined in C++ (unlike doing
the same through `void*`, which is only guaranteed for object pointers), but
calling it through the *wrong* type is not -- so the cast back to `FnType`
happens in exactly one place, `TMulticastDelegate::Broadcast`, the only code
that has ever seen the real type.

**Broadcast protocol** `TMulticastDelegate::Broadcast` drives:

```
if(BeginBroadcast()) {                  // false means refused -- see above
    for(size_t I = 0; ; ++I) {
        RawFn Fn; void* User;
        if(!GetEntry(I, Fn, User)) break;   // I reached the count captured at Begin
        if(Fn) CallThroughRealType(Fn, User, args...);  // Fn is null for an index removed mid-walk
    }
    EndBroadcast();                     // applies deferred adds/removes, compacts tombstones
}
```

`GetEntry` never returns a moved or resized entry mid-walk: the backing array
does not reallocate while `IsBroadcasting()` is true, which is exactly what
`BeginBroadcast` latches by capturing the current count and deferring
structural changes.

**`TDelegate::Execute` vs `ExecuteIfBound`.** `Execute` `CORE_CHECK`s
`IsBound()` first -- calling an unbound `TDelegate` is a programming error,
not a runtime condition, the same distinction `CoreConfig.h` draws between
`GetInt()` on a bool var (a `CORE_CHECK`) and a missing `.ini` file (a
warning). `ExecuteIfBound` is for the case where "nobody is listening" is
expected.

**`TMulticastDelegate::List` is `mutable`.** So `Broadcast()` can stay a
`const` method -- broadcasting does not conceptually mutate what the
delegate *means*, only its bookkeeping, matching `TRef::GetStrongCount()`'s
constness in `CorePointer.h`.

**`DelegateList`'s storage, in full.** A flat vector of slots, each carrying
the serial that makes a `DelegateHandle` safe to hold past the slot's reuse.
`BeginBroadcast()` increments `BroadcastDepth` and records the current slot
count as `BroadcastCount`. `GetEntry()` only ever looks at indices below that
count, so an `Add()` during the walk (which appends past `BroadcastCount`) is
invisible to *this* broadcast and picked up by the next one -- exactly
`Broadcast()`'s "the addition is not called this broadcast". A `Remove()`
during the walk does not erase the slot (that would shift every later index
mid-walk); it clears the slot's `Fn` to null and queues the index in
`PendingRemoves`. `GetEntry()` returns true with `Fn == nullptr` for such a
slot, which `Broadcast()`'s `if(Fn)` skips. `EndBroadcast()` decrements
`BroadcastDepth` and, once it reaches zero, actually erases every queued slot
-- reusing entries in `PendingRemoves` also means it does not matter that
erasing shifts every index after it, because nothing is walking the array
anymore.

`BroadcastDepth > 0` on entry to `BeginBroadcast()` means this same list is
already mid-walk somewhere up the call stack. Nesting is refused rather than
allowed, because nesting would mean `GetEntry`'s "indices below
`BroadcastCount`" contract now has two different `BroadcastCount`s in flight
against one `PendingRemoves` queue, and the inner `EndBroadcast` erasing
while the outer walk is still reading indices is exactly the corruption the
whole design exists to avoid.

`Slot::Serial` starts at 1 (never 0) so a default-constructed
`DelegateHandle{0xFFFFFFFF, 0}` never validates, and is bumped on every
*reuse* of a slot, never on first use, so a handle from a slot's first
occupant and one from its second work out to distinct serials with no
special-casing for "never used before."

`GetEntry`'s `bInUse` is deliberately not checked -- a slot removed
mid-broadcast still returns true with `Fn == nullptr` (its natural state
after `Remove()`), which is what lets `Broadcast()`'s `if(Fn)` skip it
without `GetEntry` itself knowing the caller's loop shape. A slot that was
never in use within `[0, BroadcastCount)` cannot occur, since every index
below `BroadcastCount` was allocated before this broadcast began.

---

## EventBus.h / EventBus.cpp

**Why this exists alongside `TMulticastDelegate`.** A `TMulticastDelegate<TArgs...>`
is the right primitive when both sides of a binding can `#include` the same
declaration -- no lookup, no packing, a direct call through a typed function
pointer. `EventBus` is for callers that cannot do that: a Lua script (which
has no C++ type to name), a hot module (which should not need to link
against the specific header that declares `"PlayerScored"`), or two systems
deliberately kept from knowing about each other. The cost of that decoupling
is a name lookup on Subscribe and an argument pack on Emit -- worth paying at
those boundaries, not on a hot path that already has both ends in the same
translation unit. `World` owns exactly one `EventBus` and mirrors its own
four built-in typed delegates onto it under fixed names, so Lua observes
engine events through this one mechanism rather than a second, script-specific
path.

**`EventArg` is POD and fixed-size,** crossing images and the Lua boundary
the way `ConfigValue` does in `CoreConfig.h`, for the same reason: a
`std::string` member would allocate in whichever image called `Emit` and be
read (or freed) by a subscriber in a different one. Its string form truncates
silently past 63 characters, the same call `CoreConfig.h`'s
`MakeConfigString` makes -- an event argument is a short label, not a
payload; anything long enough to truncate belongs on a component instead.
`EventArgs` caps at four slots: enough for a notification like
`"PlayerScored", 10, "Player"`; anything needing more is data, not an event,
and belongs on a component the subscriber can query by `EntityID`.

**Storage, in full.** One `DelegateList` per interned event name, held in a
growable array indexed by `EventID`. Lookup by name is a linear scan,
matching `ComponentType.h`'s registry and `CoreConfig.h`'s `ConfigVar` list:
it runs on Subscribe and on the rare first-time Emit of a name, never per
frame in a tight loop, so a linear walk over at most `EVENT_MAX` names costs
nothing worth optimizing.

---

## EntityID.h

**Why a generation and not just an index.** Indices are reused the instant
an entity is destroyed -- `CreateEntity()` pulls from a free list rather than
growing forever. Without a generation, an `EntityID` saved before a destroy
would silently address whatever new entity took that slot after it, which is
a worse failure than a null: it does not crash, it reads and writes the
wrong entity's data. `World` bumps `Generation` on every `DestroyEntity`, and
`IsAlive()`/every `GetComponent` call checks it before trusting `Index`.

POD, 8 bytes, trivially copyable -- an `EntityID` is stored by value
everywhere (inside `ComponentOwner`, inside the spatial index's leaves,
across the Lua boundary as a packed `uint64_t`) and never needs its own
lifetime management. `Pack()` puts `Index` in the low 32 bits so a bare cast
to `uint32_t` on the Lua side (a mistake, but a survivable one) still names
the right slot instead of the generation.

---

## TickPhase.h

`World::Tick` walks the four buckets in order, once per frame, and flushes
the deferred destroy/remove queue at the end of *each phase* rather than the
end of the frame -- see World.h's own phases section. That is what makes the
phase an entity sees meaningful: an entity a PrePhysics system destroys is
gone by the time Physics runs, not merely gone by next frame.

Phases are named for what they are relative to, rather than numbered, so
`World.AddSystem("Foo", TickPhase.PostPhysics, ...)` on the Lua side reads as
intent rather than an arbitrary priority. A fifth phase is one enum line
here plus one line in `ScriptBindings.cpp`'s `TickPhase` table -- nothing
else in the module enumerates phases by name.

---

## AABB.h

**`Transformed` uses `Vector4` with `w=1`, deliberately, not
`Matrix4x4::operator*(Vector3)`.** That `Vector3` overload is a *direction*
transform (`w=0` -- see its own comment in `Matrix.h`): translation cancels
out of every one of its four dot products. A corner run through it would
rotate and scale correctly and then silently land back at the matrix's own
origin. Corners are positions, so `w=1` and the `Vector4` overload, then the
`w` component is dropped -- Position/Scale/Rotation compose into an affine
matrix, so `w` comes back as 1 again, not a projective divide anything here
needs to perform. This was caught before it ever compiled, while reviewing
the first draft of this file against `Matrix.h`'s own comment.

`Transformed`'s eight-corner box is a conservative over-approximation under
rotation, not a tight rotated box -- an axis-aligned box cannot represent a
rotated one exactly, and a query structure needs a box that is never too
small. A rotating entity's world box reads larger than its silhouette, which
costs query precision, not correctness.

---

## ComponentType.h / ComponentType.cpp

**Why `StaticType()` is an out-of-line function-local static and not a
template.** A `template<typename T> ComponentTypeID GetTypeID()` would
instantiate in every image that calls it -- `NimbleEngine.dll`, `Sandbox.exe`,
a future hot module -- and `CoreMemory.h`'s structural rule applies again: a
function-local static inside a *template* is one instance per image, not one
per process, so `TransformComponent` would register itself once per image
and get a different id each time, with nothing to say so.
`GAMECORE_DEFINE_COMPONENT` expands to an ordinary out-of-line function,
defined exactly once, in exactly one `.cpp`, so there is exactly one
function-local static for that type in the whole process -- the same
reasoning `CoreConfig.h` gives for putting `ConfigVar`'s registration in
`EngineCore.dll` rather than inline in the header.

**`ComponentTypeInfo`'s layout is frozen**, read directly by every image that
includes this header -- the same rule `CoreMemory.h` states for
`AllocHeader` and `CoreConfig.h` for `ConfigVar`. This engine has no
hot-reloadable component types yet, so there is no `MODULE_ABI_REVISION` to
bump today; the note is here so that the day a component type is declared
inside a hot module, whoever adds that reads this first. `Name` is a fixed
buffer, never `std::string`, for the same reason `ConfigVar::Name` is: a
`std::string` member would allocate in whichever image registered the type.

**`ComponentOwner`, in full.** The first 16 bytes of every component: who it
belongs to. Written by `World::AddComponentRaw` immediately after copying the
caller's init bytes -- the one place this is ever set -- and read by
everything else. Composition, not inheritance: a component stays
standard-layout (no vtable, no base-class slicing question) and the erased
pool can write this header at a fixed offset without knowing the derived
type, the same way it already knows `SizeBytes` and `Alignment` without
knowing the type.

*Why a member and not a lookup.* A system iterating a `ComponentPool` has a
`T&` in hand and nothing else in scope -- no `World*`, no `EntityID` --
unless it threads both through every function signature in the module. That
friction is exactly what makes people reach for a cached raw pointer instead
of asking properly, which is the failure `CorePointer.h`'s "a raw `T*` is
fine to use, not to store" warning already exists for. Sixteen bytes bought
once, on every component, buys `Bounds.Owner.GetSibling<TransformComponent>()`
from anywhere a component is in hand.

*Why storing a `World*` here is safe*, unlike storing one anywhere else: a
component only ever exists inside one `World`'s `ComponentPool`. It cannot
outlive that `World` (the pool is destroyed with it) and the `World` cannot
be destroyed while this component is still alive to read `Owner.OwningWorld`
-- so there is no dangling case to guard against, unlike a `TWeakRef` or a
cached `IModule*`, both of which exist because their target's lifetime is
genuinely independent of the holder's.

**Why the member is named `OwningWorld`, not `World`.** A member named
`World` of type `World*` would be legal C++ (a pointer may share its
pointee type's name) but shadows the type name `World` for every later use
of the bare identifier in this class and in every inline method defined
against it -- a trap for the next person editing this struct even though it
compiles today.

**Why `GetEntity()`'s return type is qualified (`::GameCore::Entity`).** This
struct already declares a member named `Entity` (of type `EntityID`) before
this point, so an unqualified `Entity GetEntity() const;` would have the
class-scope member hide the namespace-scope type and fail to compile as
"`EntityID` is not callable" -- the same shadowing risk `OwningWorld`'s
naming sidesteps, except here it is not avoidable by renaming (`Owner.Entity`
is the name every other file in this module uses), so it is fixed by
qualification instead. `GetEntity()` is declared here but defined in
`Entity.h`, and `GetName()` is declared here but defined in `Components.h` --
both need a complete type this header cannot see without an include cycle.

**Registration lifetime.** Entries are never removed and ids are never
reused -- unlike `EntityID`'s generation-recycled slots, a component type is
assumed to exist for the life of the process. `GRegistry` is a plain array in
an anonymous namespace, constant-initialized into `.bss` before any dynamic
initializer runs -- the same `CoreConfig.h` argument for `GConfigHead`,
applied here: the first `StaticType()` call anywhere in the process (which
may itself run during another type's dynamic initialization, if a component
is ever a file-scope object) must find a valid, already-usable table. A
function-local static registry would have exactly the ordering problem
`CoreConfig.h` rejects a `std::vector` for.

`RegisterComponentType` on a name collision with mismatched size/alignment
logs loudly and keeps the first registration -- two different types fighting
over one name is a programming error, not something to resolve silently.
Hitting `COMPONENT_TYPE_MAX` returns slot 0 rather than a dangling reference
(every caller treats the return as a valid `ComponentTypeInfo&`) -- wrong
data with a loud error logged, rather than undefined behavior.

---

## ComponentPool.h / ComponentPool.cpp

**Why erased, like `DelegateList`.** `ComponentPool` deals in bytes with a
stride; `Entity::AddComponent<T>` and `Entity::GetComponent<T>` are the typed
views that cast `T*` to and from what this class hands back. A type-erased
pool means the Lua by-name path (`e:AddComponent("Transform")`) and the C++
typed path are the same code, rather than a duplicate untyped implementation
existing only for scripts.

**Classic sparse set.** `Sparse[entity.Index]` is either "not present" or a
dense slot; `Dense[slot]` is the component bytes; a parallel
`DenseEntities[slot]` array is the reverse map and carries the entity's
generation, so a stale `EntityID` (recycled index, old generation) reads back
"absent" rather than aliasing whatever now occupies that sparse slot.

**Why the dense buffer is `SystemAlignedAlloc`, not `std::vector<uint8_t>`.**
A component's alignment can be 16 (`Vector3` under `WITH_SIMD` -- see
`Math/Vector.h`'s ABI guard). `std::vector<uint8_t>` makes no alignment
promise beyond `alignof(uint8_t)`, so a Transform pool backed by one would
misalign every other element the moment the stride is not a multiple of 16.
Allocating through `SystemAlignedAlloc` at the type's alignment is the same
fix `CoreMemoryPool.h`'s `FixedBlockPool` applies for the same reason.

**Swap-and-pop, POD only.** `Remove()` memcpy's the last live element into
the hole left by the removed one -- correct only because
`GAMECORE_DEFINE_COMPONENT` already enforces trivial copyability on every
component. This is also why "a `T*` is invalidated by any Add or Remove of
that type" is not a suggestion: the object behind a held pointer may have
just been relocated eight bytes to the left.

**Growth.** The dense buffer doubles (starting at 16 elements) via a fresh
allocation and a memcpy of the live prefix, matching what
`CoreMemoryPool.h`'s `VirtualRegion`-backed pools do at a coarser
granularity. A pool never shrinks, mirroring `LinearMemoryPool`'s stance
(`Reset` is the only way back to empty) rather than paying for a resize on
every removal in a system that adds and removes at similar rates every
frame. `Sparse` is a plain `std::vector<SparseEntry>` grown to cover the
highest entity index seen -- 8 bytes per *possible* index, not per live one,
so it costs nothing until an entity count large enough to matter, at which
point `EntityID`'s own free-list reuse keeps the highest index from running
away.

**`Reorder`'s scratch buffer.** Sized like one growth step rather than
permuting in place, because an arbitrary permutation done safely with POD
memcpy needs a cycle-decomposition pass in place, and a full re-layout is
expected to be rare (see the "locality across the two structures" note in
SpatialIndex's own design notes) -- one extra allocation is the simpler
trade.

---

## Entity.h

**Why `Entity` is a thin `{World*, EntityID}` pair and not `ComponentOwner`'s
own storage.** `ComponentOwner` (in `ComponentType.h`) keeps a raw `World*`
and `EntityID` rather than an `Entity`, because `Entity.h` depends on
`World.h` (for the private component calls it forwards to) and `ComponentType.h` -- where
`ComponentOwner` lives -- is included *by* `World.h`. An `Entity` member
there would be the same include cycle `GetEntity()`'s placement already
works around; two raw fields cost nothing extra and `Entity::GetEntity()`
reconstructs the same value on demand.

**Components go through `Entity`, and only `Entity`.** `World` has no
public `AddComponent<T>(EntityID)` / `GetComponent<T>(EntityID)` family, typed
or erased: its `*ComponentRaw` functions are private, with `Entity` a friend.
So there is one spelling for "the component on this entity" everywhere --
`E.GetComponent<T>()` in C++, `e:GetComponent("T")` in Lua -- and the `*ByType`
methods on `Entity` (`GetComponentByType(ComponentTypeID)` and siblings) serve the
callers that only have an id (Lua bindings, the serializer, RenderCore's
bridge). Code holding a bare `EntityID` writes `World.GetEntity(Id)` or
`Entity(&World, Id)` first. `ComponentOwner::GetSibling<T>` is defined in
`Entity.h` for the same reason: it goes through `GetEntity()`.

**`World::CreateEntity` returns an `Entity`, not an `EntityID`,** so a
component attaches straight to what was just created --
`World.CreateEntity("Marker").AddComponent(Transform)` -- with no
`GetEntity(id)` step in between. `Entity` converts implicitly to `EntityID`, so
code that stores or passes the id keeps compiling unchanged. The conversion is
one-way on purpose: going from an `EntityID` back to an `Entity` needs a
`World`, which is what `World::GetEntity` is for. Storing the id rather than
the handle is still the right choice for anything long-lived (a level's cached
entities, a system's `void*` context). The handle's `World*` is only valid for
that World's lifetime; the id says so explicitly by needing a World to be used.
`World.h` forward-declares `Entity` (the include cycle above), so a caller
needs `Entity.h` -- or `Components.h`, which includes it -- to use the result.

`Entity` is not `GAMECORE_API` -- header-only, no out-of-line members,
instantiates in the consumer, the same rule `GameCoreAPI.h` states for
`EntityID.h`. `World.h` is what is actually exported; this is a thin wrapper
over it, adding no state and no behavior of its own -- every method forwards
straight to `World`.

**`ComponentOwner::GetEntity()`'s shadowing trap.** `ComponentOwner`'s own
member is named `Entity`, of type `EntityID`. Inside `GetEntity()`'s body,
the bare identifier `Entity` resolves to *that member*, not the class
`GameCore::Entity` being constructed. Both uses in the definition are
qualified (`::GameCore::Entity` for the type, `this->Entity` for the member)
for exactly that reason -- an unqualified `Entity(OwningWorld, Entity)` would
be the same shadowing trap `ComponentOwner`'s `OwningWorld` naming avoids,
except here it is not avoidable by renaming (`Owner.Entity` is the name
every other file in this module uses), so it is fixed by qualification
instead. This was caught and fixed before the module first compiled.

---

## Components.h / Components.cpp

**Every component here is `GAMECORE_API`, even the plain-looking ones.**
Every component using `GAMECORE_COMPONENT_BODY()` gets an out-of-line
`StaticType()`, always -- that is the whole registration mechanism (see
`ComponentType.h`). `GameCoreAPI.h`'s "do not annotate a header-only type"
rule is about a type with *nothing* out-of-line; `StaticType()` means that
exception never actually applies to a component struct, whatever its data
members look like. Without the annotation, MSVC's dllexport model does not
export `StaticType()` from `GameCore.dll`, and every other translation unit
that names e.g. `NameComponent::StaticType()` fails to link against it --
which is exactly what happened during development: `TransformComponent`
happened to need `GAMECORE_API` anyway for `GetLocalMatrix()`, so it never
exposed the gap until `NameComponent` and `BoundsComponent` were linked
against from `SmokeMain.cpp` and failed with unresolved externals.

**`TransformComponent` is 64 bytes.** With the 16-byte `ComponentOwner`
header plus three `Vector3` at 16 bytes each under `WITH_SIMD` (per
`Math/Vector.h`'s ABI guard), this is exactly one cache line.

**`GetLocalMatrix()`'s rotation composition.** `Matrix4x4::InitRotation(angle,
axis)` sets a matrix to *one* axis-angle rotation and returns `*this` by
value -- it does not compose an existing matrix with a new rotation. So three
Euler angles need three separate matrices multiplied together, not three
calls into the same one. The composition order is `Translation * (Rz * Ry *
Rx) * Scale` -- rotate about X first, then Y, then Z -- matching the common
"yaw-pitch-roll composed outermost-last" convention. This is an
arbitrary-but-stated choice; a game that wants a different order rolls its
own component rather than this one silently meaning something else than what
its member name suggests.

**`BoundsComponent::ProxyID`** starts at -1 until the first refit inserts it
-- a Bounds added mid-frame may not have its Transform yet (see
SpatialIndex's design notes on the refit pass). **`bStatic`** means never
re-fit after insertion, for level geometry that never moves; the refit pass
skips a static Bounds entirely after its first Insert.

**`ScriptComponent::InstanceRef`** defaults to `-2`, spelled as a literal
matching `LUA_NOREF`, rather than pulling `lua.h` into a public header (see
`GameCoreInternal.h`'s note on why `lua.h` never appears in `Public/`).
**`bCullOutsideActiveRegion`** is only meaningful if the entity also has a
`BoundsComponent` -- an entity with no Bounds is never in the tree and so is
never "outside" any region; the per-phase tick ticks it regardless of this
flag.

---

## World.h / World.cpp

**Ownership and threading.** `NimbleEngine::Application` owns exactly one
`World` and ticks it once per frame, after `IGame::OnTick` -- so a game acts
first (it may spawn something this frame), then the World's own phases run.
A game reaches entities through `Application::GetWorld()`, never by
constructing its own `World`. No locks anywhere in this class, matching
`GameWindowSystem`'s stated thread contract: every call happens on the
thread that runs `IGame::OnTick`, which is also the thread that pumps
`GameWindow`.

**`DestroyEntity` and `RemoveComponent<T>` are deferred to the end of the
current phase** when called from inside `Tick()`/`TickPhase()`; outside a
tick, both are immediate. `DestroyEntity` broadcasts `OnEntityDestroyed`
*before* removing a single component, so a subscriber can still read what
the entity had; only after that does every pool's `Remove()` run (firing
`ScriptComponent`'s removal observer among them), the generation bump, and
the free-list recycle.

**`Each<T>`'s one rule:** the callback must not Add or Remove a `T` on *any*
entity while it runs -- doing so would relocate or shrink the very buffer
being walked. Adding or removing a *different* component type, or
destroying the current entity (which defers to end-of-phase during a tick),
is fine.

**Systems are function pointer + `void*`, not a virtual interface** --
`GameWindowDelegates.h`'s reasoning: no allocation, no owner, no vtable into
a possibly-unloaded image. Registration order within a phase is execution
order. `TickPhase()` is public (not just an implementation detail of `Tick()`)
so a host can interleave its own work between phases without being a
registered system itself -- a future physics module stepping between
`TP_PrePhysics` and `TP_PostPhysics`, say. `Tick()` is simply four calls to
`TickPhase()` in a row plus the spatial refit.

**`EachInBounds<T>`** is `QueryBounds` followed by `GetComponent<T>` on each
hit -- only entities that have `T` are passed to the callback. The extra
fetch per hit is a sparse-set lookup (random access), which is the cost of
asking by region instead of walking a pool in order -- see SpatialIndex's
design notes for when this is and is not the right tool.

**`RefreshSpatialIndex`** recomputes every non-static `BoundsComponent`'s
world box from its owner's Transform (identity if none) and pushes it into
the `SpatialIndex` via `Move()`. `Tick()` calls this once, at the end of
`TP_Physics`; a host may call it again mid-frame (after moving something
outside the normal tick) at the cost of walking the Bounds pool again.

**The active region** is a shared hint for "the region gameplay currently
cares about" -- the common "around the player" case. `World` enforces
nothing from it itself; `ScriptSystem`'s per-phase tick honours it for a
script class that opts in with `CullOutsideActiveRegion`, skipping `OnTick`
for an entity whose world box misses the region. A `World` with no active
region set ticks everything, which is also what `ClearActiveRegion()`
restores.

**`OnEntityCreated` fires before or after the `NameComponent` is added,**
and this mattered enough to fix once: an earlier version of
`CreateEntity(name)` called the plain `CreateEntity()` (which broadcasts
immediately) and added the name afterward, so a subscriber to
`OnEntityCreated` -- including the `Events.On("EntityCreated")` bridge -- saw
an entity with no name yet, even though one had been requested. Caught by
watching Sandbox's own log print an empty name for an entity it had
explicitly created with one. Fixed by splitting slot allocation
(`Impl::AllocateEntitySlot`, no broadcast) out of both `CreateEntity`
overloads, so `CreateEntity(name)` can add the `NameComponent` before
broadcasting.

**Why `AllocateEntitySlot` and `DestroyEntityImmediate` are members of
`Impl`, not free functions taking `World::Impl&`.** `Impl` is a *private*
nested type of `World` (the PIMPL idiom every `.cpp` in this module uses),
and a free function outside `World`'s own member bodies has no access to
name that type as a parameter -- only `World`'s own members, and `Impl`'s own
members, can. Both functions touch nothing outside `Impl`'s own fields, so
they belong there rather than needing a friend declaration on `World` for
something that does not actually need `World` itself. This was a compile
error caught immediately (`cannot access private struct`) the first time
each was written as a free function.

**Why `DestroyEntityImmediate` bypasses `RemoveComponentRaw`'s own deferral
check** rather than calling it. `DestroyEntityImmediate` only ever runs from
inside `TickPhase()`'s own flush, where `bTicking` is still true (restored
only after both flush loops finish, so anything the flush itself triggers
still sees a consistent "mid-tick" state elsewhere). Calling the public
`RemoveComponentRaw` from there would see `bTicking == true` and defer
*again*, into a queue that was just cleared and will not be looked at again
this phase -- the removal would never actually happen, the entity's slot
would be recycled while its "removed" components still sit in their pools
under a generation about to be reused, and the next entity born into that
slot would silently inherit them. `DestroyEntityImmediate` is the one safe
place to mutate pools directly: it never runs from inside a system's own
iteration, only from the phase flush that happens after every system in that
phase has already returned.

**The phase flush swaps its queues out rather than iterating them in
place.** A `ComponentRemoved` subscriber (or a pool's removal observer)
called from inside the flush loop may itself request another deferred
removal or destroy -- `bTicking` is still true, so the request still defers
rather than mutating immediately. That new entry needs to land in a *fresh*
queue, not the one being iterated (which would either invalidate the
iterator or execute the same-phase re-entrant request too eagerly), so
`TickPhase` swaps `PendingComponentRemovals`/`PendingDestroys` into local
vectors before looping, leaving a clean member queue for anything that
fires during the walk. Anything added during this flush is picked up one
phase later, which is an acceptable, documented delay for a rare re-entrant
case, not a silent drop.

**Component removal and the spatial index.** `World::GetPool` wires a
removal observer onto the `BoundsComponent` pool the first time it is
created, pulling the entity's leaf out of the `SpatialIndex` the moment the
component is removed -- whether because the whole entity was destroyed or
because a caller removed just the Bounds off a still-live entity. This was
a gap caught by the smoke test's "a destroyed entity is never returned by a
query" case: without it, a leaf outlives the component that created it, and
every later query keeps returning an entity that no longer has the Bounds it
was found by.

**`RefreshSpatialIndex`'s `bStatic` handling, and the bug it fixed.** The
refit pass must still give a static Bounds its *first* leaf -- an earlier
version skipped `bStatic` entities unconditionally, including their first
insert, so a static entity's `ProxyID` stayed -1 forever and it was
invisible to every spatial query. `bStatic`'s own doc comment ("never re-fit
*after* insertion") means what it says: only *after*. The fix checks
`Bounds.bStatic && Bounds.ProxyID >= 0` before skipping, so a fresh static
Bounds still gets inserted once.

---

## SpatialIndex.h / SpatialIndex.cpp

**What is in the tree and what is not.** `World` keeps three things about an
entity in three places, each chosen for how it is accessed: identity in the
entity free-list, component data in the contiguous `ComponentPool`s (iterated
linearly, by a system that wants every instance), and position in space here
(searched by region, by a system that wants only what is nearby). A leaf is
an `EntityID` and a fattened world box -- no component bytes -- so
Insert/Remove/Move never touch a pool, and a system iterating a pool never
pays for the tree existing.

**Why fat boxes.** A leaf's stored box is its true box grown by
`GetFatMargin()` on every axis. `Move()` is then a cheap common case: if the
new true box still fits inside the stored fat box, nothing in the tree
changes -- no reinsertion, no rebalance, just a compare. Only an escape past
the margin costs a real remove+insert. For an entity that jitters or moves
slowly relative to its own size (most of them, most frames), this turns
"update every entity's tree position" into "touch the handful that actually
crossed their margin."

**Why erased and exported,** same reason `DelegateList` and `ComponentPool`
are: the node array, free list and root all live in `GameCore.dll`, so
`World`'s inline query helpers (`EachInBounds<T>`) do nothing but forward
into here and then look results up in a `ComponentPool`.

**Node layout.** One array, referenced by `int32_t` index -- never a pointer
-- with `NULL_NODE` (-1) meaning "no node." A proxy id *is* a node index;
there is no separate handle table, unlike `DelegateHandle` or
`ComponentPool`'s sparse array, because a tree node's identity does not need
to survive being "moved" the way a delegate slot or a dense component slot
does: `Move()` mutates a leaf node in place when it fits its fat box, and
only reinserts (which frees the old node and allocates a new one, same
index-stability contract as any other node) when it escapes. The proxy id a
caller holds is therefore stable across every call -- even a reinsertion
inside `Move()` keeps the same node index, because `Move()` removes and
reinserts the leaf into the same slot rather than freeing it. Pointer-free
so the array can grow by reallocation without invalidating anything a caller
holds.

**This is Box2D's `b2DynamicTree` in structure** -- a node array with an
intrusive free list, insertion by the surface-area heuristic, rebalancing by
the same two-rotation scheme -- adapted to hold an `EntityID` per leaf
instead of a fixture pointer, and grown through `SystemAlignedAlloc` instead
of a bespoke allocator, so it participates in the same allocation accounting
the rest of GameCore does.

**`Balance`**, the single-rotation rebalance, runs bottom-up from every
leaf's parent after an insert or remove (Box2D's `b2DynamicTree::Balance`).
It picks the node whose two children's heights differ by more than one,
promotes the heavier child, and re-parents the light grandchild onto the
other child -- whichever of *that* child's two children keeps the tree's
total surface area (via each side's own height, as a proxy) smaller. F/G
naming in the implementation matches `b2DynamicTree` for anyone
cross-referencing the reference implementation.

**`PickSibling`**, the insertion-point search, is the standard "minimise the
surface-area increase" descent (Catto, *Dynamic Bounding Volume
Hierarchies*): at each internal node, compare the cost of making the new
leaf a sibling of *this* node against descending into whichever child costs
less once its own enlargement is accounted for. `AABB::SurfaceArea()` is
exactly the cost function this needs.

**Raycast** uses the slab method (Kay-Kajiya, *An Efficient and Robust
Ray-Box Intersection Algorithm*): intersect the ray against each axis' pair
of planes, track the running `[TMin, TMax]` intersection of all three
intervals. A zero ray component divides by zero, which produces ±infinity
and is handled correctly by IEEE 754 comparisons without a special case,
exactly as that paper depends on.

**`Rebuild`** is a top-down median split on the longest axis -- the standard
bottom-up-quality-for-top-down-cost tree build used when insertion order is
not trusted (a fresh level's leaves, inserted in file order, produce a badly
unbalanced incremental tree; this fixes it in one pass). Every internal node
is freed and rebuilt; every leaf is reused in place (exactly as `Move()`'s
reinsertion does), so every proxy id a caller holds stays valid.

**Locality across the two structures.** Tree order and pool order are
unrelated by default. `ComponentPool::Reorder` is the primitive that lets a
host sort a pool into tree order (a leaf-order traversal) after a level
load, so that region queries over a static world walk mostly-sequential
memory -- a tool a host calls once, not something `World` does per frame.

**`Validate`** walks the whole tree checking every invariant a dynamic AABB
tree is supposed to hold: every parent's box contains both children's
boxes, every leaf's stored box is at least its own true fat box, heights are
internally consistent. Expensive (O(n)) and meant as a test oracle
(`GameCoreSmokeTest` calls it after every mutating operation in its spatial
cases), not a per-frame assertion.

---

## GameCoreInternal.h

**This is the only header in the module that includes `lua.h`/`lauxlib.h`/
`lualib.h`,** and it lives under `Private/` so it cannot be reached from
outside -- exactly the discipline `GameWindowInternal.h` states for `SDL.h`
("this is the only header in the module that includes SDL.h"). That is what
makes the PRIVATE link of `Lua::Lua` in `CMakeLists.txt` true rather than
aspirational. Public headers forward-declare `struct lua_State` and return
it as an opaque pointer; nothing outside `Private/GameCore/` ever needs to
know what a `lua_State` contains.

**Lua is C, and its errors are `longjmp`.** `lua_error`/`luaL_error`/
`luaL_argerror` do not return: they longjmp back to the nearest `lua_pcall`,
unwinding the C stack *without* running C++ destructors for any local
between the raise and that `pcall`. This is undefined behavior to rely on in
general C++, and Lua's own manual says so; in practice on every toolchain
this engine targets it works, and it is the API's normal error-signalling
mechanism -- there is no exception to throw instead. **Rule:** a
`lua_CFunction` may only have trivially destructible locals in scope at any
point that can still raise. Argument-checking helpers go first, before any
`std::string` or other non-trivial local is constructed; once a raising call
can no longer occur, ordinary C++ is fine again. `GameCoreLog.h`'s macros
build a `std::string` but never raise, so logging is safe to place anywhere.

**`LuaStackGuard`** restores the Lua stack to its size at construction, on
any exit path -- including a longjmp out of a raising call inside the scope
it guards, provided the guard itself was constructed before that call and
the unwind does not cross the guard's own scope. It does not defend against
a longjmp that jumps *past* a live guard (which skips its destructor exactly
as it would skip any other); it only saves the common case of "several
pushes, several pops, do not miscount them by hand" from becoming a slow
stack leak across thousands of calls.

---

## ScriptEngine.h / ScriptEngine.cpp

**Ownership and teardown order.** `NimbleEngine::Application` owns exactly
one `ScriptEngine` alongside its one `World`, initializes it once, binds it
to that `World`, and unbinds + shuts it down in the reverse order at
teardown -- see `NimbleEngine.cpp`'s `Shutdown()` for why that order matters:
instances reference the `World`; the `World`'s pools fire the removal
observer, which needs a live `lua_State`.

**`BindWorld` installs, in one call:** the World/Entity/component-proxy/
TickPhase/Events Lua libraries; the script-tick system into every
`ETickPhase` bucket (a `ScriptComponent`'s own `Phase` byte decides which
bucket's pass actually runs it); the `ScriptComponent` removal observer; and
a bridge of `World`'s four built-in delegates onto its `EventBus` under
fixed names. One `World` at a time -- calling it again before `UnbindWorld()`
un-installs the previous binding first and logs a warning; rebinding is not
a supported way to swap worlds mid-session, it exists so a test fixture can
bind a fresh `World` per test case.

**The Lua allocator routes through `SystemAlignedAlloc`/`Free`,** not plain
malloc/free, so a leak inside the VM (a runaway table, a script that never
releases a reference) shows up in the same accounting every other allocation
in this engine does. `lua_Alloc` is realloc-shaped (`osize`/`nsize`, pointer
may be null, `nsize` may be zero for a free), but `SystemAlignedAlloc`/`Free`
are not a matched realloc pair -- there is no aligned-realloc exposed by
`CoreMemoryPool.h`, deliberately, because nothing else in this engine needs
one. So the allocator allocates fresh, copies `min(osize, nsize)` bytes
forward, and frees the old block -- a real cost on every grow, which for a
scripting VM's typical allocation pattern (many small objects, few large
reallocations) is not where time goes. If that ever shows in a profile, the
fix is a dedicated pool registered through `RegisterMemoryPool`, not a
change to this function's contract. Alignment is `alignof(std::max_align_t)`
-- Lua itself does not ask for anything stricter.

**`print()` is rebuilt over `GAMECORE_LOG`,** tab-separated `tostring()` of
each argument, matching `lbaselib.c`'s own `luaB_print` exactly except for
the destination, so script output reaches whatever `LogHandler` the host
installed rather than a console a shipped game may not have.

**The error counter, and the bug that shaped it.** `ScriptEngine::ErrorCount`
originally only counted `RunFile`/`RunString`'s own failures (a read failure,
a `luaL_loadbuffer` syntax error). A lifecycle callback error --
`ScriptSystem.cpp`'s `CallLifecycleMethod`, which calls `LuaCallProtected`
directly -- bumped only `ScriptComponent::bErrored` and never reached
`GetErrorCount()` at all. Caught by the smoke test expecting exactly one
counted error per broken script. Fixed by making `LuaCallProtected` itself
the one choke point every runtime error passes through: it increments a
counter stored in the Lua registry (keyed by a unique static's address, the
same "address is the key" idiom `ScriptBindings.cpp`'s `GetBoundWorld`/
`SetBoundWorld` and `ScriptSystem.cpp`'s `GetScriptState`/`SetScriptState`
both use), and `ScriptEngine::GetErrorCount()` sums the plain member (for
what never reaches `LuaCallProtected`) with the registry counter (for
everything that does).

---

## ScriptBindings.cpp

**Userdata layouts.** An `"Entity"` userdata is one `EntityID` (8 bytes),
with a metatable `__index` pointing at a table of Entity methods. A
component-proxy userdata is one packed `{EntityID, ComponentTypeID}`; its
metatable and methods now come from `ComponentScript.cpp` (see below) -- every
method re-fetches through `Entity::GetComponentByType(ComponentTypeID)` before touching anything,
so a proxy held across a swap-remove errors cleanly instead of aliasing
whatever now occupies that pool slot. Neither userdata has a Lua
uservalue or a `__gc`: they own nothing (an `EntityID` and a
`ComponentTypeID` are plain data), so there is nothing to release when
Lua's collector reclaims one.

**Why `ScriptSystem_*` forward declarations sit at `GameCore` namespace
scope, not inside this file's anonymous namespace -- and the link error that
proved it matters.** A declaration inside an anonymous namespace gets that
namespace's unique internal linkage, which makes it a *different symbol*
from the externally-linked `GameCore::ScriptSystem_*` function
`ScriptSystem.cpp` actually defines. Declared inside the anonymous
namespace, this file compiled clean and then failed to *link*: it was
declaring and calling its own never-defined local symbol of the same name,
not the one across the file. This was an actual `LNK2019` hit during
development, fixed by moving the three declarations out of the anonymous
namespace.

**`Entity_AddComponent`'s zero-initialized bytes.** The generic by-name Add
path has no way to call a specific type's constructor -- it only has
`SizeBytes` -- so it copies zeroed bytes as the init value. A component that
needs non-zero defaults it cannot express this way should not rely on the
Lua by-name Add-with-no-arguments path; this is the same limitation
`World::AddComponentRaw`'s own contract already has (it copies whatever
bytes it is given). A `std::vector<uint8_t>` local is used rather than a
shared static buffer, since a static zeroed buffer would be wrong under any
future multi-threaded script host.

**`World_QueryBounds`'s closure workaround.** `World`'s own `SpatialQueryFn`
is a raw function pointer + `void*` -- the Lua callback function lives at a
stack index in the *caller's* frame, which a raw C function pointer cannot
close over. So the Lua function is `luaL_ref`'d for the duration of this one
call and a small `QueryContext` struct carries the `lua_State*` plus that
ref through `InUserData`.

**`Events_On`'s handle packing.** The handle `Events.Off` needs is
`{Index, Serial}`, packed into one integer the same way `EntityID` packs
into 64 bits, since Lua has no notion of a `DelegateHandle` struct. The
`LuaEventBinding` pointer itself is never exposed to Lua -- leaking it would
let a script corrupt this module's own bookkeeping by handing back a forged
pointer.

**`Events_Off`'s linear search across events.** The packed integer only
carries the `DelegateList` slot, not which event it belongs to, so
`Events_Off` tries every event interned so far (bounded by
`GetEventCount()`, not `EVENT_MAX`). Trying a handle against the wrong
event's list is harmless -- each event's list has its own slots array and
its own serial sequence, so `Remove()` there is simply a not-found miss.
The `LuaEventBinding` this call logically frees is intentionally *not*
deleted here: `ScriptSystem_Uninstall`'s sweep is the actual owner of
releasing every binding still outstanding at unbind time, since this
function has no reliable way to know whether it just removed the last
reference to that specific pointer or one an earlier `Events.Off` already
handled.

---

## ScriptSystem.cpp

**The class registry.** `Script.Register(name, table)` stores `table` in the
Lua registry under a private key, keyed by name -- registry space, not a Lua
global, so a script cannot accidentally clobber `"Player"` by assigning a
variable of that name at file scope.

**One system per phase, not one overall.** `ScriptSystem_Install` registers
four systems, one per `ETickPhase`, each a thin C++ trampoline that walks
the `ScriptComponent` pool once and skips any instance whose own `Phase`
byte does not match. This is what lets a `ScriptComponent` declare
`TickPhase = TickPhase.PostPhysics` and have its `OnTick` actually run
during `PostPhysics` without `ScriptComponent` needing to be four different
component types.

**`CallLifecycleMethod`'s argument order, and the bug it fixes.** An earlier
version took a generic "N values the caller already pushed" count and used
`lua_insert` to rotate the Lua function below them. Tracing through the
stack math by hand: for `InExtraArgCount = 1` (the `OnTick` case), the
rotation swapped `self` and `dt`, so a script's `OnTick(self, dt)` would
silently receive `OnTick(dt, self)` -- `self` arriving where `dt` was
expected and vice versa. This was caught by working through the exact
`lua_insert` index math before it ever compiled, not by a failing test.
Fixed by changing the signature to a fixed optional `const float*
InDeltaSeconds` (null for `OnStart`/`OnDestroy`, pointing at the delta for
`OnTick`) so every value is pushed in the order Lua should see it, with
nothing ever inserted into the middle of the stack. The general lesson:
getting a final Lua argument order right by rotating an arbitrary number of
already-pushed values is exactly the kind of stack arithmetic that is easy
to get quietly backwards, since the mistake produces no error, just swapped
arguments a script reads as if they were correct.

**`RunScriptPhase`'s base-pointer re-read.** The `ScriptComponent` pool's
base pointer is re-read every loop iteration, not hoisted once, because the
buffer may have grown if `OnStart`/`OnTick` added a script to a *different*
entity during this same pass -- `World.h`'s `Each<T>` contract forbids
mutating `T`'s *own* pool during iteration, but does not forbid a different
entity's `ScriptComponent` being added mid-walk. Re-reading the base
defends this case at no real cost.

**Lua systems run after script components, in the same phase.** Registration
order between "a script's `OnTick`" and "a Lua system" is therefore fixed
(scripts first) -- simple to state, and matches what a script author
intuitively expects: "my object's own behavior runs, then whatever else is
watching the world."

**The removal observer can safely call back into Lua.** It fires from inside
`ComponentPool::Remove`, which -- after `World`'s own deferral -- only ever
happens at the end of a phase or outside a tick, never from inside a
system's own iteration of a *different* pool. So it never re-enters the
walk that triggered it.

---

## ComponentSerializer.h / ComponentSerializer.cpp / ComponentSerializers.cpp

**Why serializers are opt-in per type.** `ComponentTypeInfo` has a name, a size
and an alignment, and no fields, so a type has to say what it persists. Writing
`SizeBytes` raw bytes is not a shortcut, it is wrong:
`EngineCore::Vector3` is `alignas(16) float[4]` under `WITH_SIMD` and
`double[3]` without it, so a `TransformComponent` written by one build reads as
garbage in the other. Several fields are also per-run identities that mean
nothing in the next process: `ComponentOwner::OwningWorld` (a live pointer),
`BoundsComponent::ProxyID` (a `SpatialIndex` leaf) and
`ScriptComponent::InstanceRef` (a `luaL_ref`). A type with no serializer is
skipped with a warning on save and on load, so one unpersisted game component
does not break the save path.

**One registry per process**, for `ComponentType.h`'s reason: a function-local
static in a template is one per image. `GSerializers` is a plain array, so it
is constant-initialised before any registrar runs, and it is indexed by
`ComponentTypeID` directly. `RenderCore::RegisterProxyType` is the same shape.

**`ComponentDefaultsFn`, and the hang that motivated it.** The loader first
wrote each restored component from a zeroed buffer. A zeroed component is not a
default-constructed one: `BoundsComponent::ProxyID` defaults to -1 ("no leaf
yet"), and zeroed it reads 0. `RefreshSpatialIndex` dispatches on
`ProxyID < 0 ? Insert : Move`, so it moved a leaf that was never inserted and
indexed past the end of the node vector. In a debug build that shows up as a
*hang*, not a crash, because MSVC's CRT assertion dialog waits for input that
never comes. `TransformComponent::Scale` (1,1,1) and `InstanceRef` (-2) hit the
same trap in quieter ways. The macro now generates a defaults function that
copies out a `TypeName{}`; this is sound because `GAMECORE_DEFINE_COMPONENT`
asserts trivial copyability. `SpatialIndex::Move` also gained the proxy
validation `Remove` always had, so a stale id is a warning rather than an
out-of-bounds read.

**What the built-in serializers leave out.** Transform: nothing. Name: written
length-prefixed and re-clamped on load, so a larger-buffer build's save still
fits. Bounds: `Local` and `bStatic` only. `World` and `ProxyID` are a cache,
and the loader's `RefreshSpatialIndex` rebuilds both, including the first
insert a static Bounds needs. Script: `ClassName`, `Phase` and
`bCullOutsideActiveRegion`. The Lua instance is re-created from the class, so
whatever its table held is lost; persisting that would need a per-class
Lua save convention.

---

## WorldSerializer.h / WorldSerializer.cpp

**Format.**

    WRLD  uint32 entity count, then count x uint32 save-local index
      CMPT  per type with a pool and a serializer
            string type NAME, uint32 instance count,
            then per instance: uint32 owner index + DATA chunk (the serializer's bytes)

Chunks are keyed by type **name** because a `ComponentTypeID` is assigned in
first-use order and shifts when startup changes. Every payload is sized, so a
type this build lacks (RenderCore absent, a removed game component) is skipped
and the rest loads. A cross-configuration test confirmed this: a non-SIMD build
(24-byte `Vector3`) read a SIMD save (16-byte), skipping only the RenderCore
chunks it had no serializers for. Types are written in id order so identical
Worlds save to identical bytes. The index table is redundant today (always
`0..n-1`) and is written so a later version can omit entities without
redefining what an index means. A newer `WRLD` version is refused rather than
skipped, because every index depends on the entity table.

**Entity references.** A saved `{Index, Generation}` addresses nothing in a
fresh World. Entities go out as dense save-local indices, and any `EntityID`
field crosses through `IEntityRemapper`. Loading creates every entity before
any component, so a reference to an entity whose components come later in the
file still resolves. After a load every handle held *outside* the World is
stale; `IGameMode::OnPostLoadSave` exists to re-derive them.

**Not persisted:** systems and delegate/event bindings (function pointers into
an image; the level's `OnLoad` re-registers them), the spatial index (rebuilt),
render proxies (WorldBridge re-extracts them) and Lua instance state.

**Never mid-tick.** Deferred destroys would be half-applied: a save would
capture entities the next phase deletes, and a load would mutate pools a system
is iterating.

**Scripts last.** `SerializeWorldWithScripts` re-instantiates restored
`ScriptComponent`s only after every component is in place, so a script's
`OnStart` sees its siblings. It collects them first because `InstantiateScript`
runs Lua, which may add a `ScriptComponent` inside `Each<ScriptComponent>`. It
also removes the restored component before re-adding it, because
`InstantiateScript` refuses a second one.

**Versioning.** Each instance's DATA chunk carries its serializer's
registered version, and the serializer reads it with `Ar.GetChunkVersion()`
(save: the version being written; load: the file's). New fields go at the
*end*, behind `if(Ar.GetChunkVersion() >= N)`. That covers both directions:
an older save leaves the new field at its default (the loader starts from
`ComponentDefaultsFn`, not zeroes), and a newer save read by an older build
has its unknown trailing fields skipped by `CloseChunk`. Reordering or
removing a field is not covered; that needs its own version branch.

---

## ComponentScript.h / ComponentScript.cpp / ComponentScripts.cpp

**Selective by construction.** A component lists what Lua may touch with
`GAMECORE_DEFINE_COMPONENT_SCRIPT`: `Field` (generates `Get<Name>`/`Set<Name>`),
`ReadOnly` (getter only), `Flag` (a `uint8_t` exposed as a boolean) and `Method`
(a typed function). Anything unlisted -- above all per-run state like
`BoundsComponent::ProxyID` or `PrimitiveComponent::Proxy` -- does not exist in
Lua. Every component, bound or not, also answers `GetEntity()`. The built-in
Transform/Name/Bounds bindings used to be hand-written `lua_CFunction`s here and
moved onto this mechanism with their Lua API unchanged (Bounds gained `GetLocal`
and `GetStatic`).

**No `lua.h` outside GameCore.** Lua is a static library linked into
`GameCore.dll`. A binding elsewhere that included `lua.h` would carry a second
copy of the Lua core operating on GameCore's `lua_State`. That happens to work
with 5.4.8 (`isdummy` tests `lastfree`, not an address), but 5.4's
`luaL_checkversion` no longer detects a second copy, so a future break would be
silent -- the same one-static-library-one-image rule GDI follows. So bindings
see typed values only: fields are described as data (type, offset, size) and
GameCore reads and writes them; methods receive a `ScriptCall`.

**The longjmp rule, and why `ScriptCall` never raises.** Lua is compiled as C,
so a Lua error is a `longjmp`, which skips C++ destructors. Binder code is C++
that may hold objects with destructors, so nothing may raise while it runs:
`ScriptCall`'s getters record the first error and return a default, and
`ComponentDispatch` raises only after the method has returned. `ScriptCall`
itself is `static_assert`ed trivially destructible, because the dispatcher
raises with it still in scope. A method that writes should check
`Call.HasError()` first, or a bad argument can half-apply (Transform's
`Translate` does).

**One dispatcher.** Each Lua method is a C closure over `ComponentDispatch`
with three upvalues: the binding entry, the type id and the mode (get field,
set field, method, `GetEntity`). A type's metatable is built the first time
one of its proxies is pushed into a given `lua_State`, and stored in the
registry under `GameCore.Component:<Name>` so a component called `"Camera"`
cannot collide with another library's metatable.

**Registration.** The macro registers at static initialisation into a
constant-initialised table (`ComponentType.cpp`'s argument again), so a type in
another image -- RenderCore's Primitive and Camera -- registers into GameCore's
one table. Names that would have to be truncated are rejected rather than
shortened, since a shortened name is a different Lua API; rejections and a
duplicate registration are logged. A binding registered *after* a type's
metatable was built in some `lua_State` (a hot module loading late) is not
picked up by that state.

**Not covered.** A component added from Lua with `e:AddComponent(name)` still
starts from zeroed bytes, not its defaults (see `Entity_AddComponent` above) --
so a Lua-added Transform has scale 0 and a Lua-added Bounds a `ProxyID` of 0.
