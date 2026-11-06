# AICore guide: a guard that patrols, chases and searches

This walks through one AI, from an empty level to a cooked build: a guard
that walks between random points, chases the player on sight, and searches
where it last saw them. README.md is the reference for every part.

---

## 1. A navmesh

1. Open the level. **World > Navigation** opens the dock.
2. **Add volume** puts a Nav Mesh Volume over the scene. In the Details
   panel, set its **Agent Radius** and **Agent Height** to your character's
   capsule (35 and 180 cm suit the CrashDummy). **Agent Max Climb** is the
   tallest step it walks up, and **Agent Max Slope** the steepest ramp.
3. Press **Build navigation**. The status line gives the tiles, polygons and
   build time. The file goes beside the level (`<Level>.nmnav`), and the
   volume's **Nav Mesh** field now names it.
4. Turn on **Show > Editor Gizmos > AI > Nav Mesh** in the viewport (or
   *Show navmesh* in the dock). Teal outlines are walkable polygons; the
   darker thick edges are the boundary.

Check it with the **Path tester**: tick it in the dock, click a start, then
click an end (Shift+click moves the end). A yellow path means a full path;
orange means it stopped short. The dock shows its corners and length.

If a floor is missing, its collider is not static, or it is outside the
volume's **Layer Mask**. If a doorway is closed, Agent Radius is too large
for it.

**Rebuild on change** rebuilds a moment after each edit while you lay the
level out. Save the level to write the rebuilt file.

### Areas and doors

- A puddle the guard should avoid: **Add modifier**, size its box over the
  puddle, set **Area** to High Cost and **Cost** to 5. Paths now go round
  unless the detour is more than five times as long.
- A door: put a **Nav Obstacle** on the door entity, sized to the doorway.
  While **Enabled** is on, the doorway is cut from the navmesh. A script
  that opens the door sets Enabled off, and paths go through again on the
  next tick.

---

## 2. The guard

Make the guard a prefab, or place it in the level, with:

| Component      | Set                                                          |
|----------------|--------------------------------------------------------------|
| Nav Agent      | Max Speed 350. **Drive Transform** on, unless a character controller moves it (below) |
| Perception     | Sight Range 2500, FOV Half Angle 60, Eye Height 160, **Team** 2 |
| AI Stimulus    | Team 2, so other teams can see it                            |
| AI Brain       | **Tree**: leave empty for now                                |

Give the player an **AI Stimulus** with Team 1. An entity perceives the
stimuli of other teams; 0 is nobody's enemy.

Spawning from game code instead? `NimbleEngine/AIFacade.h`'s `MakeAIBot`
adds all four:

```cpp
NimbleEngine::AIBotSetup Setup;
Setup.Tree = "Content/AI/Guard.nmbt";
Setup.Team = 2;
Setup.MaxSpeed = 350.0f;
NimbleEngine::MakeAIBot(World, Guard, Setup);
```

**A character controller moves it.** Leave Drive Transform off. Each tick,
turn the agent's desired velocity into the avatar's input, as a player's
would be:

```cpp
NimbleEngine::InputSnapshot Input;
const float YawLeft = NimbleEngine::SteerInput(World, Guard, Map, CurrentYaw, DeltaSeconds, Input);
// ...hand Input to the guard's avatar like a player's input
```

The guard then walks with the same physics, stairs and slopes as a player.

---

## 3. The tree

In the AI Brain's **Tree** field, press **Edit**. With the field empty, it
asks for a name and makes `Content/AI/<Name>.nmbt`: a Selector with a Wait.
The behavior tree editor opens.

**The blackboard.** Press **+ Key** three times:

| Key           | Type   |
|---------------|--------|
| TargetActor   | Entity |
| TargetVisible | Bool   |
| LastKnownPos  | Vector |

Perception fills these by name (README.md, "Perception").

**Chase.** Select the root Selector. In the palette, double-click
**Sequence**, and name it *Chase* in the details pane. With Chase selected:

1. Double-click **BlackboardCondition**. In the Decorators table, set its
   Params to `Key=TargetVisible; Op=IsSet` and its **Abort** to *Both*. The
   guard drops whatever it is doing when it sees the player (LowerPriority),
   and stops chasing when it loses sight (Self).
2. Double-click **Move To**. Set its Key param to `TargetActor`.

**Search.** Select the root again and add another Sequence, *Search*:

1. A BlackboardCondition with `Key=LastKnownPos; Op=IsSet`.
2. **Move To** with Key `LastKnownPos`.
3. **Wait**, Seconds 2: look around.
4. **Clear Blackboard**, Key `LastKnownPos`, so the search ends.

**Patrol.** Select the Wait the new tree started with, and press Delete. With
the root selected, add **Move To Random Point** (Radius 1500), then a Wait of
1 second.

The root's children run in order of priority: Chase, then Search, then the
patrol. The number in each node's corner is its place. Reorder with Move
Up/Down (Alt+↑/↓).

The status line reads *resolves* when the tree is complete. Otherwise it names
the first problem: a composite with no children, or a consideration reading
an undeclared key. **Save** (Ctrl+S).

---

## 4. Watch it think

Press Play. **World > AI Debugger** lists the running brains. Pick the guard
to see:

- the running branch, with how long each node has run;
- the blackboard: TargetVisible flips as you step in and out of view;
- what it remembers, and where;
- its path status.

In the viewport, the **Agent Paths** gizmo draws its route, and **Sight and
Perception** draws its cone, with a green line to you while it sees you and
an amber one to where it remembers you.

Double-click the guard in the debugger. Its tree opens with **Debug in PIE**
on, the running branch outlined blue. Edit the tree and save while Play
runs: the guard restarts on the new tree on its next tick.

---

## 5. Choices by score

A guard with more than one good option (fight, take cover, reload) is easier
to write with a **UtilitySelector** than with conditions. Each child is
scored by its **considerations**, and the best one runs:

1. Add keys `Health` (Float, default 1) and `TargetDistance` (Float).
2. Replace the root with a UtilitySelector (add it, Cut the old branches,
   Paste them under it).
3. Select Chase. In **Considerations**, press + and set:
   - Input `TargetVisible`, Curve **Step**, XShift 0.5: 1 while it sees the
     player, else 0;
   - Input `Health`, Curve **Logistic**, Slope 12, XShift 0.3: falls away
     below 30% health.

   The curve preview under the table shows the selected consideration's
   shape.
4. Give the patrol branch one consideration with Curve **Constant**, YShift
   0.2: a floor score, so it runs when nothing else scores higher.

The selector re-scores every 0.3 s, and switches only when another child
beats the running one by 0.15 or more. While Debugging in PIE, each child
shows its live score.

`Content/AI/ArenaBot.nmbt` in WorldsLab is a full example with five
branches.

---

## 6. A node of your own

A task the built-in nodes don't cover goes in the game module:

```cpp
#include "Public/AICore/BehaviorTree.h"

/* Shouts once, then succeeds. */
class Shout final : public AICore::BTTask
{
public:
    AICore::EBTStatus OnEnter(AICore::BTContext& C) const override
    {
        NimbleEngine::ReportAINoise(C.World, C.World.GetEntity(C.Self).GetWorldPose().Position,
                                    float(C.GetNumber("Loudness", 1.0)), C.Self);
        return AICore::EBTStatus::Success;
    }
};
AICORE_DEFINE_BT_TASK(Shout, "Shout")
```

Link AICore from the game's CMake under `if(TARGET AICore)`. After the next
build the editor's palette lists **Shout** under Tasks. Nodes are `const`
and shared by every brain. Keep per-brain state in `C.Memory`.

---

## 7. Ship it

**Build > Cook Content** checks every `.nmnav` and cooks every `.nmbt` to
binary into `Saved/Cooked`. A tree that names a node type no module
registers fails here, with its name. A Shipping build stages the cooked
trees, so cook before packaging.
