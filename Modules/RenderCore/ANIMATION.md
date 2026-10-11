# Animation

Skeletal meshes and their animation: assets imported from glTF, GPU
skinning, the Animation component and the world's animation system, anim
states (state machines, blend spaces, masked layers), sockets and notifies, and
the editor's tools for them.

---

## The pieces

| What | Where | File |
|---|---|---|
| Skeleton -- bones, parents first, and their reference pose | `SkeletonAsset.h` | `.nmskel` |
| Skinned mesh -- a mesh with bones and weights per vertex | `MeshAsset.h` (`MSKN` chunk) | `.nmmesh` |
| Clip -- bone tracks at a fixed rate, notifies, looping, root motion | `AnimClipAsset.h` | `.nmanim` |
| Anim state -- parameters, layers of state machines | `AnimStateAsset.h` | `.nmanimstate` |
| Pose math -- sample, blend, mask, compose | `AnimPose.h`, `AnimMath.h` | |
| One skeleton's playback | `AnimInstance.h` | |
| A world's animation | `AnimationSystem.h` | |
| Components | `Components/AnimationComponent.h`, `Components/AnimSocketComponent.h` | |
| Skinning on the GPU | `Shaders/Skinning.glsl`, `MaterialTemplate_VS.glsl` (SkinnedMesh) | |

Tests: `RenderCoreAnimationTests` (headless) and the editor's
`NimbleEditorImportTests`.

---

## Importing

Import a `.gltf` / `.glb` as for any mesh (Import Animations is on by
default). A file with skins writes, beside its meshes:

- `<Name>_Skeleton.nmskel` -- every skin's joints and their ancestors, as one
  skeleton (at most 256 bones);
- the meshes, skinned: in bind space (glTF ignores a skinned mesh's node
  transform), four bone indices and weights per vertex, the skin's inverse
  binds, and the skeleton's runtime path. Unskinned meshes combined with them
  ride their nearest bone rigidly. Skinned meshes get no lightmap UVs.
- `<Name>_<Animation>.nmanim` per animation, resampled at Animation Sample
  Rate (30 fps; step, linear and cubic glTF curves alike). A track that never
  changes is one key.

Coordinates convert as the static importer's do -- `C = diag(s, s, -s)` --
by conjugation for bones (`C M C^-1`), so skinning with converted bones lands
a converted vertex exactly where glTF puts it. A quaternion becomes
`(-x, -y, z, w)`.

Materials are not converted: a section keeps its glTF material's name. An
untextured material's base colour is baked into the vertex colours (Bake
material colours, on by default), so the mesh draws in its colours with
DefaultLit. Imported into `Engine/Content`, assets name what they refer to --
skeleton, source -- as `Engine/Content/...`.

Clips find bones by **name**, so a clip plays on any skeleton with those
names. Reimporting a clip -- or importing its file again over it -- keeps
what the editor set on it: notifies, looping, root motion and its root bone.

## Skinning

A skinned mesh draws through the `SkinnedMesh` material vertex input (every
material has it; key `0x400`). The vertex brings bone indices (location 11)
and weights (12); the palette -- each bone's model-space pose times its
inverse bind, three `vec4` rows a bone -- is GDI's skin block at binding 12
(`GDI_SKIN_BLOCK_BINDING`). Vulkan, OpenGL and D3D12 bind it; Metal draws no
indexed meshes.

Shadows come for free: casters use the material's vertex shader. A skinned
proxy never joins the static draw lists, and its bounds are spheres round
its posed bones, each as wide as the farthest vertex that bone moves.

A skinned mesh nothing poses draws in its skeleton's **reference pose**
(not bind space); its asset bounds are the reference pose's too.

## The Animation component

`"Animation"` on an entity that has a Render component with a skinned mesh:

| Field | |
|---|---|
| Animation | an `.nmanim` (played alone, looping as it says) or an `.nmanimstate` |
| Play Rate, Paused | |
| Parameters | up to 12 names and values; what an anim state reads |

Lua:

