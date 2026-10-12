# Adding networking to a NimbleEngine game

A step-by-step guide to making a game multiplayer with NetCore.
`README.md` beside this file explains how NetCore works inside; this guide
covers what a game writes. `Games/NetArena/NetArenaGame.cpp` is the complete
worked example, about 400 lines, and every snippet below comes from its
patterns.

**Contents**

1. The model in one page
2. Build setup
3. Decide what is networked
4. Authority: who does what
5. Players
6. Replicated fields
7. RPCs
8. Sending input (8b: from Lua and script graphs)
9. Testing
10. Keeping the game buildable without NetCore
11. Pitfalls
12. Checklist

---

## 1. The model in one page

NimbleEngine networking is **server-authoritative**:

- **One process is the server.** It is either a headless `<Game>Server` (a
  dedicated server) or a windowed game that also plays (a listen server).
- **Every process runs the same game module and the same start level.** The
  level's own entities (floor, lights, camera) exist everywhere, built by
  each process.
- **The server owns everything networked.** An entity with a
  `GameCore::NetComponent` is created on the server and *replicated*: clients
  receive it, with its components, when it appears. After that they receive
  each replicated field whenever it changes, and finally its destruction.
- **Clients never send state.** A client asks the server to do things with
  **Server RPCs**: member functions of a component, called on an entity the
  client owns. The server answers by changing state, which replicates, or
  with **Client** / **Multicast** RPCs.
- **Offline, nothing changes in your code.** With no session (or NetCore
  compiled out) every process is "standalone": it has authority over
  everything and every RPC runs locally, so the same game plays single-player.

```
 client                           server                          other clients
   │  keys → ServerMove(n, dir) ──►  runs ServerMove: moves the capsule
   │  (moves it at once: predicted)   acknowledges move n
   │  ◄── Transform, ack n ─────────  replication ──────────────────► Transform (blended)
   │  replays moves after n
```

---

## 2. Build setup

Networking is on by default (`NIMBLE_WITH_NETCORE`). Your game's
`CMakeLists.txt` needs one line beside its launcher. Games made from the
template since NetCore arrived already have it:

```cmake
nimble_add_game_launcher(${GAME_NAME} MODULE ${GAME_MODULE})
nimble_add_server_launcher(${GAME_NAME}Server MODULE ${GAME_MODULE})   # the dedicated server
```

`nimble_add_server_launcher` builds a console exe running your module
headless, with no window and the Null renderer. When NetCore is off, it skips
itself instead of failing.

Your game module links `NimbleEngine::NimbleEngine` as it already does. **Do
not link NetCore or include NetCore headers.** Everything a game needs is in
GameCore and NimbleEngine (section 10 explains why).

The headers you will use:

```cpp
#include "Public/GameCore/Components.h"          // NetComponent (GameCore/Components/NetComponent.h)
#include "Public/GameCore/ComponentReplication.h" // GAMECORE_DEFINE_COMPONENT_REPLICATION
#include "Public/GameCore/ComponentRpc.h"         // GAMECORE_DEFINE_COMPONENT_RPCS, GAMECORE_RPC
#include "Public/GameCore/ComponentSerializer.h"  // a serializer: how a component reaches clients
#include "Public/NimbleEngine/NetFacade.h"        // HasAuthority(), IsLocallyOwned(), ...
```

---

## 3. Decide what is networked

Sort your entities into three kinds:

| Kind | Example | What to do |
|---|---|---|
| **Scenery**: identical everywhere, never changes | floor, walls, static lights | Nothing. Each process builds it from the level. |
| **Local only**: per process, by design | the camera, an input controller, UI, particle effects | Nothing. Leave it without a `NetComponent`. |
| **Shared state**: the server decides it | players' pawns, pickups, doors, projectiles, score | Give it a `NetComponent`, **create it only on the server**, and replicate what changes. |

Rules for shared-state entities:

- **Create them only where `HasAuthority()` is true.** A client that creates
  one has made a local entity nobody else can see. The server's copy arrives
  on its own.
