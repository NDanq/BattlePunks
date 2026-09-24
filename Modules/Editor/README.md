# Editor

`NimbleEditor`, the Qt Widgets editor: an **Outliner** listing every entity in
the World, a **Details** panel showing the selected entity's editor-visible
component properties, and a **3D viewport** drawn by RenderCore through GDI --
the same renderer the game uses, into a native child window instead of an SDL
one.

Written for this repository. An executable, not a module: nothing links it.

---

## Position in the tree

```
NimbleEditor --> RenderCore --> GameCore --> EngineCore
             \-> GameCore
             \-> EngineCore::QtBridge (QColor <-> Color)
             \-> Qt6 Widgets
```

**Not GDI, not `GDI::QtBridge`.** GDI may live in exactly one image in the
process, `RenderCore.dll` (see `RenderCoreInternal.h`), and `GDIQtBridge` links
GDI statically. The viewport passes `QWidget::winId()` to `Renderer::Initialize`
as an `EngineCore::WindowHandle` instead -- the same native handle
`GDIQt::ToWindowHandle` would produce.

**Not `NimbleEngine`.** `Application` owns an SDL window and pumps its own event
loop on the main thread, which here belongs to Qt. `EditorContext` runs the same
frame sequence as `Application::RunFrame` (fence, collect, present, `EndFrame`)
from the viewport's timer, without `World::Tick` -- nothing simulates in the
editor yet.

## Pieces

| File | Role |
|---|---|
| `EditorContext` | Owns the World and Renderer; selection; signals every panel listens to |
| `EditorMainWindow` | Docks, menus (File / Edit / View), status bar; starts and stops the Renderer |
| `ViewportWidget` + `EditorCamera` | Native render surface, fly camera, click-to-select |
| `OutlinerPanel` | Entity list with component summary, filter, create/destroy |
| `DetailsPanel` + `PropertyWidgets` | One section per component, one editor per property type |

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
NimbleEditor [--rhi N] [world.nmworld]
```

The backend is `[Editor] RHI` in `Config/DefaultEngine.ini` (default 2, Vulkan),
overridden by `--rhi` -- a separate key from the game's `[Display] RHI`. With no
file argument the editor opens a demo scene. File > Open/Save read and write
bare `GameCore::SerializeWorld` archives (`.nmworld`), not SaveGame files;
script components keep their data but get no Lua instance, since the editor runs
no scripts.

**Viewport controls:** right-drag to look (with WASD / Q E to fly, Shift for
speed), middle-drag to pan, wheel to dolly, left-click to select, F to frame the
selection.

## Known limits

- No play mode, undo, gizmos or selection highlight in the viewport.
- The viewport has no depth sorting: RenderScene draws in submission order.
- Rotated primitives render sheared -- `Matrix4x4::InitRotation` in Core builds
  a non-orthogonal matrix; at 45 degrees about Z it collapses to a line. Picking
  follows what is drawn, and skips a transform too degenerate to invert.
