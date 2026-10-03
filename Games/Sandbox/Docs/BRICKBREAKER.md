# BrickBreaker design

BrickBreaker is Sandbox's start level: a small brick-breaking game whose real
job is to show the game framework (`Engine/Source/GAMEFRAMEWORK.md`) working.
This document explains how it is built and why. The framework's own API is
described in `GAMEFRAMEWORK.md`; this covers only what the game does with it.

---

## Goals

1. **One set of rules, three ways of scripting.** Each piece of the game is
   driven differently, and all three go through the same rules:
   - the paddle in native C++,
   - the bricks in a script graph,
   - the ball in plain Lua.
2. **The rules live in the mode.** No prefab decides what a lost ball or the
   last brick means; it reports the event, and `BrickBreakerGameMode` chooses
   the next phase.
3. **Everything is a prefab.** The level spawns prefab instances and nothing
   else, the way Sandbox's other level does.
4. **It can run unattended.** It can play itself, lose on purpose and quit on a
   timer, so a smoke run checks the whole loop without anyone at the keyboard.

Not goals: rendering polish (everything draws as a white quad; see
**Limitations**), physics (collision only reports overlaps; the ball's
script does its own bouncing), and an on-screen display (score, lives and
phases go to the log).

---

## Files

| File | Role |
|---|---|
| `Source/Private/Sandbox/BrickBreaker.cpp` | The level, the mode, the state, and the paddle's native prefab |
| `Source/Public/Sandbox/BrickBreaker.h` | `BrickBreaker::GetLevel()`, for the module to register |
| `Source/Private/Sandbox/SandboxGame.cpp` | `SandboxGameModule` registers the level and returns it as the start level |
| `Source/Public/Sandbox/SandboxScripting.h` | The collision layers `SandboxLayers::Paddle`, `Ball` and `Brick` |
| `Prefabs/Brick.nmprefab` | The brick prefab, which names its script graph |
| `Scripts/Brick.script.nmgraph` | The brick's behaviour; its Lua is generated into `<gen_code>` |
| `Prefabs/Ball.nmprefab` | The ball prefab, which names `Scripts/Ball.lua` |
| `Scripts/Ball.lua` | The ball's behaviour |
| `Config/DefaultEngine.ini` | `[Game] StartLevel=BrickBreaker`; `[BrickBreaker] AutoPlay` and `RunSeconds` |
| `Source/Private/Sandbox/SandboxInput.cpp` | The `Default` input map the paddle reads (`MoveX`, `Jump`) |

---

## Architecture

```
                         ┌──────────────────────────────────────┐
                         │ BrickBreakerGameMode (C++ rules)     │
                         │   CanEnterPhase   OnPhaseChanged     │
                         │   OnLivesChanged  OnScoreChanged     │
                         │   "BrickBroken" subscriber, OnTick   │
                         └───────────────▲──────────────────────┘
                                         │ hooks
                         ┌───────────────┴──────────────────────┐
                         │ GameSession                          │
                         │   SetPhase / AddScore / AddLives     │──► BrickBreakerGameState
                         │   GetPlayer(0)                       │      (phase, round, bricks left)
                         └──▲─────────────▲──────────────▲──────┘
            SetPhase (C++)  │   Game.*    │   Game.*     │  World events
                 ┌──────────┴───┐  ┌──────┴──────┐  ┌────┴────────┐
                 │ Paddle       │  │ Brick       │  │ Ball        │
                 │ native C++   │  │ script graph│  │ Lua         │
                 │ PlayerState  │  │ AddScore    │  │ AddLives    │
                 └──────────────┘  └─────────────┘  └─────────────┘
```

The prefabs never talk to each other directly:

- The ball finds the paddle as **player 0** (`Game.GetPlayer(0)`, which looks
  for the entity whose `PlayerState` has index 0), not by name.
- The bricks report to the mode through an event (`Events.Emit("BrickBroken")`)
  that the mode subscribed to.
- Everything else goes through the session, so the mode sees every change.

---

## The phases

