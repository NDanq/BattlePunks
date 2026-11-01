# Plugins

A plugin is a folder you can copy from one project to another: code, content
and config defaults together. A project enables it with one config line, and
nothing in `Engine/CMakeLists.txt` names it.

- Build side: [`Engine/cmake/NimblePlugins.cmake`](../cmake/NimblePlugins.cmake)
- Run side: [`NimbleEngine/Plugin.h`](../Source/Public/NimbleEngine/Plugin.h)
- Example: [`Engine/Plugins/TimeOfDay`](../Plugins/TimeOfDay)
- How to write one, step by step: [Guides/PluginGuide.md](Guides/PluginGuide.md)

**Engine plugins** (`Engine/Plugins/`):

| Plugin | What it does |
|---|---|
| [TimeOfDay](../Plugins/TimeOfDay/README.md) | A day/night cycle: the sun's path, its light, the Sky Light and a drawn physical sky |
| [Weather](../Plugins/Weather/README.md) | Rain, mist, wetness and wind from a Weather component: drives Volumetric Fog, the materials' Wind node, wet G-buffer surfaces and a camera-following particle rain (`RenderCore/RenderWeather.h`) |

Plugins are **source only**. Every project compiles its plugins with its own
toolchain, so the C++ ABI always matches. There are no prebuilt plugin
binaries.

## Layout

```
TimeOfDay/
  TimeOfDay.nmplugin            descriptor
  CMakeLists.txt                one nimble_add_plugin_module() per module
  Source/TimeOfDay/Public/TimeOfDay/...    #include "Public/TimeOfDay/X.h"
  Source/TimeOfDay/Private/TimeOfDay/...
  Content/                      read as "TimeOfDay:/..." and only so
  Content/Shaders/              custom pass shaders, cooked into the plugin's own cache
  Config/DefaultTimeOfDay.ini   defaults, under the project's config
  Tests/
```

## Descriptor

```ini
[Plugin]
Name=TimeOfDay            ; = folder and file name; the content prefix
Version=0.1.0             ; major.minor.patch
EngineVersion=0.1         ; the oldest engine it builds against
Category=Rendering
Description=...
EnabledByDefault=false

[Module_TimeOfDay]        ; one per module target
Type=Runtime              ; Runtime | Editor | ClientOnly | ServerOnly

[Dependencies]
Plugins=Other>=1.2.0      ; found AND enabled, or the configure stops
OptionalPlugins=Third     ; loaded first when enabled
```

A name is a letter followed by letters or digits, two characters at least, so
`C:/` can never be read as a plugin.

| Module type | Built in |
|---|---|
| Runtime | every target |
| Editor | Editor only (`WITH_EDITOR=1`) |
| ClientOnly | Editor and Client, never a dedicated server |
| ServerOnly | Editor and Server, never a shipped client |

## Where plugins are found and how they are enabled

The configure searches these folders in order, and the first one that has a
given name wins:

1. `<Project>/Plugins/`. A project can fork an engine plugin here.
2. `NIMBLE_PLUGIN_DIRS`, for a shared plugin library or a submodule.
3. `Engine/Plugins/`.

The project's `Config/DefaultEngine.ini` (Project Settings) enables them:

```ini
[Plugins]
TimeOfDay=true
```

A plugin the file doesn't list is built only if its descriptor sets
`EnabledByDefault=true`. A plugin that isn't enabled is not compiled, staged
or loaded at all.

Editing `[Plugins]` reconfigures the next build. Until that build, the game
and the editor warn that the build doesn't match the config
(`Plugins::WarnConfigMismatch`), the same way `[Modules]` does.

## Writing the CMakeLists.txt

```cmake
nimble_add_plugin_module(TimeOfDay
        SOURCES      Source/TimeOfDay/Private/TimeOfDay/TimeOfDay.cpp ...
        PUBLIC_DEPS  GameCore::GameCore RenderCore::RenderCore)

nimble_add_plugin_test(TimeOfDayTests SOURCES Tests/TimeOfDayTests.cpp LINK TimeOfDay)
```

`nimble_add_plugin_module` does the following:

- Builds a SHARED library named `<Module>` on every platform (no `lib`
  prefix), placed beside the engine's libraries.
- Links it to `NimbleEngine`.
- Generates `Public/<Module>/<Module>API.h` with the `<MODULE>_API` macro, so
  exports are never forgotten.
- Skips the module, with a status line, when its Type excludes the current
  target. Test for it with `if(TARGET <Module>)`.

A game module that calls into a plugin links it like any other library:

```cmake
if(TARGET TimeOfDay)
    target_link_libraries(${GAME_MODULE} PRIVATE TimeOfDay)
endif()
```

## What registers, and when

The engine loads each plugin library once, **before the game module**, in
dependency order. Plugins are never hot-reloaded: component types and script
libraries register for the life of the process.

- **Registers itself when the library loads, as in any engine module:**
  `GAMECORE_DEFINE_COMPONENT` and its serializer, script, replication and
  property tables, `GAMECORE_DEFINE_SCRIPT_LIBRARY`, `NIMBLE_CONFIG_*` vars.
- **Registers in `IPlugin::OnStartup(PluginContext&)`:** anything that needs
  the host. For now that means `AddWorldHook`, an attach/detach pair that runs
  on every level World a GameSession creates. It runs after the engine's own
  systems and before the level's `OnLoad`, and Play-in-Editor gets it too.

