# RenderCore

The render stack: a render thread RenderCore owns and runs, a game-thread ->
render-thread command queue (`ENQUEUE_RENDER_COMMAND`), and `RenderProxy`, the
render-thread mirror of a GameCore entity that draws.

`RenderCore` is written for this repository, the same as `GameCore` and
`GameWindow` -- not a vendored subtree, free to use `nimble_add_module()`.

---

## Position in the tree

```
Host exe --> NimbleEngine --> GameCore  --> EngineCore
                \-----------> RenderCore --> GameCore --> EngineCore
                \-----------> RenderCore --> GDI      --> EngineCore
                \-----------> GameWindow --> EngineCore
```

**RenderCore is the only image in the process that links GDI.** GDI is a
STATIC library, so two images linking it would each get their own
`GDynamicGDI`, their own render-thread latch and their own command queue,
with nothing to say so -- see `RenderCoreInternal.h`'s own comment for the
detail. `NimbleEngine` no longer links GDI at all; every GDI call in the
engine now lives inside `RenderCore.dll`, reached only through this module's
own public headers, none of which names a GDI type.

`RenderCore` is linked **PUBLIC** into `NimbleEngine`, alongside `GameCore` --
`Renderer`, `RenderProxy` and `ENQUEUE_RENDER_COMMAND` are the game-facing
surface, so the dependency has to be visible past `NimbleEngine`, not hidden
behind it. See the PUBLIC link's own comment in `Engine/CMakeLists.txt` for
the full reasoning.

---

## File layout

```
Engine/Modules/RenderCore/
|-- CMakeLists.txt
|-- README.md                       (this file)
|-- DESIGN.md                       the long-form "why"
|-- Source/
|   |-- Public/RenderCore/
|   |   |-- RenderCoreAPI.h         RENDERCORE_API three-state export macro (always SHARED)
|   |   |-- RenderCoreLog.h         RENDERCORE_LOG / _WARN / _ERROR over CoreLog
|   |   |-- RenderCoreVersion.h     GetModuleName() -- the skeleton's one export; see DESIGN.md
|   |   |-- RenderTypes.h           ERenderBackend, FrameInfo, RenderTransform, RenderStats
|   |   |-- RenderView.h            RenderView POD + MakeOrthographic()
|   |   |-- RenderCommand.h         RenderCommandHeader, ENQUEUE_RENDER_COMMAND, IsInRenderThread, FlushRenderCommands
|   |   |-- RenderThread.h          class RenderThread: Start/Stop, Enqueue, EndFrame, WaitForFrame, Flush, stats
|   |   |-- RenderContext.h         the render-thread draw interface a proxy uses; NO GDI types
|   |   |-- RenderProxy.h           class RenderProxy, RenderProxyHandle, the PROXY_BODY/DEFINE_PROXY_COMPONENT macros
|   |   |-- RenderScene.h           render-thread container of proxies: Add/Remove/SetView/Render
|   |   |-- RenderComponents.h      RenderComponent, CameraComponent (GameCore components)
|   |   `-- Renderer.h              the facade a host owns: Initialize/Shutdown/BindWorld/EndFrame
|   `-- Private/RenderCore/
|       |-- RenderCoreInternal.h    the ONLY header that includes a GDI header
|       |-- RenderThreadInternal.h  the process-wide RenderThread locator
|       |-- RenderThread.cpp        the drain loop, GDI lifetime, markers, present
|       |-- RenderCommand.cpp       EnqueueRenderCommand, IsInRenderThread, FlushRenderCommands
|       |-- RenderContext.cpp       the shared unit quad + Basic shaders; DrawQuad; the material library
|       |-- RenderProxy.cpp         the fixed proxy-type table
|       |-- RenderScene.cpp         the proxy vector, culling, Render()
|       |-- RenderComponentProxy.h/.cpp   the quad proxy
|       |-- RenderComponents.cpp    component registration
|       |-- WorldBridge.h/.cpp      game-thread side: proxy lifecycle, extraction, view build
|       `-- Renderer.cpp
`-- Tests/
    `-- SmokeMain.cpp
```

Include paths are Source-relative -- `#include "Public/RenderCore/RenderThread.h"` --
matching GameCore, GDI and GameWindow exactly.

---

## The two rules the whole design falls out of

**RenderCore is ALWAYS SHARED.** There is no `RENDERCORE_SHARED` option,
matching GameCore's own stance. It owns process-global state -- the render
command queue, the render-thread identity behind `IsInRenderThread()`, the
proxy-type registry -- that must exist exactly once per process. A STATIC
RenderCore pulled PUBLIC through `NimbleEngine.dll` would give
`NimbleEngine.dll` and `Sandbox.exe` each their own copy of all three, so a
command enqueued from one image would sit in a queue the render thread
(latched against the *other* copy) never drains.

**RenderCore.dll is the only image allowed to link GDI.** GDI is STATIC.
`RenderCoreInternal.h` is the one header that includes a GDI header, and it
lives under `Private/` so it cannot be reached from outside -- exactly the
discipline `GameCoreInternal.h` states for `lua.h`. `RenderCoreSmokeTest`
links `RenderCore` and nothing else -- **not** `GDI::GDI` -- which is the
structural check that this stays true.

---

## One frame of lag, not a fence

Unlike a design where command submission is fenced behind the render thread
going idle, a game thread caller may `ENQUEUE_RENDER_COMMAND` at **any
point** during its tick -- there is no rule that the render thread must be
idle first. `RenderThread::EndFrame` enqueues a **frame marker**; a command
enqueued after that call sits behind the marker and is not touched until the
render thread reaches it on a later drain. Frame N's simulation still
overlaps the render thread draining frame N-1's commands -- that overlap is
the whole point of the split -- but drawing code is no longer funnelled
through one designated place.

