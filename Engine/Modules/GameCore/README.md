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
|   |   |-- World.h                 class World: entities (+ name/script members), pools, phased systems,
|   |   |                           the script driver slot, spatial queries, delegates
|   |   |-- Entity.h                header-only handle: World* + EntityID; components, name, script path
|   |   |-- Components.h            TransformComponent, BoundsComponent
|   |   |-- ComponentSerializer.h   ComponentArchive (named fields), the serializer macro and registry
|   |   |-- WorldSerializer.h       SerializeWorld: the binary world (cooked data, save games)
|   |   |-- WorldPackage.h          .nmworld files: text package save/load, cooking, format detection
|   |   `-- ScriptEngine.h          lua_State lifetime, RunFile/RunString, BindWorld; NO lua.h
|   `-- Private/GameCore/
|       |-- GameCoreInternal.h      the ONLY header that includes lua.h/lauxlib.h/lualib.h
|       |-- ComponentType.cpp
|       |-- ComponentPool.cpp
|       |-- Components.cpp
|       |-- Delegate.cpp
|       |-- EventBus.cpp
|       |-- TickPhase.cpp
|       |-- SpatialIndex.cpp
|       |-- World.cpp
|       |-- ScriptEngine.cpp
|       |-- ScriptBindings.cpp      World/Entity/TickPhase/Events Lua libraries
|       |-- ComponentScript.cpp     component proxies: the binding registry and the one dispatcher
|       |-- ComponentScripts.cpp    what Transform/Bounds expose to Lua
|       |-- WorldPackage.cpp        the XML ComponentArchive, package save (atomic), two-pass load, cook
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

**Name and script are entity members, not components.** Every entity has
exactly one of each, stored in `World`'s slot tables alongside the
generation, and reached through the handle:

```cpp
GameCore::Entity Player = World.CreateEntity("Player");   // named before OnEntityCreated fires
GameCore::EntityGuid Id = Player.GetGuid();               // persistent; World.FindEntityByGuid(Id)
Player.SetName("Hero");
Player.SetScript("Scripts/Player.lua");                   // a path; "" or nullptr clears
const char* Path = Player.GetScriptPath();                // "" if none, never null
```

`World::FindEntityByName` is a linear scan over the live names. A
`GetName()`/`GetScriptPath()` pointer stays valid while other entities come
and go; only a `SetName`/`SetScript` on that entity, or destroying it, moves it.

