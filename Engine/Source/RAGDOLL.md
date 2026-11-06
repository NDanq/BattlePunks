# Ragdolls

A skeletal mesh posed by physics: a character's death. The API is
`Public/NimbleEngine/Ragdoll.h`. The physics underneath is Jolt's ragdoll,
reached through `JoltPhysics/PhysicsRagdoll.h`.

---

## Using it

Give a character a **Ragdoll** component. The character is an entity with an
Animation component and a skinned Render mesh.

```lua
local ragdoll = entity:GetComponent("Ragdoll")
ragdoll:ActivateWithImpulse(dx * 6000, dy * 6000, dz * 6000, hitX, hitY, hitZ)  -- the killing shot
ragdoll:AddImpulse(...)    -- kick a body that is already down
ragdoll:GetPartAt(hitX, hitY, hitZ)   -- "Head", "Chest", "Hand.L"...: which limb a shot hit
ragdoll:Reset()            -- back to the animation (respawn in place)
ragdoll:IsActive(), ragdoll:IsSettled()
Events.On("RagdollSettled", function(entity) ... end)   -- also RagdollActivated
```

| Field | Default | What it does |
|---|---|---|
| Hit Boxes | on | While alive, kinematic capsules follow the animation. Raycasts hit limbs and report the entity. |
| Start Active | off | The body is already ragdolled when the level starts (a corpse placed in the level). |
| Drop Props | on | On activation, socketed children holding a **disabled** Rigid Body are let go. |
| Mass | 70 kg | The whole body, shared out by segment. |
| Thickness | 1 | Scales the limb radii. |
| Joint Friction | 1 | Scales the joint resistance: 0 is floppy, higher is stiff. |
| Friction, Linear/Angular Damping, Layer, Mask | | Applied to every part, as on a Rigid Body. |