```cpp
ENQUEUE_RENDER_COMMAND(UpdateColor)([Proxy, NewColor](RenderCore::RenderContext& Ctx)
{
    Proxy->SetTint(NewColor);
});
```

**Captures must be trivially copyable and trivially destructible** -- values,
POD, a `RenderProxy*` or `RenderProxyHandle`. No `std::string`, no `TRef`: the
payload is never destructed, only freed, so anything non-trivial would leak,
and a captured `TRef` would be refcount traffic off the collector's thread
with nothing to catch it. `ENQUEUE_RENDER_COMMAND`'s own `static_assert`
enforces both.

A command issued from inside another command already executing on the
render thread runs **inline**, synchronously -- not through the queue --
which is what preserves "enqueued from inside Execute() runs the same
frame" without the drain loop needing to know the difference.

---

## Drawing an entity

A `GameCore::World` is bound once, through `Renderer::BindWorld`. From then
on, drawing is one of three doors, none of which is an `OnRender` method:

```cpp
GameCore::World& World = App.GetWorld();
GameCore::Entity E = World.CreateEntity("Box");
E.AddComponent<GameCore::TransformComponent>();

RenderCore::RenderComponent Prim;
Prim.Width  = 32.0f;
Prim.Height = 32.0f;
Prim.Tint   = EngineCore::Color(255, 200, 40);
E.AddComponent(Prim);
```

Adding the component is the whole of it: `Renderer`'s `WorldBridge` sees the
addition through `World`'s own `OnComponentAdded` delegate, constructs a
`RenderCore::RenderComponentProxy` mirroring it, and from then on extracts the
component's current transform and bytes into that proxy once per frame.
Removing the component or destroying the entity removes the proxy the same
way, through `OnComponentRemoved`.

**A material** replaces the plain tint: set `Prim.Material` to a material
graph's name, e.g. `std::strcpy(Prim.Material, "Glow");`. The quad then draws
with the cooked `Materials/Glow.fsh`, its parameters from
`Materials/Glow.material`, and `Tint` passed to the shader as the Instance
Tint. A material that is missing or not cooked for the running backend is
warned about once and drawn as the plain tint. See DESIGN.md's "Materials" and
NodeGraph's README.

A camera works the same way -- the first entity with an active
`CameraComponent` wins, and its position comes from its own sibling
`TransformComponent`:

```cpp
GameCore::Entity Cam = World.CreateEntity("Camera");
Cam.AddComponent<GameCore::TransformComponent>();
Cam.AddComponent<RenderCore::CameraComponent>();
```

With no active camera, the view is an orthographic projection over the whole
viewport, centred at GDI's own default origin `(0, 1, -1)`.

**A game's own drawable type** registers the same way `RenderComponent`
does:

```cpp
// MyComponent.h
struct MyComponent
{
    GAMECORE_COMPONENT_BODY()
    RENDERCORE_PROXY_BODY()      // second, always -- right after the line above
    float Value = 0.0f;
};

// MyComponent.cpp
class MyProxy : public RenderCore::RenderProxy
{
public:
    MyProxy(GameCore::EntityID InOwner, const MyComponent& InInit) : RenderProxy(InOwner) { ApplyComponent(&InInit); }
    void ApplyComponent(const void* InBytes) override { Value = static_cast<const MyComponent*>(InBytes)->Value; }
    void Draw(RenderCore::RenderContext& Ctx) override { /* ... */ }
private:
    float Value = 0.0f;
};

RENDERCORE_DEFINE_PROXY_COMPONENT(MyComponent, "My", MyProxy)

// The macro's own generated factory (MyComponent_CreateProxy) has internal
// linkage -- exactly like GAMECORE_DEFINE_COMPONENT's StaticType(), it is
// meant to be defined once, in this one .cpp. RegisterProxyType therefore
// has to be called from HERE too (or through a small exported wrapper, the
// way RenderComponents.h's GetRenderComponentFactory() does for
// RenderComponent) -- not from an arbitrary caller elsewhere that only
// has MyComponent.h.
void RegisterMyComponentProxyType()
{
    RegisterProxyType(MyComponent::StaticType().ID, &MyComponent_CreateProxy);
}
```

Call `RegisterMyComponentProxyType()` (or whatever a game names its own
wrapper) once, before `Renderer::BindWorld`.

**A proxy is a promise to remove** -- the same rule `Delegate.h` states for a
binding: a proxy the factory allocates carries its defining image's tag, and
the image barrier would destroy it out from under the render thread the
instant that image unloads. A module that registers a proxy type
unregisters it before it unloads.

---

## Threading contract

Every `Renderer`/`RenderCommand.h`/`RenderProxy` free function or method is
either **game thread only** or **render thread only** -- stated on each one,
never both. `RenderProxyHandle::Resolve<T>()` and `GetRenderThreadContext()`
both `CORE_CHECK` `IsInRenderThread()` for exactly this reason: a proxy
pointer is only meaningful inside a render command's own lambda.

`WorldBridge`, `Renderer`'s own facade methods, and everything reached
through `GameSession::GetWorld()`/`GetRenderer()` run on the game thread --
the same thread that runs `IGameMode::OnTick`, matching `GameCoreSmokeTest` and
`GameWindowSystem`'s own single-thread contracts.

---

## Lua

`RenderCore`'s components have no Lua bindings yet -- `ScriptEngine`'s proxy
metatables are private to `GameCore`, and extending them is future work, not
in scope for this module's first cut. A game that wants `RenderComponent`
from a script currently has to bind it itself.
