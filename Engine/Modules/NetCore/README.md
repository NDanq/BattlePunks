# NetCore

Networking: an ENet transport, a server-authoritative session that replicates
networked entities and routes RPCs, and the dedicated server's host. (Editor
collaboration is `Modules/Collab`, which replaced the editor link.)

`NetCore` is written for this repository and is **optional**. A game that never
networks configures without it and nothing networked is compiled or linked.

**Adding networking to a game?** `GUIDE.md` is the step-by-step guide.

---

## Position in the tree

```
Host exe --> NimbleEngine --(PRIVATE)--> NetCore --> GameCore --> EngineCore
                                            \------(PRIVATE)--> ENet (ThirdParty/Source/enet-1.3.18) --> ws2_32
```

Game code never includes a NetCore header. It networks through:

| What | Where | Without NetCore |
|---|---|---|
| `NetComponent` -- marks an entity networked | `GameCore/Components/NetComponent.h` | loads, inert |
| `GAMECORE_DEFINE_COMPONENT_REPLICATION` -- which fields replicate | `GameCore/ComponentReplication.h` | compiles, registers nothing |
| `GAMECORE_DEFINE_COMPONENT_RPCS`, `GAMECORE_RPC` -- RPCs | `GameCore/ComponentRpc.h` | every call runs locally |
| `IsServer()`, `HasAuthority()`, `IsLocallyOwned()`... | `NimbleEngine/NetFacade.h` | Standalone answers |
| `IGameMode::OnClientConnected` / `OnClientDisconnected` | `NimbleEngine/GameMode.h` | never called |

`NimbleEngine` links NetCore PRIVATE, so turning NetCore off cannot break a
game's include or link line.

---

## Turning it off

One switch per configure, like `NIMBLE_TARGET`. It becomes `WITH_NETCORE=0/1`
on every target:

```ini
; The project's Config/DefaultEngine.ini -- or Project Settings > Project > Modules
[Modules]
NetCore=false
```

CMake reads it as the game configures (`Engine/cmake/NimbleProject.cmake`),
and the next build after an edit applies it. You can also set
`NIMBLE_WITH_NETCORE` before `add_subdirectory(<engine>)` or pass
`-DNIMBLE_WITH_NETCORE=OFF`; a `[Modules]` key in the file wins over both.
Then:

- No `NetCore`, `ENet` or `ws2_32`. `dumpbin /dependents` on the game shows no
  `WS2_32.dll`.
- `nimble_add_server_launcher` skips with a status message, so the game's
  CMakeLists needs no `if`.
- `NIMBLE_TARGET=Server` refuses to configure.
- `-connect` and the other `[Net]` command-line options warn and are ignored.
- Net components still load, edit and save (they are GameCore's). A level
  that has any warns once as it loads, in the game and in the editor, that
  they will not replicate (`NimbleEngine::WarnInactiveModuleComponents`).

---

## Running a session

```
LightLabServer -port=7777                  # Engine/Source/Launcher/ServerLauncherMain.cpp
LightLab -connect=127.0.0.1:7777           # any number of clients
LightLab -listen                           # or: a windowed game that hosts
```

**The server** is `nimble_add_server_launcher(<Game>Server MODULE <module>)`.
It is a console exe that runs the game's module and start level headless: no
window, no input, the Null renderer. It runs at `[Net] ServerTickRate` until
Ctrl+C.

**A client** is the ordinary launcher with `[Net] ServerAddress` set, which is
what `-connect` does. It loads its start level, then connects.

### In the editor (Play-in-Editor)

**Edit > Editor Settings > Play** (also **Play > Network Settings...**) sets
how Play runs:

| Setting | `[Editor]` | |
|---|---|---|
| Network mode | `PlayNetMode` | Standalone; Listen Server (the viewport hosts and plays); Client (the viewport joins) |
| Server address, Port | `PlayServerAddress`, `PlayPort` | a client's server; a listen server's port |
| Start a dedicated server | `PlayLaunchServer` | Client: run `<Game>Server` here first, and stop it with the run |
| Client windows at Play | `PlayClientWindows` | more players, as game windows, when Play starts |
| Client window arguments | `PlayClientArgs` | extra command line for each window |

**Play > New Client Window** (Shift+F6, console `play.client`) adds another
player at any time: the game's own launcher, joined to the run. The server's
and every window's log is in the editor's Console, tagged `[Server]` and
`[Client N]`. All of them stop with the run.

PIE drives the same `NimbleEngine::NetHost` (`NetHost.h`) as the launcher. It
starts before the level loads, so the level and mode already know their role.

**Build > Compile Source** builds the game module, plus the game's launcher
and dedicated server: what the windows and the server run. These are the
targets `nimble_add_game_launcher` / `nimble_add_server_launcher` recorded for
the editor. **Editor Settings > Build** turns that off.

`Games/WorldsLab`'s **Steampunk PvP** (`WorldsLabPvP.cpp`) is the full sample:
a deathmatch with predicted movement, server-side hits, Multicast and Client
RPCs for feedback, a replicated match state, and bots that play through the
same input path. Its README section runs a match unattended.

The launchers' command line is config overrides (`ParseLaunchArgs`,
`NimbleEngine.h`): `-Section.Key=Value` for anything, plus `-connect`,
`-port` and `-listen`. `-headless` runs the game launcher with no window,
input or renderer: a client a bot drives, for an unattended test or a soak.

