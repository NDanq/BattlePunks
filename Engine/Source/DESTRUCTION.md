# Destruction

Things that break apart the way their material does. A concrete block
crumbles into chunks, a crate splinters into planks, a glass pane shatters
into shards. The pieces are real rigid bodies: they fall, bounce, pile up
and then clear away.

The code is `Public/NimbleEngine/Destruction.h` (the component and the
system) and `Modules/RenderCore/Source/Public/RenderCore/MeshFracture.h`
(the cutting). WorldsLab's **DestructionLab** level shows every material,
and you can fly round it and shoot it (`WorldsLabDestruction.cpp`).

Breaking needs the physics module (`NIMBLE_WITH_JOLTPHYSICS`, Project
Settings > Project > Modules > Physics). Without it nothing is ever hit
hard enough; a script's `Break` still works, but the pieces don't fall.

## Making something breakable

1. Give the entity a **Render** mesh. The mesh must be closed (no holes):
   the engine's Box, a crate, a wall, a pillar. An open mesh still breaks,
   but its cuts are left open and the log says so.
2. Give it a **Rigid Body**. Dynamic for things that get knocked about
   (crates, barrels), Static for things that stand (walls, pillars, glass).
3. Add a **Destructible** (Add Component > Physics > Destructible) and set
   its **Material**.

That's all. In Play, hit it hard enough and it breaks.

A breakable entity can have any mobility. Its pieces are always new
dynamic entities, so in a level with baked lighting they're lit by the
light probes, not by the whole's lightmap.

## Materials

The material decides how it breaks, how strong it is, and what its pieces
weigh.

| Material | Breaks into | Strength | Density (kg/m³) | Debris lasts |
|---|---|---|---|---|
| Concrete | even chunks | 15,000 | 2,400 | 8 s |
| Wood | long splinters along Y | 6,000 | 600 | 10 s |
| Glass | shards round the hit | 1,500 | 2,500 | 4 s |
| Stone | small pieces at the hit, big ones further off | 20,000 | 2,600 | 12 s |
| Metal | a few large pieces | 40,000 | 7,800 | 15 s |

Strength is the impulse of one hit, in kg × units/s, that breaks it at
once. Health, three times the strength, is how much smaller hits can add up
to before it breaks. As a feel for the numbers: a 4 kg ball at 40 m/s
(4,000 units/s) gives about 16,000, enough for everything but stone and
metal.

### Changing or adding materials

Every field can be set per project in `Config/DefaultEngine.ini`, one
section per material:

```ini
[Destruction_Concrete]
InteriorMaterial=Content/Materials/ShootingRange/Concrete.material.nmgraph
BreakParticles=Content/Particles/ShootingRange/Dust.nmparticle
BreakSound=Content/Sounds/Crumble.nmsound

; A new material: the fields you leave out are Concrete's.
[Destruction_Ice]
Pattern=Radial
Pieces=20
Strength=2500
Density=920
DebrisLifetime=3
```

| Key | Meaning |
|---|---|
| `Pattern` | `Uniform`, `Clustered` (fine at the hit), `Splinter` (long along the grain), `Radial` (shards from the hit) |
| `Pieces` | how many pieces, at most 64 |
| `GrainStretch` | Splinter: how much longer than wide a splinter is (wood: 4) |
| `Strength`, `Health` | see above |
| `Density` | kg/m³, for the pieces' mass when the whole has no Rigid Body mass |
| `Friction`, `Restitution` | the pieces' |
| `Scatter` | units/s the pieces nearest the hit are thrown out at |
| `BreakRadius` | structures: how much falls round a hit, as a fraction of its size |
| `DebrisLifetime` | seconds a fallen piece lives |
| `InteriorMaterial` | what the cut faces draw with; unset, the mesh's own first material |
| `BreakParticles` | a `.nmparticle` burst at the hit |
| `BreakSound` | a `.nmsound` played at the hit |

Set `InteriorMaterial` for every material you use. The default is the
surface material, which is right for concrete and stone, but a painted
crate looks better with bare wood inside.

## The Destructible's settings

| Setting | |
|---|---|
| Material | a preset name, above |
| Mode | **Runtime** cuts the mesh when it breaks, around the hit. **Baked** uses pieces cut in the editor (below) |
| Pieces, Seed | 0 uses the material's piece count, and a seed from the entity, the same on every machine |
| Strength, Health | 0 uses the material's |
| Structural, Anchored | see Structures |
| Lifetime | seconds its fallen pieces live; 0 uses the material's |
| Damage, Broken | while it plays: hits so far, and whether it broke |

