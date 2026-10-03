# @PROJECT_NAME@

A NimbleEngine game, made by the editor's **File > New Project** (or
`Engine/Binaries/NewProject.sh` / `.bat`) from `Engine/Templates/Game`. `Games/Sandbox` is the full example to follow.

New here? Start with [Docs/GettingStarted.md](Docs/GettingStarted.md).

## Build and run

```
cmake --preset editor-debug
cmake --build --preset editor-debug
```

Then run `Binaries/<Platform>_Debug/NimbleEditor` to edit the game, or
`Binaries/<Platform>_Debug/@PROJECT_NAME@` to play it. `client-*` presets build
the game alone, without the editor.

## Layout

| Path | What |
|------|------|
| `@PROJECT_NAME@Game.cpp` | The game module: level, game mode, game state, native prefabs |
| `@PROJECT_NAME@Main.cpp` | The launcher |
| `Config/DefaultEngine.ini` | Settings; `[Game]` names the module and start level |
| `Scripts/` | Lua and script graphs; `Main.lua` runs on every level load |
| `Content/Materials/` | Material graphs (`File > New Material Graph`) |
| `Content/Prefabs/` | Prefabs (`File > New Prefab`) |
| `Content/Levels/` | Worlds saved from the editor |
| `Content/Meshes/`, `Content/Textures/` | Imported assets |
| `Docs/` | This project's guides |

Everything in `Config/`, `Scripts/` and `Content/` is staged into `Binaries/`
by the build at the same relative path, which is how the runtime names it
(`Content/Prefabs/Door.nmprefab`). A new C++ file goes in `CMakeLists.txt`'s
`nimble_add_hot_module` list.