The game is a small state machine. The engine owns `WaitingToStart` and
`GameOver`; BrickBreaker adds the other three.

| Phase | The ball | The paddle | Leaves when |
|---|---|---|---|
| `WaitingToStart` | where it was spawned | still | the mode's `OnStart` enters `Serving` |
| `Serving` | rides 2 px above the paddle | steers; **Jump** asks for `Playing` | Jump, or 1 s with AutoPlay |
| `Playing` | moves and bounces | steers | the ball is lost, or the last brick breaks |
| `RoundCleared` | stopped | steers | 1.5 s pass (`OnTick`), then the next round |
| `GameOver` | stopped | steers; **Jump** asks for `Serving` | Jump, or 2 s with AutoPlay |

```
WaitingToStart ──► Serving ──Jump──► Playing ──ball lost, lives > 0──► Serving
                      ▲                 │
                      │                 ├──ball lost, lives = 0──► GameOver ──Jump (new game)──┐
                      │                 │                                                      │
                      │                 └──last brick──► RoundCleared ──1.5 s, round + 1───────┤
                      └────────────────────────────────────────────────────────────────────────┘
```

**Rules (`CanEnterPhase`):**
- `Playing` may only follow `Serving`. A stray request (a second Jump, or a
  script asking for it) is refused.
- `RoundCleared` may only follow `Playing`. A `BrickBroken` that arrives
  during a restart cannot clear a round.
- Every other change is allowed.

**Reactions:**
- **`OnLivesChanged`** enters `Serving` if lives remain, otherwise `GameOver`.
  The ball only says it fell out; the mode decides what that means.
- **`OnPhaseChanged`**, on `GameOver → Serving`, starts a new game:
  1. destroys the remaining bricks;
  2. sets the player's score to 0 and lives to 3, written straight to the
     component (this is set-up, not scoring);
  3. sets the round to 0;
  4. spawns a fresh wall.

  It also logs `GameOver` and `RoundCleared`.
- **`OnTick`** ends `RoundCleared` once the phase has lasted 1.5 s
  (`GetPhaseSeconds()`): it adds a round, spawns a new wall and enters
  `Serving`.

**Why the phase lives in the framework, not in BrickBreaker.** Each of the
three prefabs must see the same phase, in the same frame, in a different
language. Keeping it in the `GameState`, readable through `Game.IsPhase` and
announced by `PhaseChanged`, removes any game-specific way to share it.

---

## The three prefabs

### Paddle: native C++

`native:BrickBreaker.Paddle`, declared with
`GAMECORE_DEFINE_NATIVE_PREFAB_BEHAVIOR`.

**Components:**
- Transform
- Render (no mesh, so it draws as a unit quad scaled to 128 × 16)
- Bounds (a unit box, z −1..1)
- Input (the `Default` map)
- Collision (layer `Paddle`, mask `Ball`)
- **PlayerState** ("Player 1", index 0, 3 lives)

**Behaviour:** one callback, `OnPhase[TP_PrePhysics]`. Each frame it:
1. reads `MoveX` and moves the paddle at up to 900 px/s, keeping it on screen;
2. turns a Jump press into a phase request: `Playing` while Serving,
   `Serving` while GameOver.

It finds the rules with `FindGameSession(*InSelf.GetWorld())`.

**Why C++:**
- The paddle is the player, and the player's state is something C++ would own
  in a real game.
- It shows a native prefab and a `PlayerState` set up in a prefab builder.
- `TP_PrePhysics` runs after the engine has resolved input and before the
  ball moves, so a launch requested this frame moves the ball this frame.

### Brick: script graph

`Prefabs/Brick.nmprefab`.

**Components:**
- Transform
- Render (a quad, 110 × 28)
- Bounds
- Collision (layer `Brick`, mask `Ball`)

**Script:** `Scripts/Brick.script.nmgraph`. It has two variables, `Hits`
(default 1) and `Points` (default 10), and runs one event:

```
On Overlap Begin
  → Set Hits = Hits − 1
  → Branch (Hits <= 0)
      True → Game.AddScore(Game.GetPlayer(0), Points)
           → Events.Emit("BrickBroken")
           → Destroy Prefab
```