Components are **trivially copyable** (`GAMECORE_DEFINE_COMPONENT`
`static_assert`s it) -- a `ComponentPool` is one contiguous, aligned byte
array that `memcpy`s on swap-remove, which is both the cache-efficiency story
and the cross-image story (no destructor pointer into anyone's image).

**A `T*` from `GetComponent<T>()`, `AddComponent<T>()` or `Each<T>()`'s
callback is invalidated by any Add or Remove of `T` on ANY entity.** Hold an
`EntityID` or an `Entity` handle across a call that might mutate the pool;
re-fetch the pointer after.

---

## Tick phases

```cpp
enum ETickPhase { TP_PrePhysics, TP_Physics, TP_PostPhysics, TP_PostUpdate, TP_MAX };
```

`World::RegisterSystem(name, phase, fn, userData)` puts a system into one of
four buckets; `World::Tick()` runs them in order -- each phase opening with
the script driver's pass, so entity scripts always update before that phase's
systems -- and flushes the deferred destroy/remove/script queue **at the end
of each phase** -- not the end of the frame
-- so a `TP_PrePhysics` system that destroys an entity has it gone before
`TP_Physics` runs. `World::RefreshSpatialIndex()` (the BVH refit) runs at the
end of `TP_Physics`, after every system registered there.

Calling `DestroyEntity` / `RemoveComponent<T>` / `SetScript` from inside a tick defers to
the current phase's flush; calling them outside a tick is immediate. Never
destroy or remove from inside a pool walk you are not the observer of.

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

`World::SetActiveRegion(box)` is a shared "what gameplay currently cares
about" hint; an entity script's `CullOutsideActiveRegion` flag is the one
built-in consumer, skipping a scripted entity's `OnTick` when it is outside.

---

## Scripting

`ScriptEngine` owns one `lua_State`, bound to one `World` at a time
(`BindWorld` / `UnbindWorld`). The host (`Application`) owns exactly one,
lent to its `GameSession` -- `GameSession::GetScriptEngine()` -- which binds it
to each level's World as the level loads and unbinds it as it unloads.

**An entity's script is a file.** The file returns its class table; every
entity that names the path gets its own instance (`self`), with
`self.Entity` set:

```lua
-- Scripts/Mover.lua
return {
    TickPhase = TickPhase.PostPhysics,          -- optional, defaults to PrePhysics
    CullOutsideActiveRegion = true,             -- optional; overrides the entity's own flag
    OnStart   = function(self) end,
    OnTick    = function(self, dt) self.Entity:GetComponent("Transform"):Translate(dt, 0, 0) end,
    OnDestroy = function(self) end,
}
```

```lua
local e = World.CreateEntity("Player")
local t = e:AddComponent("Transform")
t:SetPosition(1, 2, 3)
e:SetScript("Scripts/Mover.lua")                -- also e:GetScript(), e:ClearScript(), e:SetName(n)

Events.On("EntityCreated", function(entity) print(entity:GetName()) end)
Events.Emit("PlayerScored", 10, "Player")       -- numbers, booleans, strings, entities; up to 4 args
```

How it runs:

- **Scripts update first.** `BindWorld` installs a `ScriptDriver` on the
  World, and `World::TickPhase` runs it at the start of every phase, before
  any registered system, whatever order anything was registered in. Within
  that pass, entity scripts run in the order their paths were set, then the
  Lua-defined systems (`World.AddSystem`).
- **Lazy.** `SetScript` only stores the path. The driver loads the file (once
  per path per bind, cached) and builds the instance at the start of the next
  phase it runs; `OnStart` follows in the script's own phase. A World with no
  driver -- the editor's -- just keeps the path, and so does a save.
- **Deferred inside a tick,** like `Destroy`: a script that replaces or clears
  a script sees the change at the end of the phase.
- **Errors are reported once.** A missing file, a syntax error or a file that
  does not return a table logs one error per path; an `OnStart`/`OnTick` error
  logs once and stops that instance.

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
    Bind.Field("Width", &RenderComponent::Width);      // GetWidth / SetWidth
    Bind.Field("Tint",  &RenderComponent::Tint);       // r, g, b[, a]
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
own field (`Props.Element` likewise, for the editor): there is no array type.

**Lua is compiled as C and its errors are `longjmp`.** A `lua_CFunction` in
this module's bindings may only have trivially destructible locals in scope
at any point that can still raise -- see `GameCoreInternal.h`'s header
comment. `CoreCallProtected` (used by every lifecycle call and by
`RunFile`/`RunString`) installs a traceback handler and logs through
`GAMECORE_LOG_ERROR`; a script error is never fatal to the engine.

The driver's release runs `OnDestroy` and releases the instance's Lua
registry ref when the entity is destroyed (before any of its components go)
or its script is replaced or cleared. `ScriptEngine::UnbindWorld()` walks
every still-live instance, every Lua-defined system, and every `Events.On()`
binding a script made, and releases all of them -- a Lua function cannot be
trusted to clean up before `lua_close`. Script paths survive an unbind; the
next `BindWorld` starts them again.

---

## World files

A `.nmworld` is text or binary, told apart by its first bytes
(`DetectWorldFileFormat`); `LoadWorldFile` reads either.

```
Levels/Sandbox.nmworld               text: <World Format="1"> listing entity GUIDs
Levels/Sandbox/<guid>.nmentity       text: one entity -- name, script, components
Levels/Cooked/Sandbox.nmworld        binary: what a game ships (CookWorldFile)
```

**Text is the editor's format** (`SaveWorldPackage`, editor builds only). One
small XML file per entity, named by its `EntityGuid`, so an edit to one entity
is a diff in one file, and output is deterministic: components sorted by type
name, floats in their shortest exact form, LF line endings. A save only
rewrites files whose bytes changed, commits the manifest last (temporaries,
then renames), and afterwards removes the files of deleted entities. A load
creates every listed entity first, then their components, so references
between entities resolve; files the manifest does not list are ignored.
`.gitattributes` keeps both extensions LF.

**Binary is the shipped format** -- `SerializeWorld`'s archive, one file.
`CookWorldFile(text, out)` produces it; the editor's `cook` console command
wraps that. A game build has no XML parser and refuses a text world.

**Serializers name their fields**, which is what lets one serializer write
both formats:

```cpp
GAMECORE_DEFINE_COMPONENT_SERIALIZER(RenderComponent, 1)
{
    Ar.Field("Width",  C.Width);
    Ar.Field("Tint",   C.Tint);
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
