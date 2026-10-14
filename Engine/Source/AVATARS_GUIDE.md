# Adding avatars to a NimbleEngine game

This guide builds **Walker**, a small game: each player walks a pawn with WASD
or the left stick, jumps with Space, and gets in and out of a car with E. It
plays the same way standalone, in Play-in-Editor, and networked. The steps are:

1. an input map
2. the pawn prefab, with an **Avatar Harness**
3. movement as a **simulation**, so a client predicts it
4. gameplay that reads the harness
5. a game mode built on **NetGameMode**, plus Player Starts
6. a vehicle to possess and leave

Then the same avatar is scripted in **Lua** and in a **script graph**.

`AVATARS.md` is the reference: what each piece does and why. This guide is
the walkthrough.

Every example here is compiled and run by `Engine/Tests/AvatarGuideTests.cpp`
(`NimbleEngine.AvatarGuideTests`). If you change one, change the other.

---

## The model in one picture

```
 Local Player 1 / Player 0 ──────── possesses ────────► Pawn 1
 ┌──────────────────────────┐                         ┌──────────────────────────────────┐
 │ PlayerState  score, lives│                         │ Transform                        │
 │ Net          its client  │   lends its client ───► │ Net  (Predicted)                 │
 │ Possessor    LocalPlayer │                         │ AvatarHarness  InputMap=Walker   │
 └──────────────────────────┘                         │                Simulation=WalkerMove
                                                      │ WalkerPawn     your game's state │
                                                      │  └ Eye: Transform, Camera        │
                                                      └──────────────────────────────────┘
```

- **The player** is who is playing. Its score and its client stay with it.
  The engine makes one for every client that joins, and `NetGameMode` or
  AutoPossess makes one for every local player.
- **The avatar** is what that player drives right now. It reads the player's
  input through its harness. A player can switch to another avatar (a car)
  and back.

---

## Step 1: the input map

The harness reads an input map, the same way an Input component does
(`INPUT.md`). Declare one in your game module:

```cpp
NIMBLE_DEFINE_INPUT_MAP(Walker)
{
    Map.Axis  ("MoveX",    "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis  ("MoveZ",    "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Action("Jump",     "Space, Gamepad.South");
    Map.Action("Interact", "E, Gamepad.West");
}
```

Players and designers can rebind it without a rebuild. In the editor, use
**Project Settings > Input**. In a game, use the `InputMaps` library (see
"A rebind menu" below).

---

## Step 2: the pawn prefab

### Your game's component

The harness carries input. Your game's own state goes in your own
component:

```cpp
struct WalkerPawnComponent
{
    GAMECORE_COMPONENT_BODY()

    float   Speed = 400.0f;   // world units a second
    int32_t Jumps = 0;        // the server's count, replicated
};

GAMECORE_DEFINE_COMPONENT(WalkerPawnComponent, "WalkerPawn")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(WalkerPawnComponent, 1)
{
    Ar.Field("Speed", C.Speed);
}

GAMECORE_DEFINE_COMPONENT_REPLICATION(WalkerPawnComponent)
{
    Rep.Field("Jumps", &WalkerPawnComponent::Jumps);
}

GAMECORE_DEFINE_COMPONENT_SCRIPT(WalkerPawnComponent)
{
    Bind.Field("Speed",    &WalkerPawnComponent::Speed);
    Bind.ReadOnly("Jumps", &WalkerPawnComponent::Jumps);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(WalkerPawnComponent)
{
    Props.Field("Speed", &WalkerPawnComponent::Speed);
}
#endif
```

### Authoring it in the editor

Build the game module, then right-click in the Content Browser, choose
**New Prefab...** (or open an existing one), and give the root entity these
components:

| Component | Settings |
|---|---|
| **Transform** | |
| **Net** | Replicate on. **Predicted** on, so the owning client moves it at once. |
| **Avatar Harness** | **Input Map** `Walker`. **Simulation** `WalkerMove` (step 3). **View Target** on. **Auto Possess** Disabled (the mode spawns it). **Requestable** off. |
| **WalkerPawn** | Speed 400 |
| a **Render** component | whatever the pawn looks like |

Add a child entity, **Eye**, with a Transform (behind and above the pawn) and a
**Camera** with **Active off**. The view target uses it anyway, but only for
the player driving this pawn. An inactive camera never takes over anyone
else's view.

