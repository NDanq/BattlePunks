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
| Property registry and every `GAMECORE_DEFINE_COMPONENT_PROPERTIES` list | GameCore `ComponentProperties.h/.cpp`, each component's own .cpp under GameCore and RenderCore `Components/` |
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
             \-> RenderCore, GameCore, GameWindow, NodeGraph
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
| `EditorContext` | Owns the World and Renderer; selection; the undo history; signals every panel listens to |
| `EditorHistory` (private) | Undo / redo commands and the entity and component snapshots they hold |
| `EditorMainWindow` | Docks, menus (File / Edit / View / Play / Build / Help), status bar; starts and stops the Renderer |
| `EditorTheme` (private) | `Editor.Theme`: light, dark or the system's, applied live through Fusion and an explicit palette |
| `ProjectSettingsDialog` + `ConfigIniFile` (private) | Edit > Project Settings: the project's `DefaultEngine.ini` as pages of settings, edited line by line so its comments stay |
| `SettingsDialog` (private) | Edit > Editor Settings: the `[Editor]` config vars as a form, saved to the user's `EditorSettings.ini` |
| `SourceBuild` (private) | Build > Compile Source: runs `cmake --build` on the editor's own build tree (`EditorBuildTree.h.in`) in the background, logging its output |
| `AboutDialog` (private) | Help > About: the commit and build this editor came from (`EditorBuildInfo.h.in`, stamped by Core's `GitVersion.cmake` on every build), plus configuration, target, compiler, Qt and renderer; Copy to Clipboard for bug reports |
| `ViewportPanel` | Navigation toolbar (view presets, framing, the Show menu, speed) over the viewport |
| `ViewportWidget` + `EditorCamera` | Native render surface, fly/orbit camera, click-to-select (lights by their position), selection bounds, axis gizmo, light gizmos (a point light's sphere, a spot's cone, the sun's arrow, a scene capture probe's mirror rectangle and normal or its box and reach; all lights or the selected one, Show > Editor Gizmos > Light Gizmos), a perspective game camera's frustum |
| `EditorPrimitives` (private) | World > Add Primitive and the Outliner's Add Primitive: the engine's Box, Sphere, Cylinder, Capsule or Plane mesh on a new entity, sized to see, in one undo step |
| `EditorLights` (private) | World > Add Light and the Outliner's Add Light: a light entity in one undo step, aimed down when it has a direction; also a Reflection Probe, and a Scene Capture (Omni) or (Mirror) probe |
| `Content/RenderTargetAssets` (private) | Render target assets: New Render Target 2D / 3D (Cube), the settings dialog, World > Bake Scene Captures |
| `WorldSettingsDialog` (private) | World > World Settings: the edited world's `GameCore::WorldSettings` -- baked lighting on or off, lightmap density, bounces, samples, probe spacing, lightmap size -- one undo step, saved with the world |
| `Lighting/LightBaker`, `LightBakeBVH`, `LightBakeScene` (private) | The light baker: a multithreaded CPU path tracer over a bake scene -- lightmap layout, texels, direct and bounced light, filtering, light probes (RenderCore's RENDERPIPELINE.md, "Baked lighting"). No World, no Qt; built with optimisation even in Debug; tested by `NimbleEditorLightingTests` |
| `Lighting/LightBakeWorld`, `BakeLightmaps` (private) | A World's static meshes and lights to a bake scene, and the bake to files beside the world (`Levels/<World>/Lighting/`, staged too); Build > Bake Lightmaps' worker thread and progress dialog |
| `Content/LightmapUVs` (private) | Lightmap UVs (UV1) for a mesh without: xatlas (`External/xatlas-f700c77`) charts and packs it, every vertex keeping its index |
| `TransformGizmo` | Move / rotate / scale handles on the selection: drawing, hit testing and drag math, with no Qt input or World access of its own |
| `OutlinerPanel` | Entity hierarchy with component summary, filter, create, drag to reparent; Ctrl/Shift multiple selection, F2 renames in the row, and the edit actions (below) in its context menu |
| `PlaySession` (private) | Play-in-Editor: the editor as an `IGameHost` -- game module, ScriptEngine, `GameSession`, `InputRouter` |
| `ViewportSDLInput` (private) | The viewport as an SDL window (focus hand-over, text forwarding, pumping), for the game's input and the input probe |
| `DetailsPanel` + `PropertyWidgets` | The entity's name and **Mobility** (Dynamic / Static / Stationary -- applies to every selected entity, one undo step), then one section per component, one editor per property type, lists for array properties, reset-to-default, add/remove component |
| `Graph/GraphEditorWindow` (private) | The graph editor: one window, a closable, movable tab per open `.nmgraph` (Content Browser right-click > New Material / Script Graph, File > Open Graph, or double-click in the Content Browser). Ctrl+Tab steps through them; closing the window offers to save each |
| `Graph/GraphEditorTab` (private) | One graph, with its own menus and docks. It has the canvas, Details, the generated code (live) and Messages; clicking a code line or a message selects its node. Save writes the graph with its generated code inside it, `<gen_code>` (see `Modules/NodeGraph`); nothing is written beside it |
| `Graph/GraphDocument` (private) | One open graph and its own undo history, separate from the scene's; every edit is a before/after snapshot |
| `Particles/ParticleEditorWindow`, `ParticleEditorTab` (private) | The **Particle Editor**: a tab per open `.nmparticle`, its emitters and settings beside a live preview in a World of its own (see Particle Editor) |
| `Particles/ParticleDocument`, `ParticleEmitterPanels`, `ParticleAssets` (private) | The open system and its undo history (whole-system snapshots), the Emitters and Details docks, and particle systems as project files: new, open, close. Tested offscreen by `NimbleEditorParticleTests` |
| `UI/UIEditorWindow`, `UIEditorTab` (private) | The **UI Designer**: a tab per open `.nmui` (Content Browser double-click, or right-click > New UI...). The canvas is drawn by the game's UI renderer at a design resolution (presets: 720p to 4K, ultrawide, portrait, custom), with the selection's handles drawn over it; Palette and Hierarchy on the left, Details on the right; Preview runs the document as in a game, logging its actions. Engine/Modules/UI/README.md, "Editor: UI Designer" |
| `UI/UIDesignDocument`, `UIDesignerViewport`, `UIDetailsPanel`, `UIHierarchyPanel`, `UIPalettePanel` (private) | The designer's document (the asset, its undo history as before/after snapshots, the selection), its canvas (an `EditorViewportClient` submitting its own UI list), and its panels, built from the UI module's widget registry |
| `Graph/GraphScene`, `GraphView`, `GraphItems`, `NodePalette`, `GraphDetailsPanel` (private) | The canvas: nodes, links and comments. Drag between pins to link; Alt+click a pin breaks its links; drag a linked input's link off to move it. Right-click or Tab opens the node palette, and a link dropped on empty space opens it filtered. Wheel zooms, middle-drag pans |
| `Graph/GraphAssets` (private) | Graphs as project files: where new ones go, compiling into `<gen_code>` on save, a script graph **staged beside the executable** so the next Play runs it without a build, the background glslc check, Build > Cook Content regenerating every graph's `<gen_code>`. A material is named by its graph's path, **cooked for the viewport's backend** from the compiled code, then reloaded into the renderer |
| Material graph **Preview** dock, Material > Live Preview, Apply to Selected Entity | In a material graph's window. The **Preview** dock shows the material on one of the engine's meshes -- **Shape**: Sphere (the default), Box, Cylinder, Capsule or Plane, to orbit (drag, wheel, double-click resets) -- or a flat Quad, drawn by the viewport's own renderer into an offscreen target and read back (`Renderer::RequestMeshPreview`, `RequestMaterialPreview`), so it is the backend and cooked shader the level uses. A PostProcess material always fills it. It has an Instance Tint picker and **Animate** (keep drawing, for materials that use Time). Live Preview (on by default) cooks every clean recompile, refreshing the dock and every primitive in the level whose `Material` is the graph's name; closing with the change discarded puts the saved one back. Apply to Selected Entity sets the selection's RenderComponent Material (adding one if needed), undoably |
| `EditorLog` | EngineCore's log handler: a thread-safe, bounded history of every log line, still forwarded to Qt/the debugger |
| `EditorConsole` | Command registry; unknown words fall through to Core's `ExecConfigCommand` |
| `ConsolePanel` | The Console dock: filtered transcript over a command line with history and completion |
| `ContentBrowserPanel` | The Content Browser dock: the project's files, organised, opened, and imported into, and the engine's content beside them (see Content Browser) |
| `Content/MeshViewerWindow` (private) | A `.nmmesh` opened from the Content Browser: the mesh, its statistics, and a material per section (see Content Browser) |
| `Content/TextureViewerWindow` (private) | A `.nmtexture` opened from the Content Browser: the image by mip level and channel, its statistics, and its settings -- sRGB, filter, wrap, mip generation, compression (see Content Browser) |
| `Content/MeshPreviewView` (private) | The image both of those draw into: one renderer preview at a time at the widget's size, drag to orbit, wheel to zoom |
| `Content/ImportWizard` (private) | File > Import and drops from outside: the wizard that turns glTF and images into `.nmmesh` / `.nmtexture` |
| `Content/AssetImport`, `GltfImporter` (private) | The conversions themselves, no widgets (cgltf, `External/cgltf-1.15`, reads glTF); tested by `NimbleEditorImportTests` |
| `Content/ContentPaths` (private) | Project files as the engine names them: runtime-relative paths (the engine's content as `Engine/Content/...`), material names, staging beside the executable |
| `Project/NewProjectWizard` (private) | File > New Project: a new game in `Games/<Name>` from `Engine/Templates/Game` -- module, level, game mode, game state, launcher, `Config/`, `Scripts/`, `Content/` -- configured standalone, so the engine is never edited |
| `Project/ProjectTemplate` (private) | The copy itself, no widgets: name checks, `@PROJECT_NAME@` / `@ENGINE_DIR@` / `__PROJECT__` replaced; tested by `NimbleEditorProjectTests`, which also checks `Engine/Binaries/NewProject.cmake` (the same, without the editor) writes an identical project |

## Content Browser

Docked below the viewport, tabbed with the Console: the project's own folder
(the one holding `Config/DefaultEngine.ini`, e.g. `Games/Sandbox`) as a folder
tree beside the current folder's contents. Build trees, `Binaries` and
dot-folders are hidden. Images and `.nmtexture` files show thumbnails, and
`.nmmesh` files a render of the mesh with its own materials (drawn by the
viewport's renderer, one at a time, so they fill in a moment later); hovering
an asset shows what it is (vertices and triangles, or size and colour space)
and where it was imported from.

**Engine Content**, a second tree below the project's, is the engine's own
`Engine/Content`: its meshes (`Meshes/Box`, `Sphere`, `Plane`, `Cylinder`, `Capsule`), materials
(DefaultLit) and shaders. It is read from the source tree the editor was built
from, else from the copy staged beside the executable, and is read-only here --
no rename, delete, move, new folder, reimport or drop into it. Drag its meshes
into the level or onto a Mesh field like the project's own: they are named
`Engine/Content/Meshes/Box.nmmesh`, staged beside every game by the engine's
build. Dragging one into a project folder copies it there.

| Do | How |
|---|---|
| Navigate | Click a folder in the tree, double-click one in the list, Back, Up (Backspace), or a segment of the path bar |
| New folder | The toolbar, the context menu, or Ctrl+Shift+N; type its name |
| Rename | F2, or the context menu |
| Delete | Del: confirmed, then moved to the Recycle Bin -- never deleted outright |
| Move | Drag items onto a folder (in the list) |
| Open | Double-click: a graph opens in its window, a world in the viewport (not while playing), a mesh in the mesh viewer, a texture in the texture viewer, a UI document (`.nmui`) in the UI Designer, a particle system (`.nmparticle`) in the Particle Editor, anything else with the system's own application |
| Import | Drop files from Explorer, the toolbar's Import..., or File > Import (Ctrl+I) |
| Reimport | The context menu on a `.nmmesh` / `.nmtexture`: the same source, the same options |

**Companions travel together.** A world's entity folder is renamed, moved and
deleted with it. (A graph has no generated files beside it: its code is inside
it.) A graph open in a window and the
world open in the editor are refused, as are the project folder and `Config/`.
Moving or renaming an asset does not update what names it -- a
RenderComponent's `Mesh`, or a material slot naming a graph, still holds the
old path.

**Importing.** A drop of files from outside the project opens the **Import
Wizard** for the meshes (`.gltf`, `.glb`) and images (whatever Qt reads) among
them; other files are copied in as they are, after asking. Into the folder
dropped on, or the one shown -- `Content/` when the browser is at the root.

1. **Files**: each file's kind and the asset name it gets (editable), and a
   warning beside one that will replace an existing asset.
2. **Mesh options**: scale (glTF is in metres; 100 by default), convert axes
   (glTF is right-handed and the engine left-handed, both +Y up: z flips, with
   the winding, so the model faces the default camera), generate missing normals, generate
   lightmap UVs (UV1, for baked lighting) where the file has no `TEXCOORD_1`, combine the scene into
   one mesh or one per glTF mesh, and import the textures it uses -- listed.
3. **Texture options**:
   - sRGB, plus data maps by name: an image whose name has a word like normal,
     rough, metal, AO, height, mask or ORM imports linear whatever sRGB says.
     The decision is recorded, so a reimport keeps it.
   - filtering, wrapping, and largest size (bigger images are scaled down);
   - flip vertically;
   - mipmaps: Box by default, Kaiser, or None.

   A Radiance **.hdr** (an HDRI) imports as an RGBE8 texture: linear, point
   sampled, no mips. It is for a Sky Light's Environment.
4. **Import**: on a worker thread, with a log. Each asset is written whole or
   not at all, staged beside the executable at once, and logged to the Console.

The options start from Edit > Editor Settings > Import and are saved back there.
Every asset records its source and options (relative to the project), which is
what Reimport uses; a texture that was embedded in a GLB is found in it again.

**Using a mesh.** Drag a `.nmmesh` onto the viewport: a new entity named after
it, with a RenderComponent drawing it, lands where the cursor meets the z = 0
plane (or 600 units out) -- one undo step, selected. Or drag it onto a
RenderComponent's **Mesh** field in Details (or pick one with **...**); a
material graph (or its `.material`) dropped on **Material** sets that. Selection
outlines fit the mesh's box, and a click picks the mesh by its triangles.

**The mesh viewer.** Double-clicking a `.nmmesh` opens it in its own window
(again: the same window, brought forward): the mesh as the renderer draws it,
framed by a camera to orbit (drag), zoom (wheel) and reset (double-click, or
Reset View) under a fixed studio light; its vertices, triangles, sections, size
and source; and a **Material** per section -- typed with completion from the
project's and the engine's materials, picked from the list, or a material
graph dropped from the Content Browser. Empty is DefaultLit. A pick shows at
once. **Save** (Ctrl+S) writes them into the mesh as its sections' own
materials -- what a RenderComponent draws a section with when its slot is
empty -- and restages it, so the level shows them; Revert drops the picks;
closing with unsaved picks asks. Reimporting the mesh replaces them with the
source's. The engine's meshes preview only. **Animate** keeps drawing, for
materials that use Time. The preview shows the first eight sections' picks.

**The texture viewer.** Double-clicking a `.nmtexture` opens it in its own
window (again: the same window, brought forward).

- **The image:** fitted to the view, zoomed with the wheel about the cursor,
  panned by dragging, and fitted again by a double-click. A Mip level picks the
  level shown and Channels picks RGBA, RGB, R, G, B or A (one channel shows as
  grey). Transparency shows over a checkerboard.
- **Statistics:** size (flagging one that is not a power of two), sRGB or
  linear, mip levels and how they were made, size on disk, GPU memory (every
  level), and what that would be compressed. Also whether its alpha is opaque,
  cut-out or blended, and the source it was imported from.
- **Settings:**
  - sRGB, Filter and Wrap.
  - **Mip generation:** None, Box (a 2x2 average) or Kaiser (a sharper
    windowed sinc). Mips are built from level 0 down to 1x1, in linear light
    for an sRGB texture, with edges that follow Wrap.
  - **Compression:** None, BC1 (RGB), BC3 (RGBA), BC4 (one channel), BC5 (two
    channels: normal maps) or BC7 (RGBA, the best quality).
- **As compressed** (View): the blocks decoded, as the GPU samples them. It
  encodes the texture at once if need be, and Save keeps those blocks.

A change shows at once: the chain is rebuilt in memory. **Save** (Ctrl+S)
writes the file and restages it, so the level draws the new chain. It also
records the settings in the texture's import options, so Reimport keeps them.
Revert drops the edits; closing with unsaved edits asks. The engine's textures
view only.

The renderer samples a texture's chain trilinearly with a Linear filter (on
Vulkan and OpenGL; D3D12 and Metal still upload level 0 alone). A compressed
texture is encoded on import and on Save by bc7enc_rdo (RenderCore's
`TextureCompression.h`), every level, and the file keeps the blocks beside the
RGBA8 source, so changing a setting encodes again from the source, never from
the blocks. The renderer uploads the blocks where the backend samples BC
formats (Vulkan, OpenGL) and decodes them to RGBA8 where it does not (D3D12,
Metal for now).

**Shipping.** The editor stages what it imports; a clean build of the game
stages `Content/**/*.nmmesh` and `*.nmtexture` itself (Sandbox's CMakeLists,
`SandboxStageContent`). A material samples a texture through a Texture Object
and Texture Sample nodes (NodeGraph's README); drop a `.nmtexture` on a Texture
Object's Texture field in Details.

**Image-based lighting.** World > Add Light > **Reflection Probe** adds a probe:
- **Gizmo:** a small diamond. When selected, it also shows its influence: three
  rings of its Radius, or its Box Extent.
- **What it does:** Lit surfaces within reach take their reflections and
  ambient light from it (RenderCore's RENDERPIPELINE.md, "Image-based
  lighting").
- **The sky:** a Sky Light's **Environment** takes a texture (picked,
  completed or dropped): an HDRI imported from a .hdr. It lights the scene and,
  with Draw Background, is drawn behind it.
- **Scope:** both need an HDR scene colour (every GPU backend; tested on
  Vulkan). The Texture Viewer shows an HDRI tone mapped.

**Render targets.** The context menu's **New Render Target 2D...** and **New
Render Target 3D (Cube)...** make a `.nmrt2d` (512 square) or a `.nmrt3d`
(128 per face). Double-click one to set its size, filter, wrap and clear colour,
or to clear its bake. The thumbnail shows the bake, if there is one.

- **Filling one:** a Scene Capture Probe component renders into it (RenderCore's
  RENDERPIPELINE.md, Scene capture). Add one from World > Add Light > Scene
  Capture (Omni) or (Mirror), then set its **Target** with the render-target
  picker, completion or a drop.
- **Sampling one:** set a material graph's Texture Object, or a **Reflect**
  node's Target, to the render target.
- **Baking:** **World > Bake Scene Captures** fires every probe in the level
  once, reads each target back and saves the image into its file as the bake.
  A game shows the bake until its own first capture, and keeps it when the
  probe's Frequency is Manual.
- **Staging:** a clean game build stages `Content/**/*.nmrt2d` and `*.nmrt3d`.

## Editor-visible properties

A component shows fields in the Details panel only if its type registers them,
next to its other registrations:

```cpp
#include "Public/GameCore/ComponentProperties.h"

GAMECORE_DEFINE_COMPONENT_PROPERTIES(RenderComponent)
{
    Props.Flag("Visible", &RenderComponent::bVisible);
    Props.Field("Mesh",   &RenderComponent::Mesh, GameCore::PF_AssetMesh);
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
then the user's settings (see Editor settings), then `--rhi` -- a separate key from the game's `[Display] RHI`. With no
file argument the editor opens the project's `[Editor] StartupWorld` (a path
relative to the project, e.g. LightLab's `Content/Levels/LightLab.nmworld`), viewed
from that world's game camera; with none set, or no such file, it opens the Sandbox scene -- the entities Sandbox's
level has once loaded (Marker, Camera, and the startup script's Scripted),
mirrored by hand from `SandboxGame.cpp` and `Scripts/Main.lua` until the editor
can load the game's own level. File > Save writes a text
world (`GameCore::SaveWorldPackage`): the `.nmworld` manifest plus one
`.nmentity` XML file per entity in a sibling folder of the same name, so a
change to one entity is a diff in one file. File > Open reads text or cooked
(binary) worlds; a cooked one opens without a save path, so Save asks where to
write it rather than overwriting cooked output. Neither is a SaveGame file.
An entity's script path is kept but gets no Lua instance, since the editor runs
no scripts.

**Viewport controls:**

| Input | Does |
|---|---|
| WASD / Q E (Shift faster) | Fly -- forward, left, back, right; down, up -- whenever the viewport has the focus (not under Ctrl or Alt) |
| Right drag (+ WASD / Q E) | Look while flying |
| Alt + Left drag | Orbit the pivot -- the selection's bounds centre, else the focus point |
| Alt + Right drag, wheel | Dolly toward the pivot |
| Middle drag | Pan |
| Left click | Select the primitive under the cursor -- a prefab instance whole, by its root (its parts are selected in the Outliner); Ctrl+click toggles it in or out of the selection, Shift+click adds it; on the axis gizmo, view from that axis (Shift: the opposite side) |
| Right click (no drag) | Context menu: Cut, Copy, Paste, Duplicate, Rename, Delete -- for what is under the cursor (selected first) or the selection |
| Left drag on a handle | Move / rotate / scale the selection -- every selected entity together, as one undo step (Ctrl inverts Snap); Esc or a right click cancels the drag |
| 1 / 2 / 3 / 4 | Handles off (Select) / Move / Rotate / Scale |
| X | Handles in world or local space |
| F / Home | Frame the selection / the whole scene |

**Selection and editing.** Any number of entities can be selected; the last
one picked is the *primary*, which the Details panel shows and the handles sit
on (its box brighter). Cut (Ctrl+X), Copy (Ctrl+C), Paste (Ctrl+V), Duplicate
(Ctrl+D), Rename (F2) and Delete (Del) act on the selection -- a selected
child goes with its selected parent -- each as one undo step. They are in the
viewport's and the Outliner's context menus and on Edit; their keys work while
the viewport or the Outliner has the focus (so Del and F2 in the Content
Browser stay its own, and a text field keeps Ctrl+C). Copies are kept for
every document, so entities copied in a prefab tab paste into the level.
Paste and Duplicate make new entities (fresh GUIDs; references between the
copies point at the copies) where the originals were: under the same parent,
or at the top, where they stood, if that parent is gone or inside a prefab
instance. A copied prefab instance comes back as a new instance of the same
prefab; one of its members copied alone comes back loose. Duplicate leaves the
clipboard alone. Rename asks for the name (in the Outliner, edit it in the row).

The toolbar above the viewport, after Play / Stop / Eject, is mostly combo
boxes: the handle mode (Select, Move, Rotate, Scale), the space (World,
Local), then Snap; the view preset (Persp, Front, Back, Top, Bottom, Left,
Right -- each orbits about the pivot to that axis; picking the shown one again
snaps back to it); a **Frame** button (the selection -- its arrow menu has
Frame All); **Show**; and camera speed (1/8x to 8x). The combo boxes never
take keyboard focus, so the viewport's keys keep working after one is used.

**Show** is a menu of what the viewport draws (`viewmode` in the Console too):

- **View Mode** (`Renderer::SetViewMode`, RenderCore's RENDERPIPELINE.md,
  "View modes"):
  - **Lit**: the scene as it is lit.
  - **Unlit**: base colour and emissive as they are -- no light, baked or live.
  - **Render Target...**: asks, in a list, which render target the last frame
    drew to show over the view -- scene colour and depth, the four G-buffer
    targets, post process, ambient occlusion and bloom, the shadow atlases, the
    image-based lighting atlas and strip, and every render target asset
    captured. The button then names it.
- **Lighting**: **Baked** draws with the world's bake, **Live** lights every
  light live as if there were none (`[Rendering] BakedLighting`; see the Build
  menu's Bake Lightmaps).
- **Editor Gizmos**: **All Bounds** (every entity's box, not only the
  selection's) and **Light Gizmos** (every light's reach, not only the
  selected light's).
"Top" looks down from world +Y: the editor uses the engine's left-handed,
+Y-up axes (RenderCore's RenderView.h).

**Overlays** are Renderer debug lines (`Renderer::DrawDebugLine` /
`DrawDebugBox`), not Qt painting -- nothing Qt draws can sit over the native
render surface. The selection is outlined in orange from its bounds: the
entity's `BoundsComponent::World` when it has one, else its RenderComponent quad, else
a small cube at its Transform. All Bounds adds every other entity's box in grey.
The XYZ gizmo sits bottom-left.

**Transform handles** sit where the selection is in the world (through its
parents; the drag writes back the local Transform that lands there), a fixed
size on screen, and highlight yellow under the cursor:

- **Move**: an arrow per axis, a square per plane (XY, YZ, XZ) and a centre
  square that moves in the screen plane. An arrow pointing straight at the
  camera, or a plane seen edge-on, is hidden rather than offered as a sliver.
- **Rotate**: a ring per axis -- the far half dimmed and not clickable -- and an
  outer ring about the line of sight. Where rings cross, the one facing the
  camera wins. Dragging follows the cursor around the ring (and keeps counting
  past a full turn); an edge-on ring drags along its on-screen length instead.
  Spokes show the start and current angle.
- **Scale**: an axis per cube-tipped handle, the centre square for all three.
  Always in the entity's own axes (scale applies before rotation). Dragging
  through the centre mirrors; no component goes below 0.001 in size.

**Local** space puts Move and Rotate on the entity's rotated axes. **Snap** (or Ctrl
held during a drag, which inverts it) steps by `Editor.SnapTranslate` (10;
a world grid in world space, a step along the handle in local), `SnapRotate`
(15 degrees) and `SnapScale` (0.1) -- config vars, so the console sets them.
The status bar shows what a drag has done so far. Every drag is computed from
where it started, so it never drifts, and a cancel restores that exactly. The
entity's bounds are refit as it moves, a static Bounds included
(`World::RefreshEntityBounds`). Rotation stays in the Transform's Euler angles,
chosen closest to the previous ones, so values do not jump between equivalent
forms mid-drag (`GameCore/TransformMath.h`).

## Outliner

Every entity as a tree: roots in creation order, each with its children
under it (GameCore's hierarchy, `GameCore/README.md`). The filter keeps a
match's ancestors visible. Selecting in the viewport opens the tree down to
the selection.

- **Drag onto a row** to parent the dragged entity to it, **between rows** to
  make it that row's sibling, **onto empty space** to detach it. It stays
  where it is in the world; its local Transform is rewritten to match. A drop
  that would make an entity its own ancestor is refused.
- **Right-click**: Create Empty Entity, Create Child Entity (at the parent's
  origin), Detach from Parent, and Destroy Entity -- "and Children" when it
  has some, since destroying a parent destroys its subtree.

## Details panel

The top **Entity** section edits what an entity carries besides its
components: its **Name** (what the Outliner shows) and its **Script** -- a
`.lua` file or a script graph (`.script.nmgraph`, which runs the Lua saved in
it), typed, dropped from the Content Browser or picked with **...** (as its
path in the project, and staged beside the executable), cleared with **x**.
Any other file is refused. Below that, a read-only
status says whether the script is running, which only happens in a playing
World, and the **Cull outside active region** checkbox sets the script's cull
flag. The script's class can override that flag when it loads.

An array property (`Props.Array`, such as a RenderComponent's **Materials**,
or `Props.StructArray`, such as its **Material Parameters**) is a list: a heading with how many entries it holds and **+ Add**, then one row
per entry, numbered, each with its own reset arrow and an **x** that removes it
-- the entries after it move up, and the array's end goes back to the default.
It shows the entries in use (up to the last one that is not the default) and
any added since; an added entry left empty is gone when another entity is
selected. Removing is undoable like any edit. A RenderComponent's entry *i* is
section *i*'s material; an empty one uses the mesh's own.

A struct array's entry is a numbered heading with its **x**, then one row per
member. A RenderComponent's **Material Parameters** entries are a Slot, a
Name (the material's Parameter node) and X to W. An entry is in use while any
member differs from the default, and one with an empty Name is ignored when
drawing. `Editor.Details` (`Tests/DetailsTests.cpp`) builds the panel
offscreen; `NIMBLE_DETAILS_SNAPSHOT=<file.png>` saves what it drew.

A material field -- a Materials entry, a mesh viewer section -- holds a
material graph by its path, `Materials/Glow.material.nmgraph` (the engine's:
`Engine/Content/Materials/...`): dropped, picked with **...**, or typed with
completion. Anything else typed is refused. A Post Process component's **Stack**
is a post process field (`PF_AssetPostProcess`): a stack,
`Content/PostProcess/Arch.pp.nmgraph`, or a PostProcess material graph, the
same ways. Saving a stack in its graph window updates the viewport at once.

Each property with a writable default shows an orange reset arrow to its right
while its value differs from the type's default-constructed one; clicking it
restores that property. A component header's **Reset** restores every writable
property of that component (per-run state such as a render proxy handle is left
alone), and its **x** removes the component. **+ Add Component** at the bottom
lists every type with a property list that the entity lacks; the new component
starts from its default-constructed bytes. A type opts out of that list with
`Props.NotAddable()`, for a component that only makes sense attached some other
way.

**Particle System** is added this way too: its **System** (an `.nmparticle`,
dropped, picked or completed; **Edit** opens it in the Particle Editor, or makes
one when the field is empty) and what this instance overrides -- Emitting,
Tint, Spawn Rate Scale, Time Scale, Seed, and a Beam Target for its beams. Its effect animates in the viewport
while editing, since it runs on the render thread's clock rather than the
World's (RenderCore's RENDERPIPELINE.md, "Particles").

## Particle Editor

A `.nmparticle` (a particle system: RenderCore's `ParticleSystemAsset.h`) opens
in its own window, a tab per system, from a double-click in the Content
Browser, File > Open Particle System, a System field's **Edit**, or the console
(`particles <path>`). Content Browser right-click > **New Particle System...**
writes one of a single default emitter in the current folder.

| Part | What it does |
|---|---|
| **Emitters** (left) | The system's emitters, up to 8, each simulated and drawn on its own. The check box switches one off; F2 or a double-click renames it; Add, Duplicate (seeded apart), Remove, and up/down to reorder |
| **Details** (right) | The selected emitter's settings in the Details panel's own widgets, in sections drawn as the Details panel draws components -- a bold header that collapses it (remembered while editing), **Reset** for the whole section, each row's reset arrow and tooltip. **Rendering**, **Emission**, **Size** and **Color** (and a beam's **Beam**) are always there; **Shape**, **Velocity**, **Forces**, **Rotation**, **Collision** and **Flipbook** are optional **modules**, removed with their x -- their settings back to defaults, and no effect -- and added back from **+ Add Module**, which offers only those the emitter's type uses, each one undo step. Within a section, only the rows its **Type** (Sprite, Mesh, Ribbon, Beam), **Spawn Mode** (Continuous, Burst), **Shape** (Sphere, Hemisphere, Box, Cone, Ring) and over-life modes use: Billboard for a sprite, Mesh Orientation for a mesh, Burst Count/Interval/Cycles for a burst, Box Size for a box, Cone Angle for a cone, Beam End/Segments/Noise for a beam, and so on; changing any reshapes the panel. **Size Mode** Curve and **Color Mode** Gradient replace Start/End's settings with a key editor (`ParticleCurveWidgets`): drag a key, double-click to add one (a gradient's key: its colour), right-click to remove; the selected key's time and value below. Chosen the first time, they start as Start/End gave -- the gradient with its fades -- in the same undo step; a drag previews live and records once, on release. The footer says how many particles it holds once it has run a lifetime (a beam: its points), and warns when what it spawns outgrows Max Particles, so the oldest are replaced early |
| **Viewport** | The system on a floor, under a dim sun and sky, in a World of its own (never saved). Framed from where its particles go (Frame); the emitter can be dragged with the move handle |
| **Toolbar** | Save; Pause, Restart, Speed (0.1x to 2x); Move Emitter (round a circle, to see world-space trails); Floor; Live in Level; the particle count |

**Every edit shows at once.** The preview's component names the tab's own cache
key (`transient://ParticleEditor/<n>`) and each change is stored there; an
emitter keeps its particles through an edit unless its Max Particles changes.
With **Live in Level** (on by default) an unsaved change is stored under the
asset's own path too, so the level, or a game playing in the editor, shows it;
closing without saving puts the saved one back. **Save** writes the file, stages
it beside the executable and drops it from the cache, so every component naming
it reads it again. Edit > Undo / Redo step through the system's own history: a
snapshot of the whole system a step, separate from the level's.

Dragging a `.nmparticle` from the Content Browser into the level makes an entity
running it, in one undo step.

The console drives the tab in front, for scripts and tests: `particles pause |
play | restart | speed <x> | move <on|off> | select <n> | screenshot <file.png>`,
plus `particles new` and `particles close`.

## Undo and redo

**Edit > Undo** (Ctrl+Z) and **Redo** (Ctrl+Y, Ctrl+Shift+Z) step through every
change to the edited scene: creating and deleting entities, renaming them,
parenting and detaching them, setting or clearing a script and its cull
flag, adding and removing
components, and every property write -- Details fields, the reset arrows,
and transform handle drags. The menu names the step ("Undo Move Marker");
the Console's `history` lists them all.

- **One drag, one step.** A handle drag is a single entry however far it
  went; a cancelled drag leaves none. Quick edits to the same component (a
  spin box's arrows, a few fields in a row) less than a second apart merge.
- **Deleted entities come back whole** -- the same GUID, name, script and
  components, under the same parent, with every child they had -- so later
  steps still apply to them and a save afterwards writes the same
  `.nmentity` files.
- **A reparent restores exactly.** It records the local Transform before and
  after, so undo puts the entity back on its old parent with its old values
  rather than recomputing them.
- **Unsaved changes** show as `*` in the title: the history remembers where
  the last save (or load) was, and undoing back to it clears the mark.
- New, Open and the Sandbox scene start an empty history.
- While playing, undo and redo are off, and changes to the running game are
  not recorded; the edited scene's history is there again after Stop.
- A text field with focus keeps Ctrl+Z for its own text.

Everything that changes the World goes through `EditorContext`, which records
it (`EditorHistory.h` has the details). Property writes are recorded from
`NotifyPropertyEdited`, against a copy of the selection's values taken before
the write -- so a new tool that writes components needs only to call that,
and to bracket a continuous edit with `BeginEditGesture` / `EndEditGesture`.
Reparenting goes through `SetEntityParent`, which records its own step.

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
| `viewmode <lit \| unlit \| target [name]>` | Show > View Mode; `target` alone lists the render targets it can show |
| `save` | File > Save World (the world must have a file) |
| `quit` | File > Exit, which still asks about unsaved changes |
| `lighting <bake \| baked \| live>` | Build > Bake Lightmaps, or Show > Lighting > Baked / Live |
| `screenshot <file.png>` | Save the viewport's next frame, without gizmos or debug lines (`Renderer::RequestSceneCapture`) |
| `cook [world] [out]` | Cook a world file to binary -- by default the open world's saved file, to `Cooked/` beside it |
| `cook ui [out]`, `cook <doc.nmui> [out]` | Cook every UI document to `<project>/Cooked/Content/UI/` (what a Client build stages), or one |
| `ui <path \| new \| close \| select <name> \| resolution <w>x<h> \| preview <on\|off> \| screenshot <file.png>>` | Open a `.nmui` in the UI Designer, make one, or drive the one in front -- from `--command` scripts too |
| `compile [cancel]` | As Build > Compile Source, or cancel the build running |
| `settings` | Open Edit > Editor Settings, and print where they are saved |
| `projectsettings` | Open Edit > Project Settings, and print the file it saves to |
| `stats` | The renderer's counters, with the frame, game thread and render thread times |
| `stat <unit \| frame \| none>` | `unit` toggles the frame, game thread and render thread times over the viewport (`Stats.Unit`); `frame` toggles each render stage's GPU time (`Stats.Frame`); `none` hides both |
| `gizmo [none \| move \| rotate \| scale] [world \| local]` | Set the transform handles, or print them |
| `snap [on \| off]` | Snap handle drags, or print whether they do |
| `undo [count]`, `redo [count]` | Step back or forward through the history |
| `history` | The undo history: `>` the current step, `*` the last save |

`EditorMainWindow::RegisterConsoleCommands` registers the scene commands;
`EditorConsole::Register` is the one call to add another.

## Play-in-Editor

**Play** (F5, the viewport toolbar, or `play` in the Console) runs the game in
the viewport: the module named by `[Game] Module` in `Config/DefaultEngine.ini`
(Sandbox's is `SandboxGame`), from the level that loads the open world -- the
one whose `ILevel::GetWorldFile` names it -- else from `[Game] StartLevel` or
the module's own start level. It is the game's real code -- its level, mode, systems and startup script
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

**Game input** takes the launcher's path (`Engine/Source/INPUT.md`):

- **How it gets there.** `ViewportSDLInput` wraps the viewport as an SDL
  window. Qt keeps the window and the message loop; Win32 focus moves to the
  viewport when it is focused, and text is forwarded. The input feeds the
  `PlaySession`'s `InputRouter`.
- **When.** Each frame, SDL is pumped and the router latched right before the
  game's frame. The game sees the same `InputState` it would in `Sandbox.exe`.
- **Editor keys.** **Escape** still stops the run, and Qt still sees every
  key after SDL.
- **Ejected view.** While the view is ejected, the game sees nothing held.
- **Fallback.** If SDL cannot wrap the viewport, the viewport's Qt key and
  mouse events feed the router instead. Those keys are layout-dependent, and
  the log says which path a run uses. Wrapping is implemented on Windows only,
  so on Linux every run takes this path.
- **Mouse capture.** A game's `SetMouseCaptured(true)` is SDL's relative mode
  on the SDL path. On the fallback the viewport does it: while it has the
  focus in the active window it hides and grabs the cursor and puts it back at
  its centre after every move, reporting the move as the delta. Clicking
  elsewhere (or switching windows) lets the cursor go; clicking the viewport
  takes it back.
- **Saving.** File > Save of a world inside the project also copies it (the
  manifest and its `.nmentity` folder) beside the executable, so the next Play
  of a level that loads it sees the change without a build.
- **The probe.** View > SDL Input Probe reports raw SDL events from the same
  wrapper. It switches off while playing, since a window is wrapped once.

The game's `[Display] FrameRateLimit` is reported to it but not enforced --
the viewport's timer paces the run.

## Editor settings

**Edit > Editor Settings...** (Ctrl+,, or `settings` in the Console) shows the
editor's own config vars, a page per group listed on the left (as Project
Settings lists its sections). Settings that belong to the project rather than
the user -- the game's, and RenderCore's `[Shadows]` -- are under Project
Settings instead.

| Page | Setting | Var |
|---|---|---|
| Appearance | Theme: System, Light, Dark (applies at once) | `Editor.Theme` |
| Viewport | Move / Rotate / Scale snap | `Editor.SnapTranslate`, `SnapRotate`, `SnapScale` |
| Viewport | Background gradient, top and bottom colours (a swatch opens a colour picker; applies at once to every editor viewport) | `Editor.ViewportGradient` (on), `ViewportTopColor` (`#4b515d`), `ViewportBottomColor` (`#1b1d22`) |
| Rendering | Render backend (on restart) | `Editor.RHI` |
| Build | Restart a running game after Compile | `Editor.ReloadAfterCompile` |
| Import | The Import Wizard's starting options: mesh scale, convert axes, generate normals, generate lightmap UVs, combine meshes, import a mesh's textures, sRGB, largest texture, texture mipmaps | `Editor.ImportMeshScale`, `ImportConvertCoordinates`, `ImportGenerateNormals`, `ImportLightmapUVs`, `ImportCombineMeshes`, `ImportMeshTextures`, `ImportTextureSRGB`, `ImportTextureMaxSize`, `ImportTextureMipGen` |

The settings are those config vars themselves; the dialog keeps no copy.
**Apply** and **OK** set them the way the Console would and save them;
**Restore Defaults** puts the compiled defaults in the form, for Apply to keep.
A value set from the Console shows the next time the dialog opens.

They save to `EditorSettings.ini` in the user's config directory
(`%LOCALAPPDATA%/NimbleEngine/Nimble Editor` on Windows), in the same
grammar as `DefaultEngine.ini`, and load over it at startup: the project's
file gives the defaults, the user's file overrides them, and `--rhi` overrides
both. Once saved, every setting in the dialog is in that file, so a later
change to the project's value does not reach that user. Delete the file to
go back to the project's values.

Adding a setting is a `NIMBLE_CONFIG_*` var and one row in
`SettingsDialog.cpp`.

**Theme.** The editor always uses Qt's Fusion style: the native Windows 10
style draws light whatever the palette says, so it cannot do dark. Light and
dark are explicit palettes (`EditorTheme.cpp`), so they look the same on every
machine; System picks between them from the OS's app mode and follows it if
that changes while the editor runs. `Editor.Theme 2` in the Console switches
too.

## Project settings

**Edit > Project Settings...** (`projectsettings` in the Console) edits the
project's `Config/DefaultEngine.ini` -- the game's and the engine's settings,
shared by everyone on the project, where Editor Settings are one person's.

- **Project** page: title, game module, start level and startup script, and
  where the file is.
- **One page per config section**: every registered `NIMBLE_CONFIG_*` var in
  it (outside `[Editor]`), with an editor for its type -- checkbox, spin box,
  text, or a list for known choices such as `Display.RHI` -- and a reset to
  its compiled default. **Bold** means the file sets it; the rest run on their
  default. A key whose name says too little has a label and a tooltip from
  `ProjectSettingsDialog.cpp`'s `KeyLabels` table.
- **Shadows** page: RenderCore's `[Shadows]` -- draw shadows, shadow map size,
  filter, how many point and spot lights may cast, the sun's shadow distance
  scale. Read every frame, so Apply shows in the viewport at once.
- **A game module's own settings** (`[Sandbox] RunSeconds`) are registered only
  while the module is loaded, which is during Play. Otherwise they are read
  from the file and edited as text, in italics.

**Which file.** The master copy in the game's source tree
(`Games/Sandbox/Config/DefaultEngine.ini`), not the copy staged beside the
executable, which a build overwrites. The game names it for the editor at
configure time:

```cmake
set_property(TARGET NimbleEditor PROPERTY NIMBLE_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Config/DefaultEngine.ini")
```

(`EditorProject.h` is generated from that). After a save the staged copy is
refreshed too, so the next Play sees the change without a build. With no game
setting the property, the dialog edits the staged copy and warns that a build
will overwrite it.

**How it writes.** Only values that changed, each on its own line
(`ConfigIniFile`): every comment, blank line and key order stays as it was. A
key the file lacks goes at the end of its section; a section it lacks, at the
end of the file. Values Core would not parse are refused. Apply also sets the
vars in the running editor, as the Console would, but most are read at startup
or at Play and take effect then.

## Build menu

**Compile Source** (F7, or `compile` in the Console) builds the game module --
the target named by `[Game] Module`, e.g. `SandboxGame` -- with `cmake --build`
on the tree this editor was built from, in the editor's configuration. It runs
in the background; every line it prints lands in the Console as `[Build] ...`,
errors and warnings at their own level. **Cancel Compile** (`compile cancel`)
kills it. When it succeeds:

- **Playing**: the run is stopped and started again, so it picks up the new
  DLL. Like any Stop, the run's state is lost. With *Restart a running game
  after Compile* off (Editor Settings, `Editor.ReloadAfterCompile`) the run
  keeps going on the old DLL instead.
- **Not playing**: nothing holds the module, so the next Play loads it.

Only the game module can be rebuilt from inside the editor. The engine DLLs
and the editor exe are loaded by the editor and cannot be relinked while it
runs; a change to engine code makes the game's build fail at that link --
close the editor and build from the IDE instead.

With MSVC the compiler needs Visual Studio's environment. An editor started
from an IDE or a developer prompt has it; otherwise (`INCLUDE` unset) the
build runs through the `vcvarsall.bat` of the Visual Studio that compiled the
editor, found at configure time from the compiler's path.

**Bake Lightmaps** (`lighting bake`) bakes the edited world's static lighting
-- its static lights into its static Render components' lightmaps, and light
probes for everything else (RenderCore's RENDERPIPELINE.md, "Baked lighting").
The world must be saved (the bake goes beside it, in `Levels/<World>/Lighting/`)
and have baked lighting on in World > World Settings (offered if not). It runs
on every core behind a progress dialog with Cancel, then points the world's
settings at the bake -- one undo step; save the world to keep it. Meshes
without lightmap UVs, missing materials and the like are warned about in the
Console as `[Bake]`. Before it lights anything, the viewport's renderer draws
each static material once for its base colour and once for its emissive, so
the bake reflects and glows as the materials draw (on the Null renderer, by
their BaseColor parameters). When a static entity or light changes afterwards,
the status bar says **Lighting needs to be rebuilt** (a hash of the world's
static and Stationary entities, checked a second after edits settle).

**Show > Lighting > Baked / Live** in the viewport (`lighting baked | live`, `[Rendering]
BakedLighting`) draws with the bake, or lights every light live as if there
were none -- to compare them.

Headless, from the binaries folder (a world opened, baked and saved, then the
editor closed):

    QT_QPA_PLATFORM=offscreen ./NimbleEditor --rhi 0 <world.nmworld> \
        --command "lighting bake" --command save --command quit

**Cook Content** is a stub: it says it is not implemented yet. What it should
cook (which worlds and assets, from where, to where, for which target) is not
decided. The Console's `cook` still cooks a single world.

## Known limits

- Undo does not reach other entities' references to a deleted one: a
  component that pointed at it still points at the old, dead EntityID after
  the delete is undone.
- No prompt to save unsaved changes on close, New or Open yet.
- Transform handles act on one entity; there is no multi-selection.
- Play-in-Editor input has no gamepad hot-plug outside the viewport's SDL pump
  (see INPUT.md).
- View presets stay in perspective; there are no orthographic views yet.
- Moving or renaming an asset in the Content Browser leaves components that
  name it pointing at the old path.
- A compressed texture's file holds its RGBA8 source as well as its blocks,
  and a game ships both: there is no cook step to strip the source yet.
- BC1 has no alpha (no 1-bit punch-through); BC3 or BC7 keep a cut-out.
