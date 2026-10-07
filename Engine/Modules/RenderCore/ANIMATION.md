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

Clips find bones by **name**, so a clip plays on any skeleton with those
names. Reimporting a clip keeps what the editor set on it: notifies,
looping and root motion.

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
```

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
where it started. (There is no audio module yet; a sound would go here.)

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

- A **state** plays one clip, or blends several along a float parameter. The
  samples share one phase, so a walk and a run blended step together.
- A **transition** fires when all its conditions hold (`>`, `<`, `==`, `!=`,
  is true, is false, triggered), from its state or from any; At End waits
  for the clip to come within the crossfade of its end. The first that holds
  fires, one per layer per tick; a triggered condition spends its trigger.
- A **layer** after the first is blended over the ones before through its
  mask -- each named bone and every bone below it -- at its weight, scaled by
  its weight parameter if it has one. That is how legs walk while the arms
  hold a weapon, or stay idle.

## In the editor

- **Content Browser**: double-click an `.nmanim` to preview it on a mesh of
  its skeleton; an `.nmanimstate` to edit it; New Anim State in the context
  menu.
- **Mesh viewer**, for a skinned mesh: play any clip on its skeleton or any
  anim state, scrub, change speed, draw the skeleton; drive an anim state's
  parameters and see each layer's state; edit a clip's notifies (added at the
  playhead), looping and In Place, and save them. The Skeleton tab lists
  the bones -- names for sockets and masks.
- **Anim state editor**: parameters, layers, states and transitions as
  forms, beside a live preview you can drive. A renamed parameter is renamed
  where it is used; problems (missing clips, parameters, mask bones) are
  listed.
- Console, for scripts: `mesh <path> | play | pause | seek <s> | param <n> <v>
  | bones on | windowshot <png>` and `animstate <path> | new | save |
  windowshot <png>`.

## Not yet

Inverse kinematics (feet, look-at), morph targets, additive layers, 2D blend
spaces, retargeting between differently named skeletons, clip compression,
turning root motion, skinned meshes on Metal, and animation
budgets (off-screen entities still animate).
