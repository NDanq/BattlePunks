# NetArena

Networking's test game: capsules on a flat plane, one per player. Each player
steers their own with WASD, the arrow keys or the left stick. The first
player's capsule is red, the second's blue, then green and yellow. A bar in
the middle turns, and every player sees it at the same angle.

## How it is authored

Everything placed in the game is content you open and edit in the editor. The
code (`NetArenaGame.cpp`) only gives it behaviour.

| File | What is in it |
|---|---|
| `Content/Levels/NetArena.nmworld` | The level, below |
| `Content/Prefabs/Pawn.nmprefab` | A player's capsule: Transform, Render (capsule mesh, `PawnColor` material), **Net** (Predicted), NetArenaPawn, **Avatar Harness** (Simulation `NetArenaMove`) |
| `Content/Prefabs/Kart.nmprefab` | A kart anyone may drive: **Avatar Harness** (Requestable, Simulation `NetArenaKart`), **Net** (Predicted), a Body child (box) and a ChaseCamera child (Camera, Active off). Script: `Scripts/Kart.lua` |
| `Content/Prefabs/Signpost.nmprefab` | A pole and board. Script: the graph `Scripts/Signpost.script.nmgraph` |
| `Content/Materials/PawnColor.material.nmgraph` | Lit, with a `BaseColor` parameter set per capsule |
| `Saved/Cooked/Content/Levels/`, `Saved/Cooked/Content/Prefabs/` | Their binary forms, cooked ahead for **Shipping** builds (Build > Cook Content); made on each machine, not tracked |

The level's entities:

| Entity | Components | Networked? |
|---|---|---|
| Floor | Transform, Render (plane) | No: every process loads it. |
| Sun, Sky | Directional Light, Sky Light | No |
| Camera | Camera | No: each process's own view. |
| SpawnRed, SpawnBlue, SpawnGreen, SpawnYellow | Transform, Player Start (kept for player 1..4, naming the Pawn prefab) | No: the server reads them to place capsules. |
| Spinner | Transform, Render, **Net**, NetArenaSpinner | **Yes:** one entity across the session. |
| KartRed, KartBlue | instances of `Kart.nmprefab`, by the red and blue starts | **Yes:** placed, and driven by whoever gets in. |
| Signpost | an instance of `Signpost.nmprefab` | No: its graph runs on every process. |

### Three ways content reaches a networked game

**Placed, not networked** (Floor, lights, Camera, Player Starts).
Every process loads the same world file, so each has its own copy. That is
right for anything that never changes, and for anything that must be
per-process, like the camera.

**Placed and networked** (the Spinner). Add a **Net** component in the editor
(Details > Add Component > Net, Replicate on).
- Every process still loads it, but the server gives its copy a network id,
  and each client's copy is matched to it by the entity's GUID.
- The result is one entity across the session.
- Only the server moves it (`NetArena.Spin` checks `HasAuthority()`), and
  Transform replication shows that to everyone. A client that joins late
  sees it at the server's current angle.

**Spawned and networked** (the capsules).
- Author it as a prefab with a **Net** component, and spawn it **on the
  server only**.
- NetCore sends it whole to every client, including ones that join later.
- The mode is an `AvatarGameMode` (Engine/Source/AVATARS.md). It spawns a
  capsule at the Player Start kept for each player and gives it to that
  player's client (`NetComponent::OwnerClient`). It also possesses the
  capsule, so its Avatar Harness reads that player's keys. In
  `OnAvatarSpawned` it sets the colour, before the end of the frame, when the
  capsule is first sent.

A game's own components (NetArenaSpinner, NetArenaPawn) appear
in the editor's Add Component list and Details panel like the engine's. The
editor keeps the game module loaded while editing, so they open and save
with the world.

### Smooth, and answering at once

On a client, the other capsules and the Spinner are shown about 100 ms in
the past, blended between the server's 30 updates a second
(`[Net] InterpolationDelayMs`).

Your own capsule is **predicted**. The prefab's Net component has
**Predicted** on, and its harness names the `NetArenaMove` simulation, so the
engine does the following:
- the client moves it the moment a key is down, with `NetArenaMove`;
- the harness sends each frame of input to the server, numbered;
- the server runs the same `NetArenaMove` for each frame;
- when the server's position comes back, the client replays the frames the
  server had not reached yet.

