# Editor

`NimbleEditor`, the Qt Widgets editor: an **Outliner** listing every entity in
the World, a **Details** panel showing the selected entity's editor-visible
component properties, a **3D viewport** drawn by RenderCore through GDI --
the same renderer the game uses, into a native child window instead of an SDL
one -- and a **Console** showing the process's log and running commands.

Written for this repository. An executable, not a module: nothing links it.

**Editor targets only.** The build has a target type, `NIMBLE_TARGET` (Editor,
Client or Server, default Editor), which defines `WITH_EDITOR` to 1 or 0 for
every module. Only an Editor configure adds this directory, and every source
here is additionally wrapped in `#if WITH_EDITOR`. The engine's editor-only
code is gated the same way, so a Client or Server build compiles none of it:

| Gated code | Where |
|---|---|
| Property registry and every `GAMECORE_DEFINE_COMPONENT_PROPERTIES` list | GameCore `ComponentProperties.h/.cpp`, RenderCore `RenderComponents.cpp` |
| `Renderer::SetViewOverride`, `DrawDebugLine`, `DrawDebugBox` | RenderCore `Renderer.h/.cpp` |
| The debug-line pass in `RenderScene::Render`, `RenderContext::DrawLine`, `DebugLine` | RenderCore |

In a Client or Server build the render loop always takes its view from the
World's CameraComponent and draws no overlay. Those builds write to
`Binaries/<Platform>_<Config>_<Target>` (e.g. `Windows_Debug_Client`) so their
DLLs, which lack the editor exports, never land on top of the editor's.

```
cmake -S Engine -B build                                  # Editor (default)
cmake -S Engine -B build-client -DNIMBLE_TARGET=Client    # no editor anywhere
```

---

## Position in the tree

```
NimbleEditor --> NimbleEngine --> RenderCore --> GameCore --> EngineCore
             \-> RenderCore, GameCore, GameWindow
             \-> EngineCore::QtBridge (QColor <-> Color)
             \-> Qt6 Widgets
             ..> <Game>.dll (loaded at Play, never linked)
```

**Not GDI, not `GDI::QtBridge`.** GDI may live in exactly one image in the
process, `RenderCore.dll` (see `RenderCoreInternal.h`), and `GDIQtBridge` links
GDI statically. The viewport passes `QWidget::winId()` to `Renderer::Initialize`
as an `EngineCore::WindowHandle` instead -- the same native handle
`GDIQt::ToWindowHandle` would produce.

**`NimbleEngine`, but not `Application`.** The editor uses `GameSession` and
`GameModuleHost` to host a game for Play-in-Editor. `Application` itself owns an
SDL window and pumps its own event loop on the main thread, which here belongs
to Qt; `EditorContext` runs the same frame sequence as `Application::RunFrame`
(tick, fence, collect, present, `EndFrame`) from the viewport's timer. While
editing nothing ticks -- the edited World never simulates.

## Pieces

Sources follow the engine's layout: class headers in `Source/Public/Editor/`,
implementation in `Source/Private/Editor/`, included as `"Public/Editor/..."` /
`"Private/Editor/..."`. `PropertyWidgets.h` is private -- only the Details panel
uses it.

| File | Role |
|---|---|
| `EditorContext` | Owns the World and Renderer; selection; signals every panel listens to |
| `EditorMainWindow` | Docks, menus (File / Edit / View), status bar; starts and stops the Renderer |
| `ViewportPanel` | Navigation toolbar (view presets, framing, bounds toggle, speed) over the viewport |
| `ViewportWidget` + `EditorCamera` | Native render surface, fly/orbit camera, click-to-select, selection bounds, axis gizmo |
| `OutlinerPanel` | Entity list with component summary, filter, create/destroy |
| `PlaySession` (private) | Play-in-Editor: the editor as an `IGameHost` -- game module, ScriptEngine, `GameSession` |
| `DetailsPanel` + `PropertyWidgets` | The entity's name and script path, then one section per component, one editor per property type, reset-to-default, add/remove component |
| `EditorLog` | EngineCore's log handler: a thread-safe, bounded history of every log line, still forwarded to Qt/the debugger |
| `EditorConsole` | Command registry; unknown words fall through to Core's `ExecConfigCommand` |
| `ConsolePanel` | The Console dock: filtered transcript over a command line with history and completion |

