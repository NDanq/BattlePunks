# Game framework

The game's phase, its rules and its players, the same from C++, Lua and script
graphs. Public API: `Public/NimbleEngine/GameState.h`, `GameMode.h`,
`PlayerState.h` and `GameSession.h`.

```
           C++ (GameSession)             Lua / script graphs ("Game" library)
                 │                                     │
                 └──────────► GameSession ◄────────────┘   FindGameSession(World)
                                 │
         asks ─► IGameMode::CanEnterPhase      writes ─► GameState (phase, time, round)
                                 │                       PlayerStateComponent (score, lives)
         tells ─► IGameMode::OnPhaseChanged / OnPlayerAdded / OnScoreChanged / ...
                                 │
         emits ─► World events PhaseChanged / ScoreChanged / LivesChanged
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| `GameState` | `GameState.h` | The game's progress, owned by the level. The engine keeps the **phase** here: a name (`GetPhase`, `IsPhase`), the seconds spent in it (`GetPhaseSeconds`) and a round counter (`GetRound`, `SetRound`). The rest belongs to the game's subclass. |
| `IGameMode` | `GameMode.h` | The rules. Besides the lifecycle (`OnLoad`, `OnStart`, `OnTick`, ...), it has optional hooks: `CanEnterPhase`, `OnPhaseChanged`, `OnPlayerAdded` / `OnPlayerRemoved`, `OnScoreChanged` / `OnLivesChanged`. |
| `PlayerStateComponent` | `PlayerState.h`, `"PlayerState"` | One player: `PlayerName`, `PlayerIndex`, `Score`, `Lives`, `bActive`. Being a component, it is saved with the World, placed by prefabs and visible to scripts. |
| `GameSession` | `GameSession.h` | Routes every change through the rules: `SetPhase`, `AddScore`, `AddLives`, `GetPlayer(Index)`, `GetPlayerCount`. |
| `"Game"` library | `GameFramework.cpp` | The session's half for scripts. |

## Phases

A phase is any short name (under 32 characters). `GamePhases` suggests
`WaitingToStart`, `InProgress` and `GameOver`, and a game adds its own.

- **Loading** puts the state in `GetInitialPhase()` (by default
  `WaitingToStart`) with round 0, after `GameState::OnReset`. This is a reset,
  not a transition, so no hook runs. The mode usually calls `SetPhase` in
  `OnStart`.
- **`GameSession::SetPhase(Phase)`** does nothing (and returns true) if the
  game is already in that phase. Otherwise it asks
  `IGameMode::CanEnterPhase(From, To)`. If the mode allows the change, it:
  1. writes the new phase and restarts its timer;
  2. calls `IGameMode::OnPhaseChanged(Previous, Current)`;
  3. emits `PhaseChanged(Phase, Previous)`.

  It returns false if the mode refuses, there is no `GameState`, or the name
  is empty or too long.
- **Asking for a phase while one is being announced**, from `OnPhaseChanged`
  or a `PhaseChanged` listener, queues the request. It is applied, and asked
  of the mode, once the announcement ends, so listeners always see phases in
  order. Such a request returns true because it was accepted for later.
- **The timer** advances in `GameSession::Tick`, before `IGameMode::OnTick`.
- **Saved** in a separate `GPHS` chunk, next to the game's `GMST`
  (`SaveGameFormat.h`), so a game's `Serialize` never has to handle it. A
  restore puts the phase, its time and the round back without any hooks.
  Older saves have no `GPHS` and keep the initial phase.

## Players

A player is any entity with a `PlayerStateComponent`. A native prefab adds one
in its builder, and a script uses `e:AddComponent("PlayerState")`.

- **The mode hears of every player.** When `IGameMode::OnLoad` succeeds, it
  gets `OnPlayerAdded` for each player the level already made. After that, it
  gets `OnPlayerAdded` / `OnPlayerRemoved` as components come and go, and
  `OnPlayerRemoved` also fires when the entity is destroyed.
- **`AddScore(Player, Amount)` / `AddLives(Player, Amount)`** add to the
  component (the amount may be negative). They then call `OnScoreChanged` /
  `OnLivesChanged` with the new value and emit `ScoreChanged` / `LivesChanged`
  `(Player, Value)`. They return false if the entity has no PlayerState.
- **Scripts cannot write `Score` and `Lives` directly**; they are read-only
  there, so every change goes through the rules. C++ may set the fields to
  set up or reset a player, and nobody is told.
- **`GetPlayer(Index)`** finds a player by its `PlayerIndex`.

## From scripts

The `Game` library is available in every World a `GameSession` runs. Called
from anywhere else (a bare test World, the editor's scene), it raises a script
error.

```lua
if Game.IsPhase("Playing") then ... end            -- GetPhase(), GetPhaseTime(), GetRound()
local accepted = Game.RequestPhase("Paused")       -- false if the mode's rules refuse
local paddle   = Game.GetPlayer(0)                 -- GetPlayerCount()
local score    = Game.AddScore(paddle, 10)         -- AddLives(paddle, -1)
paddle:GetComponent("PlayerState"):GetLives()      -- read-only Score / Lives / PlayerIndex

