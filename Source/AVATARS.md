# Avatars

*New to avatars? Start with `AVATARS_GUIDE.md`, the step-by-step walkthrough.*

An **avatar** is an entity a player drives: a pawn, a character, a vehicle,
a turret. Any entity or prefab with an **Avatar Harness** can be one. It reads
its player's input in the same way standalone, in split-screen, on a listen or
dedicated server, and on a client. Public API: `Public/NimbleEngine/Avatar.h`
and `Public/NimbleEngine/PlayerStart.h`.

```
player (PlayerState, Net, Possessor) ──possesses──► avatar (AvatarHarness, Net, Transform, ...)
        LocalPlayer: whose pad                           InputMap: how its input is read
                                                         Down / Pressed / Released / Axes
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| `AvatarHarnessComponent` | `"AvatarHarness"` | Makes an entity possessable. Records who drives it (`Possessor`) and its input this frame. Read it the way you read an Input component: `IsDown`, `WasPressed`, `WasReleased`, `GetAxis`. |
| `PossessorComponent` | `"Possessor"` | On the player: the avatar it drives, and which local player's pad it reads (`LocalPlayer`). |
| `Possess` / `Unpossess` | `Avatar.h` | Possession, which is the server's to change. |
| `PlayerStartComponent` | `"PlayerStart"` | A place an avatar spawns, optionally kept for one player and naming the prefab. |
| `NetGameMode` | `PlayerStart.h` | The default rules: every local player and every joining client gets an avatar at a start. |
| `AvatarWorld` | `Avatar.h` | Each level's avatars. Runs as the `Engine.Avatar` system. `GameSession::GetAvatars()`. |

## The player and the avatar

The player is the entity with the PlayerState: its score, its lives and its
client. The avatar is the body it drives at the moment. Keeping them apart
means a player can do these things without its score or ownership moving:

- get into a vehicle and out again: `Possess(Player, Car)`, then `Possess(Player, Pawn)`
- respawn: the old body is destroyed, then `SpawnAvatarFor(Player)`
- spectate: `Unpossess(Player)`, then `GameSession::SetViewTarget(Camera)`

Where players come from:

- **Clients.** The server makes a player for each client that joins
  (`[Net] SpawnPlayerPerClient`): PlayerState, Net owned by the client, and
  Possessor.
- **Local users.** `CreateLocalPlayer(World, N)` gives the same three
  components, with Net owned by nobody (the server's). `NetGameMode` and
  `AutoPossess` call it for you.

## Possession

`Possess(World, Player, Avatar)` works only where there is authority (the
server or a standalone game). It does the following:

1. Refuses entities that are not alive, which includes a stale id. Refuses an
   avatar with no harness, or one another player drives (unless
   `POSSESS_Steal` is passed).
2. Lets go of the player's previous avatar.
3. Gives the avatar's Net component to the player's client (`OwnerClient`).
   `IsLocallyOwned` is then true on that client, and the server accepts that
   client's Server RPCs on the avatar. What it owned before is kept as
   `RestOwner`.
4. Links both ends (`Possessor.Avatar` and `AvatarHarness.Possessor`, both
   replicated).
5. Notifies listeners: the World event `Possessed(Player, Avatar)`, the
   avatar's prefab script `OnPossessed(self, player)`, and the mode's
   `IGameMode::OnAvatarPossessed`.

`Unpossess` reverses these steps: the owner goes back to `RestOwner`, then
`Unpossessed`, `OnUnpossessed` and `OnAvatarUnpossessed` are emitted. Both
also happen automatically when either end is destroyed or loses its
component.

An avatar whose prefab script starts after the possession (spawned and
possessed as the level starts) gets `OnPossessed` in the frame its script
starts, after `OnStart`.

A client hears `Possessed` and `Unpossessed` too, as each replicated change
arrives. A script or HUD therefore reacts the same way everywhere.

**Who owns an avatar nobody drives** is its `RestOwner`, and that decides what
happens when a client leaves:

- An avatar spawned for a client (`SpawnAvatarFor`) is owned by that client
  from the start. It is destroyed with the client.
- A level's vehicle is the server's. A client driving it when it leaves gives
  it back (`ReleaseClientAvatars`, before the client's entities go), so it
  stays in the level.

**A client asks** to drive something rather than taking it. It calls
`possessor:Possess(car)` (or `Game.Possess`), which sends `ServerRequestPossess`.
The server accepts only an avatar marked **Requestable** that nobody drives,
and then asks the mode (`IGameMode::CanPossess`: close enough, same team).

## Input

Every frame, in `TP_PrePhysics` just after Input components and before any
script, each harness is filled according to where it is running:

| Where | Its input |
|---|---|
| The process that drives it: a client for its own avatar, a server or standalone game for a local player's | Its player's devices through `InputMap`: the keyboard and mouse, and pad `LocalPlayer`. Then the optional input filter (`AvatarWorld::SetInputFilter`: a bot, aim assist, a menu blocking the game). |
| A server, for a client's avatar | What that client sent since the last tick. |
| Anywhere else, or undriven | Nothing held. |

**The input stream.** On a client, the harness also sends each frame to the
server, numbered, in its built-in Server RPC `ServerInput`. The server merges
everything that arrived since its last tick:

- presses and releases add up, so a tap between two ticks of a 30 Hz server
  is still a press
- held keys and stick positions take the latest value
- mouse motion (an axis bound to a mouse axis) sums

Frames that are old or repeated are dropped. The newest one sent is
acknowledged in `NetComponent::InputAck`. A client that goes quiet for
`[Avatar] InputTimeoutMs` (500) reads as nothing held. A run of idle frames is
sent once, unless the harness has a simulation.

So avatar code is written once:

```cpp
void JumpSystem(GameCore::World& InWorld, float, void*)
{
    InWorld.Each<NimbleEngine::AvatarHarnessComponent>([](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
    {
        if(NimbleEngine::HasAuthority() && InHarness.WasPressed("Jump")) { Jump(InAvatar); }
    });
}
```

```lua
return {
    TickPhase = TickPhase.PrePhysics,
    OnStart = function(self) self.Harness = self.Entity:GetComponent("AvatarHarness") end,
    OnTick = function(self, dt)
        if self.Harness:WasPressed("Jump") then print("jump") end
    end,
    OnPossessed = function(self, player) print("driven by " .. player:GetName()) end,
}
```

**Local players.** Pad N belongs to local player N. As with an Input
component's `PlayerIndex`, the keyboard and mouse are read by every local
player, so split-screen games bind their second player to its pad.

## Prediction

Give a harness a **Simulation**: a function that moves the avatar by one frame
of input.

```cpp
NIMBLE_DEFINE_AVATAR_SIMULATION(CapsuleMove, 0.1f)   // name, longest frame a client may claim
{
    static const int MoveX = NimbleEngine::FindInputAxis("Default", "MoveX");
    GameCore::TransformComponent* T = InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
    T->Position.SetX(T->Position.GetX() + 450.0 * InFrame.Axes[MoveX] * InFrame.Seconds);
}
```

Set **Simulation** on the harness to `CapsuleMove`. The engine then runs it:

- on the server, once for every frame its client sends, with the seconds
  clamped
- on a server or standalone game for a local player's avatar, each frame
- on the owning client at once, when the avatar's Net component is
  **Predicted**. The client keeps each frame until the server acknowledges
  it. When the server's state arrives it replays the frames not yet
  acknowledged on top of that state.

Same code from the same place means the replay lands where the avatar already
was. `AvatarWorld::TakePredictionStats` reports how far a correction moved it
(NetArena logs `worst correction 0u`). No `SetNetCorrectionHandler` is needed.
The handler still receives corrections for anything that is not a simulated
avatar.

Predict only what the client can know. Walls it also has are fine. Being hit
by another player is the server's to say.

## Spawning

- **Player Start** (`"PlayerStart"`): an entity with a Transform. It can be
  kept for one player (`PlayerSlot` 1..4, by join order) and can name the
  `AvatarPrefab` to spawn there.
- **`SpawnAvatarFor(World, Player, Prefab, Start)`** works only with
  authority. The prefab is the one passed, else the start's, else
  `[Avatar] DefaultPrefab`. It spawns at the start, gives the avatar's Net
  component to the player's client before the avatar is first sent, then
  possesses it.
- **`NetGameMode`.** Derive from it.
  - Its `OnStart` gives each of `[Avatar] LocalPlayers` (1) local players an
    avatar, unless one was placed for that player.
  - Its `OnClientConnected` gives each joining client one.
  - `GetAvatarPrefab(Player)` picks the prefab. `OnAvatarSpawned(Player,
    Avatar, Ordinal)` dresses it (NetArena colours its capsules there).
- **AutoPossess** on a placed harness (Player 1..4). Local player N takes it as
  the level starts, before the mode's `OnStart`, with any mode and in the
  editor's Play. Not on a client or a dedicated server.

## The view

The possessed avatar of local player 1 is viewed through when its harness has
**View Target** set. The camera used is the avatar's own Camera component, or
the first one among its children. The camera is used whether or not it is
Active, so give avatar cameras Active off: other players' avatars then never
win the level's first-active rule.

`GameSession::SetViewTarget(Entity)` (Lua `Game.SetViewTarget`) overrides this
for a cutscene or a death cam. An unset target goes back to the avatar's
camera. Without either, the level's first active camera is used, as before.
The listener follows the same choice when the level has no Audio Listener.

There is one view, so split-screen shows local player 1's.

## From scripts

| Lua / graph | |
|---|---|
| `harness:IsDown("Jump")`, `:WasPressed`, `:WasReleased`, `:GetAxis("MoveX")` | its player's input |
| `harness:IsPossessed()`, `:IsLocallyControlled()`, `:GetPossessor()` | |
| `possessor:GetAvatar()`, `:Possess(avatar)`, `:Unpossess()` | on a client, a request |
| `Game.Possess(player, avatar)`, `Game.Unpossess(player)` | the same, by player |
| `Game.GetAvatar(player)`, `Game.GetPossessor(avatar)` | |
| `Game.GetLocalPlayer(n)`, `Game.GetLocalAvatar(n)` | this machine's local player n (0 first) |
| `Game.SetViewTarget(camera)` | nil goes back to the avatar's |
| `Events.On(Game.Possessed, function(player, avatar) ... end)`, `Game.Unpossessed` | every process |
| `OnPossessed = function(self, player)`, `OnUnpossessed` | in the avatar's prefab script |

## Config

| Key | Default | |
|---|---|---|
| `[Avatar] InputTimeoutMs` | 500 | a remote client's avatar reads nothing after this long without input |
| `[Avatar] DefaultPrefab` | "" | what `SpawnAvatarFor` spawns when nothing else names a prefab |
| `[Avatar] LocalPlayers` | 1 | local players `NetGameMode` gives avatars |

## Not here yet

- One renderer view, so no split-screen rendering.
- One player per networked client: NetHost makes one per connection.
- A prefab **spawned** on the server reaches clients as its components, not
  as a prefab instance. Its descendants without a Net component (a camera
  boom, a weapon) come with its spawn, as they were then; the owning client
  moves them itself after (a simulation's step does). Its script runs on the
  server only. An avatar's
  `OnPossessed` and its Lua or graph therefore run on clients only for avatars
  placed in the level (NetArena's karts). For spawned pawns, react on clients
  from a placed or startup script (`Events.On(Game.Possessed, ...)`), or in
  C++ (`IGameMode::OnAvatarPossessed` runs on every process).
- The input stream is one reliable RPC per frame. Batching frames into an
  unreliable send that carries recent history would cost less bandwidth.

## Example

`Games/NetArena`: `Content/Prefabs/Pawn.nmprefab` is a harness with the
`NetArenaMove` simulation and a predicted Net component.
`Content/Levels/NetArena.nmworld` has four Player Starts. `NetArenaGameMode`
is an `NetGameMode` that colours the capsules and reports.

## Tests

`Engine/Tests/AvatarTests.cpp` (`NimbleEngine.AvatarTests`, label `smoke`)
covers:

- possession and its rules
- giving a vehicle back when a client leaves
- local input and filters
- the server's half of the input stream
- simulations
- AutoPossess, the view target and NetGameMode through a GameSession

The client half runs in NetArena's unattended runs (`NetCore/GUIDE.md` §9).
`Engine/Tests/AvatarGuideTests.cpp` builds and runs every example in
`AVATARS_GUIDE.md`, the step-by-step guide: C++, Lua and a script graph.