## Editor-visible properties

A component shows fields in the Details panel only if its type registers them,
next to its other registrations:

```cpp
#include "Public/GameCore/ComponentProperties.h"

GAMECORE_DEFINE_COMPONENT_PROPERTIES(PrimitiveComponent)
{
    Props.Field("Width",  &PrimitiveComponent::Width);
    Props.Field("Tint",   &PrimitiveComponent::Tint);
    Props.Flag("Visible", &PrimitiveComponent::bVisible);
    Props.Field("World",  &BoundsComponent::World, GameCore::PF_ReadOnly);   // shown, never written
}
```

The list is separate from the Lua bindings (`GAMECORE_DEFINE_COMPONENT_SCRIPT`)
on purpose, so a field can be editable without being scriptable and the other
way round. Supported types are the script-binding set: `float`, `double`,
`int32_t`, `uint8_t`, `bool`, flag (`uint8_t` 0/1), `Vector3`, `AABB`, `Color`
and `char[N]`. A component with no list still appears, marked as having no
editor-visible properties.

## Running

Built by default (`NIMBLE_BUILD_EDITOR`, ON) when Qt6 Widgets is found, into the
shared `Binaries/<Platform>_<Config>` directory; `windeployqt` copies Qt beside
it after each build.

```
NimbleEditor [--rhi N] [--play] [world.nmworld]
```

`--play` starts Play-in-Editor as soon as the viewport is up.