- **Every component you want clients to have needs a serializer**
  (`GAMECORE_DEFINE_COMPONENT_SERIALIZER`). When the entity first appears on a
  client, NetCore sends each of its components through its serializer. A
  component without one never reaches clients. Engine components
  (Transform, Render, lights, PlayerState...) already have serializers.
- **Entities placed in the level file can be networked too.** Add a
  `NetComponent` in the editor (component "Net"). Both ends load it, and the
  server's copy is matched to the client's by `EntityGuid`, so it is not
  duplicated.
- **Author what you spawn as a prefab with a Net component.** The server
  spawns it with `World::SpawnPrefab`, then sets its owner and per-instance
  values before the frame ends.
- **Your game's own components author like the engine's.** The editor keeps
  the game module loaded while editing, so they appear under Add Component
  and save with the world.

`Games/NetArena/README.md` walks through a level authored all three ways:
scenery, a placed networked entity, and a spawned networked prefab.

---

## 4. Authority: who does what

`NimbleEngine/NetFacade.h` answers every role question without NetCore:

| Call | True when |
|---|---|
| `HasAuthority()` | server, listen server, or standalone. **Use this for game logic.** |
| `IsServer()` | a server (dedicated or listen) |
| `IsClient()` | connected to a server |
| `IsDedicatedServer()` | a headless server with no local player |
| `IsLocallyOwned(const NetComponent&)` | this process controls that entity |
| `GetLocalClientId()` | a client's id once welcomed, else `GameCore::NET_CLIENT_NONE` |
| `GetNetMode()` | `ENetMode::Standalone` / `Server` / `Client` |

The role is known from `ILevel::OnLoad` onwards, because networking starts
before the level loads. Typical uses:

```cpp
void OnStart() override
{
    if(NimbleEngine::HasAuthority()) { SpawnPickups(); }   // the server places them; clients receive them
}

void MySystem(GameCore::World& InWorld, float InDt, void*)
{
    if(!NimbleEngine::HasAuthority()) { return; }   // simulation runs on the server only
    ...
}
```

**Systems that move shared state should early-out on clients.** Otherwise a
client moves it too, and the next replication overwrites it. To make a
player's own pawn answer at once, predict it (section 8).

---

## 5. Players

> **The short way: avatars** (`Engine/Source/AVATARS.md`). Put an **Avatar
> Harness** on the pawn prefab and derive the mode from
> `NimbleEngine::NetGameMode`. Each player then gets a pawn at a Player
> Start, owned by its client and possessed. The harness carries the input to
> the server (section 8 by itself), and a harness Simulation predicts it.
> NetArena works this way. The rest of this section, and section 8, is what
> that does for you, for a game that wants to do it by hand.

On the server, when a client joins, the engine makes it a **player entity**:

- a `NetComponent` with `OwnerClient` set to the client's id
- a `PlayerStateComponent` named "Player N"

Then `IGameMode::OnPlayerAdded` runs, then the hook below. Turn this off with
`[Net] SpawnPlayerPerClient=false` to make players yourself.

```cpp
class MyMode : public NimbleEngine::IGameMode
{
    /* Server only. Give the player its pawn: add components to InPlayer. */
    void OnClientConnected(int32_t InClient, GameCore::EntityID InPlayer) override
    {
        GameCore::Entity Player = Session->GetWorld().GetEntity(InPlayer);
        Player.AddComponent(MyTransformAtSpawnPoint());
        Player.AddComponent(MyRenderComponent());
        Player.AddComponent<MyPawnComponent>();
    }

    /* Server only. Everything the client owned is destroyed right after this. */
    void OnClientDisconnected(int32_t InClient, GameCore::EntityID InPlayer) override {}
};
```

Components added in `OnClientConnected` are on the entity before it is first
sent, so clients get them all at once.

**The listen server's own user** is not a client, so it gets no player
automatically. Give it one in `OnStart`, owned by nobody, which means the
server owns it:

```cpp
void OnStart() override
{
    if(NimbleEngine::HasAuthority() && !NimbleEngine::IsDedicatedServer())
    {
        GameCore::Entity Host = Session->GetWorld().CreateEntity("Host");
        Host.AddComponent<GameCore::NetComponent>();   // OwnerClient = NET_CLIENT_NONE
        MakePawn(Host);
    }
}
```

