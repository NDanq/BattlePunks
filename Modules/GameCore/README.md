# GameCore

Gameplay: entities, components, a phased tick, delegates and events, and Lua
scripting, on Lua 5.4.

`GameCore` is written for this repository, the same as `GameWindow` -- not a
vendored subtree, free to use `nimble_add_module()`.

---

## Position in the tree

```
Host exe --> NimbleEngine --> GameCore  --> EngineCore
                \-----------> GameWindow --> EngineCore
                \-----------> GDI --------> EngineCore
```

**GameCore does not link GameWindow or GDI, and neither links back.** A World
has no notion of a window or a renderer; `NimbleEngine::GameSession` is the
one place that owns a `GameCore::World` (see `GameSession::GetWorld()`) and
ticks it once per frame, after `IGameMode::OnTick` -- `GameSession::Tick` --
before the host's render-thread fence (`Application::RunFrame` has the exact
sequencing).

Unlike `GameWindow` and `GDI`, which `Engine/CMakeLists.txt` links into
`NimbleEngine` **PRIVATE**, `GameCore` is linked **PUBLIC**. `World` and
`Entity` are the game-facing surface -- a game includes GameCore's own headers
to use what `GameSession::GetWorld()` hands back -- so the dependency has to
be visible past `NimbleEngine`, not hidden behind it. See the PUBLIC link's
own comment in `Engine/CMakeLists.txt` for the full reasoning.

---

## File layout

```
Engine/Modules/GameCore/
|-- CMakeLists.txt
|-- README.md                       (this file)
|-- External/
|   |-- lua-5.4.8/                  vendored Lua source; see "Lua" below
|   `-- pugixml-1.16/               vendored XML parser, editor builds only; see "World files" below
|-- Source/
|   |-- Public/GameCore/
|   |   |-- GameCoreAPI.h           GAMECORE_API three-state export macro (always SHARED)
|   |   |-- GameCoreLog.h           GAMECORE_LOG / _WARN / _ERROR over CoreLog
|   |   |-- EntityID.h              EntityID {Index, Generation}, Pack()/Unpack()
|   |   |-- EntityGuid.h            EntityGuid: an entity's persistent 128-bit identity
|   |   |-- AABB.h                  axis-aligned box over EngineCore::Vector3
|   |   |-- TickPhase.h             ETickPhase: PrePhysics/Physics/PostPhysics/PostUpdate
|   |   |-- Delegate.h              DelegateHandle, DelegateList, TDelegate, TMulticastDelegate, TScopedBinding
|   |   |-- EventBus.h              EventID, EventArg/EventArgs, class EventBus
|   |   |-- ComponentType.h         ComponentTypeID, ComponentTypeInfo, ComponentOwner, the BODY/DEFINE macros
|   |   |-- ComponentPool.h         the erased sparse-set pool
|   |   |-- SpatialIndex.h          the dynamic AABB tree (BVH)
|   |   |-- World.h                 class World: entities (+ name/parent members), pools, phased systems,
|   |   |                           prefab instances, the script driver slot, spatial queries, delegates
|   |   |-- Entity.h                header-only handle: World* + EntityID; components, name, parent
|   |   |-- Components.h            includes every built-in component header below
|   |   |-- Components/             TransformComponent.h, BoundsComponent.h, CollisionComponent.h
|   |   |-- ComponentSerializer.h   ComponentArchive (named fields), the serializer macro and registry
|   |   |-- WorldSerializer.h       SerializeWorld: the binary world (cooked data, save games)
|   |   |-- WorldPackage.h          .nmworld files: text package save/load, cooking, format detection
|   |   |-- PrefabInstanceID.h      PrefabInstanceID {Index, Generation}
|   |   |-- Prefab.h                PrefabAsset, CapturePrefab, .nmprefab files, the registry, the PrefabInstance handle
|   |   |-- NativePrefab.h          native prefabs: PrefabBuilder, NativePrefabBehavior, the DEFINE macros
|   |   |-- ScriptLibrary.h         script libraries: a game's functions, variables and typed events
|   |   |-- ScriptStruct.h          script field types and script structs: a game's own value types
|   |   `-- ScriptEngine.h          lua_State lifetime, RunFile/RunString, BindWorld; NO lua.h
|   `-- Private/GameCore/
|       |-- GameCoreInternal.h      the ONLY header that includes lua.h/lauxlib.h/lualib.h
|       |-- ComponentType.cpp
|       |-- ComponentPool.cpp
|       |-- Components/             one .cpp per built-in component: registration, serializer,
|       |                           Lua binding and editor properties together
|       |-- Delegate.cpp
|       |-- EventBus.cpp
|       |-- TickPhase.cpp
|       |-- SpatialIndex.cpp
|       |-- World.cpp
|       |-- ScriptEngine.cpp
|       |-- ScriptBindings.cpp      World/Entity/TickPhase/Events Lua libraries
|       |-- ComponentScript.cpp     component proxies: the binding registry and the one dispatcher
|       |-- ScriptLibrary.cpp       the library registry, one global table per library, its dispatcher
|       |-- Collision.cpp           CollisionComponent overlaps: the pass, pairs, events, queries
|       |-- ScriptStruct.cpp        the script struct registry: ids by name, fields while registered
|       |-- WorldPackage.cpp        package save (atomic), two-pass load, cook
|       |-- XmlComponentArchive.h   the editor XML: files, exact numbers, the text ComponentArchive (worlds and prefabs)
|       |-- PrefabInternal.h        PrefabAsset::Data and the component-blob helpers, for Prefab.cpp and World.cpp
|       |-- Prefab.cpp              the asset, blobs, capture, both .nmprefab formats, the registry
|       `-- ScriptSystem.cpp        the World's script driver: file -> class cache, instantiation,
|                                   the per-phase tick (ahead of every system), release/OnDestroy
`-- Tests/
    `-- SmokeMain.cpp
```

