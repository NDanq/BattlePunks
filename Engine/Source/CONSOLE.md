# The console

An in-game console for every game, in the launcher and in Play-in-Editor.
The tilde key drops it down over the game. Type a command and press Return.
Public API: `Public/NimbleEngine/GameConsole.h`.

```
> help
> open BakedLighting
> Shadows.Enabled 0
> cvars Shadows
```

A line is a registered command. If no command has that name, the line goes
to Core's `ExecConfigCommand`, as in the editor's Console panel:
`Section.Name` prints a config var and `Section.Name value` sets it. The
console also shows the engine's log as it arrives.

## Keys

| Key | Does |
|---|---|
| `` ` `` / `~` (Grave) | Opens and closes it |
| Escape | Closes it |
| Return | Runs the line |
| Up, Down | Steps through the lines run before |
| Tab | Completes a command's or config var's name. With several matches, it completes as far as they agree and lists them. |
| Left, Right, Home, End | Moves the cursor |
| Backspace, Delete | Deletes |
| PageUp, PageDown | Scrolls the output |

While it is open, the console takes all input before the UI and the game do
(INPUT.md, "The UI"). The game keeps running under it. It stays open across a
level change.

## Built-in commands

| Command | Does |
|---|---|
| `help [command]` | Lists the commands, or describes one |
| `echo <text>` | Prints the text |
| `clear` | Empties the console |
| `cvars [filter]` | Lists config vars and their values. `*` marks one changed from its default. |
| `reset <Section.Name>` | Puts a config var back to its compiled default |
| `open <level>` | Loads a level by its registered name (`GameSession::RequestLevel`) |
| `pause` | Pauses or resumes the World |
| `quit` | Ends the game |

## A game's own command

Declare it at file scope in any module's .cpp. It registers when the module
loads and unregisters when it unloads, so a hot reload replaces it. In the
body, `Console` is the `ConsoleCommandContext`: `Console.Args` (the words after
the name, with quotes grouping words), `Console.Session`, `Console.Print` and
`Console.PrintError`.

```cpp
#include "Public/NimbleEngine/GameConsole.h"

NIMBLE_CONSOLE_COMMAND(sun, "<Standard|Modulated|Cascaded>", "Set the sun's shadow technique")
{
    if(Console.Args.size() != 1)
    {
        Console.PrintError("Usage: sun <Standard|Modulated|Cascaded>");
        return;
    }
    GameCore::World& World = Console.Session.GetWorld();
    // ...
    Console.Print("sun: " + Console.Args[0]);
}
```

Command names are typed without regard to case. A name registered twice runs
the newer command until it is unregistered.

## Config

| Var | Default | |
|---|---|---|
| `[Console] Enabled` | `1` | `0` turns it off; a shipping game's `.ini` may want that |
| `[Console] ToggleKey` | `Grave` | Any key name an input map takes (INPUT.md, "Keys are names") |

## How it works

- **One per session.** `GameSession` owns a `GameConsole`
  (`GameSession::GetConsole`). In `Tick`, `ProcessInput` runs on the host's
  input before the UI does. `Update` runs after the World, before the UI's
  `Update`.
- **Text.** While open, it turns on the host's text input
  (`IGameHost::SetTextInputActive`). It reads `InputState::GetText()` for
  characters and `GetPressesAndRepeats` for held editing keys (INPUT.md,
  "Typed text").
- **The overlay** is a UI document built in code, not loaded from a file, so
  every build has it with nothing to cook. It is opened persistent under the
  path `Engine/Console`, on layer 100000, with `InputMode` `GameOnly`. Its text
  comes from the data store keys `Console.Log` and `Console.Prompt`. It shows
  16 lines; the console keeps the last 512.
- **The log.** While a console exists, it chains a `LogHandler` in front of the
  host's. Messages from any thread queue up and are drained on the game
  thread. The host's handler still gets every message first. What the console
  prints itself also goes to the log, but is not shown twice.

## Tests

`NimbleEngineGameFrameworkTests` ("console") covers:

- the toggle, and text input turning on and off;
- typed text going into the line, and not to the game;
- a registered command run with its quoted words;
- Backspace with its repeats, and inserting at the cursor;
- the history and Tab completion;
- setting a config var, and a line nothing handles;
- the log showing in the console;
- Escape closing it, and staying open across a level change.

`NimbleEngineInputTests` ("text and repeat") covers `InputState`'s text and
repeat counts.