The generated Lua, as stored in the graph's `<gen_code>`:

```lua
OnOverlapBegin = function(self, other, mine)
    local Score1
    local t1 = self.Hits - 1
    self.Hits = math.floor(t1)
    local t2 = self.Hits <= 0
    if t2 then
        local t3 = Game.GetPlayer(0)
        Score1 = Game.AddScore(t3, self.Points)
        Events.Emit("BrickBroken")
        self.Instance:Destroy()
    end
end,
```

**Why a graph:**
- The brick's logic is short, contains no maths beyond a counter, and is the
  kind of thing a designer would change: hit points, points per brick.
- It uses each kind of `Game` node: a value node (**Get Player**) and a
  statement (**Add Score**).

**Why `BrickBroken` is a plain event and not a `Game` library event.** It is
BrickBreaker's own message, not part of the framework. Any script can emit it
with **Emit Event**, and the mode subscribes to it on the World's EventBus.

**Authoring.** The graph was written by hand, and its Lua generated with the
command-line tool, run from `Binaries/Linux_Debug`:

```
./NodeGraphGenerate -module ./SandboxGame.so <path>/Scripts/Brick.script.nmgraph
```

Opening and saving the graph in the editor regenerates the Lua the same way.

### Ball: pure Lua

`Prefabs/Ball.nmprefab`.

**Components:**
- Transform
- Render (a quad, 14 × 14)
- Bounds
- Collision (layer `Ball`, mask `Paddle | Brick`)

**Script:** `Scripts/Ball.lua`.

| Callback | What it does |
|---|---|
| `OnStart` | Reads its half-size from the Transform's scale and subscribes to `Game.PhaseChanged` |
| `OnPhase` (the listener) | On `Playing`, launches up at a random angle within ±25° at `Speed()`. On anything else, stops. |
| `OnPhysics` | **Serving:** sits on top of the paddle. **Playing:** moves by its velocity and bounces off the left, right and top edges. Below the bottom edge it stops and calls `Game.AddLives(Game.GetPlayer(0), -1)`. |
| `OnOverlapBegin` | **Paddle:** bounces up at an angle set by where it hit, from straight up at the centre to 60° at the edge, and sets the ball on the paddle's top face. **Brick:** reverses the axis with the smaller overlap (X for a side hit, Y for a top or bottom hit), pointing away from the brick's centre. |
| `OnDestroy` | `Events.Off` for its subscription |

`Speed()` is `420 + 40 × Game.GetRound()` px/s, so each round is faster.

**Bouncing in more detail:**
- **One bounce per physics step** (`self.Bounced`). Two bricks hit at once
  would otherwise reverse the ball twice and send it straight through.
- **The new direction is a sign, not a negation.** For example,
  `VY = -abs(VY)` rather than `VY = -VY`, so a repeated or late overlap can
  never turn the ball back into what it just left.
- **A paddle hit is ignored while the ball is already moving up.**

**Why Lua.** The ball has the most logic: vectors, bounce geometry, reacting
to events. That is what plain Lua handles well. It also shows script code
asking the framework rather than holding state of its own: whether to move
comes from `Game.IsPhase`, where the paddle is from `Game.GetPlayer`, and a
lost ball goes to `Game.AddLives`.

---

## One frame

What happens in one frame, in order. "World tick" is `World::Tick`; within
each phase, prefab callbacks and scripts run before the systems.

| Step | Where | What happens |
|---|---|---|
| 1 | `GameSession::Tick` | The phase timer advances, then `BrickBreakerGameMode::OnTick` (ends `RoundCleared`; `RunSeconds`) |
| 2 | World tick, `TP_PrePhysics` | The engine's input resolve runs first (`SO_BeforeScripts`), then the paddle's callback: move, and maybe `SetPhase` |
| 3 | `TP_Physics` | The ball's `OnPhysics`: ride or move; maybe `Game.AddLives` |
| 4 | End of `TP_Physics` | Bounds refit into the spatial index |
| 5 | `TP_PostPhysics` | The overlap pass runs first. The ball's and the brick's `OnOverlapBegin` run, then the graph's `AddScore`, `BrickBroken` and `Destroy` |
| 6 | End of `TP_PostPhysics` | The deferred destroy: the brick is gone |