Include paths are Source-relative -- `#include "Public/GameCore/World.h"` --
matching Core, GDI and GameWindow exactly.

---

## The two rules the whole design falls out of

**GameCore is ALWAYS SHARED.** There is no `GAMECORE_SHARED` option, unlike
`GameWindow`'s. `ComponentType.h` and `EventBus.h` hold their registries in
fixed tables filled lazily behind function-local statics and out-of-line
`GAMECORE_API` functions. A STATIC GameCore pulled PUBLIC through
`NimbleEngine.dll` would give `NimbleEngine.dll` and `Sandbox.exe` each their
own copy of every registry, so `TransformComponent` would register once per
image and get a **different id** in each -- with no diagnostic anywhere, just
a component read at the wrong offset the first time an id crossed the
boundary. Exactly `Engine/CMakeLists.txt`'s reasoning for forcing
`CORE_SHARED` and building `NimbleEngine` itself SHARED.

**Templates compute, GameCore.dll allocates.** `Entity::GetComponent<T>()` and
`TMulticastDelegate<...>::Broadcast` are templates and instantiate in
whichever image calls them. The templates in every public header here only
cast and forward; every allocation, every registry touch and every container
lives in an out-of-line `GAMECORE_API` function inside `GameCore.dll`.
Component storage is a type-erased `ComponentPool`; a delegate is a
type-erased `DelegateList`; the spatial index is a type-erased `SpatialIndex`.
This is `CoreMemory.h`'s "templates compute, EngineCore allocates" rule,
applied three more times.

---

## Entities and components

An `EntityID` is an index plus a generation -- the generation catches a stale
handle to a recycled slot. `World` owns an index free-list, a
`ComponentPool*[COMPONENT_TYPE_MAX]` table (created lazily on first use), and
the registry every component's `StaticType()` registers into by name.

A component is declared like this:

```cpp
struct MyComponent
{
    GAMECORE_COMPONENT_BODY()   // first thing in the struct, always

    EngineCore::Vector3 Value;
};
// in exactly one .cpp:
GAMECORE_DEFINE_COMPONENT(MyComponent, "My")
```

`GAMECORE_COMPONENT_BODY()` declares `Owner` -- a `ComponentOwner` holding a
`World*` and this component's `EntityID`, written once by
`World::AddComponentRaw` right after the pool copies the caller's init bytes.
From inside any component method or any system iterating a pool:

```cpp
TransformComponent* T = Bounds.Owner.GetSibling<TransformComponent>();
const char* Name = Bounds.Owner.GetName();
```

**The name is an entity member, not a component.** Every entity has
exactly one, stored in `World`'s slot tables alongside the generation, and
reached through the handle:

```cpp
GameCore::Entity Player = World.CreateEntity("Player");   // named before OnEntityCreated fires
GameCore::EntityGuid Id = Player.GetGuid();               // persistent; World.FindEntityByGuid(Id)
Player.SetName("Hero");
const char* Name = Player.GetName();                      // "" if none, never null
```

