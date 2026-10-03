# NimbleEngine

## Building

Two settings pick a build: the **target** (`NIMBLE_TARGET`) and the
**configuration** (`CMAKE_BUILD_TYPE`).

| Target | What it builds | `WITH_EDITOR` |
|---|---|---|
| Editor | every module, `NimbleEditor`, and the game | 1 |
| Client | the modules and the game; editor-only code compiled out | 0 |
| Server | same as Client for now | 0 |

| Configuration | Optimised | Debug info | `NIMBLE_SHIPPING` |
|---|---|---|---|
| Debug | no | yes | 0 |
| RelWithDebInfo | yes | yes | 0 |
| Shipping | yes (Release flags) | no | 1 |

Shipping is Client/Server only. Configuring Editor + Shipping fails on purpose.
Both `WITH_EDITOR` and `NIMBLE_SHIPPING` are always defined, to 1 or 0, and
reach every target through EngineCore.

### Games are modules

A game's code is a module DLL, not part of an executable. Sandbox builds
`SandboxGame.dll` (`SandboxGame.cpp`: levels, mode, state, systems) and a thin
`Sandbox.exe` launcher that calls `Application::ExecGame`. The launcher loads
the module named by `[Game] Module` in `Config/DefaultEngine.ini` and runs
`[Game] StartLevel`, or the module's own start level. The module implements
`NimbleEngine::IGameModule` with `NIMBLE_IMPLEMENT_GAME_MODULE` and registers its
levels on startup (`Engine/Source/Public/NimbleEngine/GameModule.h`). Nothing
links the game, which is what lets the editor load it for Play-in-Editor.

Sandbox's start level is **BrickBreaker** (`BrickBreaker.cpp`), the example
for the game framework: phases in the `GameState`, rules in the `IGameMode`,
players as `PlayerState` components, all reachable from Lua and script graphs
(`Engine/Source/GAMEFRAMEWORK.md`). `[Game] StartLevel=Sandbox` runs the older
demo level instead.

### CLion

Open `Games/Sandbox`. Its `CMakePresets.json` gives CLion five profiles:
**Editor - Debug**, **Editor - RelWithDebInfo**, **Client - Debug**,
**Client - RelWithDebInfo** and **Client - Shipping**. Each has its own build
tree (`Games/Sandbox/cmake-build-<preset>`). Enable the ones you want under
*Settings > Build, Execution, Deployment > CMake*. Pick a profile, then pick a
run target:

- `NimbleEditor` launches the editor (Editor profiles only).
- `Sandbox` launches the game (any profile).

Both run from `Games/Sandbox/Binaries/<Platform>_<Config>[_<Target>]`, e.g.
`Windows_Debug`, `Windows_Shipping_Client`. They switch to their own
directory on startup, so CLion's default working directory is fine and no run
configuration needs editing.

### Command line

```
cd Games/Sandbox
cmake --preset client-shipping
cmake --build --preset client-shipping
```

Or configure the engine directly: `cmake -S Engine -B build -DNIMBLE_TARGET=Client -DCMAKE_BUILD_TYPE=Shipping`.

## Conventions

### Namespaces

Each module owns one namespace: `EngineCore` (Core), `GameCore`, `RenderCore`,
`GDIPlatform` and friends (GDI), `GameWindowSystem` (GameWindow). A handful of
nested ones exist where a module has a distinct sub-area — `EngineCore::Memory`,
`EngineCore::Threads` — and GDI additionally carries module-private helper
namespaces (`VKOps`, `VKUtil`, `D3D12Ops`, `D3D12Util`, `MTLUtil`) that are not
part of any public surface.

**`using namespace` is permitted at file scope in `.cpp` files only**, placed
after all `#include`s. Never before an include: that changes how the included
header resolves names.

**Never in a header** — public or `*Internal.h`. Both are included transitively:
a directive in `Public/Core/CoreTypes.h` lands in every game under `Games/`, and
one in `Private/RenderCore/RenderCoreInternal.h` lands in every `.cpp` in the
module. The resulting ambiguity errors surface at the use site, in code whose
author did not write the directive.

**Headers qualify fully.** The `GameCore::` and `EngineCore::` prefixes
throughout `Public/RenderCore/RenderProxy.h`, `Public/GDI/GDI.h` and the rest
are not noise to be cleaned up — they tell a reader which module a type comes
from, and they are what keeps the forward-declaration discipline in
`Engine/Source/Public/NimbleEngine/NimbleEngine.h` legible. Leave them.

**Namespace aliases (`namespace GC = GameCore;`) are not used.** Every module
namespace here is a single short word, so an alias saves a few characters at the
cost of grep-ability: `GC::World` is not findable by searching for
`GameCore::World`. Aliases would earn their keep against deep `A::B::C::` chains;
this tree has none. They also cannot be forward-declared through, which the
`NimbleEngine.h` pattern above depends on.

Self-qualification inside a module — `Memory::Allocate` written from within
`namespace EngineCore`, as in `Public/Core/CorePointer.h` — is already the short
form and stays as is.

#### Which files carry a directive

As of this writing: the four module `Tests/SmokeMain.cpp`, `CoreMemoryExample.cpp`,
`Engine/Source/Private/NimbleEngine/NimbleEngine.cpp`, the GDI Vulkan
`VKTexture.cpp` / `VKRenderTarget.cpp`, and the four files the convention was
written alongside — `RenderCore/.../WorldBridge.cpp`, `RenderCore/.../RenderThread.cpp`,
`GDI/.../VulkanDynamicGDI.cpp`, `GDI/.../D3D12DynamicGDI.cpp`.

The two GDI backend files sit at global scope rather than inside a module
namespace, so their directives are wider than the RenderCore ones. That is still
contained — nothing includes a `.cpp` — but it is the reason those two carry a
note saying so.

Game code under `Games/` decides for itself; `SandboxGame.cpp` qualifies
everything deliberately, which doubles as a check that the public surface is
usable without any directive at all.