Saved, `Content/Prefabs/Pawn.nmprefab` reads:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Prefab Format="1" Name="Pawn" NextId="3">
  <Tick Mode="Tick" />
  <Entity Id="1" Name="Pawn">
    <Component Type="Transform" Version="1">
      <Position X="0" Y="0" Z="0" />
      <Rotation X="0" Y="0" Z="0" />
      <Scale X="1" Y="1" Z="1" />
    </Component>
    <Component Type="Net" Version="2">
      <Replicate Value="true" />
      <Predicted Value="true" />
    </Component>
    <Component Type="AvatarHarness" Version="1">
      <InputMap Value="Walker" />
      <Simulation Value="WalkerMove" />
      <AutoPossess Value="0" />
      <Enabled Value="true" />
      <ViewTarget Value="true" />
      <Requestable Value="false" />
    </Component>
    <Component Type="WalkerPawn" Version="1">
      <Speed Value="400" />
    </Component>
  </Entity>
  <Entity Id="2" Name="Eye" Parent="1">
    <Component Type="Transform" Version="1">
      <Position X="0" Y="200" Z="-500" />
      <Rotation X="0" Y="0" Z="0" />
      <Scale X="1" Y="1" Z="1" />
    </Component>
    <Component Type="Camera" Version="2">
      <ZNear Value="1" />
      <ZFar Value="10000" />
      <Active Value="false" />
      <Projection Value="1" />
      <FovY Value="60" />
    </Component>
  </Entity>
</Prefab>
```

### Or building it in code

A native prefab does the same in C++. Spawn it as `native:Walker.Pawn`:

```cpp
static void BuildWalkerPawn(GameCore::PrefabBuilder& B)
{
    GameCore::PrefabBuilder::Node Root = B.Root("Pawn");
    Root.Add<GameCore::TransformComponent>();

    GameCore::NetComponent Net;
    Net.bPredicted = 1;                                   // its owner moves it at once
    Root.Add(Net);

    NimbleEngine::AvatarHarnessComponent Harness;
    std::snprintf(Harness.InputMap,   sizeof(Harness.InputMap),   "%s", "Walker");
    std::snprintf(Harness.Simulation, sizeof(Harness.Simulation), "%s", "WalkerMove");
    Root.Add(Harness);

    Root.Add<WalkerPawnComponent>();

    /* Its camera: inactive, so only its own player's view uses it. */
    GameCore::PrefabBuilder::Node Eye = Root.Child("Eye");
    GameCore::TransformComponent At;
    At.Position = EngineCore::Vector3(0.0, 200.0, -500.0);
    Eye.Add(At);
    RenderCore::CameraComponent Camera;
    Camera.Projection = RenderCore::CP_Perspective;
    Camera.bActive    = 0;
    Eye.Add(Camera);
}
GAMECORE_DEFINE_NATIVE_PREFAB(WalkerPawn, "Walker.Pawn", &BuildWalkerPawn)
```

---

## Step 3: movement, as a simulation

A **simulation** moves the avatar by one frame of its player's input. Name it
in the harness's **Simulation** field and the engine runs it:

- on the server, for each frame the client sends
- on the owning client the moment a key is down, then replayed over the
  server's state when that arrives
- on a server or standalone game, each frame, for a local player's avatar

```cpp
NIMBLE_DEFINE_AVATAR_SIMULATION(WalkerMove, 0.1f)
{
    static const int MoveX = NimbleEngine::FindInputAxis("Walker", "MoveX");
    static const int MoveZ = NimbleEngine::FindInputAxis("Walker", "MoveZ");

    GameCore::Entity Pawn = InWorld.GetEntity(InAvatar);
    auto* Transform = Pawn.GetComponent<GameCore::TransformComponent>();
    auto* State     = Pawn.GetComponent<WalkerPawnComponent>();
    if(!Transform || !State || MoveX < 0 || MoveZ < 0) { return; }

    const double Step = double(State->Speed) * double(InFrame.Seconds);
    Transform->Position = EngineCore::Vector3(Transform->Position.GetX() + double(InFrame.Axes[MoveX]) * Step,
                                              Transform->Position.GetY(),
                                              Transform->Position.GetZ() + double(InFrame.Axes[MoveZ]) * Step);
}
```

Rules for a simulation:

- **Deterministic.** The same frame from the same state must give the same
  result on both ends. Use only the avatar's own components and the world
  the client also has, such as walls. Use nothing only the server knows.
- **No `HasAuthority()` inside.** The function runs on both ends by design.
- **Look up map indices once** (`static const int`), not every frame.
- **The second macro argument** (0.1 s here) is the longest frame a client may
  claim. A client says where to go, never how far.

Without prediction (a turret, a menu cursor), leave **Simulation** empty and
read the harness in a system, as in step 4.

---

## Step 4: gameplay from the harness

A harness reads like an Input component: `IsDown`, `WasPressed`,
`WasReleased`, `GetAxis`. It reads the same on every process: the server
sees its client's presses, including any that landed between two of its
ticks. Rules are the server's, so guard them with `HasAuthority()`:

```cpp
static void WalkerJumpSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* /*InUserData*/)
{
    if(!NimbleEngine::HasAuthority()) { return; }
    InWorld.Each<NimbleEngine::AvatarHarnessComponent>([](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
    {
        if(!InHarness.WasPressed("Jump")) { return; }
        if(WalkerPawnComponent* State = InAvatar.GetComponent<WalkerPawnComponent>()) { ++State->Jumps; }
    });
}
```

`Jumps` replicates, so every client sees the count.

Useful questions to ask a harness:

| Call | True when |
|---|---|
| `IsPossessed()` | a player drives it |
| `IsLocallyControlled()` | it is driven from this process, so play its sounds, show its HUD, shake its camera |
| `GetPossessor()` | returns the player driving it, for scoring with `GameSession::AddScore` |

---

## Step 5: the game mode

Derive from `NimbleEngine::NetGameMode`. Without further code, it:

- gives every local player an avatar when the level starts (not on a client,
  and not on a dedicated server)
- gives every joining client an avatar
- spawns at the level's **Player Starts**, possesses, and lends each avatar to
  its player's client

Override `GetAvatarPrefab` and `OnAvatarSpawned`, and call the base when you
override `OnLoad`, `OnStart` or `OnClientConnected`:

```cpp
class WalkerMode : public NimbleEngine::NetGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        NetGameMode::OnLoad(InSession);
        GameCore::World& World = InSession.GetWorld();
        World.RegisterSystem("Walker.Jump",     GameCore::TP_PrePhysics, &WalkerJumpSystem);
        World.RegisterSystem("Walker.Interact", GameCore::TP_PrePhysics, &InteractSystem, this);
        return true;
    }

    void OnTick(const NimbleEngine::FrameContext& /*InFrame*/) override {}

    void OnAvatarPossessed(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar) override
    {
        GameCore::World& World = Session->GetWorld();
        CORE_LOG(std::string("[Walker] ") + World.GetEntity(InPlayer).GetName() + " drives " + World.GetEntity(InAvatar).GetName())
    }