```cpp
class TimeOfDayPlugin : public NimbleEngine::IPlugin
{
    bool OnStartup(NimbleEngine::PluginContext& InContext) override
    {
        NimbleEngine::PluginWorldHook Hook;
        Hook.Attach = &AttachWorld;   // World.RegisterSystem(...)
        Hook.Detach = &DetachWorld;   // World.UnregisterAllSystems(...)
        InContext.AddWorldHook(Hook);
        return true;
    }
};
NIMBLE_IMPLEMENT_PLUGIN(TimeOfDayPlugin)
```

The host refuses any library built against a different plugin ABI. The check
covers SIMD, the debug CRT, `WITH_EDITOR` and `PLUGIN_ABI_REVISION`.

## Content is strict

A plugin's `Content/` can be read **only** as `<Name>:/<path>`:

| Path | Result |
|---|---|
| `TimeOfDay:/Presets/Default.todpreset` | the plugin's `Content/Presets/Default.todpreset` |
| `Presets/Default.todpreset`, `Content/Presets/...` | never the plugin's file |
| `Nope:/...` with no plugin `Nope` enabled | nothing, plus one error naming the missing `[Plugins]` line |

Two plugins can ship `Presets/Default.todpreset` without colliding, and a
project's own `Content/` is never shadowed by a plugin.

The prefix is a namespace, not a sandbox. In a development build the staged
files also sit on disk under `Plugins/<Name>/` beside the executable, so don't
reference those paths.

## Shaders

A plugin's `Content/Shaders` (custom render pass shaders) and
`Content/Materials` are cooked by `NimbleCook`, the same way a game's are. The
build never runs a shader compiler.

```bash
NimbleCook shaders -project Games/WorldsLab
```

This cooks the engine, the project, and every plugin the project enables.
`-plugin <dir>` adds one more plugin, for example one from
`NIMBLE_PLUGIN_DIRS`. Each plugin gets its own cache,
`<plugin>/Saved/Cooked/Shaders/Runtime/Shaders/Cache/<Name>.<Platform>.nmsc`.

- **Build:** stages the cache under `Plugins/<Name>/`, or packs it into
  `Paks/Plugins/<Name>.nmpak`. If the plugin hasn't been cooked, the
  configure warns, as it does for a game's materials.
- **Run:** the host mounts each cache on the renderer
  (`Plugins::MountShaderCaches`). In development builds it reads a newer cook
  straight from the plugin folder, so you don't need to rebuild after
  recooking.

Shader names are one shared namespace (a custom pass names its shader by
file), so prefix yours with the plugin's name: `TimeOfDaySky.fsh`. The cache
file is the only thing a plugin exposes without its `<Name>:/` prefix.

## What the build leaves beside the executable

```
Plugins/Plugins.manifest        enabled plugins in load order, their libraries,
                                and (outside Shipping) their source folders
Plugins/<Name>/                 descriptor, Config/, Content/, Shaders/Cache/: a development build
Paks/Plugins/<Name>.nmpak       the same files, packaged (NIMBLE_PACKAGE_CONTENT)
<Module>.so / .dll
```

**Where each host reads plugin files:**
- The game reads what was staged or packed.
- With `-uncooked`, the game reads each plugin's source folder.
- The editor always reads the source folder, so the files you edit are the
  plugin's own.

**Config order:** the plugin's `Config/Default<Name>.ini` loads first, then
the project's file, then the user's settings, then the command line. A
`[TimeOfDay]` key in the project's file overrides the plugin's default.

## Tests

- `Plugins.Resolve` (ctest): descriptor parsing, enabling, overrides,
  dependency order and every refusal, run in CMake script mode.
- `Plugin.TimeOfDay.TimeOfDayTests` (ctest): the host end to end on the
  build's real manifest. It covers config layering, strict prefixes, startup,
  world hooks and shutdown, then TimeOfDay itself.
- `Plugin.Weather.WeatherTests` (ctest): Weather through the host, then its
  wind, wetness, mist and rain.

## In the editor

- **Project Settings > Plugins** lists every plugin in the search folders,
  enabled or not, with its version, location, status (loaded, needs a
  rebuild, error) and details. Its checkboxes edit `[Plugins]`, offering to
  enable what a plugin needs, or disable what needs it. Saving offers to
  build, and when the build is done, to restart the editor.
- **New Plugin** (File menu, or the page's button) makes a plugin from a
  starter in [`Engine/Templates/Plugin`](../Templates/Plugin): Empty,
  Component or Render Pass. Everything is derived from the one name, which is
  checked as you type. It lists the files it will write before writing them.
  `Engine/Binaries/NewPlugin.cmake` makes the same plugin without the editor.
- **Rules:** both read descriptors with `PluginCatalog`
  (`NimbleEngine/PluginCatalog.h`), the C++ copy of this file's rules. A
  configure that would stop says so on the page first, with the same message.
- **Shadow copies:** the editor loads each plugin from a copy
  (`Plugins::SetShadowCopyDir`), so Compile Source can rebuild one while the
  editor runs. Windows would otherwise refuse to overwrite the loaded DLL.
- **Status bar:** shows "Plugins: rebuild and restart" while `[Plugins]` and
  the running build disagree.
- **Starter check:** configure with `-DNIMBLE_CHECK_PLUGIN_TEMPLATES=ON` to
  build every starter and run its test (`ctest -L template`), so a starter
  that has stopped compiling is caught.

## Not done yet

- **No editor extension points:** no menus, panels, Details customizations or
  importers. An `Editor` module builds and loads, but has nothing to register
  into.
- **No plugin asset kinds:** the Details asset pickers (`PF_Asset*`) are a
  fixed list, so a plugin's own file types are edited as plain text.
- **No migration of AICore, Sequencer, JoltPhysics, Audio or Collab.** They
  move once TimeOfDay has proven the system. NetCore stays an engine feature
  switch, because `WITH_NETCORE` changes GameCore's layouts.
