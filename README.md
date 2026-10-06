# NimbleEngine

## Building

Two settings pick a build: the **target** (`NIMBLE_TARGET`) and the
**configuration** (`CMAKE_BUILD_TYPE`).

| Target | What it builds | `WITH_EDITOR` |
|---|---|---|
| Editor | every module, `NimbleEditor`, and the game | 1 |
| Client | the modules and the game; editor-only code compiled out | 0 |
| Server | same as Client for now; needs NetCore | 0 |

| Configuration | Optimised | Debug info | `NIMBLE_SHIPPING` |
|---|---|---|---|
| Debug | no | yes | 0 |
| RelWithDebInfo | yes | yes | 0 |
| Shipping | yes (Release flags) | no | 1 |

Shipping is Client/Server only. Configuring Editor + Shipping fails on purpose.

Every configuration but Shipping runs **uncooked** (`WITH_UNCOOKED=1`). The
build stages the project's text worlds and prefabs, and a Debug or
RelWithDebInfo Client or Server cooks each one the first time it loads it,
again whenever it changes, into `Intermediate/Cooked/` beside the
executable. Shipping reads only content cooked ahead in the editor (`cook`,
`cook prefabs`). Games load content by its source path
(`GameCore::LoadContentWorld`), so the code is the same for every build.
Both `WITH_EDITOR` and `NIMBLE_SHIPPING` are always defined, to 1 or 0, and
reach every target through EngineCore.

### Networking

`NIMBLE_WITH_NETCORE` (on by default) builds NetCore and defines
`WITH_NETCORE=1` everywhere. NetCore provides:

- replication and RPCs
- each game's dedicated server, `<Game>Server`
- `<Game> -connect=host:port`

A game that never networks turns it off before adding the engine, and
nothing networked is compiled or linked:

```cmake
set(NIMBLE_WITH_NETCORE OFF CACHE BOOL "" FORCE)
```

See `Engine/Modules/NetCore/README.md`.

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

### LightLab