```lua
local animation = entity:GetComponent("Animation")   -- an entity with one
animation:SetFloat("Speed", 300)
animation:SetBool("Crouched", true)
animation:SetTrigger("Jump")             -- cleared when a transition uses it
local x, y, z, found = animation:GetBoneLocation("Hand_R")
local seconds = animation:GetTime()
local state = animation:GetState(0)        -- layer 0's state; IsInTransition(0) while it blends in
```

**Script graphs** get the same as nodes: Get/Add Animation, and on it Set
Float, Set Bool, Set Trigger, Get Float, Get Bool, Get Bone Location (a
Vector3), Get State, Is In Transition, Get Time and its fields; the Anim
Socket's Bone and offsets; and, from the `Animation` script library, typed
**On AnimNotify** (Entity, Name) and **On AnimEnded** (Entity, State, Layer)
events, with Emit nodes for each. The events fire for every character in the
world: check the Entity is your own with **Same Entity** (against Self or
Prefab Root) before acting -- Equal does not take entities, and in Lua compare
`entity:GetID()`, since two handles to one entity are not `==`.

It replicates what it plays and its parameters; each client animates on its
own. Poses are never sent.

## The world's animation system

`AnimationSystem` keeps an `AnimInstance` per Animation component (the
component stays POD; poses live beside it). WorldBridge attaches one to each
world it binds; it ticks in `TP_PostUpdate`, after gameplay sets parameters
and before the frame's extraction, which sends every pose to its Render
proxy as one batch.

**Servers** animate too: a headless session's renderer is the Null backend
(no GPU, no window), and binding its world attaches the system, so sockets
and `GetBoneLocation` work on a dedicated server; only the skinning, on the
GPU, is skipped.

A world that does not tick -- the editor's, outside Play -- still plays its
animations in the viewport (`[Animation] PreviewInEditor`, on by default).
That preview emits nothing and writes to no component.

**Notifies** fire as playback passes them, on the world's EventBus as
`"AnimNotify"` with (entity, name):

```lua
Events.On("AnimNotify", function(entity, name)
    if name == "Footstep" then PlaySound(entity) end
end)
```

A notify can also name a **particle system** and a **bone** (mesh viewer,
Clip > Notifies): in game, as it fires, the system plays there -- an
`AnimEffect` entity at the bone's place, emitting a moment (or its bursts)
and destroyed once its particles are gone. It is not attached: it plays
where it started.