This also covers standalone play: the same code gives the single player a
pawn.

**Finding "my" pawn** on any process: iterate pawns and check
`IsLocallyOwned(*Net)`. On a client that matches `OwnerClient ==
GetLocalClientId()`. On a server or standalone process it matches entities
owned by nobody.

---

## 6. Replicated fields

Replication is for state that **changes after spawn** and that every client
must see. Declare it once per component, in one `.cpp`:

```cpp
GAMECORE_DEFINE_COMPONENT_REPLICATION(DoorComponent)
{
    Rep.Field("OpenAmount", &DoorComponent::OpenAmount);            // float
    Rep.Flag("Locked",      &DoorComponent::bLocked);               // uint8_t 0/1
    Rep.Entity("OpenedBy",  &DoorComponent::OpenedBy);              // EntityID, sent as a NetId
    Rep.Field("Label",      &DoorComponent::Label, GameCore::RFF_InitialOnly);  // char[N], spawn only
}
```

- **Field types:** `float`, `double`, `int32_t`, `uint8_t`, `bool`, `Vector3`,
  `AABB`, `Color`, `char[N]`, `EntityID` (through `Rep.Entity`). Script
  structs are not supported yet.
- **At most 64 fields per component.**
- **`RFF_InitialOnly`** sends the field when the entity appears and never
  again.
- **The server sends a field only when its bytes change.** It compares with
  what it last sent, once per frame after the World ticks.
- **`TransformComponent` already replicates** Position, Rotation and Scale.
  Moving a networked entity on the server is enough.
- **The serializer and the replication table do different jobs:**
  - The serializer is the whole component, once, when the entity appears.
  - The replication table is the fields, as they change.

  A field needed only at spawn can be in the serializer alone.
- **Clients must not write replicated fields.** The server's next change
  overwrites them.

---

## 7. RPCs

An RPC is a member function of a component, run on the same entity in another
process.

**Declare** the function as an ordinary member returning `void`. An optional
first parameter, `const GameCore::RpcContext&`, tells you who sent it:

```cpp
struct ShipComponent
{
    GAMECORE_COMPONENT_BODY()
    int32_t Ammo = 10;

    void ServerFire(const GameCore::RpcContext& InCtx, const EngineCore::Vector3& InAim);
    void ClientOutOfAmmo();
    void MulticastExplode(const EngineCore::Color& InColor);
};
```

**Register** them, in one `.cpp`:

```cpp
GAMECORE_DEFINE_COMPONENT_RPCS(ShipComponent)
{
    Rpc.Server<&ShipComponent::ServerFire>("ServerFire");
    Rpc.Client<&ShipComponent::ClientOutOfAmmo>("ClientOutOfAmmo");
    Rpc.Multicast<&ShipComponent::MulticastExplode>("MulticastExplode");
}
```

**Call** them. `GAMECORE_RPC(Type, Function)` returns a callable that takes the
entity, then the arguments:

```cpp
GAMECORE_RPC(ShipComponent, ServerFire)(MyShip, AimDirection);
```

**Where a call runs:**

| Target | Called on the server | Called on a client |
|---|---|---|
| `Server` | runs there | sent to the server |
| `Client` | sent to the **owning** client; runs on the server if no client owns it | runs there |
| `Multicast` | runs on the server **and** is sent to every client | runs there |

**Rules:**

- **The entity must be networked** (have a `NetComponent`) and must already
  have been sent, which happens at the end of the frame it was created. Before
  that:
  - a client's Server RPC is dropped, with a log line;
  - the server's Client RPC to an owning client is dropped, with a log line;
  - the server's Multicast runs on the server only.
- **The server accepts a Server RPC only from the client that owns the
  entity.** Make input go through an entity the player owns, such as their
  pawn or player entity. To ask about another entity, pass it as an argument.
- **Validate arguments on the server.** A Server RPC is a client's request,
  not an order: clamp, check range, check cooldowns.
