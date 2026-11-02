# Plugin starters

What File > New Plugin (the editor's Project Settings > Plugins > New Plugin)
and `Engine/Binaries/NewPlugin.cmake` make a new plugin from:

| Folder | |
|---|---|
| `Common/` | every starter: the descriptor, a README, the optional Content/ and Editor module |
| `Empty/` | a runtime module whose IPlugin starts and stops; nothing else |
| `Component/` | a component, its system on every level's World, a script library, a config default |
| `RenderPass/` | a custom render pass and its shader, cooked by NimbleCook |

A starter's file replaces Common's at the same path. In file and folder
names `__PLUGIN__` is the plugin's name; in contents:

| Token | |
|---|---|
| `@PLUGIN_NAME@` | the name: `Weather` |
| `@PLUGIN_UPPER@` | the name in capitals, the export macro's prefix: `WEATHER` |
| `@PLUGIN_CATEGORY@` | the descriptor's Category, and the Add Component group |
| `@PLUGIN_DESCRIPTION@` | the descriptor's Description |
| `@PLUGIN_STARTER@` | Empty, Component or RenderPass |
| `@ENGINE_VERSION@` | the engine's version: the descriptor's EngineVersion |

A line starting (after its indent) with `@IF_TEST@`, `@IF_EDITOR@` or
`@IF_CONTENT@` is kept, less the marker, only with that option; otherwise it
is dropped. `Tests/` is written only with a test, `Source/__PLUGIN__Editor/`
only with an Editor module, and Common's `Content/` only with a Content folder.

The editor (`Editor/Source/Private/Editor/Project/PluginTemplate.cpp`) and
the script (`Engine/cmake/NimblePluginTemplate.cmake`) must write the same
plugin byte for byte: NimbleEditorProjectTests compares them. With
`-DNIMBLE_CHECK_PLUGIN_TEMPLATES=ON` a configure makes each starter and
builds and tests it, so a starter that no longer compiles is caught.