protected:
    const char* GetAvatarPrefab(GameCore::EntityID /*InPlayer*/) override { return "native:Walker.Pawn"; }

    void OnAvatarSpawned(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar, const uint32_t InOrdinal) override
    {
        GameCore::World& World = Session->GetWorld();
        World.GetEntity(InAvatar).SetName(("Pawn " + std::to_string(InOrdinal + 1)).c_str());
        Pawns[InPlayer.Pack()] = InAvatar;
    }

private:
    static void InteractSystem(GameCore::World& InWorld, float InDeltaSeconds, void* InUserData);   // step 6
    GameCore::EntityID FindVehicleNear(GameCore::World& InWorld, GameCore::EntityID InFrom, double InReach);

    std::unordered_map<uint64_t, GameCore::EntityID> Pawns;   // each player's own, by packed id
};
```

Notes on the code above:

- **`GetAvatarPrefab`** can return `Content/Prefabs/Pawn.nmprefab` for the
  authored prefab. Return `nullptr` to use the Player Start's **Avatar
  Prefab**, else `[Avatar] DefaultPrefab`.
- **`OnAvatarSpawned`** runs on the server before the avatar is first sent,
  so a colour or name set there reaches every client with it.
- **`OnAvatarPossessed`** and **`OnAvatarUnpossessed`** run on every process.
  A client gets them as the change replicates.

**Register the mode**, at file scope after the class, and **name it** for the world:

```cpp
NIMBLE_REGISTER_GAME_MODE(WalkerMode, "Walker")
```

```ini
; Config/DefaultEngine.ini
[World_Walker]
File=Content/Levels/Walker.nmworld
GameMode=Walker
```

**Place the Player Starts.** In the level, add entities with a Transform and a
**Player Start** component:

- **Player Slot** `Any`: the starts are used in turn.
- **Player Slot** `Player 1`..`Player 4`: kept for that player, in join order.
- **Avatar Prefab**: the prefab to spawn there, if the mode names none.

With no start at all, the avatar spawns at the origin.

---

## Step 6: getting into a vehicle

A vehicle is just another avatar. Give it an **Avatar Harness** and a **Net**
component, and set **Requestable** on so clients may ask to drive it. The
harness can share the `Walker` map or use a map of its own. Put a child camera
on it and the view switches as the player gets in.

On the server, `Possess(World, Player, Avatar)` moves the player into it and
lends it the player's client. Possessing the player's own pawn again gets it
out:

```cpp
void WalkerMode::InteractSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
{
    if(!NimbleEngine::HasAuthority()) { return; }
    WalkerMode& Self = *static_cast<WalkerMode*>(InUserData);

    /* Collected first: Possess changes harnesses under Each. */
    std::vector<std::pair<GameCore::EntityID, GameCore::EntityID>> Asking;   // player, what it drives
    InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
    {
        if(InHarness.WasPressed("Interact")) { Asking.push_back({ InHarness.GetPossessor(), InAvatar.GetID() }); }
    });

    for(const auto& [Player, Driven] : Asking)
    {
        const GameCore::EntityID Own = Self.Pawns[Player.Pack()];
        if(Driven != Own && InWorld.IsAlive(Own))
        {
            NimbleEngine::Possess(InWorld, Player, Own);   // out
            continue;
        }
        const GameCore::EntityID Vehicle = Self.FindVehicleNear(InWorld, Driven, 300.0);
        if(Vehicle.IsSet()) { NimbleEngine::Possess(InWorld, Player, Vehicle); }   // in
    }
}

