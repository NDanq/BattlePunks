# NetArena

Networking's test game: capsules on a flat plane, one per player. Each player
steers their own with WASD, the arrow keys or the left stick. The first
player's capsule is red, the second's blue, then green and yellow.

`NetArenaGame.cpp` explains how it works: the server owns and moves the
capsules, each player sends input as a Server RPC, and Transform replication
shows the result.

## Playing it

Build the editor tree first:

```
cmake --preset editor-debug
cmake --build --preset editor-debug
```

**In the editor (Play-in-Editor):**

1. Open **Edit > Editor Settings > Play** (or **Play > Network Settings...**).
2. Set **Network mode** to **Listen Server**. The viewport hosts, and is the
   red player.
3. Press **Play** (F5).
4. Press **Play > New Client Window** (Shift+F6). The game opens in a window
   of its own, joins as the blue player, and shows both capsules.

Or set **Network mode** to **Client** with **Start a dedicated server** on. The
editor starts `NetArenaServer` first, the viewport joins it as red, and each
client window joins as blue and the colours after it. Set **Client windows at
Play** to open them automatically.

**Build > Compile Source** rebuilds the game module, its launcher
(`NetArena`) and its server (`NetArenaServer`): what the client windows and
the dedicated server run.

**From a command line**, in `Binaries/<Platform>_Debug`:

```
NetArenaServer -port=7777
NetArena -connect=127.0.0.1:7777
NetArena -connect=127.0.0.1:7777
```

or `NetArena -listen` and one `NetArena -connect=127.0.0.1:7777`.

## Testing without a keyboard

`-NetArena.AutoMove=1` (or `NetArena.AutoMove 1` in the console) steers this
process's capsule round a square by itself, a new side each second. Every 5
seconds each process logs where each capsule is, as it sees it:

```
[NetArena] red=(-120,40) blue(you)=(80,-300)
```

To have the editor's client windows run the bot too, put
`-NetArena.AutoMove=1` in **Editor Settings > Play > Client window
arguments**.
