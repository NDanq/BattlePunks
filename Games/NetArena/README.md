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
| `Content/Prefabs/Pawn.nmprefab` | A player's capsule: Transform, Render (capsule mesh, `PawnColor` material), **Net**, NetArenaPawn |
| `Content/Materials/PawnColor.material.nmgraph` | Lit, with a `BaseColor` parameter set per capsule |
| `Saved/Cooked/Content/Levels/`, `Saved/Cooked/Content/Prefabs/` | Their binary forms, cooked ahead for **Shipping** builds (Build > Cook Content) |

The level's entities:

| Entity | Components | Networked? |
|---|---|---|
| Floor | Transform, Render (plane) | No: every process loads it. |
| Sun, Sky | Directional Light, Sky Light | No |
| Camera | Camera | No: each process's own view. |
| Controller | Input | No: each process's own keyboard. |
| SpawnRed, SpawnBlue, SpawnGreen, SpawnYellow | Transform, NetArenaSpawn (colour) | No: the server reads them to place capsules. |
| Spinner | Transform, Render, **Net**, NetArenaSpinner | **Yes:** one entity across the session. |

### Three ways content reaches a networked game

**Placed, not networked** (Floor, lights, Camera, Controller, spawn points).
Every process loads the same world file, so each has its own copy. That is
right for anything that never changes, and for anything that must be
per-process, like the camera and the input reader.

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
- After spawning, the server sets who owns it (`NetComponent::OwnerClient`)
  and its colour, before the end of the frame, when it is first sent.

A game's own components (NetArenaSpawn, NetArenaSpinner, NetArenaPawn) appear
in the editor's Add Component list and Details panel like the engine's. The
editor keeps the game module loaded while editing, so they open and save
with the world.

### Smooth, and answering at once

On a client, the other capsules and the Spinner are shown about 100 ms in
the past, blended between the server's 30 updates a second
(`[Net] InterpolationDelayMs`).

Your own capsule is **predicted**. The prefab's Net component has
**Predicted** on, so:
- the client moves it the moment a key is down;
- each frame's move goes to the server, numbered, in `ServerMove`;
- the server runs the same `ApplyMove`;
- when the server's position comes back, the client replays the moves the
  server had not reached yet.

GUIDE.md, section 8, explains the pattern.

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