GameCore::EntityID WalkerMode::FindVehicleNear(GameCore::World& InWorld, const GameCore::EntityID InFrom, const double InReach)
{
    const EngineCore::Vector3 From = InWorld.GetEntity(InFrom).GetWorldPose().Position;
    GameCore::EntityID Best;
    double BestDistance = InReach;
    InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
    {
        if(!InHarness.bRequestable || InHarness.IsPossessed()) { return; }
        const EngineCore::Vector3 Delta = InAvatar.GetWorldPose().Position - From;
        const double Distance = std::sqrt(double(Delta.GetX()) * double(Delta.GetX()) + double(Delta.GetZ()) * double(Delta.GetZ()));
        if(Distance <= BestDistance) { BestDistance = Distance; Best = InAvatar.GetID(); }
    });
    return Best;
}
```

The interaction runs on the server from the harness's replicated input, so
it needs no RPC of its own. A **client** that wants to ask directly (a script,
a UI button) calls `possessor:Possess(car)` or `Game.Possess(player, car)`.
The server then checks that the car is Requestable and free, and asks
`IGameMode::CanPossess`. Override that for your own rules, such as distance
or team.

When the driver's client leaves, the car goes back to the server and stays in
the level. The client's own pawn goes with it.

---

## Lua

### The pawn's script

Give the prefab a script: in the Prefab Editor's **Script** panel, or with
`B.Script(...)` in a native prefab. A harness is a component like any other:

```lua
-- Scripts/WalkerPawn.lua: the pawn prefab's Script.
return {
    TickPhase = TickPhase.PrePhysics,

    OnStart = function(self)
        self.Harness = self.Entity:GetComponent("AvatarHarness")
    end,

    -- Possession is announced on every process; on a client, as it replicates.
    OnPossessed = function(self, player)
        self.Entity:SetName(player:GetName() .. "'s pawn")
    end,

    OnUnpossessed = function(self, player)
        self.Entity:SetName("Empty pawn")
    end,

    OnTick = function(self, dt)
        if not self.Harness:IsPossessed() then return end

        -- Rules are the server's: on a client this reads the same press,
        -- and the score arrives by replication.
        if Net.HasAuthority() and self.Harness:WasPressed("Jump") then
            Game.AddScore(self.Harness:GetPossessor(), 1)
        end

        -- Feedback only the player at the keys should get.
        if self.Harness:IsLocallyControlled() and self.Harness:GetAxis("MoveX") ~= 0 then
            self.Moving = true
        end
    end,
}
```

- **`OnPossessed(self, player)`** and **`OnUnpossessed(self, player)`** come
  from the engine. An avatar spawned and possessed as the level starts has a
  script that starts afterwards. It still gets `OnPossessed`, right after its
  `OnStart` in the same frame.
- **`TickPhase.PrePhysics`** runs after the harness is filled, so it sees this
  frame's input.
- **On a client, this runs only for avatars placed in the level.** A prefab
  spawned on the server (the pawns `NetGameMode` makes) reaches clients as
  its components, not as an instance, so its script runs on the server only.
  The server half above (`Net.HasAuthority()`) works either way. For client
  feedback on spawned pawns, use a placed object's script or the startup
  script (`Game.GetLocalAvatar`, `Events.On(Game.Possessed, ...)`), as
  NetArena's Signpost does.

### The level's script, or a HUD

The startup script (`[World_Walker] StartupScript`) or a UI document hears
every possession:

```lua
Events.On(Game.Possessed, function(player, avatar)
    print(player:GetName() .. " now drives " .. avatar:GetName())
end)

