# NetCore

Networking: an ENet transport, a server-authoritative session that replicates
networked entities and routes RPCs, the dedicated server's host, and a stub
of the editor link.

`NetCore` is written for this repository and is **optional**. A game that never
networks configures without it and nothing networked is compiled or linked.

---

## Position in the tree

```
Host exe --> NimbleEngine --(PRIVATE)--> NetCore --> GameCore --> EngineCore
                                            \------(PRIVATE)--> ENet (External/enet-1.3.18) --> ws2_32
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

```cmake
# In a game's CMakeLists.txt, before add_subdirectory(<engine>):
set(NIMBLE_WITH_NETCORE OFF CACHE BOOL "" FORCE)
```

or `-DNIMBLE_WITH_NETCORE=OFF` on the configure line. Then:

- No `NetCore`, `ENet` or `ws2_32`. `dumpbin /dependents` on the game shows no
  `WS2_32.dll`.
- `nimble_add_server_launcher` skips with a status message, so the game's
  CMakeLists needs no `if`.
- `NIMBLE_TARGET=Server` refuses to configure.
- `-connect` and the other `[Net]` command-line options warn and are ignored.

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

The launchers' command line is config overrides (`ParseLaunchArgs`,
`NimbleEngine.h`): `-Section.Key=Value` for anything, plus `-connect`,
`-port` and `-listen`.

| `[Net]` | Default | |
|---|---|---|
| `ServerAddress` | "" | a client's server; empty plays standalone |
| `Port` | 7777 | |
| `Listen` | false | a windowed game hosts |
| `MaxClients` | 16 | |
| `TimeoutMs` | 30000 | the most a peer may go unanswered (see Timeouts) |
| `ServerTickRate` | 30 | a headless server's frame rate, and so its send rate |
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
- A type allows at most 32 fields, one bit each in the change mask.
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

- The server accepts a Server RPC only from the entity's owner.
- Argument types: `float`, `double`, `int32_t`, `uint32_t`, `uint8_t`, `bool`,
  `Vector3`, `Color`, `EntityID` / `Entity` (as a `NetId`) and
  `RpcString<N>`, each by value or `const&`.
- A call made with the wrong argument types is refused, not misread.
- An RPC on an entity created this frame stays local until the entity has been
  sent, which happens at the end of the frame.

---

## Wire

- **Transport.** ENet, with two reliable, ordered channels: game and editor.
  Spawns, state, destroys and RPCs share one channel, so state never arrives
  for an entity the client does not have yet.
- **Packets.** Every packet is an `EngineCore::Archive` (little-endian). A
  packet that does not parse disconnects its sender.
  `Private/NetCore/NetProtocol.h` has the layouts.
- **Handshake.** It carries the protocol version and a hash of every type's
  replicated fields and RPCs (`NetSchema`), so two builds that disagree refuse
  each other. Types travel by name, never `ComponentTypeID`, because ids are
  assigned in first-use order.

### Timeouts

`TimeoutMs` is ENet's maximum. The minimum stays at ENet's 5 s, with its
resend limit.

ENet checks timeouts *before* it reads the socket. With minimum equal to
maximum, a frame longer than that loses the connection, even though the
answer is already waiting. A client's first frame, while it compiles
pipelines, can take that long. `NetCore.Loopback` stalls a client for 6 s to
keep this honest.

---

## Editor link (stub)

Run these in the editor console:

- `net.host [port]`, then `net.join host[:port]` from another editor (default
  port 7787)
- `net.leave`
- `net.status`

Every undo step one editor records is announced to the others by its text,
relayed by the host, and printed. **Nothing is applied.** Look for
`STUB(NetCore-Editor)`.

The road from here:

1. **A snapshot of the host's world** for an editor that joins, through
   `SerializeWorld`.
2. **Edits as data.** An edit is entity GUID, component type *name*, field
   *name*, and the value encoded by field type: `EditorPropertyEdit` in
   `NetProtocol.h`, reserved. It must never be the undo history's raw
   component bytes, whose layout is the build's.
3. **Remote edits applied through `EditorDocument`,** so they are undoable and
   every panel hears of them.
4. **Ownership while editing,** so two people do not drag the same entity.

---

## What is not here yet

- **Unreliable state with acknowledged baselines.** v1 sends state reliably,
  which is simple and correct but queues behind a lost packet.
- **Relevancy and interest management.** Every client gets every networked
  entity.
- **Interpolation and client-side prediction.** A remote entity moves at the
  server's send rate.
- **Level travel during a session.** Both ends must run the same start level.
- **RPCs and replicated fields from Lua.**

---

## Tests

`NetCore.Loopback` (`Tests/LoopbackTests.cpp`, label `smoke`) runs a server and
a client in one process over 127.0.0.1. It covers:

- the handshake
- spawning, both matched by GUID and new
- replicated and initial-only fields, and entity references
- all three RPC targets, and the ownership check
- a 6 s client stall
- a late joiner
- destroys and disconnects
- the editor link

`NimbleEngine.GameFrameworkTests` covers what every build has: local RPCs,
the facade, `ParseLaunchArgs` and a headless Application.