| `[Net]` | Default | |
|---|---|---|
| `ServerAddress` | "" | a client's server; empty plays standalone |
| `Port` | 7777 | |
| `Listen` | false | a windowed game hosts |
| `MaxClients` | 16 | |
| `TimeoutMs` | 30000 | the most a peer may go unanswered (see Timeouts) |
| `ServerTickRate` | 30 | a headless server's frame rate, and so its send rate |
| `InterpolationDelayMs` | 100 | how far in the past a client shows remote Transforms and `Smooth()` fields; 0 shows them as they arrive |
| `SimulatedLossPercent` | 0 | testing: this share of the state packets the process sends is dropped |
| `SpawnPlayerPerClient` | true | the host makes each client a player entity |

---

## The model

**Server-authoritative.**

- The server gives every entity with a `NetComponent` (`bReplicate` on) a
  `NetId`. It sends each client that entity's serialized components when the
  entity appears. If the client's level already has that entity, it is matched
  by `EntityGuid`; otherwise the client creates it.
- After that, the server sends each replicated field whenever it changes,
  diffed against what it last sent. It also sends the entity's destruction.
- Its shape follows it too: components added or removed on the server, and
  a new parent, reach the client. Turning `bReplicate` off, or removing the
  `NetComponent`, destroys the entity on clients; turned back on, it spawns
  again under a new `NetId`.
- A client that joins late gets everything at once.
- Clients never send state. They send Server RPCs.

**Ownership.** `NetComponent::OwnerClient` is the client that controls an
entity. With `SpawnPlayerPerClient`, the server makes each joining client an
entity with a `PlayerStateComponent` and a `NetComponent` that client owns.
`IGameMode::OnPlayerAdded` runs for it, then `IGameMode::OnClientConnected`.
When the client leaves, everything it owned is destroyed after
`OnClientDisconnected`.

**Authority in game code.** Both ends run the same level and mode. Anything
that creates or changes networked state goes behind `NimbleEngine::HasAuthority()`.

### Replicated fields

```cpp
GAMECORE_DEFINE_COMPONENT_REPLICATION(TurretComponent)   // in one .cpp
{
    Rep.Field("Heading", &TurretComponent::Heading);
    Rep.Flag("Firing",   &TurretComponent::bFiring);        // uint8_t 0/1
    Rep.Entity("Target", &TurretComponent::Target);         // sent as the target's NetId
    Rep.Field("Model",   &TurretComponent::Model, GameCore::RFF_InitialOnly);
}
```

- Fields are encoded by type, never as raw bytes: `Vector3`'s layout depends on
  `WITH_SIMD`.
- **Quantized fields travel smaller.** A `float` or `Vector3` can say how
  precise it needs to be:

  ```cpp
  Rep.Field("Position", &TransformComponent::Position).Precision(0.01f);   // whole 0.01s: a variable-length integer
  Rep.Field("Rotation", &TransformComponent::Rotation).Angles();           // degrees, 2 bytes each, read back in [-180, 180)
  ```

  `TransformComponent` does both, plus `Scale` to 0.001. A client reads the
  rounded value; the server keeps its own. An avatar's quantized fields are
  snapped after every simulation step on both ends
  (`QuantizeEntityReplicatedFields`), so prediction stays exact. The
  encoding is part of the schema hash.
- **`Smooth()` fields are interpolated** on clients, as the Transform is
  (see Smoothing and prediction). `float` and `Vector3` only; with
  `Angles()` they blend the short way round:

  ```cpp
  Rep.Field("Pitch", &MoverComponent::Pitch).Precision(0.01f).Smooth();
  Rep.Field("Yaw",   &MoverComponent::Yaw).Angles().Smooth();
  ```
