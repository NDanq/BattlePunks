# Split-screen

Two to four local players share one window. Each player gets their own view
of the game, drawn into their own rectangle of the window.

## Views

A game's local players are its avatars' possessors with `LocalPlayer` 0-3
(AVATARS.md). `[Avatar] LocalPlayers` sets how many `NetGameMode` makes.

`GameSession` gives each local player a view, from player 1 up to the last
player that has a camera:
- a camera set with `SetViewTarget(camera, player)`;
- else the camera of the avatar that player possesses, if that avatar's
  harness has View Target set.

A player in between with no camera sees player 1's. Each frame the session
hands the renderer one camera per view and the view count.

| Players | Layout |
|---|---|
| 1 | the whole window |
| 2 | top and bottom halves; left and right with `[Splitscreen] SideBySide` |
| 3 | player 1 across the top half, players 2 and 3 side by side below |
| 4 | quadrants: 1 top left, 2 top right, 3 bottom left, 4 bottom right |

Sometimes the window shows one view whatever the player count:
- while a sequence's camera cut holds the camera, or a cinematic plays;
- with `[Splitscreen] Enabled=false`.

`GameSession::SetViewCount(n)` (Lua `Game.SetViewCount`) fixes the count;
0 goes back to counting cameras.

### Lua

| | |
|---|---|
| `Game.SetViewTarget(camera [, player])` | that player's view (0 first); nil goes back to its avatar's camera |
| `Game.GetViewCount()` | views this frame |
| `Game.SetViewCount(n)` | 1-4, or 0 to count |

## Joining and leaving

Players 2-4 can join and leave during play. Player 1 is always in.

- `GameSession::JoinLocalPlayer(n)` and `LeaveLocalPlayer(n)` (Lua
  `Game.JoinLocalPlayer(n)`, `Game.LeaveLocalPlayer(n)`), where n is 1-3 (0
  is player 1). They ask the mode: `IGameMode::OnLocalPlayerJoin` and
  `OnLocalPlayerLeave` refuse by default.
- `NetGameMode` takes a joining player in: it makes their player and spawns
  their avatar at the next Player Start. A leaving player's avatar and player
  are destroyed. This needs authority (standalone or a listen server).
- **Drop-in**, with `[Splitscreen] DropIn=true`: pressing Start on a pad that
  no player has yet joins that pad's player. The press doesn't reach the
  game. Unplugging a joined player's pad makes them leave. Nothing joins
  while paused or during a cinematic.

Views, listeners and player areas follow by themselves: the next frame has
one more view, or one fewer.

## Making a game split-screen

1. Give each player an avatar with a camera: an Avatar Harness with View
   Target set and a Camera (Active off) in its prefab. A Character Mover's
   camera boom works.
2. Start with more players (`[Avatar] LocalPlayers=2`), or let them drop in
   (`[Splitscreen] DropIn=true`). The mode must take them in: `NetGameMode`
   does.
3. Make sure the level has a Player Start per player (Player Slot 1-4), or
   enough enabled ones. Players sharing one start spawn on top of each other.
4. Bind each player's actions to their pad. The keyboard and mouse are
   player 1's.
5. Open a HUD per player: `GetUI().Open("UI/HUD.nmui", false, player)`, with
   bindings like `{Bind:Score}`.
6. For a top-down or other fixed-angle camera, consider the dynamic split
   (`[Splitscreen] Dynamic=true` and its keys).
7. Check the game's own code for "player 1 only" assumptions. For example,
   `GetLocalAvatar(World, 0)` reads player 1's avatar alone, and WorldsLab's
   PvP treats local players past the first as bots.

## Dynamic split

For games whose cameras all face one way (top-down, isometric, a fixed side
or three-quarter view), players can share one camera while they are close
and split apart as they separate. The window is cut along a line at right
angles to the way between the players. The line turns as they move, the
divider fades in, and the views join again with no seam when the players
come back together. A free-look third-person camera can't share a view like
this, so use the fixed layouts for those.

Turn it on with `[Splitscreen] Dynamic=true`, or from code with
`GameSession::SetSplitCamera(SplitCameraSettings)` (SplitCamera.h). The
session then places cameras of its own, all with the settings' rotation,
`Distance` back from what each looks at. The avatars' cameras aren't used.