- **Argument types:** `float`, `double`, `int32_t`, `uint32_t`, `uint8_t`,
  `bool`, `Vector3`, `Color`, `EntityID` / `Entity` (resolved to the receiver's
  entity), and `GameCore::RpcString<N>` for text, by value or `const&`.
  At most 15 arguments.
- **RPCs are reliable and ordered** with spawns and destroys, so an RPC never
  arrives before the entity it is addressed to. Replicated fields travel
  separately and unreliably, so an RPC can arrive a tick before or after a
  field the server changed at the same time. Pass what the receiver needs as
  arguments, rather than relying on a field it may not have yet.
- **The call returns `false`** (and logs) if the entity is dead, no RPC has
  that name, or the arguments' types do not match the registration.

---

## 8. Sending input

With an Avatar Harness on the pawn, all of this is built in (AVATARS.md,
"Input" and "Prediction"): the harness numbers and sends each frame, the
server merges presses that arrive between its ticks, and a Simulation is
predicted and replayed. What follows is the hand-rolled version.

The pattern NetArena uses, and the one to start from:

1. **A local controller entity** in the level: an `InputComponent`, no
   `NetComponent`. Every process has its own, which reads that process's
   keyboard.
2. **One move function** that both ends run: given a Transform, a direction
   and a frame's seconds, it moves the Transform. Keep it deterministic and
   free of anything only the server has.
3. **In `IGameMode::OnTick`,** read the controller's axes and find the pawn
   this process owns (`IsLocallyOwned`). Each frame it moves, give the move a
   number and send it in a Server RPC.
4. **On the server, the RPC validates and applies the move** with the same
   function, then records its number in `NetComponent::InputAck`. The
   Transform and the ack replicate.

```cpp
void ServerMove(int32_t InSequence, const EngineCore::Vector3& InDir, float InSeconds)   // on the pawn component
{
    GameCore::NetComponent* Net = Owner.GetSibling<GameCore::NetComponent>();
    if(Net && InSequence <= Net->InputAck) { return; }                  // old or repeated
    ApplyMove(*Owner.GetSibling<GameCore::TransformComponent>(), InDir,  // clamps the direction and the seconds
              std::min(InSeconds, 0.1f));
    if(Net) { Net->InputAck = InSequence; }
}
```

On a listen server or a standalone game the same call simply runs locally.

### Smoothing