- A type allows at most 64 fields (`REPLICATED_FIELDS_PER_TYPE_MAX`), one bit
  each in the change mask. A 65th is refused with an error at startup. The
  mask is a variable-length integer, so a type with few fields pays no more
  for the room.
- `TransformComponent` replicates Position, Rotation and Scale out of the box.

### RPCs

```cpp
struct ShipComponent
{
    GAMECORE_COMPONENT_BODY()
    void ServerFire(const EngineCore::Vector3& InAim);
    void ClientHit(const GameCore::RpcContext& InContext, int32_t InDamage);   // context: optional, first
};

GAMECORE_DEFINE_COMPONENT_RPCS(ShipComponent)
{
    Rpc.Server<&ShipComponent::ServerFire>("ServerFire");
    Rpc.Client<&ShipComponent::ClientHit>("ClientHit");
}

GAMECORE_RPC(ShipComponent, ServerFire)(Ship, Aim);
```

|             | called on the server             | called on a client        |
|-------------|----------------------------------|---------------------------|
| `Server`    | runs                             | sent to the server        |
| `Client`    | sent to the owning client (runs if the server owns it) | runs |
| `Multicast` | runs, and sent to every client   | runs                      |

- The server accepts a Server RPC only from the entity's owner, and only so
  many a second (RPC limits, below).
- Argument types: `float`, `double`, `int32_t`, `uint32_t`, `uint8_t`, `bool`,
  `Vector3`, `Color`, `EntityID` / `Entity` (as a `NetId`) and
  `RpcString<N>`, each by value or `const&`.
- A call made with the wrong argument types is refused, not misread.
- An RPC on an entity created this frame stays local until the entity has been
  sent, which happens at the end of the frame.
- **Reliable by default.** A call is resent until it arrives, in order with
  everything else on the game channel. One that is stale a moment later,
  such as a muzzle flash or a hit marker, can be sent once instead:

  ```cpp
  Rpc.Multicast<&ShipComponent::MulticastFlash>("MulticastFlash").Unreliable();
  ```

  It goes on its own unsequenced channel, so a lost packet holds nothing up
  and nothing is resent. It may be lost, or arrive before or after other
  calls; one that reaches a client before its entity's spawn is dropped. It
  runs locally exactly as a reliable one does. Rate limits apply the same.
  `[Net] SimulatedLossPercent` drops it as it drops state. Steampunk PvP
  sends shots and hit markers this way, and kills reliably.

### RPC limits

A server holds each client to a budget, so a broken or hostile client cannot
make it run handlers without end:

| `[Net]` | Default | |
|---|---|---|
| `RpcPerSecond` | 1000 | Server RPCs a second from one client, all together. It may burst to two seconds' worth; past that its calls are dropped |
| `RpcFloodPerSecond` | 1000 | more than this many dropped in one second, and the client is disconnected |
| `MaxRpcBytes` | 1024 | an RPC whose arguments are longer is malformed, and its sender is disconnected |

0 turns any of them off. Every call costs a token, even one the server then
refuses: an RPC on an entity the client does not own, or one that names no
entity. Drops are logged once a second per client, not once per call, and so
are refusals.

The defaults are generous because an avatar sends its input once a frame. A
client running uncapped at 1000 fps would fill the budget alone, so give
such a game a `[Display] FrameRateLimit`.

**One RPC can be held to less.** This keeps a client from doing something
faster than the game allows, such as firing faster than the gun:

```cpp
GAMECORE_DEFINE_COMPONENT_RPCS(ShipComponent)
{
    Rpc.Server<&ShipComponent::ServerFire>("ServerFire").RateLimit(8);   // at most 8 a second, from one client
}
```

It allows a burst of up to one second's worth. The limit is the server's
own, so it is not part of the handshake's schema.

The limits stop floods. They do not check what a call asks for: that an aim
is a unit vector, that a shot starts near the shooter. Each handler checks
its own arguments, as `PvpFighterComponent::ServerFire` does.

### Smoothing and prediction

**Smoothing.**
- Each state batch carries the server's clock, so a client knows when
  its state was true.
- A client keeps about a second of each networked Transform it receives.
- It shows the Transform `InterpolationDelayMs` in the past, blended between
  the samples on either side. If the server goes quiet, it holds the newest
  sample rather than guessing ahead.