Changes caused inside these steps run within the same frame:

- **A lost ball** (step 3) is handled at once: `AddLives`, then the mode's
  `OnLivesChanged`, then `SetPhase(Serving)`, then `PhaseChanged`, which the
  ball's own listener hears and stops. The ball is riding the paddle again
  by its next `OnPhysics`.
- **A phase change asked for while one is being announced** waits until that
  announcement ends (GAMEFRAMEWORK.md), so every listener sees the phases in
  order.

---

## Collision

| Collider | Layer | Mask | Shape |
|---|---|---|---|
| Paddle | `Paddle` (16) | `Ball` | unit Bounds × 128 × 16 |
| Ball | `Ball` (32) | `Paddle \| Brick` (80) | unit Bounds × 14 × 14 |
| Brick | `Brick` (64) | `Ball` | unit Bounds × 110 × 28 |

- Every Bounds is a unit box with **z −1..1**, so the colliders, all at z = 0,
  overlap face to face rather than relying on zero-thickness boxes.
- The masks mean that only ball-paddle and ball-brick pairs are ever tested.
- There are no wall colliders. The ball's script compares against the
  1280 × 720 view (`FieldWidth`, `FieldHeight`). That is cheaper and keeps the
  wall bounce exact, without a push-out.

---

## Layout

All units are pixels, with +Y pointing down. Sandbox's camera
(`native:Sandbox.Camera`) at the origin gives a 1280 × 720 orthographic view.

| Piece | Where | Size |
|---|---|---|
| Paddle | centred at (640, 670) | 128 × 16 |
| Ball | (640, 650) at spawn; above the paddle while Serving | 14 × 14 |
| Bricks | 10 columns × 5 rows, centred at x = 109 + 118·c, y = 80 + 36·r | 110 × 28, 8 px gaps |

The wall is centred: ten bricks plus nine gaps are 1172 px wide, which leaves
54 px on each side.

---

## State, saving and restoring