Events.On(Game.Unpossessed, function(player, avatar)
    print(player:GetName() .. " left " .. avatar:GetName())
end)

-- This machine's player and what it drives, e.g. for a HUD:
local me   = Game.GetLocalPlayer(0)
local mine = Game.GetLocalAvatar(0)
```

### Possessing from script

```lua
-- On the server: direct. On a client: a request the server's rules may refuse.
local car = World.FindEntity("Car")
Game.Possess(Game.GetLocalPlayer(0), car)

-- Out again: back to the pawn it came from (or Game.Unpossess(player) to drive nothing).
Game.Possess(Game.GetLocalPlayer(0), myPawn)

-- A cutscene: view through another camera, then back to the avatar's.
Game.SetViewTarget(World.FindEntity("CutsceneCamera"))
Game.SetViewTarget(nil)
```

### A rebind menu

```lua
-- List the Walker map: name, is it an axis, its keys, the game's own keys.
for i = 0, InputMaps.GetEntryCount("Walker") - 1 do
    local name, isAxis, keys, default = InputMaps.GetEntry("Walker", i)
    print(name, keys)
end

-- "Press a key for Jump": call each frame until a key comes.
local key = InputMaps.CaptureKey(0)
if key then
    local clash = InputMaps.FindConflict("Walker", "Jump", key)
    if clash then print(key .. " is already on " .. clash) end
    InputMaps.SetBinding("Walker", "Jump", key .. ", Gamepad.South")
    InputMaps.Save()   -- into the player's User.ini