## What breaks it

- **Physics hits.** Anything that hits its body: a thrown ball, a falling
  crate, a car. One hit at or over Strength breaks it. Smaller hits add to
  Damage until it reaches Health. Resting and sliding contacts do nothing.
- **Scripts.**

  ```lua
  local d = entity:GetComponent("Destructible")
  d:Break(point, impulse)          -- breaks it now, around point
  d:ApplyDamage(point, impulse)    -- a hit: breaks it only if it is enough
  if d:IsBroken() then ... end

  -- An explosion: damages every Destructible within the radius and pushes
  -- every dynamic body, falling off to nothing at the edge.
  Destruction.RadialDamage(centre, 400, 200000)

  -- Told when anything breaks; the entity's own script also gets OnBroken.
  Events.On("DestructibleBroken", function(entity) ... end)
  ```

  `point` is in world units; `impulse` is a Vector3 in kg × units/s, the
  hit's direction and size.
- **C++.** The same calls on `NimbleEngine::DestructibleComponent`, and
  `NimbleEngine::RadialDamage(World, Centre, Radius, Impulse)`.

## Structures

Tick **Structural** on a wall, a pillar or a floor whose Rigid Body is
Static (or that has none). Then a hit doesn't break the whole thing:

- the pieces near the hit fall (`BreakRadius` of its size);
- the rest stay standing, still Static;
- with **Anchored** on, the pieces on its base hold the rest up, and any
  part a break cuts off from the base falls too;
- each later hit on what's left breaks more of it there, at half the
  strength.

So a few shots knock a hole in a wall, and more bring the top down. Pieces
of a structure falling onto what still stands of it don't break it further.

Without Anchored nothing holds it up, and the first break drops it all.

## Runtime or Baked

**Runtime**, the default, cuts the mesh at the moment it breaks. Clustered
and Radial materials cut round the actual hit, so a shot shatters glass
where it lands. A cut takes a few milliseconds (about 10 ms for a box in a
Debug build), done once and shared by every break that cuts the same mesh
the same way: ten identical crates cut once.

**Baked** cuts the pieces in the editor and saves them, for a mesh that's
expensive to cut or a break you want to look exactly so. Select the entity
and type in the editor's console:

```
destruction bake
```

That writes `Content/Fractures/<Name>.nmfrac` and a folder of `.nmmesh`
pieces beside it, and sets the component to Baked. Save the level. Bake
again after changing the mesh, the material or the piece count.

## Debris

Fallen pieces live for the material's `DebrisLifetime`, or half of it once
they've come to rest. Then they shrink away and are destroyed.

| `[Destruction]` key | Default | |
|---|---|---|
| `MaxDebris` | 256 | fallen pieces alive at once; past it the oldest start going |
| `CosmeticVolume` | 1000 | cubic units: pieces smaller than this go on the debris layer |
| `DebrisLayer`, `DebrisMask` | 1, -1 | that layer, and what it collides with. To let players walk through small rubble, give debris its own layer bit and leave the player's bit out of the mask |
| `MaxCachedCuts` | 16 | runtime cuts kept for reuse once nothing uses them |

## Multiplayer

A server decides what breaks and tells every client, with the seed it cut
with, so each machine cuts identical pieces. The pieces then fall on each
machine on its own and are never sent: they are cosmetic. A client never
breaks a networked entity (one with a Net component) itself. Damage and
Broken are replicated, so a player who joins later sees it broken.

Gameplay that has to agree on every machine, like whether a wall still
gives cover, should read Broken, not where the pieces landed.

## Trying it in the editor

While a level plays, the editor's console has:

| Command | |
|---|---|
| `destruction stats` | how many destructibles, broken, debris, standing pieces, cached cuts |
| `destruction break [name]` | breaks it (or the selection) |
| `destruction boom x y z radius impulse` | an explosion there |
| `destruction bake [name]` | while editing: Baked mode, above |

In DestructionLab, fly with WASD and the mouse, left click to shoot a
ball, right click a heavy one. `WorldsLab.DestructionDemo true` flies a
fixed tour and shoots on its own.

## Not here yet

- Breaking with joints that snap under load; structures use the support
  rule above instead.
- Cutting off the game thread: a big runtime cut is a visible hitch.
- A late joiner sees a broken structure gone, not its standing pieces.
- An exploded-view preview of the pieces in the editor.
- Particles draw only with Vulkan, so break bursts don't show in D3D12
  or OpenGL.