**Props (the dummy's pistol).** Give the socketed child a Rigid Body with
`Enabled` off. While the character lives, the Anim Socket carries the prop. On
activation the ragdoll:

1. removes the socket,
2. unparents the prop, unless a prefab instance holds it (instance members
   keep their parent; the paused body no longer moves, so the prop stays where
   it falls),
3. enables the prop's body, with the velocity of the hand that held it.

**Demo.** WorldsLab's ShootingRange has target dummies
(`Content/Prefabs/ShootingRange/RangeDummy.nmprefab`):

- They die after three body shots, or one head shot. `RangeShooter.lua`
  asks the ragdoll which part was hit with `GetPartAt`.
- A dead dummy falls along the shot and drops its pistol.
- Four seconds after the body comes to rest, a fresh dummy stands in its
  place. The dummy's own logic is a script graph,
  `Scripts/ShootingRange/RangeDummy.script.nmgraph`, written with the
  Ragdoll library's On Ragdoll Settled node:
  1. On Start, it stands armed.
  2. On Ragdoll Settled, it starts the respawn clock.
  3. On Tick, once the clock passes Respawn Seconds, it calls Spawn Prefab
     and Destroy Prefab.
  Regenerate its Lua after editing outside the editor with
  `NodeGraphGenerate -module ./libRenderCore.so -module ./libNimbleEngine.so <graph>`,
  run from the binaries folder.

---

## How it works

**The profile.** `BuildRagdollProfile` reads the skeleton as a humanoid, from
bone names and the hierarchy alone:

| Part | How it is found |
|---|---|
| Thigh, shin, foot | Name contains `thigh`/`upleg`, then `shin`/`calf`/`lowerleg`/`leg`, then `foot`, each under the last. |
| Upper arm, forearm, hand | Name contains `upper_arm`/`upperarm`/`arm`, then `forearm`/`lowerarm`, then `hand`/`wrist`. |
| Side | Which side of the body the bone lies on, not its name. |
| Pelvis | The common ancestor of the two thighs. |
| Chest | The common ancestor of the two upper arms. |
| Spine | The middle bone between pelvis and chest, if there is one. |
| Head | The chest's longest child chain that is not an arm. |

- Prefixes such as `DEF-`, `mixamorig:` and `Bip01 ` are ignored.
- The shallowest match wins, so a twist segment such as `DEF-thigh.L.001` is
  never chosen.
- Bones that are not a part (fingers, twist bones, the upper neck) keep their
  animated pose under whichever part now carries them.

CrashDummy (Rigify, 73 bones) gives 16 parts: pelvis, spine, chest, head, and
on each side thigh, shin, foot, upper arm, forearm and hand.

**Shapes.**

- Limbs are capsules spanning joint to joint.
- The torso is capsules across the body: pelvis as wide as the hips, chest as
  wide as the shoulders.
- Radii are fractions of the body's height.
- Each part's body is its bone's frame, so a pose maps between the bones and
  the bodies with no offsets.

**Masses.** Dempster's segment fractions via Winter (sources below):

| Segment | Share |
|---|---|
| Pelvis | 0.142 |
| Abdomen | 0.139 |
| Thorax | 0.216 |
| Head and neck | 0.081 |
| Thigh | 0.100 |
| Shank (shin) | 0.0465 |
| Foot | 0.0145 |
| Upper arm | 0.028 |
| Forearm | 0.016 |
| Hand | 0.006 |

**Joints.**

- Knees and elbows are Jolt **hinges**, at −3° to 140° and 145°. The hinge
  axis is chosen so that positive flexion moves the foot back and the hand
  forward. A test checks the sign against Jolt.
- Everything else is a **swing-twist** cone: an elliptical swing (normal and
  plane half-angles) plus a twist range.
- Asymmetric ranges (a hip swings 100° forward but 25° back) are made by
  turning the parent's twist axis toward the middle of the range. The rest
  pose then sits off-centre inside the cone.
- The ranges are typical adult ranges of motion, narrowed so a limp body does
  not fold through itself. They live in `RagdollProfile.cpp`.

**Jolt** (`PhysicsWorld::CreateRagdoll`). The neutral `RagdollDesc` becomes a
`JPH::RagdollSettings`, built at the skeleton's rest pose so the joint limits
are measured from it. Then, following Jolt's own recipe
(`Samples/Utils/RagdollLoader.cpp`):

- `Stabilize()`
- `DisableParentChildCollisions(rest pose)`, so parts that overlap at rest
  (arms against the chest) never collide
- `CalculateConstraintPriorities()`

The parts are created **kinematic** and switched to **dynamic** on activation:

- **Kinematic.** `MoveKinematic` drives the parts to the animated pose over
  the frame's fixed steps (`SetRagdollTarget`), so they carry the animation's
  velocity.
- **Dynamic.** The switch keeps that velocity, so a running body falls
  forward.
- **Without hit boxes,** the ragdoll is made at activation and given the
  velocities of the last two animated poses.

Other details:

- Each ragdoll gets its own collision group.
- Its parts emit no contact events.
- Raycasts that hit a part report the owner entity, the ragdoll handle and the
  part index.

**Frame order.**

1. `TP_Physics`: Jolt steps. Kinematic parts move to the last frame's
   animated pose.
2. `TP_PostPhysics`, `Engine.Ragdoll.Pose`: each dynamic ragdoll's
   interpolated parts are put into the entity's model space and handed to the
   animation as `AnimInstance::SetPoseOverride` (mask = the part bones,
   weight 1).
3. `TP_PostUpdate`, `RenderCore.Animation`: evaluates the pose. At weight 1
   the layers are skipped and the unmapped bones hold their last animated
   locals. Sockets follow the result.
4. `TP_PostUpdate`, `Engine.Ragdoll` (registered after the animation, so it
   runs after it): makes ragdolls, sets hit-box targets from the pose just
   evaluated, and applies `Activate`, impulses and `Reset`. On activation it
   also pauses the Animation component, which stops root motion and notifies,
   and lets go of props.

Because the pose is converted into model space using the entity's current
transform, the mesh draws the parts where physics has them even if the entity
moves.

**Networking.** Ragdolls are local and cosmetic. Each process simulates its
own; nothing is replicated beyond what game code sends (who died, and the
shot).

---

## Not yet

- **No fallback without Jolt.** With `NIMBLE_WITH_JOLTPHYSICS=OFF`, or in a
  world that is not simulated, `Activate` logs once and the character keeps
  animating. The fallback design below needs more thought first.
- **Humanoids only.** A skeleton the profile cannot read is refused, with the
  reason logged.
  - There is no `.nmragdoll` asset or editor tab for hand-tuned shapes and
    limits yet.
  - The profile is regenerated from the skeleton and cached per skeleton.
- **No powered ragdolls.** Nothing drives a ragdoll to a pose with motors
  (`DriveToPoseUsingMotors`): no hit reactions, stumbles or blending back to
  animation.
- **No get-up.** `Reset` snaps back to the animation; nothing blends out of the
  ragdoll.
- **Uniform scale only.** A ragdoll takes the entity's largest scale axis.

---

## The fallback, when Jolt is unavailable (open)

The seam already exists. `RagdollDesc` and `RagdollTransform` are
backend-neutral (rest frames, capsules, swing-twist and hinge limits in
radians, masses), and `RagdollWorld` only needs create, target, dynamic,
pose, impulse and awake. A second backend would implement those six calls.
What is still undecided:

1. **Solver.**
   - XPBD rigid bodies (Müller et al. 2020): each part a real rigid body,
     joints as positional plus angular constraints, swing and twist limits as
     angle constraints, many substeps with one iteration each (Macklin et al.
     2019). This keeps the same profile and the same pose round-trip as Jolt.
   - Verlet particles with distance constraints (Jakobsen 2001): simpler, but
     the rigid frames have to be rebuilt from particles, limits are
     approximate, and it is "bouncy" with few iterations.
   - XPBD looks right, but it is a small rigid-body engine of its own to
     maintain.
2. **What it collides with.** Without Jolt the world has AABBs from Bounds
   components (`World::QueryBounds`, the overlap system's broad phase) and
   terrain heightfields. Is capsule-vs-AABB plus capsule-vs-heightfield enough
   for a body to land on stairs, props and slopes? Should it ignore everything
   but the ground?
3. **Self-collision.** None, beyond the joint limits, is cheapest. Limbs
   crossing the torso are then possible.
4. **Hit boxes.** Without Jolt, `PhysicsRaycast` tests Bounds. Should the
   fallback's capsules join it, so that shots hit limbs?
5. **Is a simulated fallback needed at all?** The alternative is a death
   animation (a state in the anim state, then hold), which costs nothing and
   needs no collision.
6. **Determinism and cost.** Fifteen bodies at 10–20 substeps is cheap per
   body. With many deaths in a frame, should the job system step ragdolls in
   parallel, as the animation system does?

---

## Sources

- **Jolt Physics 5.6.0** (vendored):
  - `Jolt/Physics/Ragdoll/Ragdoll.h`: `RagdollSettings::Part`/`mToParent`,
    `Stabilize`, `DisableParentChildCollisions`, `SetPose`/`GetPose`,
    `DriveToPoseUsingKinematics`.
  - `Jolt/Physics/Constraints/SwingTwistConstraint.h` and `HingeConstraint.h`.
  - `Jolt/Skeleton/Skeleton.h`.
- **Jolt samples:** `Samples/Tests/Rig/` (`KinematicRigTest`, `PoweredRigTest`,
  `RigPileTest`, `SkeletonMapperTest`) and `Samples/Utils/RagdollLoader.cpp`,
  at https://github.com/jrouwe/JoltPhysics.
- **Segment masses:** D. A. Winter, *Biomechanics and Motor Control of Human
  Movement*, 4th ed., Wiley 2009, Table 4.1 (after W. T. Dempster, *Space
  Requirements of the Seated Operator*, WADC TR 55-159, 1955).
- **Fallback:**
  - M. Müller, M. Macklin, N. Chentanez, S. Jeschke, T.-Y. Kim, "Detailed Rigid
    Body Simulation with Extended Position Based Dynamics", *Computer Graphics
    Forum* 39(8), SCA 2020, doi:10.1111/cgf.14105.
  - M. Macklin, K. Storey, M. Lu, P. Terdiman, N. Chentanez, S. Jeschke,
    M. Müller, "Small Steps in Physics Simulation", SCA 2019,
    doi:10.1145/3309486.3340247.
  - M. Macklin, M. Müller, N. Chentanez, "XPBD: Position-Based Simulation of
    Compliant Constrained Dynamics", MIG 2016.
  - T. Jakobsen, "Advanced Character Physics", GDC 2001,
    https://www.gamedeveloper.com/programming/advanced-character-physics.
- **Prior art:** ezEngine's Jolt ragdoll component
  (`Code/EnginePlugins/JoltPlugin/Components/JoltRagdollComponent.h`), for how
  another engine integrates Jolt's ragdoll.