- Fields declared `Smooth()` get the same treatment, per field: about a
  second of samples, shown the same delay behind, held at the newest. A
  field that sat unchanged through a batch is taken as true at that batch
  too, so a step after a pause blends over one interval, not the whole
  pause.
- Everything else (flags, integers, entity references, fields without
  `Smooth()`) is written as it arrives.

**Prediction.**
- An entity with `NetComponent::bPredicted`, on the client that owns it, is
  never interpolated. The server's Transform and smooth fields are written
  as they arrive.
- **Replays start from the server's whole state.** A batch carries only the
  fields that changed. So the client keeps every replicated component as the
  server last stated it, and before a replay it copies all of them, every
  component, onto the predicted entity. Fields the batch left out would
  otherwise still hold the client's own later prediction, and the replay
  would apply that input twice.
- After each poll, the handler set by
  `NimbleEngine::SetNetCorrectionHandler` runs once for each such entity
  whose state arrived (any of its components). It gets the replicated `NetComponent::InputAck`, the
  last input the server applied, so the game can replay the inputs after it.
- NetFacade.h and GUIDE.md section 8 describe the game's side.

---

## Wire

- **Transport.** ENet, with four channels:
  - **game** (reliable, ordered): the handshake, spawns, destroys and RPCs;
  - **state** (unreliable, sequenced): replicated fields and their acks;
  - **unreliable** (unreliable, unsequenced): RPCs declared `Unreliable`;
  - channel 1 is unused: it was the editor link's, kept so the state channel
    keeps its number.
- **Packets.** Every packet is an `EngineCore::Archive` (little-endian). A
  packet that does not parse disconnects its sender.
  `Private/NetCore/NetProtocol.h` has the layouts.
- **Handshake.** It carries the protocol version and a hash of every type's
  replicated fields and RPCs (`NetSchema`), so two builds that disagree refuse
  each other. Types travel by name, never `ComponentTypeID`, because ids are
  assigned in first-use order.