local h = Events.On(Game.PhaseChanged, function(phase, previous) end)   -- ScoreChanged, LivesChanged
Events.Off(h)
```

In script graphs, under **Game**:

- value nodes: **Get Phase**, **Is Phase**, **Get Phase Time**, **Get Round**,
  **Get Player**, **Get Player Count**;
- statements: **Request Phase**, **Add Score**, **Add Lives**;
- **On Phase Changed**, **On Score Changed** and **On Lives Changed**, with
  their Emit nodes.

The PlayerState component has the usual **Component.PlayerState** nodes.

## How scripts find the rules

`FindGameSession(World)` returns the session whose level owns a World, from
the start of `LoadLevel` to `UnloadLevel`. The `Game` library uses it, and so
can a native prefab's callbacks (they receive their instance's World).

## The startup script

`ILevel::GetStartupScript()` controls what runs after `IGameMode::OnLoad`:

- `nullptr` (the default) runs `[Scripting] StartupScript`, as before;
- a path runs that file instead;
- `""` runs nothing.

BrickBreaker returns `""`, so Sandbox's `Scripts/Main.lua` does not run there.

## Example: BrickBreaker

`Games/Sandbox/Source/Private/Sandbox/BrickBreaker.cpp` is Sandbox's start level (design: `Games/Sandbox/Docs/BRICKBREAKER.md`). It drives three
prefabs in three different ways:

| Prefab | Driven by | What it does with the framework |
|---|---|---|
| Paddle, `native:BrickBreaker.Paddle` | C++ (`NativePrefabBehavior`) | Holds the player's `PlayerState`. `Jump` asks `SetPhase("Playing")` while Serving, or `"Serving"` after GameOver. |
| Brick, `Prefabs/Brick.nmprefab` | script graph (`Scripts/Brick.script.nmgraph`) | On overlap, counts its `Brick` component's `Hits` down (more on higher rows). At 0: `Game.AddScore(Game.GetPlayer(0), Points)`, `Events.Emit("BrickBroken")`, then destroys its prefab. |
| Ball, `Prefabs/Ball.nmprefab` | Lua (`Scripts/Ball.lua`) | Moves only while `Game.IsPhase("Playing")` and launches on `PhaseChanged`. Out of the bottom: `Game.AddLives(Game.GetPlayer(0), -1)`. |

The phases run `WaitingToStart → Serving → Playing`, and from Playing:

- a lost ball goes back to `Serving`;
- losing the last life goes to `GameOver`, and Jump starts a new game;
- breaking the last brick goes to `RoundCleared`, then the next round, a little
  faster.

`BrickBreakerGameMode` holds the rules:

- `CanEnterPhase` allows `Playing` only from `Serving`;
- `OnLivesChanged` picks `Serving` or `GameOver`;
- counting `BrickBroken` events triggers `RoundCleared`;
- `GameOver → Serving` resets the game.

`[BrickBreaker] AutoPlay=1` plays by itself; `AutoPlay=2` never steers, so it
loses every ball. `[BrickBreaker] RunSeconds` quits after a while.
`[Game] StartLevel=Sandbox` brings back the older demo.

Client and Server targets need cooked prefabs for `Brick` and `Ball` (`cook
prefabs` in the editor writes `Cooked/Prefabs/`), as Sandbox's other
`.nmprefab` files do.