`World::FindEntityByName` is a linear scan over the live names. A
`GetName()` pointer stays valid while other entities come and go; only a
`SetName` on that entity, or destroying it, moves it. Behaviour belongs to a
prefab: an entity has no script of its own (see **A prefab's script**).

Components are **trivially copyable** (`GAMECORE_DEFINE_COMPONENT`
`static_assert`s it) -- a `ComponentPool` is one contiguous, aligned byte
array that `memcpy`s on swap-remove, which is both the cache-efficiency story
and the cross-image story (no destructor pointer into anyone's image).

**A `T*` from `GetComponent<T>()`, `AddComponent<T>()` or `Each<T>()`'s
callback is invalidated by any Add or Remove of `T` on ANY entity.** Hold an
`EntityID` or an `Entity` handle across a call that might mutate the pool;
re-fetch the pointer after.

---

## Hierarchy

An entity may have a parent. Parent and children are entity members, like the
name, so they survive component changes and cost no pool lookup.

```cpp
Wheel.SetParent(Car);                  // keeps the wheel where it is in the world
Wheel.SetParent(Car, false);           // keeps its local Transform; it moves with the car
Wheel.ClearParent();                   // a root again, still where it was
for(EntityID C = Car.GetFirstChild(); C.IsSet(); C = World.GetEntity(C).GetNextSibling()) { ... }

GameCore::TransformPose Where = Wheel.GetWorldPose();   // Position, Rotation, Scale in the world
Wheel.SetWorldPose(Where);                               // writes the local Transform that lands there
EngineCore::Matrix4x4 M = Wheel.GetWorldMatrix();
```

- **`TransformComponent` is local**, relative to the parent. Without a parent
  local is world, so an unparented entity behaves exactly as before.
- **World transforms compose position, rotation and scale**, not matrices: the
  renderer takes position/rotation/scale, so a parent with non-uniform scale
  stretches a rotated child along the child's axes and never shears it
  (`TransformMath::Compose`).
- **Nothing is cached.** `GetWorldPose()` walks up the parents on each call.
  The spatial refit, `WorldBridge`'s render extraction and the editor all
  use it, so they always agree. An unparented entity takes the old path, at
  the old cost.
- **An entity without a Transform passes its parent's through**, so an empty
  entity can group others without carrying a position of its own.
- **`SetParent` is immediate**, even inside a tick. It refuses a dead entity
  and a cycle (parenting to yourself or a descendant). Children keep the
  order they were parented in.
- **Destroying a parent destroys its subtree.** Children go first, while the
  parent's components still exist; each one's `OnEntityDestroyed` and
  `OnDestroy` run as usual. Inside a tick the whole subtree goes at the
  phase flush.
- **`RefreshEntityBounds(e)` refits `e` and its descendants**, so a tool
  moving a parent refits everything that moved with it.
- **Saved** in both formats: a `Parent="<guid>"` attribute on a child's
  `.nmentity`, and a parent index per entity in the binary `WRLD` chunk.

Lua: `e:SetParent(p[, keepWorld])` (`nil` detaches), `e:GetParent()` (an
Entity, `IsValid()` false if none), `e:GetChildCount()`, `e:GetChild(i)`
(1-based) and `e:GetChildren()`. A Transform adds `GetWorldPosition` /
`SetWorldPosition`, `GetWorldRotation` / `SetWorldRotation` and
`GetWorldScale`. Script graphs have **Get Parent**, **Set Parent**, **Get
Child Count** and **Get Child**.

---

## Prefabs

A prefab (`Prefab.h`) is a tree of entity templates, each with a name, a
parent and components. It also names the script and tick mode that will
drive its instances (PREFABS.md). Spawning one
makes a **prefab instance**: ordinary entities that remember which instance
and which template they came from.

```cpp
const GameCore::PrefabAsset* Turret = GameCore::FindOrLoadPrefab("Prefabs/Turret.nmprefab");   // loaded once

GameCore::TransformPose At;
At.Position = EngineCore::Vector3(100.0, 0.0, 0.0);
GameCore::PrefabInstance Instance = World.SpawnPrefab(*Turret, At);   // or World.SpawnPrefab("Prefabs/Turret.nmprefab", At)

GameCore::Entity Root   = Instance.GetRoot();             // its Transform is the placement
GameCore::Entity Barrel = Instance.FindEntity("Barrel");  // by TEMPLATE name, whatever it is called now
Instance.Destroy();                                       // the root and everything under it
```

- **Making one.** `CapturePrefab(World, Root, Asset)` takes `Root`'s
  subtree as it is in a World. The root's Transform is stored as identity,
  because where an instance stands is its placement, not part of the
  prefab. Capturing an instance of the same prefab keeps each entity's
  template id. New entities get new ids, and ids are never reused; with
  `bInAdoptNew` they also join the root's instance under those ids
  (`World::AdoptIntoInstance`), so the next capture keeps them.
- **References between its entities survive.** A component's `EntityID`
  field that points inside the prefab points at the matching entity of each
  instance. One that points outside is stored unset, with a warning.
- **Membership.** `Entity::GetInstance()` and `GetPrefabLocalId()`. An
  instance's entities stay inside it: `SetParent` refuses to move one under
  a loose entity or detach it. Only the root may be attached elsewhere,
  for example a weapon on a hand.
- **Lifetime.** Destroying the root destroys the instance, deferred inside
  a tick like any destroy. `OnInstanceDestroyed` fires first, while every
  entity is still there. `OnInstanceSpawned` fires once everything is in
  place. Destroying a single member just removes it.
- **GUIDs.** An instance has a GUID; each of its entities' GUIDs is derived
  from it and the template id (`DerivePrefabEntityGuid`). The same instance
  therefore spawns the same entity GUIDs every time.
- **Sharing.** `PrefabAsset` copies share their data until one is changed.
  An instance keeps the version it was spawned from, even across
  `ReloadPrefab`; an editor respawns it from the reloaded file with
  `RefreshPrefabInstance` (`WorldPackage.h`), keeping its GUID, placement
  and overrides. `ReadPrefabTemplateComponent` reads one template
  component as an instance would get it.
- **Native prefabs** (`NativePrefab.h`) are declared in C++ by a game module:
  a `PrefabBuilder` function for the layout and, optionally, a
  `NativePrefabBehavior` table of callbacks that run before the script
  (`GAMECORE_DEFINE_NATIVE_PREFAB` / `GAMECORE_DEFINE_NATIVE_PREFAB_BEHAVIOR`).
  Its path is `"native:<Name>"` wherever a prefab path goes.
- **Saved with the world** as instances (see **World files**). An instance
  spawned from an in-memory asset has no file to name, so a text world
  saves its entities as plain entities, with a warning.

**Files.** `.nmprefab`, text or cooked, told apart like worlds. `LoadPrefabFile` reads
either; `SavePrefabFile` and `CookPrefabFile` are editor-only.

```xml
<Prefab Format="1" Name="Turret" NextId="4">
  <Script Path="Scripts/Turret.lua" />
  <Tick Mode="Tick" />
  <Entity Id="1" Name="Root">
    <Component Type="TestLink" Version="1">
      <Target Entity="3" />            <!-- a reference: another entity's Id -->
    </Component>
  </Entity>
  <Entity Id="2" Name="Barrel" Parent="1"> ... </Entity>
  <Entity Id="3" Name="Muzzle" Parent="2"> ... </Entity>
</Prefab>
```

Components use the same serializers and the same field elements as
`.nmentity` files. Output is deterministic, and an unchanged file is not
rewritten. A hand-edited file with a child listed before its parent is put
back in tree order; one with two roots, a missing parent or a cycle is
refused.

Lua: `World.SpawnPrefab(path[, x, y, z])` returns the root Entity;
`IsValid()` is false if the prefab could not be loaded. Script graphs have
**Spawn Prefab**.

### A prefab's script

A prefab names one script (`PrefabAsset::SetScriptPath`, a `.lua` or
`.script.nmgraph`), and each instance gets its own object of that class.
Instead of one `OnTick` in one phase, a class defines a function for each
phase it needs:

```lua
return {
    OnStart       = function(self) end,          -- once, at the first phase after spawning
    OnPrePhysics  = function(self, dt) end,      -- define only the phases you need:
    OnPhysics     = function(self, dt) end,      --   an instance is listed in those
    OnPostPhysics = function(self, dt) end,      --   phases and nowhere else
    OnPostUpdate  = function(self, dt) end,
    OnDestroy     = function(self) end,          -- the instance is destroyed, or turned Off

    OnOverlapBegin = function(self, other, mine) end,   -- mine: which of ITS entities
    OnOverlapEnd   = function(self, other, mine) end,
}
```

- `self.Root` (and `self.Entity`, the same) is the root. `self.Instance` is
  the instance: `:Find(templateName)`, `:GetRoot()`, `:GetName()`,
  `:Get/SetTickMode("Tick" | "EventsOnly" | "Off")`,
  `:IsPhaseEnabled(TickPhase.X)` / `:SetPhaseEnabled(TickPhase.X, on)`,
  `:Destroy()`. `e:GetInstance()` gives an entity's instance.
- **`OnTick` still works.** With `TickPhase = TickPhase.X` it stands for
  `OnX`, unless the class defines `OnX` too.
- **Tick modes.** `Tick` runs phase functions. `EventsOnly` keeps the script
  for `OnStart`, `OnDestroy`, overlaps and events, but calls no phase
  function. `Off` has no script at all: switching to it calls `OnDestroy`,
  and switching back starts the script afresh. A prefab spawned `Off` is
  static geometry: its Bounds are fitted once and never refit.
- **`SetTickMode` and `SetPhaseEnabled` wait for the phase flush** when
  called inside a tick, like `DestroyEntity`, because the phase's list is
  being walked.
- **Overlaps.** Each of the instance's colliders reports to the instance's
  script, with `mine` set to that collider's entity. Two of its own
  colliders touching each other are not reported.
- **Order within a phase:** instances (in the order they started), then Lua
  systems, then C++ systems. A failing instance
  script is reported once and not called again.

Script graphs: **On Pre-Physics**, **On Physics**, **On Post-Physics** and
**On Post Update** events; **On Overlap Begin / End** gain a **Mine**
output. The **Prefab** group has **Prefab Root**, **Find Prefab Entity**,
**Set Tick Mode**, **Set Phase Enabled** and **Destroy Prefab**.

---

## Tick phases

```cpp
enum ETickPhase { TP_PrePhysics, TP_Physics, TP_PostPhysics, TP_PostUpdate, TP_MAX };
```

`World::RegisterSystem(name, phase, fn, userData)` puts a system into one of
four buckets; `World::Tick()` runs them in order -- each phase opening with
the script driver's pass, so prefab scripts always update before that phase's
systems -- and flushes the deferred destroy/remove/tick-mode queue **at the end
of each phase** -- not the end of the frame
-- so a `TP_PrePhysics` system that destroys an entity has it gone before
`TP_Physics` runs. `World::RefreshSpatialIndex()` (the BVH refit) runs at the
end of `TP_Physics`, after every system registered there.

A system registered with `SO_BeforeScripts` (the optional last argument) runs
at the start of its phase, **ahead of** that phase's scripts. Use it for what
the scripts must already see this frame. The engine's input resolve is the
case it exists for: it is the first thing in `TP_PrePhysics` (`INPUT.md`).

Calling `DestroyEntity` / `RemoveComponent<T>` / `SetTickMode` from inside a tick defers to
the current phase's flush; calling them outside a tick is immediate. Never
destroy or remove from inside a pool walk you are not the observer of.

---

## Collision

A `CollisionComponent` ("Collision") makes an entity report overlaps. It is
for overlap detection and intersection queries: nothing is pushed apart.

- **Shape.** The entity's `BoundsComponent` box, so World's spatial index is
  the broad phase.
  - `Source = CS_Bounds` uses `Bounds.Local` as authored.
  - `Source = CS_Mesh` uses the bounds of the sibling RenderComponent's mesh.
- **When.** `World::UpdateOverlaps` runs first in `TP_PostPhysics`
  (`SO_BeforeScripts`): the boxes were refit at the end of `TP_Physics`, and
  that phase's scripts see the results.
- **Filter.** A overlaps B only if `A.Layer & B.Mask` and `B.Layer & A.Mask`.
  Two static boxes are never tested.
- **Events.** For each pair that begins or ends, each side is told as Self:
  - C++: `World::OnOverlapBegin()` / `OnOverlapEnd()`, with `(Self, Other)`;
  - Lua: the prefab script's `OnOverlapBegin(self, other, mine)` / `OnOverlapEnd(self, other, mine)`.
- **When pairs end.** Disabling or removing a collider ends its pairs.
  Destroying an entity tells only the survivor, whose `other` is then not
  valid.
- **Queries, without events:**
  - `World::AreOverlapping(A, B)` and `GetOverlaps(E, ...)`, as of the last
    pass;
  - `World::OverlapBox(box, mask, ...)`, which tests now.

```cpp
GameCore::CollisionComponent Hit;
Hit.Layer = Layers::Pickup;
Hit.Mask  = Layers::Player;            // only the player sets it off
Pickup.AddComponent(Hit);              // needs a Bounds (CS_Bounds), or a Mesh (CS_Mesh)

World.OnOverlapBegin().Add([](GameCore::EntityID Self, GameCore::EntityID Other, void*) { ... }, nullptr);
```

```lua
return {
    OnOverlapBegin = function(self, other) print(other:GetName() .. " touched me") end,
    OnOverlapEnd   = function(self, other) end,   -- other:IsValid() is false if it was destroyed
}
```

Script graphs have **On Overlap Begin** and **On Overlap End** event nodes,
with an `Other` output. The `Collision` script library queries without events:

- `Collision.OverlapBox(minX, minY, minZ, maxX, maxY, maxZ[, mask])` returns
  every hit;
- `Collision.CountInBox(min, max, mask)`;
- `Collision.AreOverlapping(a, b)`.

Overlaps are not saved: after a load, pairs that still hold begin again.

In the editor's Details panel, **Source** is a dropdown: *Bounds component*
or *Render component mesh*. It is a choice property,
`Props.Choice(name, &T::Field, "A|B|C")`, which any component can use for
an enum-like `uint8_t` or `int32_t` field. The editor runs the overlap pass
on the edited scene too, so choosing the mesh shapes the entity right away.

Sandbox has one example of each:
- **C++:** the mesh-shaped "Hazard" and `OnHazardTouched` on `World::OnOverlapBegin()`, in `SandboxGame.cpp`.
- **Lua:** the trigger "Zone", in `Scripts/Main.lua` and `Scripts/Zone.lua`.
- **Script graph:** pickups collected on *On Overlap Begin*, in `Scripts/Pickup.script.nmgraph`.

Layers are in `SandboxLayers`.

---

## Delegates and events

Two layers, both `{function pointer, void*}`, never `std::function` -- see
`Delegate.h`'s own long comment for why (the same cross-heap-free argument
`CoreConfig.h` makes for `ConfigValue`).

```cpp
TMulticastDelegate<EntityID> MyDelegate;
DelegateHandle H = MyDelegate.Add(&Callback, UserPtr);
MyDelegate.Broadcast(SomeEntity);
MyDelegate.Remove(H);                 // or RemoveAll(UserPtr)
```

`World` exposes four built-in ones: `OnEntityCreated`, `OnEntityDestroyed`,
`OnComponentAdded`, `OnComponentRemoved`. `World::GetEvents()` is the layer
above for callers that do not share a header -- a Lua script, most notably --
keyed by interned name instead of a typed function pointer:

```cpp
EventID Id = World.GetEvents().GetEventID("PlayerScored");
World.GetEvents().Subscribe(Id, &OnScored, nullptr);
World.GetEvents().Emit(Id, /* up to four EventArgs */);
```

**A binding is a promise to unbind.** A callback into an unloaded module left
in a `DelegateList` is `CoreModule.h`'s DANGLING CALLBACKS hazard verbatim.
`RemoveAll(userData)` / `UnsubscribeAll(userData)` exist so an `OnShutdown` is
one call.

---

## The spatial index (BVH)

`World::GetSpatialIndex()` is a dynamic AABB tree (Box2D's `b2DynamicTree`
shape) of `EntityID` leaves and fattened world-space boxes -- no component
data, just position. A `BoundsComponent` is what puts an entity in it:

```cpp
BoundsComponent* B = E.AddComponent<BoundsComponent>();   // E: the Entity CreateEntity returned
B->Local = AABB(Vector3(-0.5,-0.5,-0.5), Vector3(0.5,0.5,0.5));
```

`World::RefreshSpatialIndex()` (called automatically at the end of
`TP_Physics`) transforms `Local` by the entity's `TransformComponent` (identity
if none), writes the result to `World`, and `Insert`s or `Move`s the leaf.
`bStatic = 1` gets the entity ONE leaf (its first refit) and is never re-fit
again -- for level geometry that never moves.