Every networked Transform a client receives is shown slightly in the past
(`[Net] InterpolationDelayMs`, 100 by default) and blended between the
server's updates. The server stamps each update with its clock, so a 30 Hz
server moves remote entities smoothly at any frame rate. This needs nothing
from the game. `-Net.InterpolationDelayMs=0` turns it off. Keep the delay
above two server ticks (the server's `[Net] ServerTickRate`), or a single
late packet shows as a stop.

### Prediction

Smoothing would make a player's own pawn answer its keys a round trip late,
plus the delay. So the owner **predicts** it:

1. **Mark the pawn predicted:** `Predicted` on its Net component in the
   prefab (`NetComponent::bPredicted`). Its owner then takes the server's
   Transform for it as-is, never blended. Everyone else still blends it.
2. **On the owning client, apply each move at once** with the same function,
   keep it in a list, and send it:

   ```cpp
   const int32_t Sequence = ++NextSequence;
   if(NimbleEngine::IsClient() && Net->bPredicted)
   {
       ApplyMove(*Transform, Wanted, Seconds);
       Pending.push_back({ Sequence, Wanted, Seconds });
   }
   GAMECORE_RPC(PawnComponent, ServerMove)(InPawn, Sequence, Wanted, Seconds);
   ```

3. **Reconcile when the server's state arrives.** Register a handler in the
   mode's `OnLoad` (`NimbleEngine::SetNetCorrectionHandler`) and clear it in
   `OnUnload`. NetCore calls it once per predicted pawn whose state arrived,
   after writing the server's Transform. Drop the moves up to the ack and
   replay the rest:

   ```cpp
   static void OnCorrection(GameCore::World& InWorld, GameCore::EntityID InPawn, int32_t InAck, void* InMode)
   {
       MyMode& Self = *static_cast<MyMode*>(InMode);
       GameCore::TransformComponent* T = InWorld.GetEntity(InPawn).GetComponent<GameCore::TransformComponent>();
       Self.Pending.erase(std::remove_if(Self.Pending.begin(), Self.Pending.end(),
                                         [&](const PendingMove& M) { return M.Sequence <= InAck; }),
                          Self.Pending.end());
       for(const PendingMove& M : Self.Pending) { ApplyMove(*T, M.Direction, M.Seconds); }
   }
   ```

When both ends run the same code from the same place, the replay lands where
the pawn already was and nothing visibly moves. When the server disagrees
(it clamped a move, or something pushed the pawn), the pawn snaps to the
server's answer. NetArena logs the worst correction every five seconds:
`worst correction 0u` means client and server agree.

Predict only what the client can know. Movement into a wall the client also
has is fine. Being hit by another player is the server's to say.

---

## 8b. From Lua and script graphs

The `Net` script library and component RPCs give scripts the same reach as C++.
None of it needs NetCore: with networking compiled out, or no session, every
answer is standalone's and every call runs locally.

**Roles:**
- `Net.HasAuthority()`, `Net.IsServer()`, `Net.IsClient()`
- `Net.IsDedicatedServer()`, `Net.IsStandalone()`, `Net.IsConnected()`
- `Net.GetLocalClientId()`

**Entities:**
- `Net.IsNetworked(entity)`
- `Net.GetOwner(entity)`: a client id, or -1 for the server's own.
- `Net.IsLocallyOwned(entity)`: an entity without a `NetComponent` counts as
  this process's.

**Calling RPCs.** Every RPC a component registers is a method of its Lua
proxy, routed exactly as a C++ call is:

```lua
local ship = player:GetComponent("Ship")
ship:ServerFire(aimX, aimY, aimZ)                        -- a Vector3 is three values
Net.Call(player, "Ship", "ServerFire", aimX, aimY, aimZ)  -- the same by name: the graph node
```

- **Arguments follow the RPC's C++ types.** A `Vector3` is three numbers, a
  `Color` four integers (alpha optional when it is last), an `Entity` an
  entity, `RpcString<N>` a string.
- **A wrong type or too many arguments raises a Lua error.**
- **The call returns `true` once handed off.** Routing problems are logged,
  as for C++.
- **An RPC wins over a script binding of the same name.** A handler exposed
  with `Bind.Native` under its RPC name still goes through the network. Bound
  under another name, it runs locally: don't.

**Events, server only:**

```lua
Events.On("ClientConnected", function(client, player) ... end)      -- after IGameMode::OnClientConnected
Events.On("ClientDisconnected", function(client, player) ... end)   -- before its entities are destroyed
```

`client` arrives as a Lua float, as every EventBus number does.
`string.format("%d", client)` prints it as an integer.

**Where scripts run.** The startup script and prefab scripts run on every
process, so guard shared-state changes with `Net.HasAuthority()`, as in C++.
Scripts cannot yet declare RPCs or replicated fields of their own. Declare
them on a C++ component and call or read them from Lua.

---

## 9. Testing

### In the editor

1. Build your game's editor tree. **Build > Compile Source** rebuilds your
   module, `<Game>` and `<Game>Server` together.
2. Open **Play > Network Settings...** (Editor Settings > Play):
   - **Listen Server:** the viewport hosts and plays.
   - **Client** with **Start a dedicated server:** the editor runs
     `<Game>Server` and the viewport joins it.
   - **Client windows at Play:** more players, opened automatically.
   - **Client window arguments:** extra command line for those windows.
3. **Play** (F5), then **Play > New Client Window** (Shift+F6) for each extra
   player.
4. Every process logs into the editor's Console: `[Server] ...` and
   `[Client 1] ...` beside your own lines. **Stop** ends them all.

### From a command line

In `Binaries/<Platform>_<Config>`:

```
<Game>Server -port=7777
<Game> -connect=127.0.0.1:7777
<Game> -connect=127.0.0.1:7777
```

or `<Game> -listen` plus clients. Any config var can be set on the command
line with `-Section.Key=Value`, including your module's own.

### Unattended

Give your game a bot switch: a config var that replaces keyboard input with a
pattern. NetArena's is `[NetArena] AutoMove`. Have the mode log what each
process sees every few seconds. Then a server plus two clients started by a
script proves replication from their logs alone:

```
[NetArena] red(you)=(-720,0) blue=(-175,-210)
```

The editor runs the same way through `--command`:

```
NimbleEditor --command "Editor.PlayNetMode 1" --command "play" --command "after 3000 play.client" ...
```

---

## 10. Keeping the game buildable without NetCore

A game that turns networking off must still compile. That holds automatically
if you follow one rule: **use only GameCore and NimbleEngine headers.**

- `GAMECORE_DEFINE_COMPONENT_REPLICATION` compiles against a binder that
  records nothing.
- `GAMECORE_DEFINE_COMPONENT_RPCS` and `GAMECORE_RPC` work as usual, and calls
  run locally.
- `NetComponent` still exists, so levels and prefabs with it still load.
- `NetFacade.h` answers Standalone.
- `OnClientConnected` and `OnClientDisconnected` are never called.

To turn it off, in your game's `CMakeLists.txt` before the engine is added:

```cmake
set(NIMBLE_WITH_NETCORE OFF CACHE BOOL "" FORCE)
```

`nimble_add_server_launcher` then skips the server, and no NetCore, ENet or
`ws2_32` is built or linked.

If you must branch, test `NimbleEngine::IsNetworkingAvailable()` at runtime,
or `#if WITH_NETCORE` at compile time. The define is always 0 or 1.

---

## 11. Pitfalls

**A capsule appears twice, or a client has an extra one.**
Shared state was created on a client. Guard creation with `HasAuthority()`.

**A component's data never reaches clients.**
The component has no serializer, or the changing field is not in its
replication table.

**"Server RPC ...: not connected, or the entity is not a networked one; dropped".**
The entity has no `NetComponent`, or the server has not sent it yet. Call
from an entity the client received, such as its own pawn.

**"client N asked for X on an entity it does not own ... refused".**
The Server RPC was called on an entity another player, or the server, owns.

**"... registered with other argument types".**
The arguments passed do not match the function's parameters. Each argument
is converted to its parameter's type, so pass compatible values.

**A client's vehicle vanishes when the client leaves.**
Its Net component was the client's when nobody drove it. `Possess` lends a
level's vehicle to its driver's client and gives it back (`RestOwner`), and
NetHost does that before destroying a leaving client's entities. A vehicle
given to the client outright, by setting `OwnerClient` by hand, goes with the
client.

**State flickers back on a client.**
The client is writing a replicated field. Only the server writes; clients
send requests.

**Clients see different levels.**
Every process must start the same level (`[Game] StartLevel`). Level changes
during a session are not supported yet.

**"its replicated components or RPCs differ from the server's (a different build of the game)".**
The handshake compares every replicated field and RPC, so client and server
must come from the same build. **Build > Compile Source** keeps them together.

**A client window will not start from the editor.**
`<Game>.exe` is not built, or a running one is locked. Stop Play, then
Compile Source.

---

## 12. Checklist

- [ ] `nimble_add_server_launcher(<Game>Server MODULE <Game>Game)` in the
      game's `CMakeLists.txt`.
- [ ] Shared-state entities get a `NetComponent` and are created only under
      `HasAuthority()`.
- [ ] Every component they carry has a `GAMECORE_DEFINE_COMPONENT_SERIALIZER`.
- [ ] Fields that change after spawn are in
      `GAMECORE_DEFINE_COMPONENT_REPLICATION` (Transform already is).
- [ ] Simulation systems that change shared state early-out unless
      `HasAuthority()`.
- [ ] `OnClientConnected` turns the engine's player entity into a pawn
      (or the mode is an `NetGameMode`, and the pawn has an Avatar Harness).
- [ ] `OnStart` gives a listen server's (and standalone) user a pawn when
      `HasAuthority() && !IsDedicatedServer()`.
- [ ] Input is read locally and sent as a Server RPC on the owned pawn, on
      change. The server validates it.
- [ ] No NetCore includes in game code.
- [ ] Tested in the editor as Listen Server plus a client window, and as
      Client with a dedicated server.
