# Getting started with @PROJECT_NAME@

@PROJECT_NAME@ was made by the editor's **File > New Project** (or
`Engine/Binaries/NewProject.sh` / `.bat`), from `Engine/Templates/Game`. It already runs: a level with a camera and a player
quad, a game mode that ticks, and a game state a save keeps. This guide takes
it from there. `Games/Sandbox` is the full example of everything below.

## 1. Build

From this folder:

```
cmake --preset editor-debug
cmake --build --preset editor-debug
```

The first build compiles the engine too; later ones only what changed. The
presets (`CMakePresets.json`):

| Preset | Builds |
|--------|--------|
| `editor-debug`, `editor-relwithdebinfo` | The game, the launcher and **NimbleEditor** |
| `client-debug`, `client-relwithdebinfo`, `client-shipping` | The game and the launcher only |

Or open this folder in an IDE as a CMake project and pick a preset. Everything
lands in `Binaries/<Platform>_<Config>/` (`Linux_Debug`, `Windows_Debug`, ...;
`_Client` is appended for client presets).

## 2. Run

| Run | To |
|-----|----|
| `Binaries/<Platform>_Debug/NimbleEditor` | Edit the game. **Play** (F5) runs it in the viewport, **Stop** (Shift+F5) ends it |
| `Binaries/<Platform>_Debug/@PROJECT_NAME@` | Play it in its own window |

Run from `Binaries/<Platform>_<Config>/`: the game reads `Config/`,
`Scripts/` and `Content/` relative to its working directory. An IDE run is set
up to start there.

## 3. Where things go

| Path | What | Made with |
|------|------|-----------|
| `@PROJECT_NAME@Game.cpp` | Game module: level, game mode, game state, native prefabs | Your IDE |
| `Config/DefaultEngine.ini` | Settings: window, renderer, start level, startup script | Edit > Project Settings |
| `Scripts/` | Lua (`.lua`) and script graphs (`.script.nmgraph`) | File > New Script Graph |
| `Content/Materials/` | Material graphs (`.material.nmgraph`) | File > New Material Graph |
| `Content/Prefabs/` | Prefabs (`.nmprefab`) | File > New Prefab, World > Convert Selection to Prefab |
| `Content/Levels/` | Worlds (`.nmworld`) | File > Save World As |
| `Content/Meshes/`, `Content/Textures/` | Imported assets (`.nmmesh`, `.nmtexture`) | File > Import (glTF, images) |

The build copies `Config/`, `Scripts/` and `Content/` into `Binaries/` at the
same relative paths, and that path is the asset's name at runtime:
`Content/Prefabs/Door.nmprefab`, `Content/Materials/Glow.material.nmgraph`.
Edit the files here, never the copies in `Binaries/` -- the next build
overwrites those. New files are picked up by the next build; nothing needs
listing.

## 4. The game's C++

Open `@PROJECT_NAME@Game.cpp`. Four classes, top to bottom:

- **Native prefabs** (`BuildCamera`, `BuildPlayer`) -- entities built in C++,
  named `"native:@PROJECT_NAME@.Player"`. The editor lists them under
  World > Native Prefabs. Add one with another `GAMECORE_DEFINE_NATIVE_PREFAB`.
- **`@PROJECT_NAME@GameState`** -- data the rules use. `OnReset` sets the
  starting values (every level start); `Serialize` writes and reads the same
  fields for saves. Add a field to both; once saves exist, bump
  `GetSerialVersion()` and read the new field behind
  `if(Ar.GetChunkVersion() >= 2)`.
- **`@PROJECT_NAME@GameMode`** -- the rules. `OnTick` runs once per frame on
  the game thread; `Session->GetWorld()` is the scene, `Session->RequestQuit()`
  ends the game. Cache entity IDs in `OnLoad`, and find them again in
  `OnPostLoadSave` after a save is restored.
- **`@PROJECT_NAME@Level`** -- builds the scene in `OnLoad` (spawn prefabs,
  register systems) and owns the mode and state. Its name, `"@PROJECT_NAME@"`,
  is `[Game] StartLevel` in the config.
- **`@PROJECT_NAME@GameModule`** -- registers the level. A second level is a
  second `ILevel` class, registered beside the first.

Per-entity logic that runs every frame belongs in a system rather than the
mode: `World.RegisterSystem("@PROJECT_NAME@.Move", GameCore::TP_PrePhysics, &MoveSystem, &Context)`
in the level's `OnLoad` (see `DriveMarkerSystem` in `Games/Sandbox/SandboxGame.cpp`).

There is no `main()` to write: the `@PROJECT_NAME@` executable is the
engine's launcher (`nimble_add_game_launcher` in `CMakeLists.txt`), which
loads the game module and runs the start level. A game that needs its own
passes `SOURCES MyMain.cpp` to it.

A new `.cpp` goes in `CMakeLists.txt`, in the `nimble_add_hot_module(...)`
`SOURCES` list. Inside the editor, **Build > Compile Source** (F7) rebuilds the
game module without closing it.

## 5. Scripts

`Scripts/Main.lua` runs on every level load, after the level's `OnLoad`:

```lua
local door = World.SpawnPrefab("Content/Prefabs/Door.nmprefab", 100, 200, 0)
Events.On("EntityCreated", function(entity) print(entity:GetName()) end)
```

A prefab can name a script; every instance of it then runs its own copy,
with a function per phase it needs:

```lua
-- Scripts/Door.lua
return {
    OnStart        = function(self) end,
    OnPrePhysics   = function(self, dt) end,
    OnOverlapBegin = function(self, other, mine) end,
}
```

Script graphs (`.script.nmgraph`) are the same thing drawn as nodes. After
adding components or script functions in C++, **Build > Refresh Script Nodes**
shows them in the graph editor. `Engine/Modules/GameCore/README.md`
(Scripting, Lua) has the whole API.

## 6. Config

`Config/DefaultEngine.ini` is the master copy; Edit > Project Settings edits
it. The keys that matter first:

| Key | Default | |
|-----|---------|---|
| `[Display] ResolutionX`, `ResolutionY` | 1280 x 720 | Window size |
| `[Display] RHI` | 2 | Renderer: 0 Null (headless), 2 Vulkan, 4 D3D12 |
| `[Application] Title` | @PROJECT_NAME@ | Window title |
| `[Game] StartLevel` | @PROJECT_NAME@ | A registered level's name |
| `[Scripting] StartupScript` | Scripts/Main.lua | Empty skips scripting |

A game declares its own keys in C++, in the same file:

```cpp
NIMBLE_CONFIG_INT(GLives, "@PROJECT_NAME@", "Lives", 3, nullptr);   // [@PROJECT_NAME@] Lives=3
```

## 7. Shipping a client build

A client build cannot read text prefabs. Before building a `client-*`
preset, open the editor's Console and run `cook prefabs`: it writes binary
copies to `Cooked/Content/Prefabs/`, which the client build stages instead.
Commit `Cooked/` with the game. Material graphs cook themselves during the
build.

## 8. Next

- `Games/Sandbox` -- input, collision, systems, scripting, saves, all working.
- `Engine/Source/INPUT.md` -- input maps and the Input component.
- `Engine/Modules/GameCore/PREFABS.md` -- prefabs, instances and overrides.
- `Engine/Modules/RenderCore/MATERIALS.md` -- material graphs.