```cpp
World.QueryBounds(SomeBox, [](EntityID E, void*) { return true; }, nullptr);
World.EachInBounds<TransformComponent>(SomeBox, [](Entity E, TransformComponent& T) { ... });
bool bHit = World.Raycast(Origin, Dir, MaxT, OutHit);
```

Iterate a pool (`Each<T>`) when a system wants *every* instance of `T`; query
the tree (`QueryBounds` / `EachInBounds`) when it wants only what is *nearby*
-- the per-hit fetch behind `EachInBounds` is a sparse-set lookup, the price
of asking by region instead of walking a pool in order.

---

## Scripting

`ScriptEngine` owns one `lua_State`, bound to one `World` at a time
(`BindWorld` / `UnbindWorld`). The host (`Application`) owns exactly one,
lent to its `GameSession` -- `GameSession::GetScriptEngine()` -- which binds it
to each level's World as the level loads and unbinds it as it unloads.

**A script is a file, named by a prefab.** The file returns its class
table; every instance of a prefab that names the path gets its own object
(`self`), with `self.Entity` / `self.Root` (the root) and `self.Instance`
set (see **A prefab's script** for the phase functions):

```lua
-- Scripts/Mover.lua, named by Prefabs/Mover.nmprefab's <Script Path>
return {
    OnStart       = function(self) end,
    OnPostPhysics = function(self, dt) self.Entity:GetComponent("Transform"):Translate(dt, 0, 0) end,
    OnDestroy     = function(self) end,
}
```

```lua
local m = World.SpawnPrefab("Prefabs/Mover.nmprefab", 1, 2, 3)   -- the root; its instance runs Mover.lua
local e = World.CreateEntity("Marker")          -- a loose entity: components, no script
e:AddComponent("Transform"):SetPosition(1, 2, 3)
e:SetParent(m)                                  -- also e:GetParent(), e:GetChildren(), e:GetChild(i), e:GetChildCount()

Events.On("EntityCreated", function(entity) print(entity:GetName()) end)
Events.Emit("PlayerScored", 10, "Player")       -- numbers, booleans, strings, entities; up to 4 args
```

How it runs:

- **Scripts update first.** `BindWorld` installs a `ScriptDriver` on the
  World, and `World::TickPhase` runs it at the start of every phase, before
  any registered system, whatever order anything was registered in. Within
  that pass, prefab instances run in the order they started, then the
  Lua-defined systems (`World.AddSystem`).
- **Lazy.** Spawning only queues the instance. The driver loads the file
  (once per path per bind, cached) and builds the object at the start of the
  next phase it runs, then calls `OnStart`. A World with no driver -- the
  editor's -- leaves its instances queued and runs nothing.
- **Errors are reported once.** A missing file, a syntax error or a file that
  does not return a table logs one error per path; an error in `OnStart` or a
  phase function logs once and stops that instance.

A component proxy (what `e:GetComponent("Transform")` returns) holds an
`EntityID` and a `ComponentTypeID`, never a pointer -- every method re-fetches
through `Entity::GetComponentByType(ComponentTypeID)`, so a proxy held across a `RemoveComponent`
errors cleanly instead of aliasing whatever now occupies that pool slot.

**A component chooses what Lua sees** (`ComponentScript.h`). Anything not
listed is invisible; every component also gets `GetEntity()`. Binding code
never includes `lua.h`:

```cpp
GAMECORE_DEFINE_COMPONENT_SCRIPT(RenderComponent)
{
    Bind.Field("Mesh",   &RenderComponent::Mesh);      // GetMesh / SetMesh
    Bind.Flag("Visible", &RenderComponent::bVisible);  // uint8_t as a boolean
}

GAMECORE_DEFINE_COMPONENT_SCRIPT(TransformComponent)
{
    Bind.Field("Position", &TransformComponent::Position);   // three numbers
    Bind.Method("Translate", [](TransformComponent& C, ScriptCall& Call)
    {
        const EngineCore::Vector3 Delta = Call.GetVector3(1);
        if(Call.HasError()) { return; }
        C.Position = C.Position + Delta;
    });
}
```

`Bind.ReadOnly` gives a getter only. Field types: float, double, int32,
uint8, bool, Vector3, AABB, Color and `char[N]`. `Bind.Element("Material3",
&RenderComponent::Materials, 3)` binds one element of an array member as its
own field (`Props.Element` likewise, for the editor). For the editor,
`Props.Array("Materials", &RenderComponent::Materials, PF_AssetMaterial)` lists
the whole array as one property (`ArrayCount` elements, `Stride` apart), which
the Details panel shows as a list to add to and remove from.

**A C++ function that already exists needs no glue.** `Native` writes it from
the function's types, and the string only names the graph's pins:

```cpp
Bind.Native<&PickupComponent::Collect>("Collect", "-> Value");               // int32_t Collect()
Bind.Native<&PickupComponent::PositionAt>("PositionAt", "Time -> Position"); // Vector3 PositionAt(double) const
Lib.Native<&SpawnPickup>("SpawnPickup", "Position, Value -> Pickup");        // EntityID SpawnPickup(World&, const Vector3&, int32_t)
Lib.NativePure<&RankForScore>("RankForScore", "Score -> Rank");              // const char* RankForScore(int32_t)
```

- **Parameter and return types:** float, double, int32_t, bool, `const char*`,
  `std::string`, Vector3, Color and EntityID, by value or const reference. A
  Vector3 is three Lua values and a Color is four.
- **Context parameters:** `World&` and `ScriptCall&` are filled in by the
  engine. They are not script arguments, and `ScriptCall&` lets a function
  call `Error()`.
- **Pin names:** omit the names to get Arg1, Arg2, ... and Result.
- **Unsupported types:** any other type is a compile error that lists these.
  Bind it with `Method` and a `ScriptCall` instead, as you also would for
  several results or optional arguments.

**A game's own value types are script structs** (`ScriptStruct.h`):

```cpp
struct PickupStats                                   // a header
{
    GAMECORE_SCRIPT_STRUCT_BODY()
    int32_t Value = 1;
    int32_t ID    = 0;
};

GAMECORE_DEFINE_SCRIPT_STRUCT(PickupStats, "PickupStats")   // one .cpp
{
    Struct.Field("Value", &PickupStats::Value);
    Struct.Field("ID",    &PickupStats::ID);
}
```

Once declared, a struct can be used in these places:
- **component fields:** `Bind.Field("Stats", &PickupComponent::Stats)`;
- **library variables:** `Lib.Variable`;
- **Native signatures:** a parameter or return of `Native` and `NativePure`;
- **`Method` bodies:** `ScriptCall::GetStruct` and `ScriptCall::PushStruct`.

In Lua it is its fields' values in order (`local value, id = pickup:GetStats()`).
In a script graph it is a pin type with Make and Break nodes. Its fields may be
any field type except another struct, and it unregisters with its module
like everything else. A save stores a struct field's members individually:
`Ar.Field("Value", C.Stats.Value)`.

**Anything else a game exposes is a script library** (`ScriptLibrary.h`): free
functions, C++ variables and typed events, under one global table.

```cpp
GAMECORE_DEFINE_SCRIPT_LIBRARY(Sandbox)
{
    Lib.Function("SpawnPickup", &SpawnPickup, "Position:Vector3, Value:integer -> Pickup:Entity");
    Lib.Pure("RankForScore", &RankForScore, "Score:integer -> Rank:string");   // no side effects
    Lib.Variable("Score", &GScore);                         // Sandbox.GetScore / SetScore
    Lib.ReadOnly("Elapsed", &GElapsed);                     // Sandbox.GetElapsed only
    Lib.Event("ScoreChanged", "Score:integer");             // on the World's EventBus
}
```

A function takes a `ScriptCall`, as a component method does, with arguments
from 1. Signatures are for tools: the script graph editor turns each member
into nodes (NodeGraph's README).

**From a game module.** All three macros (component types, component
bindings and serializers, and libraries) work in a game module DLL the same
way as in the engine. Each registrar unregisters when its image unloads,
so a module can be loaded, unloaded and loaded again: by the editor, to read
its declarations, and by every Play. Calling a binding whose module has
unloaded is a Lua error rather than a jump into unmapped code.
`Games/Sandbox/Source/Private/Sandbox/SandboxScripting.cpp` is the example.

**Lua is compiled as C and its errors are `longjmp`.** A `lua_CFunction` in
this module's bindings may only have trivially destructible locals in scope
at any point that can still raise -- see `GameCoreInternal.h`'s header
comment. `CoreCallProtected` (used by every lifecycle call and by
`RunFile`/`RunString`) installs a traceback handler and logs through
`GAMECORE_LOG_ERROR`; a script error is never fatal to the engine.

The driver's release runs `OnDestroy` and releases the object's Lua
registry ref when the instance is destroyed (before any of its components go)
or turned `Off`. `ScriptEngine::UnbindWorld()` walks
every still-live instance, every Lua-defined system, and every `Events.On()`
binding a script made, and releases all of them -- a Lua function cannot be
trusted to clean up before `lua_close`. Script paths survive an unbind; the
next `BindWorld` starts them again.

---

## World files

A `.nmworld` is text or binary, told apart by its first bytes
(`DetectWorldFileFormat`); `LoadWorldFile` reads either.

```
Levels/Sandbox.nmworld               text: <World Format="1"> listing entity and instance GUIDs
Levels/Sandbox/<guid>.nmentity       text: one loose entity -- name, parent, components
Levels/Sandbox/<guid>.nmprefabinst   text: one prefab instance -- its prefab and what it changed
Levels/Cooked/Sandbox.nmworld        binary: what a game ships (CookWorldFile)
```

**Prefab instances.** The manifest lists them beside the loose entities, as
`<Instance>` entries. An instance's file names its prefab and records only
what this instance changed:

```xml
<Instance Guid="..." Name="TurretA" Prefab="Prefabs/Turret.nmprefab" TickMode="EventsOnly" Parent="<guid>">
  <Entity Id="1">                                   <!-- the placement: the root's Transform -->
    <Component Type="Transform" Version="1"><Position X="100" Y="0" Z="0" /></Component>
  </Entity>
  <Entity Id="2" Name="Gun">                        <!-- renamed; one field changed -->
    <Component Type="Transform" Version="1"><Position X="0" Y="7" Z="0" /></Component>
  </Entity>
  <Entity Id="3">
    <RemovedComponent Type="Bounds" />              <!-- and a component added: written whole -->
    <Component Type="Collision" Version="1">...</Component>
  </Entity>
  <Removed Entity="4" />                            <!-- a template entity it lost -->
</Instance>
```

- A load spawns the prefab with the instance's GUID, so its entities get the
  same derived GUIDs and references to them resolve. It then loads each
  written field over the spawned component; an unwritten field keeps the
  prefab's current value, so a prefab edit reaches every instance that did
  not change that field.
- An instance exactly as its prefab is one line. A template entity added to
  the prefab later appears in every instance on the next load. An override
  for one the prefab dropped is ignored, with a warning.
- A loose entity under an instance's entity is an ordinary `.nmentity` whose
  `Parent` is that entity's GUID.
- Not recorded in text: per-phase enables (`SetPhaseEnabled`), which are
  runtime state. Binary keeps them.

**Binary stores instances whole:** every entity in full, like a loose one,
plus an instance table: GUID, name, prefab path, script path, tick mode,
phase enables, and its members with their template ids.
`World::RestorePrefabInstance` rebuilds each instance from those entities, so
a cooked level or save game needs the prefab file only for template names
(`FindEntity("Barrel")`).

**Text is the editor's format** (`SaveWorldPackage`, editor builds only). One
small XML file per entity, named by its `EntityGuid`, so an edit to one entity
is a diff in one file, and output is deterministic: components sorted by type
name, floats in their shortest exact form, LF line endings. A save only
rewrites files whose bytes changed, commits the manifest last (temporaries,
then renames), and afterwards removes the files of deleted entities. A load
creates every listed entity first, then their components, so references
between entities resolve; files the manifest does not list are ignored.
`.gitattributes` keeps every extension LF (`.nmworld`, `.nmentity`,
`.nmprefab`, `.nmprefabinst`).

**Binary is the shipped format** -- `SerializeWorld`'s archive, one file.
`CookWorldFile(text, out)` produces it; the editor's `cook` console command
wraps that. A game build has no XML parser and refuses a text world.

**Serializers name their fields**, which is what lets one serializer write
both formats:

```cpp
GAMECORE_DEFINE_COMPONENT_SERIALIZER(RenderComponent, 1)
{
    Ar.Field("Mesh",   C.Mesh);
    Ar.Flag("Visible", C.bVisible);
    Ar.Entity("Owner", C.OwnerEntity);            // an EntityID: a GUID in text, an index in binary
    if(Ar.GetVersion() >= 2) { Ar.Field("Depth", C.Depth); }   // added later: binary needs the guard
}
```

In text a missing field keeps its default and an unknown one is skipped; binary
is positional, so new fields go at the end behind a version check.

pugixml 1.16 is vendored at `External/pugixml-1.16/` (MIT, `LICENSE.md`
there) the way Lua is, built by `CMakeLists.txt` only when `WITH_EDITOR`, with
`PUGIXML_NO_XPATH`. `pugixml.hpp` is included only by `WorldPackage.cpp`.

---

## Threading contract

No locks anywhere in this module. Every call -- `World`, `ScriptEngine`,
`SpatialIndex`, every delegate -- happens on the thread that runs
`IGame::OnTick`, which `NimbleEngine.cpp` also uses as the `GameWindow` pump
thread. The same contract `GameWindowSystem.h` states for itself.

---

## Lua

Lua 5.4.8 is vendored in source form at:

```
Engine/Modules/GameCore/External/lua-5.4.8/
```

Mirroring how `Modules/GameWindow` carries `External/SDL-release-3.4.16`: the
dependency lives with the one module that needs it, there is no
`find_package`, no `FetchContent`, and no fallback to a system Lua. Lua ships
no CMakeLists of its own (unlike SDL); `CMakeLists.txt` builds it directly
from `src/*.c`, excluding `lua.c` and `luac.c` -- the standalone interpreter
and compiler's own `main()`s, listed explicitly so a future Lua source
addition does not silently join the target without review.

`luaconf.h` auto-detects `_WIN32` into `LUA_USE_WINDOWS` on its own; Linux and
macOS get `LUA_USE_LINUX` / `LUA_USE_MACOSX` defined explicitly by
`CMakeLists.txt`, matching what `lua.c`'s own build would set.

**lua.h never appears in `Public/`.** `GameCoreInternal.h`, under `Private/`,
is the only header that includes it; `ScriptEngine.h` forward-declares
`struct lua_State` and hands it back as an opaque pointer. `GameCoreSmokeTest`
links `GameCore` and nothing else -- **not** `Lua::Lua` -- which is the
structural check that this stays true: if it ever fails to link because
something needs a raw Lua symbol, that property has broken.

**Bumping Lua** is two edits: drop the new tree in beside the old one and
change `GAMECORE_LUA_DIR` in `CMakeLists.txt`, plus the source list if any
file was added, removed or renamed upstream.