end
```

---

## Script graph

A script graph is a prefab script drawn as nodes. This one does two things.
On a server or standalone game, pressing Jump scores 2 for whoever drives
the pawn. Separately, whenever anyone takes any avatar, it prints who drives
what. Create it with the Content Browser's **New Script Graph...**, set the graph's
**Tick Phase** to `PrePhysics`, and set it as the pawn prefab's script in the
Prefab Editor's **Script** panel.

The harness's nodes appear under **Components/AvatarHarness**: Get
AvatarHarness, Is Down, Was Pressed, Was Released, Get Axis, Is Possessed, Is
Locally Controlled, Get Possessor. Possession events and calls are under
**Game**: On Possessed, On Unpossessed, Possess, Unpossess, Get Avatar, Get
Possessor, Get Local Player, Get Local Avatar, Set View Target.

**Jump scores** (nodes 1 to 8):

| # | Node (type) | Settings |
|---|---|---|
| 1 | On Tick (`Event.OnTick`) | |
| 2 | Get AvatarHarness (`Component.AvatarHarness.Get`) | Entity left empty means this one |
| 3 | Was Pressed (`Component.AvatarHarness.WasPressed`) | Action = `Jump` |
| 4 | Has Authority (`Library.Net.HasAuthority`) | |
| 5 | And (`Logic.And`) | |
| 6 | Branch (`Flow.Branch`) | |
| 7 | Get Possessor (`Component.AvatarHarness.GetPossessor`) | |
| 8 | Add Score (`Library.Game.AddScore`) | Amount = `2` |

| From | To |
|---|---|
| 2 **AvatarHarness** | 3 **Target**, 7 **Target** |
| 3 **Pressed** | 5 **A** |
| 4 **Authority** | 5 **B** |
| 1 **Then** | 6 **Exec** |
| 5 **Out** | 6 **Condition** |
| 6 **True** | 8 **Exec** |
| 7 **Player** | 8 **Player** |

**Who drives what** (nodes 10 to 15):

| # | Node (type) | Settings |
|---|---|---|
| 10 | On Possessed (`Library.Game.OnPossessed`) | |
| 11 | Get Name (`Entity.GetName`) | |
| 12 | Append (`String.Append`) | B = ` drives ` |
| 13 | Get Name (`Entity.GetName`) | |
| 14 | Append (`String.Append`) | |
| 15 | Print (`Debug.Print`) | |

| From | To |
|---|---|
| 10 **Player** | 11 **Entity** |
| 10 **Avatar** | 13 **Entity** |
| 11 **Name** | 12 **A** |
| 12 **Out** | 14 **A** |
| 13 **Name** | 14 **B** |
| 10 **Then** | 15 **Exec** |
| 14 **Out** | 15 **Value** |

The editor saves the graph with the Lua it compiles to. For this one:

```lua
-- Generated by NimbleEditor from the script graph "WalkerPawn". Do not edit.
return {
    TickPhase = TickPhase.PrePhysics,

    OnStart = function(self)
        self.GraphEventHandles = {
            Events.On("Possessed", function(...) self:Event_Possessed(...) end),
        }
    end,

    OnTick = function(self, dt)
        local Score1
        local t1 = self.Entity:GetComponent("AvatarHarness")
        local t2 = t1:WasPressed("Jump")
        local t3 = Net.HasAuthority()
        local t4 = t2 and t3
        if t4 then
            local t5 = self.Entity:GetComponent("AvatarHarness")
            local t6 = t5:GetPossessor()
            Score1 = Game.AddScore(t6, 2)
        end
    end,

    OnDestroy = function(self)
        for _, Handle in ipairs(self.GraphEventHandles or {}) do Events.Off(Handle) end
        self.GraphEventHandles = nil
    end,

    Event_Possessed = function(self, Arg1, Arg2, Arg3, Arg4)
        local t1 = Arg1:GetName()
        local t2 = t1 .. " drives "
        local t3 = Arg2:GetName()
        local t4 = t2 .. t3
        print(t4)
    end,
}
```

A graph has no event node for the avatar's own `OnPossessed`. Use **On
Possessed** under Game, which fires for every possession, or test **Is
Possessed** / **Is Locally Controlled** on its tick.

---

## No mode at all: placing an avatar

A level that only needs "the player drives this" does not need
`NetGameMode`:

1. Place the prefab in the level.
2. Set its harness's **Auto Possess** to `Player 1`.

When the level starts, local player 1 is made and takes it. This happens with
any game mode, and in the editor's Play. For split-screen, set `Player 2` on
another avatar, and have player 2 use its gamepad: the keyboard and mouse are
read by every local player.

---

## Networking checklist

- [ ] The avatar prefab has a **Net** component, and **Predicted** if it has a
      simulation.
- [ ] Every component on it has a serializer, so it reaches clients when it
      spawns. Fields that change afterwards are in its replication table.
- [ ] Rules (scoring, damage, spawning, `Possess`) run only under
      `HasAuthority()`. The simulation does not check it.
- [ ] Vehicles that clients may ask for are **Requestable**. Your mode's
      `CanPossess` has the final say.
- [ ] Avatar cameras are **Active off**.

---

## Trying it

Standalone or listen server: press Play in the editor. The pawn spawns at a
Player Start and the view goes through its Eye. Networked: set
**Edit > Editor Settings > Play > Network mode** to **Listen Server**, press
**Play**, then **Play > New Client Window**. Or run from a command line, in
`Binaries/<Platform>_Debug`. Start a server:

```bash
./WalkerServer -port=7777
```

then a client:

```bash
./Walker -connect=127.0.0.1:7777
```

`Games/NetArena` is a complete networked game built this way, with this
guide's vehicle as its karts (README, "The avatar demo"). It logs each
client's prediction: `worst correction 0u` means client and server agree.

---

## When something is off

| Symptom | Likely cause |
|---|---|
| The avatar spawns but does not move | The harness's **Input Map** does not match your `NIMBLE_DEFINE_INPUT_MAP` name. Or **Simulation** names nothing registered: the game module is not loaded, or there is a typo. Or **Enabled** is off. |
| It moves for the host but not for clients | The prefab has no **Net** component, so the client's input cannot be sent ("has no Net component" is logged). |
| A client's avatar stutters or snaps back | The simulation is not the same on both ends (it reads something only the server has), or **Predicted** is off. |
| Every player sees through the same camera | The avatar's camera is **Active**, so it wins the level's first-active rule for players who are not driving it. Turn Active off. |
| A client's `Possess` does nothing | The avatar is not **Requestable**, someone already drives it, or `CanPossess` refused. |
| `OnPossessed` never runs | It is the avatar's prefab script that hears it, not a startup script. For those, use `Events.On(Game.Possessed, ...)`. |
| Pressing the key once counts twice | The rule runs on every process. Guard it with `HasAuthority()`. |