`Games/LightLab` is a second game, laid out as File > New Project lays one out: a fly-around scene
showing the lights and shadows (RenderCore's `RENDERPIPELINE.md`, "Lights" and
"Shadows"). Open it in CLion or configure it standalone the same way; it has
its own `CMakePresets.json` and stages into `Games/LightLab/Binaries`. The
level is `Games/LightLab/Content/Levels/LightLab.nmworld`: the editor opens it
(`[Editor] StartupWorld`), and the game loads it and finds its Player, Sun,
SweepSpot and OrbitLight by name, so edits saved in the editor show in the
next Play. A Debug or RelWithDebInfo Client cooks it the first time it loads it;
a Shipping build reads the copy cooked into `Content/Levels/Cooked/`. Without
a level file the game builds the same scene in code; `[LightLab]
ExportLevel=<path>` in an editor build writes that code-built scene out. Mouse
looks, WASD moves, E/Q (or Space/Ctrl) rise and sink, Shift is faster, T cycles
the sun's shadow technique, F toggles shadows and L switches between baked and
live lighting. Escape (or the gamepad's Start) pauses under a menu
(`Content/UI/PauseMenu.nmui`, Engine/Modules/UI/README.md): Resume, Escape
again or the pad's East goes back to flying, Quit ends the game.

Two post process volumes (`Content/PostProcess`, RENDERPIPELINE.md's "Post
process") cover the yard. `Global.pp.nmgraph` has no Bounds, so it applies
everywhere: ambient occlusion in the corners, a soft bloom and FXAA. `Arch.pp.nmgraph` sits in a Bounds box round
the arch and blends in over 300 units as the camera nears it: depth of field
past the arch, motion blur, and a warmer, stronger glow.

A colonnade down the right of the yard is four instances of a prefab file,
`Content/Prefabs/Column.nmprefab`: a Cylinder shaft (the engine's
`Cylinder.nmmesh`) on a Box base under a Box capital. The level stores them as
instances (`.nmprefabinst`). A Debug or RelWithDebInfo Client cooks it as it
loads; a Shipping build reads the cooked copy in `Cooked/Content/Prefabs/`,
which the editor console's `cook prefabs` regenerates after the prefab changes.

A road flare stands in the front of the yard, showing GPU particles
(RENDERPIPELINE.md, "Particles"): a red stick, a flame ball whose emissive
flickers far past 1 so Bloom spreads it, a red point light, and its smoke -- a
**Particle System** component running `Content/Particles/FlareSmoke.nmparticle`,
soft puffs (`FlareSmoke.material.nmgraph`, Translucent Unlit) born red and
turning black as they rise, drift and spread. Double-click it in the Content
Browser to open it in the editor's Particle Editor. Across the front of the
yard is a sample of each particle type (`Content/Particles/`): `Sparks`
(stretched sprites in bursts) by the left pillars, `Debris` (spinning box
meshes and a dust puff, bursting) by the ball, `Laser` (two jittering beams)
strung between the right pillars by its component's Beam Target, and `Trail` (a
ribbon) behind a point that flies in a circle while playing.
It is simulated by compute shaders, so it shows on Vulkan only for now, and it
animates in the editor's viewport as well as in Play.

`Content/Prefabs/ReflectionDemo.nmprefab` shows scene capture
(RENDERPIPELINE.md, "Scene capture"). It is a mirror pool with a chrome ball
over it:

- **MirrorPool:** a Plane with a Planar Mirror probe into
  `Content/RenderTargets/MirrorPool.nmrt2d`, captured every frame.
- **ChromeBall:** a Sphere with an Omnidirectional probe into `ChromeBall.nmrt3d`,
  captured every 0.25 s.
- **Materials:** `Mirror` and `Chrome` both sample their target through a Reflect
  node.

Drag the prefab into the level from the Content Browser, or bake its captures
with World > Bake Scene Captures. A cooked copy sits beside Column's.

`Content/Prefabs/PBRShowcase.nmprefab` shows the physically based shading
(RENDERPIPELINE.md, "HDR and tone mapping" and "Image-based lighting"):

- **The spheres:** three rows (white dielectric, gold, silver) by five
  roughnesses, 0 to 1. All use one `PBR` material, its Base Color, Metallic
  and Roughness set per sphere.
- **The rest:** a dark backdrop, and a Reflection Probe lighting them from
  among them.

The level's `Global.pp.nmgraph` tone maps the HDR scene with ACES at -1 EV. The
bricks on the Column prefab take their normal map through a Normal Map node,
with their data maps imported linear.

For the full effect, set the level's Sky Light **Environment** to
`Content/Textures/Sky/ProceduralSky.nmtexture`. That is a generated HDRI: a
blue sky with a sun, glowing toward it, over warm ground. The sky then lights
everything and is drawn behind the scene.

A second level, **BakedLighting** (`Content/Levels/BakedLighting.nmworld`;
open it in the editor and Play, or set `[Game] StartLevel=BakedLighting` for the
game), shows baked lighting
(RENDERPIPELINE.md, "Baked lighting"). It is a Cornell-style room:

- **Static, baked:** red and green walls, a white floor, ceiling and back wall
  with a window, two boxes and a capsule, lit by a sun through the window, a
  warm lamp and the sky. Its bake is checked in beside it (`Lighting/`):
  soft shadows, the lamp's light bounced round the room, the walls' colour
  on the floor.
- **Dynamic, lit by the light probes:** a ball circling the room
  (LightLab.Swing) and a capsule.

**L** (or Show > Lighting in the editor's viewport) switches between the bake and live
lighting, in either level. `Games/LightLab/Tools/make_baked_lighting_level.py`
writes the world; Build > Bake Lightmaps (or the headless line in the editor's
README, "Build menu") bakes it again.

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

### Coordinate system

**Left-handed, +X right, +Y up, +Z into the screen** (away from the viewer).
A camera at rest looks along +Z. The 2D view is one unit per pixel, with the
camera's position at the bottom-left corner of the screen, so a 1280 × 720
view spans x 0..1280 and y 0..720 (`RenderCore/RenderView.h`). Screen-space
values are the exception, the way the platform gives them: mouse positions
(`Input.GetMousePosition`) are window pixels with y down, and so is a stick's
`Gamepad.LeftY` axis.

- A positive rotation about +Z turns +X toward +Y: counter-clockwise on screen.
- glTF meshes are converted on import by flipping z, with the winding
  (`GltfImporter.cpp`). The engine's primitive meshes are made the same way
  (`Engine/Scripts/generate_primitive_meshes.py`).
- GDI's own `GDIView`, the Odessa 2D path, stays Y-down. RenderCore builds its
  own views and does not use it.

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
