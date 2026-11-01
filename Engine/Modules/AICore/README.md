# AICore

Game AI for a GameCore `World`, in four parts:

- **navigation**: a tiled navmesh baked from the level, with paths, queries
  and moving obstacles (Recast and Detour);
- **crowds**: agents that steer along their paths around each other
  (DetourCrowd);
- **behavior trees**, with **utility** scoring for the choices that are not
  yes-or-no;
- **perception**: sight and hearing, remembered for a while, written to the
  brain's blackboard.

How it is built, and where to change it:
`Engine/Docs/Guides/AICoreImplementationGuide.md`. One page per component:
`Engine/Docs/Components/` (AI Brain, Nav Agent, Nav Mesh Volume, Nav
Modifier, Nav Obstacle, Perception, AI Stimulus).

Each is a set of components a level places, or a game's spawn code adds. The
World's `AIWorld` runs them. The editor builds navmeshes, edits trees and
debugs agents while Play runs. The Toon Arena (`Games/WorldsLab`, below) is
the demo and the integration test.

---

## Position in the tree

```
Host exe --> NimbleEngine --> AICore --> GameCore --> EngineCore
                                  `----> RecastNav (PRIVATE; Engine/ThirdParty)
```

`NimbleEngine` links it **PRIVATE**. A game reaches AI without an AICore
header in three ways:

- the **components** below, placed in the level or a prefab;
- the `AI` script library;
- `NimbleEngine/AIFacade.h`: `MakeAIBot`, `SteerInput`, `ReportAINoise`,
  blackboard access, `BuildNavigation`.

A game that defines its own behavior tree nodes links AICore itself (as
WorldsLab does under `if(TARGET AICore)`). `NIMBLE_WITH_AICORE=OFF` drops the
module, and `WITH_AICORE` is then 0: the facade's functions return false.

Recast Navigation 1.6.0 (zlib) is vendored at
`Engine/ThirdParty/Source/recastnavigation-1.6.0` and prebuilt with the other
third-party libraries. `DT_VIRTUAL_QUERYFILTER` is a PUBLIC define of
`RecastNav::RecastNav`, because it changes `dtQueryFilter`'s layout. No other
module links RecastNav: the editor draws through `NavDebugDraw.h`.

---

## Where it runs

`GameSession` attaches an `AIWorld` to the level's World on every authority:
standalone, listen server, dedicated server. A client has none and sees the
results replicated, like any other entity's movement. It adds three systems:

| System          | Phase          | Does                                                    |
|-----------------|----------------|---------------------------------------------------------|
| `AI.Brains`     | TP_PrePhysics  | ticks every AI Brain's tree                             |
| `AI.Nav`        | TP_PrePhysics  | obstacles into the tile cache, then the crowd's step    |
| `AI.Perception` | TP_PostPhysics | staggered sight checks, hearing, memory, the blackboard |

The engine supplies the host callbacks. **LineOfSight** is a physics raycast
that steps out of the viewer's own capsules. **GroundHeight** puts agents on
the floor, skipping every agent's colliders. The navmesh has no detail mesh,
so its heights are approximate. **SetAnimParam** reaches RenderCore's
Animation component.

---

## Navigation

### The volume

A **Nav Mesh Volume** marks what to build. It holds the agent's size (Agent
Radius, Height, Max Climb, Max Slope), the voxel and tile sizes, and the
physics **Layer Mask** of what counts as ground. One volume per level is
used, the first found.

**Nav Mesh** names the baked file (`.nmnav`). A level loads it on its first
query. **Build At Runtime** builds one when there is no file, or when the
file is older than the level's geometry. **Rebuild On Change**: the editor
rebuilds the tiles under static geometry as it moves, and a running level
rebuilds the tiles under a terrain that changes.

The build reads the static rigid bodies in the volume (their shapes, or
their meshes' triangles) and terrain heightfields. It voxelises them per tile
into DetourTileCache layers, so tiles can be rebuilt one at a time when an
obstacle moves.

### Areas and costs

A **Nav Modifier** box marks the polygons inside it with an area:

- **Blocked**: a hole, where nothing walks;
- **High Cost**: walkable, but **Cost** times dearer, so paths go round. The
  Toon Arena's mud costs 6×;
- **Custom**: area **Custom Area** (2..62), at **Cost**, for a game's own
  filters.

Area colours show in the editor overlay.

### Obstacles

A **Nav Obstacle** (a box turned with its entity's yaw, or an upright
cylinder) cuts the navmesh while **Enabled** is on. Moving it or switching it
re-cuts the tiles it touches on the next `AI.Nav` tick. The Toon Arena's
doors open and shut this way every 12 s.

### Queries

`NavQuery`: `FindPath` (string-pulled corners), `ProjectPoint`,
`RandomPointInRadius` and `Raycast`. Script: `AI.FindPath`, `AI.RandomPoint`,
`AI.ProjectPoint`. Facade: `RandomNavPoint`, `NavPathLength`.

---

## Agents

A **Nav Agent** is a crowd agent. **Max Speed**, **Max Acceleration** and
**Separation Weight** shape how it steers, and **Acceptance Radius** sets
when it has arrived. It writes **DesiredVelocity** every tick.

- **Drive Transform** on: the agent moves its own entity, turning to face
  along its path at **Turn Rate**. **Mesh Yaw** is 180 for a mesh modelled
  facing −Z.
- **Drive Transform** off: a character controller moves it. A game feeds the
  avatar `SteerInput(World, Avatar, Map, Yaw)` each tick, which turns the
  desired velocity into the avatar's move axes. This is how the CrashDummy
  and PvP bots walk, with the same physics as a player.

`AI.MoveTo(Agent, X, Y, Z)` / `MoveAIAgent` gives it a target, and
`AI.Stop` clears it. Its **Status** is idle, moving, arrived or failed.

---

## Behavior trees

### The asset: `.nmbt`

The XML is the authored form. **Build > Cook Content** writes the binary
form ("NMBT", chunked), the only form a Shipping build reads. The first bytes
tell the two apart.

```xml
<BehaviorTree Format="1" Name="Guard">
  <Blackboard>
    <Key Name="TargetActor" Type="Entity" />
    <Key Name="TargetVisible" Type="Bool" Default="false" />
    <Key Name="LastKnownPos" Type="Vector" />
  </Blackboard>
  <Selector>
    <Sequence Name="Chase">
      <Decorator Type="BlackboardCondition" Key="TargetVisible" Op="IsSet" Abort="Both" />
      <Task Type="MoveTo" Key="TargetActor" AcceptanceRadius="150" />
    </Sequence>
    <Sequence Name="Search">
      <Decorator Type="BlackboardCondition" Key="LastKnownPos" Op="IsSet" />
      <MoveTo Key="LastKnownPos" />
      <Task Type="ClearBlackboard" Key="LastKnownPos" />
    </Sequence>
    <Task Type="MoveToRandom" Radius="1200" />
  </Selector>
