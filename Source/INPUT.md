# Input

SDL input, through GameWindow, to a per-frame snapshot, to named actions and
axes that an entity reads. Public API: `Public/NimbleEngine/Input.h`.

```
SDL events ──► GameWindow callbacks ──► InputRouter (pending) ──Latch()──► InputState (this frame)
                 (inside PumpEvents)                                         │
                                                       TP_PrePhysics, before scripts
                                                                             ▼
                                         InputMap (bindings) ──► InputComponent: Down / Pressed / Released / Axes
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| `InputState` | `Input.h` | One frame's devices. Keys, mouse buttons and pad buttons have down / pressed-this-frame / released-this-frame. Edges are counts, so a tap that is pressed and released inside one frame is both. It also holds mouse position, delta and wheel, and pad sticks and triggers per player. |
| `InputRouter` | `InputRouter.h` (host side) | Takes over a window's delegate table and GameWindow's process-wide gamepad table, accumulates events as `PumpEvents` delivers them, and publishes them with `Latch()`. Focus loss releases everything. The launcher's `Application` owns one; the editor's Play-in-Editor owns another. |
| Input maps | `NIMBLE_DEFINE_INPUT_MAP` | Named actions and axes, bound to key names. A game declares them in its module, and they unregister with it. |
| `InputComponent` | `"Input"` | Which map an entity reads (`Map`), and as which player (`PlayerIndex`). Resolved each frame. |
| Resolve | `GameSession` | `ResolveInputComponents`, registered on every level's World as the first `TP_PrePhysics` system, with `SO_BeforeScripts`. |

## When it runs

1. **The pump.** `PumpEvents` delivers SDL events to the router, on the game thread.
2. **`Latch`.** What arrived becomes this frame's `InputState`.
3. **`GameSession::Tick`.** `IGameMode::OnTick` can read `GameSession::GetInput()`.
4. **`World::Tick`, `TP_PrePhysics`.** The input resolve runs **before** the
   phase's scripts, then the scripts, then the other systems. Every reader
   this frame sees this frame's input.

## Keys are names

Keys, buttons and axes are named, and lookups ignore case:

| Device | Names |
|---|---|
| Keyboard keys | `W`, `Space`, `LeftShift`, `F5`, `KP_Enter` (physical keys, so `W` stays `W` on AZERTY) |
| Mouse buttons | `Mouse.Left`, `Mouse.Right` |
| Mouse axes | `Mouse.DeltaX`, `Mouse.DeltaY`, `Mouse.WheelX`, `Mouse.WheelY` |
| Gamepad buttons | `Gamepad.South`, `Gamepad.East`, `Gamepad.West`, `Gamepad.North`, `Gamepad.Back`, `Gamepad.Guide`, `Gamepad.Start`, `Gamepad.LeftStick`, `Gamepad.RightStick`, `Gamepad.LeftShoulder`, `Gamepad.RightShoulder`, `Gamepad.DPadUp` … `Gamepad.DPadRight` |
| Gamepad axes | `Gamepad.LeftX`, `Gamepad.LeftY`, `Gamepad.RightX`, `Gamepad.RightY`, `Gamepad.LeftTrigger`, `Gamepad.RightTrigger` |

GameWindow's key enum never reaches game code. The full key list is
`GameWindowKeyNames.cpp`.

## Gamepads

Up to four pads, one per player. A pad takes the lowest free player slot when
it connects and keeps it until it disconnects.

- **Reading by player.** `InputComponent::PlayerIndex` picks whose pad an
  entity reads. Keyboard and mouse belong to player 0.
- **Dead zone.** Sticks have a dead zone of 0.15 per axis
  (`InputRouter::SetStickDeadZone`), rescaled so that a full push still reads 1.
- **Triggers** have no dead zone. As buttons, a trigger or stick is down past
  half way.
- **Unplugging** releases everything the pad held.
- **Focus.** SDL ignores button presses while the game's window does not have
  focus.

## Typed text

Keys are physical; text is not. `InputState::GetText()` is the frame's typed
text, UTF-8 after the keyboard layout and IME (GameWindow's `OnText`). It only
arrives while the host's text input is on (`IGameHost::SetTextInputActive`,
`InputRouter::SetTextInputActive`), which the console turns on while it is
open. A text field's Backspace and arrows read
`InputState::GetPressesAndRepeats`, which counts the OS's auto-repeats too;
`WasPressed` never does.

## Mouse capture

`GameSession::SetMouseCaptured(true)` (Lua: `Input.SetMouseCaptured(true)`)
hides the cursor and holds it in the window. `Mouse.DeltaX` and
`Mouse.DeltaY` keep reporting motion past the screen's edge, for mouse-look.
The host lets go when the run ends.

## Maps

```cpp
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Action("Jump",  "Space, Gamepad.South");
    Map.Axis  ("MoveX", "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis  ("LookX", "Mouse.DeltaX*0.2");
}
```

- **Actions.** An action is down while any of its keys is.
  - **Pressed** and **Released** are the action's own edges: a second bound
    key does not press a held action again.
- **Axes.** An axis sums its keys: a button counts 1 and an analog key its
  value.
  - **Modifiers:** `-` negates a key and `*0.5` scales it.
  - **Clamping:** the sum is clamped to -1..1, unless a mouse axis is bound.
    Mouse look wants pixels.
- **Limits:** 32 actions and 16 axes per map, 8 keys each.

**Rebinding.** Each entry is a config var, `[Input_<Map>] <Name>`, so an
.ini or the console rebinds it without a rebuild:

```ini
[Input_Default]
Jump = K, Gamepad.South
```

`Input_Default.Jump Space` in the console does the same, live.

## Reading it

**C++.** A system or the mode reads the component:

```cpp
const auto* Input = Entity.GetComponent<NimbleEngine::InputComponent>();
if(Input->WasPressed("Jump")) { ... }
const float X = Input->GetAxis("MoveX");
```

To read many components in a hot loop, `FindInputAction(Map, Name)` gives
an action's bit index.

**Lua.** The same queries on the component, plus the `Input` library for
raw device queries:

```lua
local input = self.Entity:GetComponent("Input")
if input:WasPressed("Jump") then ... end
local x = input:GetAxis("MoveX")
if Input.WasKeyPressed("F3") then print(Input.GetMousePosition()) end
```

**Script graphs.** These nodes come for free:
- `Input: Is Down`, `Was Pressed`, `Was Released` and `Get Axis` under
  Components/Input;
- the `Input` library's nodes.

## The UI

A game session's UI (`GameSession::GetUI`, Engine/Modules/UI/README.md) sees
each frame's input **before** the game, and what it takes the game does not:

1. `GameSession::Tick` copies the host's `InputState`, builds a
   `UI::UIInputFrame` from it (the mouse, unless it is captured, and the `UI`
   map's navigation) and calls `UISystem::ProcessInput`.
2. What the result says was taken is consumed from the copy --
   `InputState::ConsumeMouseButtons`, `ConsumeKeyboard`, `ConsumeGamepad`:
   up, with no edges.
3. Everything the game reads reads the copy: `GameSession::GetInput()`, the
   `Input` library, and every `InputComponent` (resolved from it in
   `TP_PrePhysics`).

So a click on a button never fires the weapon behind it, and a menu's Escape
never reaches the game. A key consumed while held looks to the game as if it
went up without a release edge; an `InputComponent` reports it released.

| Document `InputMode` | Takes |
|---|---|
| `UIOnly` (a menu) | everything, and shows the cursor |
| `GameAndUI` (a HUD) | the mouse over a widget that takes hits; keys only while one of its widgets has focus |
| `GameOnly` | nothing |

The `UI` input map (NimbleEngine's `GameUI.cpp`) is what menus navigate with:
Up, Down, Left, Right (arrows, the D-pad), Accept (Return, Space, South), Back
(Escape, East) and Next (Tab; Shift+Tab goes back). Rebind it as any map:
`[Input_UI] Accept=Return, Gamepad.South`. `WasInputActionPressed(Map, Action,
State)` reads a map's action straight from a state, for a reader with no
`InputComponent`.

**Typing into the UI.** While a UI text field has focus (a `TextInput`), the
frame also carries the typed text (`InputState::GetText`) and the editing
keys' presses and repeats (Backspace, Delete, Left, Right, Home, End, Return,
Shift, Ctrl+A/C/X/V), and the result asks for text input:
`GameSession` turns the window's text input on (`IGameHost::SetTextInputActive`)
while it does, and off after -- again after the console closes, which turns
it off. The field's copy and paste use the system clipboard through
`IGameHost::GetClipboardText` / `SetClipboardText` (the launcher: SDL's;
Play-in-Editor: Qt's).

Ahead of the UI, the console (CONSOLE.md) sees the input first: the toggle
key (`Grave`, the tilde key) opens it, and while it is open it consumes
everything (`InputState::ConsumeAll`), so neither the UI nor the game sees any
of it.

While the session is paused (`GameSession::SetPaused`) the World does not tick,
but `InputComponent`s are still resolved from the copy, so what a pause menu
consumed is not waiting for the game when it resumes.

## The Sandbox example

| File | Shows |
|---|---|
| `SandboxInput.cpp` | The `Default` input map: Jump, Boost, MoveX and MoveY, for keys and pads |
| `SandboxGame.cpp` | Marker with an Input component. The `Sandbox.DriveMarker` PrePhysics system steers it from MoveX/MoveY, faster with Boost. Marker drifts on its own until the player first steers it, so an unattended run still plays. |
| `Scripts/JumpCounter.script.nmgraph` | A graph on Marker: `Input: Was Pressed (Jump)` into a Branch, counting jumps |
| `Scripts/InputDemo.lua` | Hand-written Lua: the component's actions, plus the `Input` library's raw keys (F3 prints the mouse) and mouse capture (C) |
| `Config/DefaultEngine.ini` | A commented `[Input_Default]` rebinding |

## Tests

`NimbleEngineInputTests` (ctest `NimbleEngine.InputTests`) needs no window.
It covers:

- key names;
- frame edges and taps;
- latching;
- per-player pads;
- the resolve running ahead of a `TP_PrePhysics` script;
- action edges with two bound keys;
- axis sums and scales;
- rebinding through config;
- a disabled component;
- a **virtual SDL gamepad** through the real pump and GameWindow's gamepad
  table: connection, a button, the stick dead zone, a trigger, and unplugging.