- **A spawn carries the entity's parts:** its descendants with no Net
  component of their own (an avatar's camera boom, its weapon), parents
  first, as they are at that moment. They are not networked after: each
  process moves its copy itself. A descendant with a Net component is sent
  as its own entity.

### Unreliable state, acknowledged baselines

A lost reliable packet holds up everything sent after it until it is
resent: a whole round trip of frozen state. So state does not go reliably.

- **Every server tick, each client gets a `StateBatch`,** even an empty one,
  which also keeps its clock. A batch stays under 1200 bytes; a big tick
  splits into several, between entities, never within one.
- **Entries are compact.** Each starts with its NetId, wire type and field
  mask as variable-length integers: usually 3 or 4 bytes, not 10. With
  quantization, a moving entity's Position update fell from 22 bytes to 9
  (the `state:` line of `NetCore.Loopback`).
- **The client acknowledges** the batches it applied (`StateAck`: the newest,
  and the 32 before it as bits), unreliably too; each ack repeats the last
  32, so losing one costs little.
- **The server keeps, per client and component, what the client has
  acknowledged.** A field is in every batch from the tick it changes until a
  batch carrying that value is acknowledged. So nothing waits to be resent
  after a loss: the next batch already has it. And every batch is complete
  against the client's acknowledged state, so a client never shows a
  position beside an older `InputAck`, or misses a value that changed and
  changed back while a packet was lost.
- **A batch older than the newest received is never applied,** so old values
  cannot land on new ones.
- **State may outrun a spawn,** since they travel on different channels. A
  client does not acknowledge a batch that mentions an entity it has not
  been sent yet, so the server keeps sending that state until the spawn has
  landed. The same goes for a component added after the spawn: state goes
  only for components the client has been told of, and a batch naming one
  whose `AddComponents` has not landed is not acknowledged.

- **What changed is found once a tick, not once per client.** Each
  replicated component is compared with its own copy from the last flush,
  with one `memcmp` shared by every client. A client in step then looks only
  at this tick's changes and its own unacknowledged components. Everything
  else it already has, so it costs that client nothing. Only a changed
  component is diffed field by field against the client's baseline. A
  client's first tick (new, back, or after a module reload) looks at
  everything once.

  With 2000 entities of three replicated components and 12 clients, a
  RelWithDebInfo server's flush went from about 2 ms a tick to 0.1 ms with
  everything still, and to 0.3 ms with 100 moving (the `[flush cost]` line
  of `NetCore.Loopback` prints it). What is left grows with the entity
  count, not the clients: the shared comparison.

`[Net] SimulatedLossPercent` drops that share of the state packets a process
sends, for testing. In NetArena at 20% on every process, prediction's worst
correction stays 0: each batch that arrives is enough on its own.

### Bandwidth budget

A server sends each client at most `[Net] ClientBandwidthKBps` of state
(default 256, which is 2 Mbit/s). Set it in **Project Settings > Net >
Bandwidth per client**; 0 means no limit. It meters the state batches
only. Spawns, destroys, RPCs and the editor link are reliable and are not
counted.

- **A token budget.** Each client earns bytes at the rate, up to a quarter
  second's worth (never under one packet). An entity's entry goes out
  whole or not at all.
- **What goes first.** The client's own entities always go, and are never
  held back. Then the rest, by score: the entity's `Net` component
  **Priority** (1 normally; 4 is four times as eager), times the ticks it
  has waited plus one, times how near it is to the client's own avatar
  (1 at its feet, a half at 20 m).
- **Nothing is lost.** Whatever does not fit is looked at again the next
  tick, diffed in full against what the client has. Waiting raises its
  score, so a low priority is late, never starved. A joining client's first
  full pass spreads over as many ticks as it needs.
- `GetNetStats` (`StateDeferred`) reports how many entities a client's
  budget held back last tick.
- **ENet's own packet throttle is off.** It sheds unreliable packets at
  random, down to 1 in 32, whenever a round trip jumps: a big spawn's
  reliable burst was enough to lose most state for seconds. The budget
  chooses what waits instead.

Under a 4 KB/s budget with 300 entities all changing, `NetCore.Loopback`
sees the client's own entity get every change, a Priority 8 entity about
six times as many updates as a Priority 1 one at the same distance, and
everything arrive once things settle.

### Reconnecting

A client whose connection is lost can rejoin as itself. It keeps the same
client id, the same player and avatar, and its score.

- **The server keeps its place.** When a welcomed client times out, the
  server holds its place for `[Net] ReconnectGraceMs` (default 30000).
  Everything it owns stays, and its avatar stands idle with no input. A
  client that quits on purpose (`Stop`, closing the game) or is kicked is
  gone at once: ENet's disconnect reason tells the cases apart.
- **The client tries to rejoin** for as long, once a second. Its Hello
  carries the token the server gave it at its welcome. A token the server
  knows gets the same client id back. That works from a kept place, or over
  a connection the server has not yet noticed is dead, which it then drops.
- **Back in time:** the server sends its snapshot again, then
  `SnapshotDone`. The client keeps its entities and updates them in place,
  so its own avatar never blinks out. Anything the snapshot did not mention
  was destroyed on the server while it was away, so the client removes it.
  State starts again from a full batch.
- **Too late:** the server has already given its place up
  (`OnClientDisconnected`, which destroys what it owned). The token is
  unknown, so the client joins as a new one. What its old session spawned
  is cleared first, and the level's own entities are matched again.

| | Server | Client |
|---|---|---|
| `IGameMode::OnClientAway(Client, Player)` | lost, place kept | |
| `IGameMode::OnClientReturned(Client, Player)` | back in time | |
| `IGameMode::OnClientDisconnected` | quit, kicked, or not back in time | |
| Lua `ClientAway`, `ClientReturned` events | the same | |
| `NimbleEngine::IsNetReconnecting()`, Lua `Net.IsReconnecting()` | | trying to rejoin |

`NetSession::GetReconnectToken()` is the client's token, and
`NetConfig::ReconnectToken` starts a session with one. A game that saves
the token can rejoin after the client itself restarts. A dropped
connection is noticed only after `TimeoutMs`, and lost clients count
against `MaxClients` until their grace ends.

### Lag compensation

A client shows other players in the past: its interpolation delay, plus the
time their state took to arrive. Its shot then takes as long again to reach
the server. Judged against where targets are now, a shot at a runner lands
behind it.

- **Every Server RPC from a client carries a view time.** That is the server
  time of what the client was showing as it called: its interpolation render
  time, on its estimate of the server's clock. The server clamps it to the
  last second and hands it to the handler as `RpcContext::ViewTimeMs`. A
  local call (the host's own player, a bot) has none.
- **The server keeps positions.** It records a second of positions for every
  entity with a **LagCompensated** component (a standing capsule: `Radius`,
  `Bottom`, `Top`). `NetHost` records them each server frame, on the server's
  clock (`NetSession::GetTimeMs`, `NimbleEngine::GetNetTimeMs`).
- **`NimbleEngine::LagCompensatedRaycast(World, Context, ...)`**
  (`NimbleEngine/LagCompensation.h`) tests the world as it is, plus each
  compensated capsule where the caller saw it, skipping the caller. It
  reports the hit, whether it was a capsule, and how high above the
  target's feet (a head shot). `GetLagCompensatedPosition` gives the
  rewound position for a test of the game's own.

Keep compensated entities out of the world mask, by giving any body they
have a layer of its own. Otherwise their current place is hit first.

There is no allowance past one second, and the rewind moves only the
capsules, not physics bodies. Steampunk PvP uses it for every shot, and
`[WorldsLab] PvpLagCompensation=0` judges shots against now to compare.

### Connection stats

`NimbleEngine::GetNetStats(Client, Stats)` (`NetFacade.h`) reports a
connection as this end sees it. On a client that is its link to the server;
on a server, its link to `Client` (an RPC's `SenderClient`, say). It gives:

- the round trip and its variance (ENet's smoothed estimate)
- packet loss (ENet's, from reliable resends)
- bytes and packets each way since connecting
- each direction's rate over the last whole second

`NetSession::GetServerStats` / `GetClientStats` are the same, inside NetCore.
Steampunk PvP shows a client's ping with it.

### Timeouts

`TimeoutMs` is both ENet's minimum and its maximum: how long either end
waits on an unanswered peer before dropping it.

ENet drops a peer once a command has been resent enough times *and* the
minimum has passed. On a fast link the resends run out in milliseconds, so
with ENet's default 5 s minimum, the end still running dropped a peer stalled
for just over 5 s, whatever `TimeoutMs` said. A client's first frame, while it
compiles pipelines, can take that long. The stalled end, waking, has resent
nothing, so only the maximum applies to it; ENet checks timeouts *before* it
reads the socket, so a stall longer than `TimeoutMs` drops it even with the
answer waiting. `NetCore.Loopback` stalls a client for 6 s to keep this
honest.

---

## Editor link (retired)

The `net.host` / `net.join` editor link and `NetEditorLink` are gone. Editors
work together through `NimbleEditorServer` and `Modules/Collab` instead
(`Engine/Modules/Editor/COLLABORATION.md`; P6 there finishes what the link
started: snapshots, edits as data, applying them undoably, ownership).

---

## What is not here yet

- **Relevancy and interest management.** Every client gets every networked
  entity.
- **Level travel during a session.** Both ends must run the same start level.
- **RPCs and replicated fields declared in Lua.** Lua can call any C++ RPC and
  query roles (the `Net` library, GUIDE.md), but cannot declare its own yet.

---

## Tests

`NetCore.Loopback` (`Tests/LoopbackTests.cpp`, label `smoke`) runs a server and
a client in one process over 127.0.0.1. It covers:

- the handshake
- connection stats: traffic, round trip and rates, from both ends
- spawning, both matched by GUID and new, and a spawn's parts
- components added and removed after a spawn, reparenting, and replication
  turned off and on, clean and under 50% loss
- replicated and initial-only fields, and entity references
- all three RPC targets, and the ownership check
- RPC limits: a RateLimit and its refill, the overall budget, a flood
  disconnect, and MaxRpcBytes
- a 6 s client stall
- interpolation, and the prediction handler
- a 64-field type, clean and under 50% loss: fields past the 32nd arriving
  alone and with low ones, the top one smoothed, a 65th refused
- smooth fields: in-between values while a field steps, an unsmoothed field
  never in between, settling on the last value without overshooting
- state under 50% simulated loss each way: changing values, a value changed
  and changed back, and a change just after a spawn
- a late joiner
- the flush's cost with 2000 entities and 4 clients, printed (and the
  movers' state checked to arrive)
- unreliable RPCs: arriving on a clean link, lost under total loss while a
  reliable one sent with them still arrives
- the bandwidth budget: entities held back, the client's own entity
  keeping up, Priority, and everything arriving once things settle
- quantization: the rounding, and a Transform arriving bit for bit as
  `QuantizeReplicatedFields` predicts
- view times: a client's Server RPC stamped about its interpolation delay
  behind the server's clock, a local call with none
- reconnecting: a lost client kept away and taken back with the same id and
  entities (updated, with what was destroyed meanwhile removed); one back too
  late joining as new; one that quits gone at once
- destroys and disconnects

`NimbleEngine.GameFrameworkTests` covers what every build has: local RPCs,
the facade, `ParseLaunchArgs` and a headless Application.
