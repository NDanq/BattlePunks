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

## Config

| Key | Default | |
|---|---|---|
| `[Splitscreen] Enabled` | true | a view per local player; off, one view |
| `[Splitscreen] SideBySide` | false | two players left and right |
| `[Input] KeyboardMousePlayer` | 0 | the local player the keyboard and mouse belong to; -1 nobody |
| `[Splitscreen] Views` | 0 | debugging: draw this many views of the default scene, whatever the game says |
| `[Splitscreen] DebugOffset` | "" | debugging: `x,y,z` moves view i's borrowed camera by i times that |

Try any world with `Splitscreen.Views 4` in the console. To see players 2-4
in a level, start it with `-Avatar.LocalPlayers=4`.