| What | Where | Saved |
|---|---|---|
| Phase, time in phase, round | `GameState` (the engine's half) | the `GPHS` chunk |
| Bricks remaining | `BrickBreakerGameState::BricksRemaining` | `GMST`, version 1 |
| Score, lives, name, index | the paddle's `PlayerStateComponent` | with the World (`WRLD`) |
| Brick `Hits`, ball velocity | Lua instance tables | not saved |

**Restoring a save:**
1. `OnPrepareForLoadedGame` clears the World, because the save brings its own
   copy of every entity.
2. `OnPostLoadSave` sets `bRestored`.
3. `OnStart` then counts the bricks the save brought back (instances whose
   prefab path is `Prefabs/Brick.nmprefab`) instead of spawning a new wall.
4. `OnStart` re-enters `Serving`. The ball's velocity was not saved, so a
   game saved mid-rally resumes with a serve.

**How bricks are counted.** The mode counts `BrickBroken` events rather than
recounting instances each frame. A brick's destroy is deferred to the end of
its tick phase, so a recount in the same frame would still see it. The event
is exact at the moment it fires. A restore or a new game recounts or resets
the number.

---

## Configuration

`Config/DefaultEngine.ini`:

```ini
[Game]
StartLevel=BrickBreaker     ; Sandbox for the older demo level

[BrickBreaker]
AutoPlay=0                  ; 1 plays itself, 2 never steers (loses every ball)
RunSeconds=0                ; quit after this many seconds; 0 runs until closed
```

| `AutoPlay` | Steering | Launch | Restart after GameOver |
|---|---|---|---|
| 0 | the player | Jump | Jump |
| 1 | under the ball, 20% of the paddle's width off centre so it returns at an angle | after 1 s of Serving | after 2 s |
| 2 | none | after 1 s of Serving | after 2 s |

With AutoPlay on, steering by the player still takes over. `AutoPlay=2` makes
an unattended run go through lost lives, `GameOver` and a new game.

The level returns `""` from `ILevel::GetStartupScript()`. That stops Sandbox's
`[Scripting] StartupScript` (`Scripts/Main.lua`, written for the other level)
from spawning its demo entities here.

---

## Decisions

| Decision | Why | Cost |
|---|---|---|
| The mode reacts to lives and bricks; the prefabs only report | One place holds the rules, which is the point of the framework | Extra hops: script → session → mode → phase |
| `Score` and `Lives` are read-only to scripts | Every change reaches `OnScoreChanged` / `OnLivesChanged` | Scripts must call `Game.AddScore` rather than set a field |
| The ball does its own bouncing, with collision for overlaps only | GameCore's collision does not push objects apart; a script-side bounce stays readable | Fast balls at low frame rates could tunnel (see Limitations) |
| No wall colliders | Exact, cheap, and no layer to manage | The field size is a constant in the ball's script |
| Bricks are spawned by the mode, not the level | Every round builds a fresh wall, and a new game rebuilds it | The level alone does not show the whole scene in the editor |
| `BrickBroken` is a plain event | It is the game's message, not the framework's | A brick destroyed some other way would not be counted |
| `RestartGame` writes `PlayerState` fields directly | Resetting is set-up, not scoring, so no hook should fire | No `ScoreChanged` for the reset to 0 |

---

## Limitations

- **Everything draws white.** `RenderComponent` has no per-entity tint, so the
  rows cannot be coloured without a material per colour or a tint field on
  the component (a RenderCore change).
- **There is no on-screen display.** Score, lives and phase changes go to the
  log (`[BrickBreaker] ...`, `[Game] phase ...`).
- **Client and Server targets need cooked prefabs.** `Brick` and `Ball` are
  text `.nmprefab` files, and only Editor targets read those. Run `cook
  prefabs` in the editor to write `Cooked/Prefabs/`, as for Sandbox's other
  prefabs.
- **Tunnelling.** The ball moves in fixed steps of `Speed × dt`. At 60 fps in
  round 10 (820 px/s) a step is about 14 px, still less than a brick's height
  plus the ball's. A much lower frame rate or a very late round could step
  past a brick.
- **The launch angle is random** (`math.random`), so two AutoPlay runs do not
  break the same bricks in the same order.
- **A brick removed without its graph** (for example by the editor during
  Play) is not counted, so that round would never clear. Nothing in the game
  does this.

---

## Verification

- **`NimbleEngine.GameFrameworkTests`** (`Engine/Tests/GameFrameworkTests.cpp`)
  covers the framework BrickBreaker relies on: phases, refusals, ordering
  during an announcement, players, the `Game` library from Lua, and the save
  round trip.
- **`NodeGraphGenerate`** compiles `Brick.script.nmgraph` without errors.
- **Unattended runs** on the Editor-Debug build, with `[BrickBreaker]`
  `RunSeconds` set:
  - `AutoPlay=1`: 50 bricks spawn, `Serving → Playing`, each brick scores 10,
    and it quits cleanly with no leaks reported.
  - `AutoPlay=2`: lives go 3 → 2 → 1 → 0, then `Playing → GameOver`, the
    final score is logged, `GameOver → Serving` starts a new game, and the
    score starts again from 10.
- **`[Game] StartLevel=Sandbox`**: the older level still runs, including its
  save-and-load demo.

---

## Possible next steps

- A `Tint` colour on `RenderComponent`, to colour bricks by row and the ball
  by speed.
- Tougher bricks: set `Hits` per row as a prefab-instance override. The graph
  already counts hits.
- A text display bound to the `Game` events (`ScoreChanged`, `LivesChanged`,
  `PhaseChanged`), which every source already emits.
- A `Paused` phase. `Game.RequestPhase("Paused")` and one `CanEnterPhase`
  rule are all it needs, and the ball and paddle already stop outside
  `Playing`.