The backend is `[Editor] RHI` in `Config/DefaultEngine.ini` (default 2, Vulkan),
overridden by `--rhi` -- a separate key from the game's `[Display] RHI`. With no
file argument the editor opens the Sandbox scene -- the entities Sandbox's
level has once loaded (Marker, Camera, and the startup script's Scripted),
mirrored by hand from `SandboxGame.cpp` and `Scripts/Main.lua` until the editor
can load the game's own level. File > Open/Save read and write
bare `GameCore::SerializeWorld` archives (`.nmworld`), not SaveGame files;
an entity's script path is kept but gets no Lua instance, since the editor runs
no scripts.

**Viewport controls:**

| Input | Does |
|---|---|
| Right drag (+ WASD / Q E, Shift faster) | Look / fly |
| Alt + Left drag | Orbit the pivot -- the selection's bounds centre, else the focus point |
| Alt + Right drag, wheel | Dolly toward the pivot |
| Middle drag | Pan |
| Left click | Select the primitive under the cursor; on the axis gizmo, view from that axis (Shift: the opposite side) |
| F / Home | Frame the selection / the whole scene |

The toolbar above the viewport has the view presets (Persp, Front, Back, Top,
Bottom, Left, Right -- each orbits about the pivot to that axis), Frame
Selected / Frame All, All Bounds and a camera speed slider (1/8x to 8x). "Top"
looks from screen-up, which is world -Y: the editor keeps the game's +Y-down
convention.

**Overlays** are Renderer debug lines (`Renderer::DrawDebugLine` /
`DrawDebugBox`), not Qt painting -- nothing Qt draws can sit over the native
render surface. The selection is outlined in orange from its bounds: the
entity's `BoundsComponent::World` when it has one, else its Primitive quad, else
a small cube at its Transform. All Bounds adds every other entity's box in grey.
The XYZ gizmo sits bottom-left.

## Details panel

The top **Entity** section edits what an entity carries besides its
components: its **Name** (what the Outliner shows) and its **Script** -- a
`.lua` path, typed or picked with **...** (made relative to the working
directory when it is under it), cleared with **x**. Below that, a read-only
status says whether the script is running, which only happens in a playing
World, and the **Cull outside active region** checkbox sets the script's cull
flag. The script's class can override that flag when it loads.

Each property with a writable default shows an orange reset arrow to its right
while its value differs from the type's default-constructed one; clicking it
restores that property. A component header's **Reset** restores every writable
property of that component (per-run state such as a render proxy handle is left
alone), and its **x** removes the component. **+ Add Component** at the bottom
lists every type with a property list that the entity lacks; the new component
starts from its default-constructed bytes. A type opts out of that list with
`Props.NotAddable()`, for a component that only makes sense attached some other
way.

## Console

Docked below the viewport; **Ctrl+`** (View > Focus Console) jumps to its
command line. The transcript holds everything logged since startup -- Core,
GDI, GameWindow, GameCore, RenderCore and the editor, from any thread -- with
**Log / Warnings / Errors** toggles (each showing its count), a text filter and
optional timestamps. It follows new output while scrolled to the bottom.

The command line keeps a history (Up/Down) and completes command and config var
names (Tab). A line is split shell-style (quotes group words); the first word
is looked up among the commands, and anything else goes to Core's config
grammar, so every `NIMBLE_CONFIG_*` var in the process is reachable with nothing
registered:

| Command | |
|---|---|
| `help [command]` | List commands, or describe one |
| `echo <text>`, `clear` | |
| `cvars [filter]` | Config vars and values; `*` marks one set by an .ini or the console |
| `reset <Section.Name>` | A config var back to its compiled default |
| `Section.Name [value]` | Print a config var, or set it |
| `entities [filter]` | The scene's entities, the selection marked `*` |
| `select <name \| #index \| none>` | Select an entity (drives Outliner and Details) |
| `create [name]`, `delete [name \| #index]` | As the Edit menu |
| `frame [all]` | Frame the selection, or the scene |
| `scene <new \| sandbox>` | As the File menu |
| `stats` | The renderer's counters |

`EditorMainWindow::RegisterConsoleCommands` registers the scene commands;
`EditorConsole::Register` is the one call to add another.

## Play-in-Editor

**Play** (F5, the viewport toolbar, or `play` in the Console) runs the game in
the viewport: the module named by `[Game] Module` in `Config/DefaultEngine.ini`
(Sandbox's is `SandboxGame`), from `[Game] StartLevel` or the module's own start
level. It is the game's real code -- its level, mode, systems and startup script
-- in a `GameSession`, exactly as the game's launcher runs it; the editor only
stands in for `Application` as the host.

- **The edited scene is untouched.** The game loads its level into a World of
  its own. **Stop** (Shift+F5, Esc in the viewport, `stop`) unloads it and hands
  back the edited World and selection as they were.
- **Panels follow the game.** While playing, the Outliner, Details and Console
  commands act on the running World -- select an entity to watch its values
  change live; an edit applies to the run only. When the game replaces its
  World (Sandbox restores a save four seconds in) the panels switch to the new
  one and the selection clears.
- **The view is the game's camera.** **Eject** (F8, `eject`) switches to the
  editor camera, with its navigation, selection outline and overlays, to look
  around the running game; eject again to go back.
- **Rebuild and play again.** The game module is loaded at Play and unloaded at
  Stop, from a shadow copy, so a rebuilt game DLL is picked up by the next Play
  without restarting the editor. Building NimbleEditor builds the game module.
- New / Open and the `scene` command are disabled while playing; Save still
  writes the edited scene.
- The game ends a run itself with `GameSession::RequestQuit` -- the editor stops
  rather than exiting.

Not yet: game input. Games have no input API to feed, so a run receives no
keyboard or mouse; the SDL path proven by the input probe (View > SDL Input
Probe) is what will deliver it. The game's `[Display] FrameRateLimit` is
reported to it but not enforced -- the viewport's timer paces the run.

## Known limits

- No undo, or transform gizmos (move/rotate/scale handles).
- Play-in-Editor has no game input yet (see above).
- View presets stay in perspective; there are no orthographic views yet.
- The viewport has no depth sorting: RenderScene draws in submission order.
- Rotated primitives render sheared -- `Matrix4x4::InitRotation` in Core builds
  a non-orthogonal matrix; at 45 degrees about Z it collapses to a line. Picking
  follows what is drawn, and skips a transform too degenerate to invert.
