# JoltPhysics

Rigid-body physics for a GameCore `World`, on
[Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.6.0. Entities with a
**Rigid Body** component fall, collide, stack and can be pushed. **Physics
Constraint** components join them with joints. Both components belong to
GameCore. This module simulates them.

`JoltPhysics` is written for this repository and uses `nimble_add_module()`.
It vendors Jolt under `External/`.

---

## Position in the tree

```
Host exe --> NimbleEngine --> JoltPhysics --> GameCore --> EngineCore
                                   `-------> Jolt (STATIC, inside JoltPhysics only)
```

`NimbleEngine` links it **PRIVATE**. A game never includes a JoltPhysics
header. It uses GameCore's components and `NimbleEngine/PhysicsFacade.h`, so
switching the module off cannot break a game's include or link line.

Jolt is a STATIC library inside `JoltPhysics.dll` / `libJoltPhysics.so` and
nowhere else. That gives one factory, one type registry and one job system
per process. GDI lives inside RenderCore for the same reason.

---

## Turning it on

It is **off by default**, one switch per configure, `NIMBLE_WITH_JOLTPHYSICS`.
It becomes `WITH_JOLTPHYSICS=0/1` on every target. A project opts in from its
config file, the way LightLab does:

```ini
; Config/DefaultEngine.ini
[Modules]
JoltPhysics=true
```

In the editor the same switch is **Project Settings > Project > Modules >
Physics (JoltPhysics)**. CMake reads the file as the game configures
(`Engine/cmake/NimbleProject.cmake`). The file is a configure dependency, so
the next build applies an edit. A build that disagrees with the file says so
at startup: `NimbleEngine::WarnModuleConfigMismatch`.

You can also opt in without the file: put
`set(NIMBLE_WITH_JOLTPHYSICS ON CACHE BOOL "" FORCE)` before
`add_subdirectory(<engine>)`, or pass `-DNIMBLE_WITH_JOLTPHYSICS=ON`. A
`[Modules]` key in the file wins over both.

**Off**, nothing of Jolt is compiled:

- **The components stay.** Rigid Body, Physics Constraint and Vehicle are GameCore's,
  so levels and prefabs that carry them load, edit, save and cook unchanged.
  They are inert.
- **It says so.** A level that has any warns once as it loads, naming the
  module and its option. The first such component added later warns too, and
  so does the editor as it opens such a world:
  `NimbleEngine::WarnInactiveModuleComponents`, `GameSession.h`. NetCore's
  `NetComponent` gets the same treatment when `NIMBLE_WITH_NETCORE` is off.
- **The facade falls back.** `NimbleEngine::IsPhysicsAvailable()` is false.
  `PhysicsRaycast` tests the World's Bounds (`World::Raycast`) and gives no
  normal.

---

## The components

### Rigid Body (`GameCore/Components/RigidBodyComponent.h`)

| Field | |
|---|---|
| `Motion` | **Dynamic** (simulated; it writes the Transform), **Kinematic** (follows its Transform and pushes dynamic bodies), **Static** (never moves) |
| `Shape`, `ShapeSource` | see *Shapes* below |
| `Mass` | kilograms. 0 means the shape's volume at water's density |
| `Friction`, `Restitution` | Jolt combines friction by geometric mean and restitution by maximum |
| `LinearDamping`, `AngularDamping`, `GravityFactor` | |
| `Continuous` | swept collision, so small fast bodies do not tunnel |
| `Sensor` | reports `SensorEnter` / `SensorExit` and never collides |
| `AllowSleeping` | |
| `Layer`, `Mask` | 15 bits. Two bodies collide only if each one's Layer is in the other's Mask, and at least one of them moves |
| `InitialLinearVelocity`, `InitialAngularVelocity` | units/s and degrees/s, at creation |

**Methods only record a request; the next step applies it and clears it:**

- `AddImpulse` and `AddAngularImpulse`
- `AddForce` and `AddTorque`, which last **one frame**
- `SetLinearVelocity` and `SetAngularVelocity`

`LinearVelocity`, `AngularVelocity` and `IsAwake()` are written back after
every step. All of these are bound for Lua and script graphs, plus
`GetMotion` / `SetMotion("Dynamic" | "Kinematic" | "Static")`.

**The Transform:**

- **Dynamic body:** the step writes the entity's world pose. To teleport the
  body, set its Transform yourself. The step sees it differs from what it
  last wrote and moves the body there.
- **Kinematic body:** it is moved to its Transform over each frame's steps, so
  it pushes what is in its way.
- **Static body:** placed where its Transform is, and re-placed only if the
  Transform is moved.

### Physics Constraint (`GameCore/Components/PhysicsConstraintComponent.h`)

`Type` is one of:

| Type | What it does |
|---|---|
| **Fixed** | welded |
| **Point** | a ball joint at `Pivot` |
| **Hinge** | turns about `Axis` through `Pivot`. The limits are in degrees |
| **Slider** | slides along `Axis`. The limits are in units |
| **Distance** | a rope from this body's origin to `Pivot`. The limits are min and max lengths; 0/0 keeps today's length |

The other end is chosen in this order:

1. `Target`, an EntityID set from code or saved with the level as a GUID.
2. Otherwise the entity named `TargetName`, which is how the editor sets it.
3. Otherwise the world: the joint pins the body where it stands.

`Pivot` and `Axis` are in this entity's space, and are taken where the bodies
stand when the joint is made. `LimitMin == LimitMax` means no limit. `Motor`
and `MotorSpeed` drive a Hinge in degrees/s or a Slider in units/s.

The joint is made once both ends have bodies. It is remade if a field changes
or either body is remade.

### Vehicle (`GameCore/Components/VehicleComponent.h`)

A wheeled vehicle on Jolt's `VehicleConstraint` and `WheeledVehicleController`.
It has:
- suspension that follows the ground over bumps, rocks and ramps
- tyres with grip
- an engine with an automatic gearbox
- differentials and anti-roll bars

| Part | |
|---|---|
| **Chassis** | this entity's **Dynamic** Rigid Body, usually shaped by a Bounds box. Up is +Y and forward is +Z. Keep it unscaled: the wheels' positions are read in its space |
| **Wheels** | four other entities, best this entity's children, in the order front-left, front-right, rear-left, rear-right. `Wheels[]` names them; an unset slot is the child named `WheelFL`, `WheelFR`, `WheelRL` or `WheelRR`. They have no Rigid Body: Jolt casts a cylinder per wheel against the world |
| **Wheel size** | the wheel's Render mesh bounds through its rotation and scale. Width is along the chassis' X and radius comes from the other two axes. `WheelRadius` / `WheelWidth` override it |
| **Wheel position** | where the wheel was placed is its centre half way through its travel (`SuspensionMinLength`..`MaxLength`) |
| **Drive** | rear, front or all wheels. The front wheels steer (`MaxSteerAngle`); the rear take the handbrake |
| **Tuning** | `EngineTorque`, `MaxRPM`, `BrakeTorque`, `HandBrakeTorque`, `SuspensionFrequency` / `Damping`, `AntiRollStiffness`, and `MaxPitchRollAngle` (how far it may tip) |

**Driving:**
- **Input:** set `Forward` (−1..1), `Right` (−1..1; +1 toward the chassis'
  +X), `Brake` and `HandBrake` (0..1), or call `SetInput` for all four.
- **Reversing:** asking for the other direction while still moving the first
  way brakes first.
- **Readouts:** `Speed` (units/s along forward), `EngineRPM`, `Gear` and
  `WheelsOnGround` are written back each frame.
- **Wheels:** each wheel's Transform is written each frame (suspension,
  steering, spin), relative to the chassis, so the wheels move with its
  interpolated pose.
- **Rest pose:** each wheel's placement is read once, when the vehicle is first
  made. The step owns it from then on.

**Rubble:** a heavy vehicle ploughing light loose bodies onto the next
obstacle can wedge them under its chassis and get stuck. This is a known
limit of Jolt's iterative solver. Keep the chassis' collision low between
the wheels (it then shoves rather than rides up), and give scattered debris
room.

LightLab's buggy (`Content/Prefabs/Vehicle.nmprefab`) is the example, built
only from engine meshes.

---

## Shapes

The shape is inferred from the entity, and the world scale is applied:

| `Shape` | From |
|---|---|
| **Auto**, on an engine primitive mesh (`Engine/Content/Meshes/Box`, `Sphere`, `Capsule`, `Cylinder`, `Plane`, `Quad`) | that primitive, fitted to the mesh bounds. A non-uniformly scaled sphere, capsule or cylinder uses its hull instead |
| **Auto**, on any other mesh | its **triangles** when Static or Kinematic, its **convex hull** when Dynamic |
| **Auto**, with no Render mesh | a **box**: the `BoundsComponent`'s |
| **Box**, **Sphere**, **Capsule**, **Cylinder** | fitted to the box `ShapeSource` names. *Auto* uses the mesh's bounds if there is a mesh, else the Bounds |
| **ConvexHull**, **TriangleMesh** | the Render mesh's vertices or triangles. A Dynamic triangle mesh becomes its hull, with a warning |
| **HeightField** (and **Auto**, on an entity with a Terrain) | the Terrain's samples as a Jolt `HeightFieldShape`, on the same cell split the terrain draws with. Static or Kinematic only; rebuilt when the terrain is edited (its revision is in the signature). See `Engine/Source/TERRAIN.md` |

- **Off-centre boxes:** a box that is not centred on the entity's origin is
  offset to match.
- **Flat shapes:** a flat box (the Plane mesh) is thickened below its
  surface, so things resting on it stay on top.
- **Where the mesh data comes from:** GameCore's `CollisionMeshProvider`, the
  same one CS_Mesh colliders use. It supplies bounds, and now triangles
  (`GetMeshGeometry`), which NimbleEngine serves from RenderCore's
  `MeshAssetCache`.
- **Sharing:** hulls and triangle meshes are built once per mesh, in the
  mesh's own units, and shared through Jolt `ScaledShape`s.
- **Rebuilds:** the shape is rebuilt when anything it depends on changes.
  That includes the mesh path, the Bounds, the world scale and the
  component's fields. The body keeps its velocity across a rebuild.

---

## Units and stepping

- **Units:** Jolt works in metres. The World's units are centimetres in all
  shipped content, so `[Physics] UnitsPerMeter=100`.
- **Gravity:** `[Physics] GravityY=-981`, in units/s².
- **Penetration slop:** set to 0.5 cm. Jolt's default of 2 cm shows at this
  scale.
- **Fixed steps:** `[Physics] FixedHz=60` steps a second, at most
  `MaxSubSteps=4` in one frame. A longer frame drops the backlog: the game
  slows down instead of spiralling.
- **Interpolation:** dynamic Transforms are blended between the last two
  steps (`Interpolate=true`), so motion is smooth at any frame rate.
- **Worker threads:** Jolt's job system uses `[Physics] WorkerThreads`
  threads. 0, the default, means the hardware's threads less one, at most 4.
  The pool exists only while some World is simulated.
- **Capacity:** `[Physics] MaxBodies=16384`.

**Each frame**, `Physics.Step` runs in `TP_Physics`, before the World's
spatial refit at the end of that phase:

1. **Sync.**
   - Bodies are made for new or re-enabled Rigid Bodies, and remade when
     changed.
   - Bodies whose component or entity went are removed.
   - Moved Transforms teleport their bodies; kinematic bodies are moved.
   - Recorded impulses, forces and velocities are applied.
   - Joints are made or remade.
2. **Step**, as many fixed steps as the frame holds.
3. **Write back** dynamic poses and every body's velocities.
4. **Tell** what began or stopped touching.

**Where it runs:**

- `GameSession::LoadLevel` attaches a `JoltPhysics::PhysicsWorld` to each
  level's World, before the level's `OnLoad`. `UnloadLevel` detaches it.
- So **Play-in-Editor simulates**, and the edited world (never ticked) does
  not.
- **On a network client**, dynamic bodies on networked entities follow their
  replicated Transform kinematically, because the server simulates them.
- A test or tool can attach its own: `PhysicsWorld::Attach(World)`. Outside
  this module, `NimbleEngine::StartSimulating(World)` / `StopSimulating` do
  it. The prefab editor's Simulate uses them, on a ground of its own when
  the prefab has Rigid Bodies (GameCore's PREFABS.md).

---

## stat physics

`stat physics` toggles `[Stats] Physics`, in the editor's console or the
game's. It adds a Physics section to the stats panel over the viewport while
a level runs:

- **Bodies:** the total, how many are awake, and how many are Dynamic,
  Kinematic, Static and sensors.
- **Joints**, and **Contacts** (body pairs touching).
- **Steps:** fixed steps taken last frame.
- **Step:** milliseconds in Jolt's steps last frame.
- **Sync:** milliseconds making, moving and pushing bodies before them.

`GameSession` publishes the section from `PhysicsWorld::GetStats` through
`Renderer::SetStatText`. With the module compiled out it says so. The edited
world is not simulated, so in the editor the section shows during Play. Set
`[Stats] Physics=true` in a game's ini to start with it on.

---

## Contacts and sensors

When two bodies begin or stop touching, the World's `EventBus` emits
`ContactBegin` / `ContactEnd` with `(Self, Other)`, once for each side. The
prefab scripts' `OnContactBegin(self, other, mine)` / `OnContactEnd` run too.

A **sensor** emits `SensorEnter` / `SensorExit` with `(Sensor, Other)`, and the
sensor's script gets `OnSensorEnter` / `OnSensorExit`.

**What counts as an end:**

- Bodies falling asleep while touching is not an end. Jolt drops a sleeping
  pair's contacts, and this module keeps the pair touching.
- Destroying a body ends its pairs at once. Only the survivor hears it.

```cpp
World.GetEvents().Subscribe("SensorEnter", &OnPad, this);   // Args[0] the sensor, Args[1] what entered
```

```lua
Events.On("ContactBegin", function(self, other) print(self:GetName() .. " hit " .. other:GetName()) end)
local hit, entity, x, y, z = Physics.Raycast(ox, oy, oz, dx, dy, dz, 5000)
```

---

## Files

```
Engine/Modules/JoltPhysics/
|-- CMakeLists.txt                    the Jolt library (glob of the vendored sources) and the module
|-- External/JoltPhysics-5.6.0/
|   |-- LICENSE                       MIT
|   `-- Jolt/                         only what Jolt.cmake compiles with every option off
`-- Source/
    |-- Public/JoltPhysics/
    |   |-- JoltPhysicsAPI.h          JOLTPHYSICS_API; #error without WITH_JOLTPHYSICS
    |   `-- PhysicsWorld.h            PhysicsWorld, PhysicsSettings, PhysicsHit -- no Jolt headers
    `-- Private/JoltPhysics/
        |-- JoltRuntime.*             process-wide Jolt: allocator, factory, types, job system, logging
        |-- JoltLayers.h              Layer/Mask packed into Jolt object layers, and the filters
        |-- JoltConvert.h             units <-> metres, Euler degrees <-> quaternions
        |-- JoltShapes.*              shape inference
        |-- PhysicsSettings.cpp       [Physics] config
        `-- PhysicsWorld.cpp          sync, step, write back, contacts, raycasts
```

### The vendored Jolt

`External/JoltPhysics-5.6.0` holds the release's `LICENSE` and the library
sources that `Jolt/Jolt.cmake` compiles with every option off. It has none of
Jolt's samples, tests, viewer, GPU compute backends, shaders or object-stream
I/O. Two headers that sources include unconditionally are kept too:
`ObjectStream/ObjectStreamTypes.h`, and `Shaders/HairStructs.h` with its
`ShaderCore.h`.

**Build flags:**

- **Shared defines:** `JPH_OBJECT_LAYER_BITS=32` and, in Debug,
  `JPH_ENABLE_ASSERTS`. They are PUBLIC on the `Jolt` target, so every
  translation unit agrees with the library.
- **SIMD:** Jolt takes EngineCore's SIMD flags (`-mavx2 -mfma` /
  `/arch:AVX2`). With GCC and Clang it also needs `-mbmi -mlzcnt -mf16c`,
  because Jolt treats AVX2 as implying them.