</BehaviorTree>
```

A task may be written by its own name (`<MoveTo .../>`). Every attribute
that is not Type, Name or Weight is a param. A param naming a declared key is
resolved to it at load.

Key types: Bool, Int, Float, Vector, Entity, Name.

### Running

The asset is immutable and shared. Each brain has a `BTInstance`: a flat
array of node memory and its blackboard, so ticking allocates nothing. Only
the running branch is ticked. A finished root starts again on the next
tick.

| Composites      |                                                                       |
|-----------------|-----------------------------------------------------------------------|
| Sequence        | each child in order; fails with the first that fails                  |
| Selector        | the first child that succeeds (a "Fallback"); priority by order       |
| Parallel        | every child each tick; `Success` All/One, `Failure` One/All           |
| RandomSelector  | a weighted pick, then the rest (the child's **Weight**)               |
| UtilitySelector | the best-scoring child by its considerations (below)                  |

| Tasks             | Params                                                |
|-------------------|-------------------------------------------------------|
| Wait              | Seconds, RandomDeviation                              |
| SetBlackboard     | Key, Value or From                                    |
| ClearBlackboard   | Key                                                   |
| MoveTo            | Key (Vector or Entity), AcceptanceRadius, RepathDistance |
| MoveToRandom      | Origin, Radius, Out                                   |
| FindCover         | Threat, Radius, MinDistance, Samples, Out             |
| FaceTarget        | Key, Tolerance, TimeLimit                             |
| PlayAnimParam     | Param, Value                                          |
| RunScript         | Method, Arg (the entity's Lua script)                 |
| Log               | Text                                                  |

| Decorators          |                                                                        |
|---------------------|------------------------------------------------------------------------|
| BlackboardCondition | Key, Op (IsSet, Equals, NotEquals, Less, LessOrEqual, Greater, GreaterOrEqual), Value |
| Cooldown            | Seconds before its node may run again                                  |
| TimeLimit           | Seconds, then its node is aborted                                      |
| Loop                | Count times (0: forever)                                               |
| Inverter, ForceSuccess, ForceFailure | change the result                                     |

| Services       |                                                         |
|----------------|---------------------------------------------------------|
| DistanceToKey  | Key, Out: the distance to an Entity or Vector key       |

A service runs every **Interval** seconds while its composite is active.

**Aborts** (Unreal's observer aborts). A BlackboardCondition with
**Abort Self** stops its node when the condition turns false while it runs.
With **Abort LowerPriority**, a Selector running a later child switches back
to this one when the condition turns true. **Both** does both. They re-check
only when the blackboard's revision moved.

### Utility

A UtilitySelector's children carry **considerations**. Each reads a key or
`distance(Key)` and maps **Min**..**Max** onto 0..1. A response curve shapes
it: Linear, InverseLinear, Quadratic, Logistic, Step, Constant, or Points
(piecewise-linear). The result is raised to the consideration's **Weight**.

A child's score is the product of its considerations, with Dave Mark's
compensation factor so many considerations don't punish it, times the
child's Weight. A child whose condition decorators fail scores 0. The
selector re-scores every **Interval** (0.3 s). It switches only when another
child beats the running one by more than **Hysteresis** (0.15).
**Pick**=Weighted draws among the **TopN** best instead of taking the best.

### A game's own nodes

```cpp
class AimAndFire final : public AICore::BTTask
{
public:
    AICore::EBTStatus OnEnter(AICore::BTContext& C) const override { ... return AICore::EBTStatus::Running; }
    AICore::EBTStatus OnTick(AICore::BTContext& C) const override  { ... }
};
AICORE_DEFINE_BT_TASK(AimAndFire, "Aim and Fire")
```

Node objects are stateless and `const`. What they keep goes in
`C.Memory` (BTNodeMemory: a status, a child, a counter, times, four floats, a
vector). `AICORE_DEFINE_BT_DECORATOR` and `AICORE_DEFINE_BT_SERVICE` do the
same for the other kinds. A registration lasts as long as its module stays
loaded. A tree naming a type no module registered fails to load, naming the
type. `Games/WorldsLab/WorldsLabArena.cpp` has four: AimAndFire, Reload,
PickPatrolPoint, ArenaStatus.

### Brains

An **AI Brain** runs **Tree**. **Blackboard** holds starting values
(`Key=Value; ...`). **Tick Interval** of 0 means every frame. **Auto Start**
starts it with the level. Script: `AI.StartBrain`, `AI.StopBrain`,
`AI.SetKey`, `AI.SetKeyEntity`, `AI.SetKeyVector`. Facade: `GetAIKeyNumber`,
`SetAIKey`, `SetAIKeyEntity`, `GetAIKeyEntity`.

Trees load through `BehaviorTreeAssetCache`. When the editor saves a tree,
the cache's generation moves, and running brains restart on the new tree on
their next tick.

---

## Perception

A **Perception** component sees within **Sight Range** and **FOV Half
Angle** from **Eye Height**, and hears within **Hearing Range**. An **AI
Stimulus** makes an entity perceivable; **Target Height** is where others
look at it. Both carry a **Team**: an entity perceives the stimuli of other
non-zero teams. Sight checks are staggered over **Sight Interval**, and each
one is a host LineOfSight ray.

`AI.ReportNoise(X, Y, Z, Loudness, Instigator)` / `ReportAINoise` is a noise.
The Toon Arena's gunfire is one.

What is perceived is remembered for **Forget Seconds**, and the nearest is
written to the brain's blackboard if it declares these keys:

| Key            | Type   |                                         |
|----------------|--------|-----------------------------------------|
| TargetActor    | Entity | the nearest perceived enemy             |
| TargetVisible  | Bool   | seen now                                |
| TargetDistance | Float  | cm                                      |
| LastKnownPos   | Vector | where it was last seen or heard         |
| HeardNoise     | Bool   | a noise not yet investigated            |
| NoisePos       | Vector | where                                   |

---

## In the editor

**World > Navigation** is the Navigation dock:

- the volume's status (none, not built, up to date, out of date, building)
  and the last build's tiles, polygons, time and size;
- **Build navigation**, which writes the volume's Nav Mesh file (beside the
  level as `<level>.nmnav` if it names none);
- **Rebuild on change**: an edit rebuilds 0.7 s later, in memory, and the
  next level save writes it;
- the overlay toggles;
- the **path tester**: click a start and an end in the viewport (Shift+click
  moves the end), and the path, its corners and length show;
- **Add** a volume over the scene, a modifier, or an obstacle in front of the
  camera.

**Show > Editor Gizmos > AI** in the viewport holds what the overlay draws,
remembered in `[Editor] ViewAI*`:

| Gizmo                    | Draws                                               | Default |
|--------------------------|-----------------------------------------------------|---------|
| Nav Mesh                 | polygon edges in their area colours, the boundary   | off     |
| Nav Tile Bounds          | each tile's box                                     | off     |
| Nav Obstacles            | each obstacle: red while it blocks, amber while open | on     |
| Modifiers and Volumes    | modifier boxes in their area colours, the volume    | on      |
| Agent Paths              | each agent's corridor (the debugged one brighter)   | on      |
| Sight and Perception     | sight cones; green to what is seen, amber to what is remembered | on |

The agent gizmos also draw while Play shows the game's camera. The dock
checkboxes and the menu stay in step.

**World > AI Debugger** lists every running brain while Play runs. Picking
one selects it in the Outliner and narrows the sight gizmo to it. For the
picked brain it shows:

- the running branch, root first, with seconds active, and each utility
  child's score;
- the blackboard;
- perception memory;
- navigation status, speed, target and corners.

Double-click an agent to open its tree in the behavior tree editor, debugging
it.

**The behavior tree editor** opens on double-clicking an `.nmbt` in the
Content Browser. **New Behavior Tree...** is in the Content Browser's menu,
and an AI Brain's Tree field has **Edit**, which makes a tree if the field is
empty.

- The left pane holds the blackboard (key, type, default) and the palette:
  composites, then every registered task, decorator and service.
  Double-click or Insert adds the pick under the selected node (after it,
  for a task). A decorator or service goes onto the node.
- The centre pane is the tree, top-down. Each node shows its order among its
  siblings, its params, decorators (◇), services (⟳) and considerations
  (∿). Nodes of types this editor has not registered are drawn red.
- The right pane edits the selected node: name, weight, params, decorators
  with abort modes, services, and considerations, with the selected one's
  response curve drawn.
- Move Up/Down (Alt+↑/↓) reorder siblings. Cut, Copy, Paste and Duplicate
  move whole branches. Every edit can be undone.
- The status line says whether the tree resolves, or names the first
  problem.
- **Debug in PIE** lights the running branch of an agent running this tree
  (the debugger's pick if it runs it) and shows utility scores live.
- **Save** writes the XML, restages it, and running brains restart on it.

Console commands, also usable from `-command`:

| Command                 |                                                     |
|-------------------------|-----------------------------------------------------|
| `ai.navigation`         | show the Navigation dock                            |
| `ai.buildnav`           | build the level's navmesh and write it              |
| `ai.debugger [row]`     | show the AI Debugger; with a row, pick that brain   |
| `ai.opentree <file> [debug] [node]` | open a tree in its editor, a node selected |
| `ai.testpath ax ay az bx by bz` | set the path tester's ends (`clear` clears) |
| `ai.show <gizmo> <on/off>` | navmesh, tiles, obstacles, modifiers, paths, sight |
| `ai.overlaystats`       | the lines each gizmo drew last frame                |
| `ai.treeshot <file.png>`| save the front tree window as an image              |
| `screenshot -gizmos <file>` | the viewport with its gizmos (`windowshot -gizmos`: the window) |

A running game has `AI.ShowNavMesh` and `AI.ShowPaths` (config vars).

---

## Cooking and staging

**Build > Cook Content** has two AICore steps:

- **Navmeshes** loads every `.nmnav` to check it, then copies it to
  `Saved/Cooked`. It is already binary.
- **Behavior trees** resolves every `.nmbt` against the registered types,
  then writes the binary form. A tree naming a type that no module
  registers fails the cook, not the shipped game.

A game's CMake stages `.nmnav` with its Content. It stages `.nmbt` the way it
stages sequences: XML in every build but Shipping, the cooked trees in
Shipping and packaged builds. The game template does both.

---

## The Toon Arena

`Games/WorldsLab`, level ToonArena, game mode `WorldsLabArena`. Three Red
CrashDummy bots fight three Blue across a 60 m arena that tests the
navigation features:

- ramps and steps to a raised plaza, plus a slope too steep to climb;
- a bridge over a gap, and a mud patch (a Nav Modifier) that paths avoid;
- low and high cover walls, and a pillar maze for crowd avoidance;
- two doors that are Nav Obstacles.

`Content/AI/ArenaBot.nmbt` is a UtilitySelector over Engage, TakeCover,
Reload, Investigate and Patrol. Tab follows each bot in turn.

`WorldsLab -Game.StartLevel=ToonArena -WorldsLab.ArenaAutoplay=90` plays
spectating, then logs `[ToonArena]` stats and PASS or FAIL. To pass there
must be kills on both teams, no stuck agents, no ground bobbing, and every
branch must have run. The level and its Toony content are generated by
`Tools/Toony/` (`make_toon_arena.py`, with the textures and materials
scripts).

Demo Lab's Watchmen and Brutes (`Games/DemoLab/Source/AI`, brains in
`Games/DemoLab/Content/AI/*.nmbt`) are AI Brains too, on the navmeshes its
levels build at runtime (`Games/DemoLab/Docs/Guide.md`).

---

## Tests

`AICore.Tests` (`Tests/AICoreTests.cpp`, 154 checks) covers:

- blackboards, every composite, decorator and abort mode, and services;
- utility curves and selection, with hysteresis;
- the XML and binary round-trips;
- navmesh builds of `Tests/Data/ToonArena.obj`: a path base to base, around
  the mud, none up the steep slope, across the bridge;
- obstacles cutting and re-opening, crowds reaching distinct targets;
- perception.

The Toon Arena autoplay is the integration test.
