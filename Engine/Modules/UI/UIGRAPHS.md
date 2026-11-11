# UI graphs: implementation guide

How to give a UI document its logic as a node graph (`*.ui.nmgraph`). It
also covers how to make your own C++ and Lua reachable from that graph:
functions, variables, events, and value types. [README.md](README.md),
"UI graphs", is the reference; this file is the how-to.

- [1. The pieces](#1-the-pieces)
- [2. Make a UI graph](#2-make-a-ui-graph)
- [3. What a graph has without any code](#3-what-a-graph-has-without-any-code)
- [4. Expose C++ to graphs: script libraries](#4-expose-c-to-graphs-script-libraries)
- [5. Value types: script structs](#5-value-types-script-structs)
- [6. Components](#6-components)
- [7. Lua and graphs](#7-lua-and-graphs)
- [8. Calling into a graph from C++ or Lua](#8-calling-into-a-graph-from-c-or-lua)
- [9. Worked example: an audio settings page](#9-worked-example-an-audio-settings-page)
- [10. Checklist and troubleshooting](#10-checklist-and-troubleshooting)

## 1. The pieces

```
Menu.nmui ─ Graph="Content/UI/Menu.ui.nmgraph"
  <Button OnClick="Graph:Play_OnClick"/>          ─── calls ──►  UI Event "Play_OnClick"
  <Slider Value="{Bind:Audio.Volume}" .../>  ◄─ data store ─►  UI.SetNumber / UI.GetNumber nodes

Menu.ui.nmgraph ─ compiled to Lua when saved, stored inside the file (<gen_code>)
  nodes: flow, maths, variables, graph functions, events,
         and one node per member of every script library ◄── your C++ (section 4)
```

| Piece | Where it lives | Made by |
|---|---|---|
| The document | `Content/UI/Menu.nmui` | UI Designer |
| Its UI graph | `Content/UI/Menu.ui.nmgraph`, its `Graph` setting | Graph Editor (UI schema) |
| Nodes for your code | `GAMECORE_DEFINE_SCRIPT_LIBRARY` in any `.cpp` of a game module or engine module | you, in C++ |
| Shared values | the data store (`UISystem::SetValue`, `{Bind:Key}`) | C++, Lua or the graph |

At runtime the graph is a **Lua class**. Each open copy of the document gets
its own instance (`self`), so graph variables are per document. Its methods
are `OnOpen`, `OnClose`, `OnTick(dt)`, and one per UI Event, plus your
handlers. Anything a graph node can call is something its Lua can call, so
exposing code to graphs means exposing it to Lua as a **script library**.

## 2. Make a UI graph

In the UI Designer:

1. Save the document. The graph is created next to the document file, so the
   document needs a path first.
2. Select a widget. On an event field (`OnClick`, `OnChanged`, ...), click
   **+ > Graph handler**. This does four things:
   - makes `<Document>.ui.nmgraph` if the document has no graph, and sets the
     document's **Graph** to it;
   - adds a **UI Event** node named `<Widget>_<Event>` (for example
     `Play_OnClick`), or finds the one that already exists;
   - opens the Graph Editor on that node;
   - writes `Graph:Play_OnClick` into the field. If the field already holds
     actions, it is added after them.
3. Build the logic from that node's **Then** pin, then save the graph
   (Ctrl+S). Saving compiles the graph to Lua and stages it. If Play-in-Editor
   is running, the document and its logic reload.

To give the document itself a handler, first deselect everything so the panel
shows the document's settings. Then use **+ > Graph handler** on `OnBack`,
`OnOpen` or `OnClose`; the handler is named `Document_OnBack` and so on. The
**Graph** row's **Create** / **Open** button makes or opens the graph without
adding a handler.

The **UI Event** node has these outputs:

| Output | Meaning |
|---|---|
| `Value` | The widget's value: a slider's value, or a check box's state. If the node's `ValueType` is `Bool` (set automatically for a CheckBox's `OnChanged`), it is a bool; otherwise a number. Buttons give 0. |
| `Sender` | The widget's `Name`. Useful when one handler serves several widgets. |

Handlers are found by name, so renaming a widget breaks nothing; the
handler's name just stops matching the widget. To rename a handler, change
its `Handler` setting and the field's `Graph:` text together. A field that
names a handler the graph lacks is outlined in orange.

Notes:
- Games stage `Content/UI/*.ui.nmgraph` for every target. The graph carries
  its Lua, so a Client build reads it directly; only the `.nmui` needs `cook ui`.
- The designer's **Preview** runs the logic, in a scratch World and script
  engine of its own: On Document Open when it starts, your handlers as you
  click. What it does to config vars is undone when it stops, and the `Game`
  library answers defaults there. Saving the graph reloads it.

## 3. What a graph has without any code

**Events.** On Document Open, On Document Close, On Tick, UI Event, and On
Event (any EventBus event, listened to while the document is open).

**Document nodes.**
- This Document: the document's path, the first input of the `UI` library
  nodes.
- Key: a data store key as this document reads it -- its Include's or list
  row's prefix + the key (`Saves.3.Name`). Use it to write what a row shows.
- List Item Key: `List.Index.Field`, for filling a ListView (set its count
  with UI Set Count).
- Close This Document.
- Set Widget Text / Visible / Enabled / Focus, and Show Page. Each picks its
  widget from a dropdown of the document's names.

**The graph's own variables and functions.** These are made in the Graph
Editor's Details panel; nothing else is needed.
- *Variables* (number, integer, bool, string, Vector3, Color, or a script
  struct) become `self.<Name>`: kept for as long as the document is open,
  separately for each open copy. Use Get / Set Variable.
- *Functions* are reusable chains. Give one inputs and outputs, then call it
  from several handlers. LightLab's `Resume` function is called by the Resume
  button and by Back.

**Libraries already registered:**

| Library | Useful for UI |
|---|---|
| `UI` | `Open`, `Close`, `IsOpen`, `SetText`, `SetVisible`, `SetEnabled`, `SetProperty`, `SetFocus`, `ShowPage`, `SetNumber` / `SetString` / `SetBool`, `GetNumber` / `GetString` / `GetBool`, `SetCount` (a ListView's length), `PlayAnimation` / `StopAnimation` / `IsAnimationPlaying`, `GoToState` |
| `Config` | `GetBool` / `GetNumber` / `GetString`, `SetBool` / `SetNumber` / `SetString`, `Has`, `Save` (the player's User.ini). Every `NIMBLE_CONFIG_*` var, by `"Section.Key"` |
| `Game` | `SetPaused`, `IsPaused`, the phase, score and lives, and the `PhaseChanged` / `ScoreChanged` / `LivesChanged` events |
| `Input` | `SetMouseCaptured`, `IsKeyDown`, `GetMousePosition`, ... |
| `World`, `Events`, `Debug` | finding entities, Emit, Print |

So a settings page that only reads and writes **config vars** needs no new
C++ at all. Declare the var with `NIMBLE_CONFIG_FLOAT` / `_BOOL` / `_INT`,
then use the `Config` nodes. LightLab's pause menu works this way.

## 4. Expose C++ to graphs: script libraries

A **script library** is a named set of functions, variables and events. It is
declared once in C++, and becomes a Lua global table and a palette category
in every script graph, UI graphs included. The API is in
`GameCore/ScriptLibrary.h`.

### 4.1 Declare one

In exactly one `.cpp` (a game module's, or an engine module's), at file
scope:

```cpp
#include "Public/GameCore/ScriptLibrary.h"

namespace
{
    float   GMasterVolume = 0.8f;        // must outlive the library: a global or a static
    int32_t GUnlockedLevels = 3;

    void   ApplyVolume(double InVolume) { GMasterVolume = float(InVolume); Audio::SetMaster(GMasterVolume); }
    double DecibelsFor(double InVolume)  { return InVolume > 0.0 ? 20.0 * std::log10(InVolume) : -80.0; }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Audio)      // Lua global "Audio"; palette category "Audio"
{
    Lib.Native<&ApplyVolume>("SetVolume", "Volume")
       .Tooltip("Sets the master volume, 0-1");
    Lib.NativePure<&DecibelsFor>("Decibels", "Volume -> Decibels");
    Lib.Variable("MasterVolume", &GMasterVolume);            // Get MasterVolume, Set MasterVolume
    Lib.ReadOnly("UnlockedLevels", &GUnlockedLevels);         // Get UnlockedLevels only
    Lib.Event("VolumeChanged", "Volume:number");              // On VolumeChanged, Emit VolumeChanged
}
```

That is all the code needed. The next time the editor reads the game's
declarations (section 4.5), the palette has **Audio > Set Volume**,
**Decibels**, **Get/Set MasterVolume**, **Get UnlockedLevels**, **On
VolumeChanged** and **Emit VolumeChanged**. Lua has `Audio.SetVolume(0.5)`,
`Audio.GetMasterVolume()`, and so on.

### 4.2 Member kinds

| Declaration | Graph node | Lua | Use for |
|---|---|---|---|
| `Lib.Native<&Fn>("Name", "A, B -> R")` | statement (exec in/out) | `Lib.Name(a, b)` | an ordinary C++ function with effects; the types come from its signature |
| `Lib.NativePure<&Fn>("Name", "A -> R")` | value node | same | a function with **no side effects**. The graph may call it once for each place its output is used |
| `Lib.Function("Name", &Fn, "A:number -> R:bool")` | statement | same | hand-written glue: `void Fn(GameCore::ScriptCall&)`, with the types written in the signature |
| `Lib.Pure("Name", &Fn, "...")` | value node | same | the same, with no side effects |
| `Lib.Variable("Name", &Storage)` | Get Name, Set Name | `GetName()`, `SetName(v)` | a C++ global or static the graph reads and writes |
| `Lib.ReadOnly("Name", &Storage)` | Get Name | `GetName()` | the same, read only |
| `Lib.Flag("Name", &Uint8, bReadOnly)` | bool Get/Set | | a `uint8_t` 0/1 shown as a bool |
| `Lib.Event("Name", "A:type, ...")` | On Name, Emit Name | `Events.On("Name", fn)` | an EventBus event with up to 4 typed values |

Every declaration takes `.Tooltip("...")`, which is shown in the palette and
on the node.

### 4.3 Signatures and types

- **Native / NativePure** read the C++ parameter and return types. The string
  only *names* the pins: `"Position, Value -> Pickup"`. Leave it out to get
  `Arg1, Arg2 -> Result`.
  - Supported C++ types: `float`, `double`, `int32_t`, `bool`, `const char*`,
    `std::string`, `EngineCore::Vector3`, `EngineCore::Color`,
    `GameCore::EntityID`, and registered script structs.
  - Two parameter types are **filled in, not pins**: `GameCore::World&` (the
    caller's World) and `GameCore::ScriptCall&` (to report errors).
  - Only free or static functions can be passed: `&Function`. A member
    function belongs on a component (section 6).
- **Function / Pure** take a written signature: `"Name:type, ... -> Name:type, ..."`.
  The types are `number`, `integer`, `bool`, `string`, `Vector3`, `Color`,
  `Box` and `Entity`. Tools read the signature; your function reads its
  arguments itself:

  ```cpp
  Lib.Function("SetSlot", [](GameCore::ScriptCall& Call)
  {
      const int32_t     Slot = Call.GetInt(1);          // arguments count from 1
      const char* const Name = Call.GetString(2);       // valid until the function returns
      if(Slot < 0) { Call.Error("SetSlot: negative slot"); return; }   // a Lua error; never throws
      SaveSlots::Rename(Slot, Name);
      Call.PushBool(true);                              // results, in order
  }, "Slot:integer, Name:string -> Done:bool");
  ```

  `ScriptCall` has `GetNumber`, `GetFloat`, `GetInt`, `GetBool`, `GetString`,
  `GetVector3`, `GetColor`, `GetEntity` and `GetStruct`. Its results are
  `Push*` and `PushNil`. It also has `GetWorld()`, `HasArg(n)` and
  `GetArgCount()`.
- **Variable** accepts any field type: `float`, `double`, `int32_t`,
  `uint8_t`, `bool`, `Vector3`, `AABB`, `Color`, a fixed string, or a script
  struct. The storage must outlive the library: a global or a `static`, never
  a local or a member of something that is freed.
- **Event** parameters: at most 4, each a one-value type (`number`,
  `integer`, `bool`, `string`, `Entity`), because an `EventArgs` carries four.

### 4.4 Rules and limits

- The library name is a Lua global and must be an identifier that does not
  clash with an existing global (`World`, `Events`, `UI`, `Config`, `Game`,
  `Input`, `math`, `string`, ...). The library name can have at most 31
  characters, a member's name at most 39 (`Get`/`Set` are added to
  variables'), and the library at most 48 members.
- Use **one library per name**: a second library with the same name is
  refused. Declare it in exactly one `.cpp`.
- A name or signature that is too long is **rejected**, not truncated, and
  logged with the member.
- Game thread only. A library's functions run when the graph's Lua runs: in
  UI actions (after input, before the World ticks) and in `OnTick`.
- **Pure means pure.** A pure node is called again each time its output is
  used, and it is not called at all if nothing uses it. Anything that changes
  state is a Function / Native.
- To act on the UI from C++, use `UI::GetScriptUISystem()` (the session's
  `UISystem`). From the game side use `GameSession::GetUI()`.
- Lifetime: the library appears when its image loads and is removed when it
  unloads (hot reload included). A Lua state receives the libraries that
  exist when a World is bound.

### 4.5 Getting new nodes into the editor

The editor reads the libraries that are **registered in its process**:

- **Engine modules** (GameCore, UI, NimbleEngine): always loaded, so their
  libraries are always there.
- **Game modules**: the editor loads `[Game] Module` briefly to read them
  (`EditorGraphs::RefreshGameDeclarations`). This happens:
  - at startup;
  - after **Build > Compile Source** succeeds;
  - on **Build > Refresh Script Nodes**.

After adding a member, rebuild the game module and use **Build > Refresh
Script Nodes**. A graph that was already open keeps its old nodes until it is
reopened. If the game is not loaded, nodes from it appear as placeholders,
and the graph keeps them.

From a command line, regenerate a graph's Lua with the game's declarations
loaded:

```sh
./NodeGraphGenerate -module ./MyGame.so Content/UI/Menu.ui.nmgraph
```

## 5. Value types: script structs

To pass a small record (a save slot, a score row) as **one pin**, register a
script struct (`GameCore/ScriptStruct.h`):

```cpp
struct SaveSlotInfo                          // in a header
{
    GAMECORE_SCRIPT_STRUCT_BODY()
    int32_t Slot    = 0;
    int32_t Minutes = 0;
    bool    bUsed   = false;
};

GAMECORE_DEFINE_SCRIPT_STRUCT(SaveSlotInfo, "SaveSlotInfo")   // in one .cpp
{
    Struct.Field("Slot",    &SaveSlotInfo::Slot);
    Struct.Field("Minutes", &SaveSlotInfo::Minutes);
    Struct.Field("Used",    &SaveSlotInfo::bUsed);
}

SaveSlotInfo GetSlot(int32_t InSlot);
Lib.NativePure<&GetSlot>("GetSlot", "Slot -> Info");   // Info is a SaveSlotInfo pin
```

In the graph, the struct is a pin type with **Make SaveSlotInfo** and **Break
SaveSlotInfo** nodes, and it can be a graph variable's type. In Lua it is
its fields' values in order: `local slot, minutes, used = Saves.GetSlot(1)`.

The rules: fields are field types (not other structs), and the struct must
be default-constructible and trivially copyable. An unlinked struct input is
all zeros; use Make to build one.

## 6. Components

Components exposed with `GAMECORE_DEFINE_COMPONENT_SCRIPT`
(`GameCore/ComponentScript.h`) get Get / Has / Add / Remove nodes, field
getters and setters, and one node per bound method, in UI graphs too. A UI
graph has no entity of its own, so it reaches one through the World: **Find
Entity** (by name), or an Entity that an event or a library function gives
it. A HUD that reads the player's components works this way. For values the
UI only displays, prefer pushing them to the data store from C++ and binding
to them (section 8). That costs nothing while nothing changes.

## 7. Lua and graphs

**Lua cannot add nodes to the palette.** The palette is built from the C++
registries; tools never run the game's Lua. If a graph should *call*
something, declare it in C++ (section 4).

Lua and a graph can still work together. Pick the channel that fits:

| You want | Lua side | Graph side |
|---|---|---|
| a graph to run Lua code | `Events.On("Menu.Refresh", function(a, b) ... end)` | **Emit Event** `Menu.Refresh` (up to 4 args) |
| Lua to run a graph's logic | `Events.Emit("Menu.Refresh", 1)` | **On Event** `Menu.Refresh`; it listens while the document is open |
| typed events both ways | (declare `Lib.Event` in C++) | **On / Emit** nodes with typed pins |
| shared values | `UI.SetNumber("Menu.Count", 3)`, `UI.GetNumber(...)` | `UI.GetNumber` / `UI.SetNumber` nodes, or `{Bind:Menu.Count}` on a widget |
| one click to do both | (none) | `OnClick="Graph:Play_OnClick; Lua:Menu.Play($self)"`: actions chain with `;` and run in order |

Notes:
- Lua globals made by a document's `Script="...lua"` (run once when the
  document first opens) and by the game's own scripts share the Lua state
  with the graph. So the generated Lua *could* call them, but no node does
  that; use the channels above.
- An event name is a plain string. Prefix it with the menu (`Menu.Refresh`)
  so it does not collide with gameplay events.
- On Event handlers receive the event's four values as `Any`. Convert them
  where you use them.

## 8. Calling into a graph from C++ or Lua

- **Push values; let bindings and the graph read them.** From C++, call
  `Session.GetUI().SetValue("Audio.Volume", UI::UIValue::Number(0.8))`; from
  Lua, `UI.SetNumber("Audio.Volume", 0.8)`. Widgets bound to the key update,
  and the graph's `UI.Get*` nodes see the new value. `GameSession` already
  publishes `Player<N>.Score`, `Player<N>.Lives` and `Game.Phase` every frame.
- **Emit an event** that the graph's On Event node listens to. In C++:
  ```cpp
  GameCore::EventArgs Args;
  Args.Args[0] = GameCore::EventArg::Make(3.0);
  Args.Count   = 1;
  World.GetEvents().Emit("Menu.Refresh", Args);
  ```
- **Run a handler directly**: queue a `Graph:` action on the document. It
  runs on that document's own instance, with the sender's value and name if
  there is a sender:
  ```cpp
  UI::UISystem& UI = Session.GetUI();
  if(UI::UIDocumentInstance* Menu = UI.Find("Content/UI/Menu.nmui"))
  {
      UI::UIActionList Run;
      Run.Parse("Graph:Refresh");
      UI.QueueActions(Run, Menu, nullptr);   // runs with the session's next RunQueuedActions
  }
  ```
- **Native UI actions** (`UI_DEFINE_ACTIONS`, `Native:Group.Name`) are
  *action text*, not graph nodes. Keep them for one-line wiring in XML. For
  logic that a graph should call, use a script library.

## 9. Worked example: an audio settings page

The goal: a volume slider and a mute check box, saved as config, with a
read-out in decibels.

**C++** (the game module), one var and one library function:

```cpp
void OnVolumeChanged(const EngineCore::ConfigVar& InVar);   // applies it to the mixer (ConfigChangedFn)

NIMBLE_CONFIG_FLOAT(GMasterVolume, "Audio", "MasterVolume", 0.8f, &OnVolumeChanged);
NIMBLE_CONFIG_BOOL(GMuted, "Audio", "Muted", false, &OnVolumeChanged);

namespace { double Decibels(double InVolume) { return InVolume > 0.0 ? 20.0 * std::log10(InVolume) : -80.0; } }

GAMECORE_DEFINE_SCRIPT_LIBRARY(Audio)
{
    Lib.NativePure<&Decibels>("Decibels", "Volume -> Decibels").Tooltip("20 log10(Volume)");
}
```

**Document** (`Content/UI/Audio.nmui`), with the widgets bound two-way:

```xml
<UI ... Graph="Content/UI/Audio.ui.nmgraph" OnBack="Native:UI.Close">
  <Slider Name="Volume" Min="0" Max="1" Step="0.05" Value="{Bind:Audio.Volume}" OnChanged="Graph:Volume_OnChanged"/>
  <Text Text="{Bind:Audio.Decibels} dB"/>
  <CheckBox Name="Mute" Checked="{Bind:Audio.Muted}" OnChanged="Graph:Mute_OnChanged"><Text Text="Mute"/></CheckBox>
```

**Graph** (`Content/UI/Audio.ui.nmgraph`). The two `OnChanged` handlers were
made with **+ > Graph handler**:

```
On Document Open ─► UI Set Number ("Audio.Volume", Config Get Number "Audio.MasterVolume")
                 ─► UI Set Bool   ("Audio.Muted",  Config Get Bool  "Audio.Muted")
                 ─► UI Set Number ("Audio.Decibels", Audio Decibels(Config Get Number "Audio.MasterVolume"))

UI Event Volume_OnChanged (Value) ─► Config Set Number ("Audio.MasterVolume", Value)
                                  ─► UI Set Number ("Audio.Decibels", Audio Decibels(Value))

UI Event Mute_OnChanged (Value: bool) ─► Config Set Bool ("Audio.Muted", Value)
```

The generated Lua (in the graph's `<gen_code>`) looks like this:

```lua
return {
    OnOpen = function(self)
        local t1 = Config.GetNumber("Audio.MasterVolume")
        UI.SetNumber("Audio.Volume", t1)
        ...
    end,
    Volume_OnChanged = function(self, Value, Sender)
        Config.SetNumber("Audio.MasterVolume", Value)
        UI.SetNumber("Audio.Decibels", Audio.Decibels(Value))
    end,
    Mute_OnChanged = function(self, Value, Sender)
        Config.SetBool("Audio.Muted", (Value ~= 0))
    end,
}
```

A complete shipped example is LightLab's pause menu:
`Games/LightLab/Content/UI/PauseMenu.nmui` and `PauseMenu.ui.nmgraph`.

## 10. Checklist and troubleshooting

**Exposing a C++ function to UI graphs:**
1. `GAMECORE_DEFINE_SCRIPT_LIBRARY(Name)` in one `.cpp`, with `Lib.Native<&Fn>` (or `NativePure`, `Variable`, `Event`).
2. Rebuild the module. In the editor, **Build > Refresh Script Nodes** (or Compile Source).
3. Open the graph. The members are in the palette under `Name`.
4. Save the graph, which regenerates its Lua.

**Wiring a widget:**
1. Select it. On the event field, **+ > Graph handler**.
2. Build the chain from the UI Event's `Then` pin, then save the graph.
3. Try it in the designer's Preview, or Play-in-Editor.

| Symptom | Cause |
|---|---|
| Log: `'Graph:X' did not run: '...' has no UI graph running` | The document has no `Graph`, or the graph did not load (check earlier log lines). |
| Log: `Config: no config var 'X.Y'` in the Preview | A game module's var, and the editor has not loaded the module: it reads as a default there. It is there in Play-in-Editor and in the game. |
| Log: `... has no handler X` | The graph has no UI Event named `X`: the action and the `Handler` setting differ, or the graph was not saved. The field is outlined in orange in the designer. |
| A library's nodes are missing | The module was not rebuilt or refreshed (Build > Refresh Script Nodes); the library name clashes with a global; or members were rejected (names too long, more than 48 members). Check the log. |
| The node shows as a placeholder | The graph uses a game's node and the game module is not loaded. Load the game, or refresh. |
| Lua error `attempt to index a nil value (global 'MyLib')` at runtime | The library's image was not loaded when the World was bound, or it has unloaded since. |
| A setting does not stick | Check that the widget is bound (`{Bind:Key}`) **and** that the handler writes the var. The binding only shows values; the handler applies them. |
| A pure function runs more often than expected | Pure nodes are called again for each use of their output. Make it a Function (Native) if it has effects or is expensive. |
| The graph has errors and nothing changed | A graph with errors keeps its last good Lua. Fix the errors shown in the Graph Editor's Messages dock, then save. |