- **Backstop:** `JPH::VerifyJoltVersionID()` refuses to start a mismatched
  build.

**Bumping Jolt:**

1. Extract the same file list from the new release's `Jolt/Jolt.cmake`.
2. Keep any new header that a source includes unconditionally.
3. Rename the directory and change `JOLTPHYSICS_JOLT_DIR`.

---

## What is not here yet

- Character controllers (`CharacterVirtual`), tracked vehicles and motorcycles, soft bodies.
- Powered ragdolls. Ragdolls themselves are here (`PhysicsRagdoll.h`, `PhysicsWorld::CreateRagdoll`),
  driven by NimbleEngine's Ragdoll component (`Engine/Source/RAGDOLL.md`): kinematic hit boxes,
  then simulated; nothing drives one with motors (`Ragdoll::DriveToPoseUsingMotors`).
- Debug drawing of shapes and contacts.
- Saving simulation state. A saved game stores the components; after a load,
  bodies start from their Transforms as if new (with their initial
  velocities), and contacts begin again.
- Per-world gravity (`WorldSettings`). Gravity is `[Physics] GravityY` for now.

---

## Tests

- `Engine/Tests/PhysicsTests.cpp` (`NimbleEngine.PhysicsTests`, smoke) is
  built with the module. It covers:
  - falling and resting, stacking and sleeping
  - impulses, forces and velocities
  - kinematic and teleported bodies
  - Point and motorised Hinge joints
  - removal, enabling and disabling
  - shapes from the Box and Sphere meshes, hulls and triangle meshes
  - layers
  - contact and sensor events, including no false end on sleep
  - raycasts
  - a vehicle that settles on its suspension, drives, steers toward +X,
    brakes into reverse, and rides over a bump upright
- `Engine/Tests/GameFrameworkTests.cpp` builds in every configure. It checks
  that a session simulates a level only with the module, and that the
  inactive-module warning fires exactly when it should.

LightLab's **PhysicsLab** level is the visual check: `LightLabPhysics.cpp`
and `Content/Levels/PhysicsLab.nmworld`.
