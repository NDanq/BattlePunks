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

- **Two players.** One camera looks at their midpoint while both stay within
  `Margin` of the screen's centre (0.6 of half the screen by default).
  - Past that, the window splits. Each view looks at a point that slides from
    the midpoint toward its player, so each player stays at the margin on
    their own side.
  - At the moment of splitting, both views still look at the midpoint, so
    nothing jumps.
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
| `[Splitscreen] DynamicMargin` | 0.6 | how far out a player may get before it splits |

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