- **Two players.** One camera holds both players symmetrically about the
  screen's centre until each is farther out than the centre of their half of
  the window.
  - Past that, the window splits, and each view's camera holds its player at
    the centre of their half, measured along the line between the players.
  - The cameras are solved in perspective and all move in one plane, so this
    holds under a pitched camera too. At the moment of splitting, both views
    are still the shared camera, so nothing jumps.
  - The divider fades in over the first fifth of the distance past the split.
  - The line follows the players at `TurnRate` (per second).
- **Three or four players.** One camera on their centroid while they all fit;
  otherwise the fixed layout, one camera per player. A many-way split is for
  later.
- **The renderer.** In a dynamic split every view is the whole window, and
  each pixel comes from the view whose site is nearest it
  (`Renderer::SetViewSites`, `ViewSites.fsh`).
  - A view that appears starts from view 0's TAA history, exposure and
    motion, rather than from nothing, so antialiasing and exposure carry on
    as it splits off.
  - The cost is a whole frame's pixels per view. While the players fit there
    is one view, so it costs the same as single-player.

| Key | Default | |
|---|---|---|
| `[Splitscreen] Dynamic` | false | the dynamic split |
| `[Splitscreen] DynamicPitch`, `DynamicYaw` | 55, 0 | the cameras' rotation, degrees |
| `[Splitscreen] DynamicDistance` | 1500 | how far back the cameras sit |
| `[Splitscreen] DynamicFov` | 50 | their vertical field of view |
| `[Splitscreen] DynamicMargin` | 0.6 | three or four players: how far out one may get before it splits |

## Rendering

The renderer draws each view with its own camera, lights, post process
volumes and render targets, then places the views in the window. The UI and
"stat unit" draw over the whole window. RENDERPIPELINE.md, "Views", covers how.

Each view adds roughly the CPU cost of a frame: culling, queueing draws and
shadows are done once per view. The pixel cost stays about the same, because
each view is smaller.

## Input

Each local player plays on their own pad. The keyboard and mouse belong to
one player, player 1 by default (`[Input] KeyboardMousePlayer`, -1 for
nobody). For any other player they read as up and still. INPUT.md, "Local
players", has the details. A menu consumes only player 1's pad, so the
others keep playing while it is open.

## Audio

Each view gets a listener at its camera. Every 3D sound is heard from the
listener nearest it, and from that one only, so each player hears what is
near them. One-shots are culled by their distance
from the nearest listener. A level's Audio Listener is ignored while there is
more than one view. Engine/Modules/Audio/README.md has the details.

## UI

A document opened for one player (`GetUI().Open(path, persistent, player)`,
Lua `UI.Open(path, persistent, player)`) is that player's copy. Two players
opening a HUD get one each.
- It is laid out in the player's view rectangle. In the dynamic split it uses
  a fixed half of the window (quadrant with more players), so a HUD stays in
  its player's corner while the line turns.
- It reads that player's keys: `{Bind:Score}` is `Player<N>.Score`.
- It is navigated by that player's pad, and as a menu it holds only that
  player.

A whole-window document (a pause menu) lays out against the window. Player
1's pad and the keyboard navigate it, and so does any player's pad with
`[Splitscreen] MenuAnyPad` (on by default). Engine/Modules/UI/README.md has
the details.

## Config

| Key | Default | |
|---|---|---|
| `[Splitscreen] Enabled` | true | a view per local player; off, one view |
| `[Splitscreen] SideBySide` | false | two players left and right |
| `[Input] KeyboardMousePlayer` | 0 | the local player the keyboard and mouse belong to; -1 nobody |
| `[Splitscreen] MenuAnyPad` | true | any player's pad navigates a whole-window menu, not only player 1's |
| `[Splitscreen] Views` | 0 | debugging: draw this many views of the default scene, whatever the game says |
| `[Splitscreen] DebugOffset` | "" | debugging: `x,y,z` moves view i's borrowed camera by i times that |

Try any world with `Splitscreen.Views 4` in the console. To see players 2-4
in a level, start it with `-Avatar.LocalPlayers=4`.

| Key | Default | |
|---|---|---|
| `[Splitscreen] DropIn` | false | Start on a pad joins its player; unplugging leaves |

## Not here yet

- **Online split-screen.** A networked client has one player (NetHost makes
  one per connection). Split-screen works standalone and on a listen
  server's machine.
- **Per-player settings.** Input rebinds, look sensitivity and other
  settings are shared by all players.
- **A many-way dynamic split.** Three or four players apart use the fixed
  layout.
- **Mirrors and reflection captures** are drawn from player 1's view.
- **Cost.** Each view adds a frame's culling and shadow work. There are no
  lower-cost shadow settings per view yet.