A notify can name a **sound** too (an `.nmsound`, the same table's Sound
column): a footstep. As it fires, the sound plays once at the bone (or the
entity), in 3D. RenderCore plays no sound itself. It hands the sound and the
place to the animation system's sound sink (`AnimationSystem::SetSoundSink`),
and NimbleEngine's GameSession points the sink at the level's audio (the
Audio module's `AudioWorld::PlaySoundAt`). Without the Audio module, the
sound is ignored. LightLab's `Tools/add_footstep_notifies.py` places
footsteps where each paw lands, from the clip's poses.

A state that plays once emits **`"AnimEnded"`** (entity, state name, layer)
as it ends -- reaching its end, or leaving through an At End transition. A
lone clip that does not loop does too, its state name "".

```lua
Events.On("AnimEnded", function(entity, state, layer)
    if state == "Jump" then Land(entity) end
end)
```

**Sockets**: `"Anim Socket"` on a child entity sets its Transform each tick
to where `Bone` is in its parent's pose, moved by its offsets in the bone's
frame. Change `Bone` to move it -- `"Spine_02"` holstered, `"Hand_R"` drawn.

**Root motion** is the clip's (mesh viewer, Clip > Root motion):

| Mode | |
|---|---|
| As authored | the root bone travels: the character walks off its entity |
| In place | the root's model-space X and Z held at the first frame's: a walk on the spot, the entity's own movement carries it |
| Moves the entity | held in place, and the travel moves the entity instead -- turned and sized as the entity is, each game tick |

The travel is the first layer's (`AnimInstance::GetRootMotion`), blended
as its poses are -- a blend space, a crossfade. Only X and Z move the
entity; turning is not extracted. The editor's preview moves nothing.

## Budget

What each character costs falls off with distance from the frame's views
(every camera WorldBridge draws the world from), and outside them, in
`[Animation]`:

| Var | Default | |
|---|---|---|
| `FullRateDistance` | 3000 | past it, an update every (distance / this)th tick, the time saved up |
| `MaxSkipFrames` | 3 | at most this many ticks skipped in a row |
| `OffscreenSkipFrames` | 3 | outside every view's frustum (behind the camera, off to the side): an update every (this + 1)th tick, the time saved up, its pose still made then -- for its shadow and sockets. Whichever of this and the distance skips more |
| `FreezeDistance` | 20000 | past it, time passes -- notifies, transitions, root motion -- but no pose is evaluated |
| `ParallelMin` | 16 | at least this many updating in a tick, and they run across the job system |

0 turns a distance (or the off-screen rate) off. A world with no view -- a
dedicated server -- and the editor's preview of a level run everything at
full rate. `AnimationSystem::GetLastStats` says what the last tick did
(`Offscreen` counts those outside every view) and how long it took:
`Milliseconds` the whole tick, `UpdateMilliseconds` the poses alone.

**Visibility.** WorldBridge hands each frame's view to the system with its
view-projection (`AnimationSystem::AddView`), and a character is tested as a
sphere round its mesh's own box and its bones where they are now -- root
motion as authored can walk it off its entity -- padded a quarter for the
skin, against each view's frustum. A view given by origin alone
(`AddViewOrigin`) sees everything. One that comes back into view takes the
time it saved at once. Occlusion is not considered: a character hidden behind
an occluder (RENDERPIPELINE.md, "Occlusion culling") is still in view.

## Anim states

```
Parameters   Speed (float), Armed (float), Jump (trigger)
Layer Base   (every bone)
  Locomotion   blend space on Speed: Idle 0, Walk 150, Run 400
  Jump         clip, once
  Any -> Jump        when Jump triggered, crossfade 0.1 s
  Jump -> Locomotion at end, crossfade 0.2 s
Layer Arms   mask spine_02, weight scaled by Armed
  Holster      clip
```

- A **state** plays one clip, or blends several along a float parameter (a
  1D blend space), or over two (a 2D one: each clip at a point, X and Y --
  walks forward, back and to the sides, by speed and direction). The
  samples share one phase, so a walk and a run blended step together.
  A 2D space weighs its clips by gradient band interpolation: any layout of
  points works, each clip is whole at its own point, and past the edge the
  nearest clips play. The axes are measured as they are, so give them
  similar ranges (direction in -1..1 beside speed in 0..1, not 0..400).
- A **transition** fires when all its conditions hold (`>`, `<`, `==`, `!=`,
  is true, is false, triggered), from its state or from any; At End waits
  for the clip to come within the crossfade of its end. The first that holds
  fires, one per layer per tick; a triggered condition spends its trigger.
- A **sync group** keeps states in step. Name the same group on states of
  different layers -- an upper-body walk and the legs' -- and each later
  layer in one takes the phase (0..1 through the state) of the first layer
  playing one of the group, instead of keeping its own time: the arms swing
  with the steps, whatever the clips' lengths. It locks on as it enters,
  passing no notifies, and fires those it passes from then on. A transition
  between two states of one layer in the same group starts the new state
  where the old one was (Unarmed walk to Armed walk keeps the stride). With
  no layer before it playing the group, a state plays at its own pace.
- A **layer** after the first is blended over the ones before through its
  mask -- each named bone and every bone below it -- at its weight, scaled by
  its weight parameter if it has one. That is how legs walk while the arms
  hold a weapon, or stay idle.
- An **additive** layer adds movement instead of replacing the pose: each of
  its clips is taken relative to a base -- its own first frame, or the
  skeleton's reference pose -- and that difference (translation, rotation,
  scale) is laid over the layers before it, by weight and mask. Breathing,
  a flinch or a lean then plays over whatever the body is doing. Its clips
  give no root motion.