`NetArenaGame.cpp` holds no networking code of its own. AVATARS.md,
"Prediction", explains it.

## The avatar demo: karts

Walk a capsule up to a kart and press **E** (pad: West) to get in. The view
moves to the kart's chase camera, WASD drives it at twice the speed, and the
kart lights up in your colour. **E** again gets you out: your capsule is put
beside the kart, and the view goes back to the arena camera. Everyone sees
who is driving what.

| Piece | What it shows (Engine/Source/AVATARS.md) |
|---|---|
| **Kart prefab** | A second kind of avatar. Its harness is **Requestable**, and its own simulation `NetArenaKart` is predicted like the capsule's. Its child camera becomes the driver's **view target**; it is Active off, so nobody else sees through it. |
| **`NetArena.Interact`** (C++) | Gameplay from the harness on the server. Interact in a capsule `Possess`es the nearest free kart within 300 units; Interact in a kart possesses the player's capsule again. No RPC: the harness carried the press. |
| **`Scripts/Kart.lua`** (Lua) | `OnPossessed` / `OnUnpossessed` on every process: each tints its own copy of the kart in the driver's colour. |
| **`Scripts/Signpost.script.nmgraph`** (script graph) | Reads this machine's own avatar each tick (Game: Get Local Avatar, then Was Pressed Interact), and hears **On Unpossessed** for everyone. |
| **`Scripts/Main.lua`** | `Events.On(Game.Possessed, ...)` from the startup script. |

The karts and the signpost are placed in the world file, so the editor opens
and edits them like everything else. `-NetArena.AutoKart=1` (with AutoMove)
has the bot get in after a second and out five seconds later, for unattended
runs.

**Why the Lua and the graph are on placed props, not on the capsule.** A
prefab instance placed in the level is loaded by every process, script and
all. A prefab *spawned* on the server reaches clients as its components
only, so a script on the spawned capsule would run on the server alone.

## Editing

1. Build the editor tree (`cmake --preset editor-debug`, then
   `cmake --build --preset editor-debug`; on Linux, `editor-debug-clang`) and open
   `Content/Levels/NetArena.nmworld`.
2. Move a spawn point, change the Spinner's *Degrees per second*, or add a Net
   component to another entity, and save.
3. Debug and RelWithDebInfo Client and Server builds read the text sources and
   cook them themselves when they load them, so there is nothing to do while
   developing. A **Shipping** build reads only the cooked files. Before
   shipping, run **Build > Cook Content** in the editor: the listed worlds,
   the prefabs and UI documents go to `Saved/Cooked/<their path>`.

## Playing it

**In the editor (Play-in-Editor):**

1. With the world open, open **Edit > Editor Settings > Play** (or
   **Play > Network Settings...**).
2. Set **Network mode** to **Listen Server**. The viewport hosts, and is the
   red player.
3. Press **Play** (F5).
4. Press **Play > New Client Window** (Shift+F6). The game opens in a window
   of its own and joins as blue.

Or set **Network mode** to **Client** with **Start a dedicated server** on. The
editor starts `NetArenaServer`, and the viewport and each client window join
it.

**Build > Compile Source** rebuilds the game module, `NetArena` and
`NetArenaServer`.

**From a command line**, in `Binaries/<Platform>_Debug`:

```
NetArenaServer -port=7777
NetArena -connect=127.0.0.1:7777
NetArena -connect=127.0.0.1:7777
```

or `NetArena -listen` and one `NetArena -connect=127.0.0.1:7777`.

## Testing without a keyboard

`-NetArena.AutoMove=1` (or `NetArena.AutoMove 1` in the console) steers this
process's capsule round a square by itself. Every 5 seconds each process logs
what it sees. A client adds its prediction: how many server states it
replayed its moves on, and the furthest one moved its capsule. 0u means
client and server agree:

```
[NetArena] red(you)=(-900,-408) blue=(832,-400) spinner=42deg predicted: 149 states, 1 pending, worst correction 0u
```

Put `-NetArena.AutoMove=1` in **Editor Settings > Play > Client window
arguments** to have the editor's client windows run it too.

`Scripts/Main.lua` shows the Lua side: it reports which role it runs in, and
hears clients join and leave.