- A **look-at**, after the layers, bends a chain of bones by an angle: a
  float parameter in degrees, clamped to Min..Max, turns each listed bone
  about one model-space axis by its share of the angle. Each bone turns
  about its already-turned parent, so the turns add up along the chain and
  the last bone ends at the whole angle (when the shares add to 1). An
  optional weight parameter scales it (0..1), and Response eases it toward
  the parameter (`1 - e^(-Response t)` each update, in real time; 0 snaps).
  An explicit `Evaluate()` -- after a seek or a change -- goes straight to
  the target. Everything below the chain follows: arms, and whatever an
  Anim Socket holds. Stored as the optional ALOK chunk; files without it
  have none.

## In the editor

- **Content Browser**: double-click an `.nmanim` to preview it on a mesh of
  its skeleton; an `.nmanimstate` to edit it; New Anim State in the context
  menu.
- **Mesh viewer**, for a skinned mesh: play any clip on its skeleton or any
  anim state, scrub, change speed, draw the skeleton; drive an anim state's
  parameters and see each layer's state; edit a clip and save it. A clip's
  timeline shows its frames, the playhead (click or drag to scrub) and its
  notifies (drag to move one, double-click to add one); drag its In and Out
  handles and Crop to Range to keep only those frames -- what plays, and
  what loops (`TrimClip`; reimporting restores the whole clip). Beside it:
  looping, root motion (as authored, in place, or moving the entity) and
  the root bone whose travel that is, and each notify's particle system,
  bone and sound. The Skeleton tab lists
  the bones -- names for sockets and masks.
- **Anim state editor**: parameters and layers as forms; the picked layer's
  states and transitions as a graph -- boxes and arrows, Any State among
  them, the entry state barred, the state the preview is in lit (dashed
  while it blends in). Right-click to add a state, make a transition (then
  click the state it goes to), set the entry or delete; drag boxes about;
  Delete removes what is picked. What is picked is edited in the form beside
  it: a state's clips or blend space and sync group, a transition's
  crossfade and conditions. The Look-At box sets the angle and weight
  parameters, range, axis, response and the chain (`bone:share, ...`).
  Dashed arrows wait for the clip's end. Where the boxes sit is
  saved in the file (ALYR v4); older files are laid out on opening. A live
  preview you can drive sits alongside. A renamed parameter is renamed where
  it is used; problems (missing clips, parameters, mask or look-at bones)
  are listed.
- Console, for scripts: `mesh <path> | play | pause | seek <s> | param <n> <v>
  | bones on | windowshot <png>` and `animstate <path> | new | save |
  windowshot <png>`.

## Engine characters

`Engine/Content/Characters/<Name>/` holds characters every project has, staged
beside every executable at the same path (Engine/CMakeLists.txt): the mesh,
skeleton, clips and anim states, and the source `.glb` they reimport from
(not staged).

**CrashDummy** -- a 74-bone Rigify deform skeleton (root `CrashDummy_Rig`),
six colours baked as vertex colours, and its pistol (`Weapon_Pistol.nmmesh`,
gripped at its origin). Clips, in place (no root travel):

| Clip | Frames | |
|---|---|---|
| `FB_Idle`, `FB_Walk`, `FB_Run` | 60, 32, 24 | full body, looping; the walk's planted foot moves about 145 units/s |
| `FB_JumpStart`, `FB_Fall`, `FB_Land` | 24 each | full body |
| `UB_Idle`, `UB_Walk`, `UB_Run` | 60, 32, 24 | upper body (`DEF-spine.002` and below it) |
| `UB_Idle_Armed`, `UB_Walk_Armed`, `UB_Run_Armed` | 60, 32, 24 | upper body, holding the pistol |
| `UB_EquipWeapon` | 40 | the right hand draws from the holster; notify `WeaponToHand` at 0.333 s (frame 11, the hand on the grip) |
| `UB_HolsterWeapon` | 36 | and puts it back; notify `WeaponToHolster` at 0.633 s (frame 20, the gun in the holster) |

`CrashDummy.nmanimstate` puts them together:

```
Parameters   Speed (float, units/s), Grounded (bool, 1), Jump (trigger), Armed (bool),
             AimPitch (float, degrees, up positive)
Layer FullBody
  Locomotion   blend space on Speed: FB_Idle 0, FB_Walk 150, FB_Run 400    sync group Locomotion
  JumpStart    once     Locomotion -> JumpStart when Jump triggered, 0.1 s
  Fall         loop     JumpStart -> Fall at end; Locomotion -> Fall when not Grounded
  Land         once     Fall -> Land when Grounded; Land -> Locomotion at end, or -> JumpStart on Jump
Layer UpperBody   mask DEF-spine.002; weight scaled by Grounded (in the air the full body's arms play)
  Unarmed      blend space on Speed: UB_Idle 0, UB_Walk 150, UB_Run 400    sync group Locomotion
  Equip        once     Unarmed -> Equip when Armed; Equip -> Armed at end
  Armed        blend space on Speed: UB_*_Armed at 0, 150, 400              sync group Locomotion
  Holster      once     Armed (or Equip) -> Holster when not Armed; Holster -> Unarmed at end
Look-at      AimPitch, -40..55 deg about model X, response 15:
             DEF-spine.001 0.25, DEF-spine.002 0.35, DEF-spine.003 0.40
```

The chain stops at the chest, so the arms -- and the pistol -- take the whole
angle: a game sets AimPitch from its camera's pitch (negated, if the camera's
is down-positive) and the gun points where the camera does.

The run's 400 is a guess (its foot is planted for two frames); set the
blend positions to the speeds the character moves at.

**`CrashDummyDemo.nmprefab`** shows it working: the dummy (Render,
Animation on the anim state) and a child **Pistol** (Render, Anim Socket),
with `CrashDummyDemo.lua` walking it round a 17-second loop -- idle, walk,
draw while walking, armed run, stop, jump, land, holster -- nodding AimPitch
35 degrees up and down every 4 seconds throughout. The script moves the
pistol as the notifies fire:

| Notify | Socket bone | Position offset | Rotation offset |
|---|---|---|---|
| (start), `WeaponToHolster` | `DEF-spine` | -19.719, 0.128, -4.773 | -75.407, 7.744, 2.009 |
| `WeaponToHand` | `DEF-hand.R` | -2.110, 7.087, -0.769 | -85.592, 13.264, 175.197 |

The offsets are the source `.blend`'s two Child Of constraints on the pistol,
converted to engine space: what a game copies to hold the pistol itself.

## Not yet

Inverse kinematics (feet, a hand on a weapon), morph targets,
retargeting between differently named skeletons, clip compression, turning
root motion, skinned meshes on Metal, and an occlusion-aware budget. Also:

- **Socket swaps as data**: a notify cannot move a socket by itself; a script
  listens for it and sets the socket's bone and offsets (as
  `CrashDummyDemo.lua` does).
- **Saving a level with sockets**: the editor's preview moves a socket's
  entity, and saving records where it was as an override of the prefab.
- **Interrupted crossfades** start from the state being left, not the blend
  on screen: a transition taken mid-fade pops.
- **Layer weights** follow their parameter at once; nothing eases them.
- **Look-at** is one angle about one axis: pitch, or yaw, not both, and not
  aimed at a point. A camera offset to the side (an over-the-shoulder scope)
  leaves the gun a little off the crosshair; aiming at the camera ray's hit
  point would need a second axis and a target.
- **Masks** are whole bones and their children, blended in local space: no
  partial weights down a spine, no mesh-space blending.
- **Scripted one-shots**: gameplay plays a clip only through a state and a
  trigger authored for it; there is no "play this clip on this layer now".
- **Clip curves and notify windows**: notifies are moments; clips carry no
  float curves (an IK weight, a footstep volume) or spans.
- **Ragdoll**: Jolt has one; nothing drives a skeleton from physics.
- **Bone queries**: `GetBoneLocation` only, no rotation or transform.
- **Parameters**: at most 12 per Animation component.
- **Playback speed by parameter**: a state's Speed is fixed; nothing scales
  a walk to the ground speed it moves at.
- **Engine content in the editor**: anim states and clips under
  `Engine/Content` open read-only.
