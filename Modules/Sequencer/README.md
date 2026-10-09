# Sequencer

Cutscenes for a GameCore `World`. A **sequence** (`.nmseq`) is a timeline. It
names the entities it controls (**bindings**) and what it does to them over
time (**tracks**). A **Sequence Player** component plays one. The World's
`SequenceWorld` advances the players and writes their tracks.

This is phase 0 of the Sequencer: the asset, the runtime and scripting.
There is no editor window yet, so a sequence is written by hand as XML.

---

## Position in the tree

```
Host exe --> NimbleEngine --> Sequencer --> GameCore --> EngineCore
```

`NimbleEngine` links it **PRIVATE**. A game reaches cutscenes in three ways,
none of which needs a Sequencer header:

- the **Sequence Player** component;
- the `Sequence` script library;
- `NimbleEngine/SequenceFacade.h`.

`GameSession` attaches a `SequenceWorld` to every level's World before the
level loads, so a player placed in the level with **Auto Play** starts with
the level.

---

## The file: `.nmseq`

The XML is the authored form, readable in a diff. **Build > Cook Content**
writes a binary form, which is the only form a Shipping build reads. The
first bytes tell the two apart, as they do for `.nmui`.

```xml
<Sequence version="1" fps="24" length="240">
  <Binding id="hale" kind="possess" guid="7f3a...c21" name="Inspector_Hale">
    <Track type="Transform">
      <Channel name="Position.X">
        <Key f="24" v="-4" interp="CubicInOut"/>
        <Key f="96" v="2.5"/>
      </Channel>
    </Track>
    <Track type="Float" component="Camera" field="FovY">
      <Key f="0" v="55"/>
      <Key f="120" v="48"/>
    </Track>
    <Track type="Bool" component="Render" field="Visible">
      <Key f="0" v="true"/>
    </Track>
  </Binding>
  <Binding id="guard" kind="spawn" name="Guard" prefab="Content/Prefabs/Guard.nmprefab">
    ...
  </Binding>
  <Track type="Event">
    <Key f="228" event="Combat.Start" arg0="cellar" arg1="@hale" fireOnSkip="true"/>
  </Track>
</Sequence>
```

- **Times are whole frames** at `fps`. The sequence ends at `length`. The
  playhead between frames is fractional.
- **Bindings.**
  - A `possess` binding finds an entity already in the level by `guid`. If no
    entity has that guid, it uses the first entity with that `name`.
  - A `spawn` binding spawns its `prefab` when the sequence first needs it,
    and destroys it when the sequence stops or finishes.
  - Lua or C++ can override any binding with `Bind`.
- **Tracks.**

  | Track | Belongs to | Writes |
  |---|---|---|
  | `Transform` | a binding | The entity's local Transform. Channels are `Position.X` to `Scale.Z`; an unkeyed channel is left alone |
  | `Float` | a binding | One number field (float, double, int, uint8) of one of the entity's components |
  | `Bool` | a binding | One flag field, stepped (never blended) |
  | `Event` | anywhere | EventBus events at frames |

  `component` is the component's registered name. `field` is any writable
  field that the component binds for scripts (`Bind.Field` / `Bind.Flag`):
  the same name Lua uses, without Get/Set.
- **Keys.** `interp` controls how a key's value travels to the next key:
  - `Auto` (the default) is a smooth curve. It is flat at the first and last
    keys and at peaks, so it never overshoots a key.
  - `Constant` and `Linear`.
  - The eases from `UIAnimation.h`: `QuadIn`, `QuadOut`, `QuadInOut`,
    `CubicIn`, `CubicOut`, `CubicInOut`, `BackOut`, `ElasticOut`.
- **Event arguments.** Up to four, `arg0` to `arg3`. A number, `true` or
  `false` are typed. `@id` passes the entity that binding resolves to.
  Anything else is a string.

Loading never refuses an asset for its content. `SequenceAsset::Validate`
lists problems such as duplicate ids, keys past the end, or an `@id` that
names no binding. They are logged as the asset loads.

---

## Playing

`SequenceWorld` ticks in **TP_PostPhysics**. That is after gameplay scripts
and physics, so the sequence has the last word on what it binds. It is also
before the animation system in TP_PostUpdate.

- **Continuous tracks** (Transform, Float, Bool) depend only on time.
  Scrubbing, seeking and skipping all land on the same values.
- **Events** fire for the keys the playhead passes during playback.
  - Starting from the beginning includes frame 0.
  - A **Seek** fires nothing.
  - A **Skip** to the end fires only keys marked `fireOnSkip`.
  - A loop fires the rest of the pass, then the start of the next.
- **The first tick** after a start shows the frame it starts on. No time
  passes on that tick.
- **What it changed.** The first time a track writes a value, the player
  saves the original. With **Restore State** on, stopping or finishing puts
  every saved value back. With it off (the default), the last frame stays,
  which suits a cutscene that hands over to gameplay. Spawned entities are
  destroyed either way.
- **Pause.** A paused game (`GameSession::SetPaused`) does not tick the World,
  so sequences pause with it. Each player can also be paused on its own.
- The playhead never runs backwards. A `PlayRate` of 0 holds.

Each player emits these on the World's EventBus, each with
`(player entity, sequence path)`:

- `SequenceStarted`
- `SequenceFinished`: the sequence reached the end, or was skipped to it
- `SequenceStopped`

### Lua and script graphs

```lua
local p = Sequence.Play("Content/Cinematics/Intro.nmseq")   -- a new player, destroyed when it finishes
Sequence.Bind(p, "hale", Self)                               -- before its first tick
Events.On("SequenceFinished", function(player, path) ... end)
Events.On("Combat.Start", function(arena, who) ... end)      -- an Event track's key

Sequence.Pause(p)  Sequence.Resume(p)  Sequence.Seek(p, 4.5)
Sequence.Skip(p)   Sequence.Stop(p)    Sequence.Start(p)
Sequence.IsPlaying(p)  Sequence.GetTime(p)  Sequence.GetDuration(p)
```

A player placed in a level is driven the same way through its entity.

### C++

```cpp
#include "Public/NimbleEngine/SequenceFacade.h"

const GameCore::EntityID Player = NimbleEngine::PlaySequence(World, "Content/Cinematics/Intro.nmseq");
NimbleEngine::SkipSequence(World, Player);
```

Engine and editor code can use `Sequencer::SequenceInstance` directly. It
applies an asset to a World at any frame, fires events over a range, and
restores what it changed. The editor's preview will use it, so what the
editor shows is what the game plays.

---

## Tests

`SequencerTests` covers:

- curve sampling;
- XML and binary round trips;
- validation;
- an instance writing transforms and fields and restoring them;
- players auto-playing, pausing, seeking, looping, skipping and finishing on
  a bare World ticked by hand.

```
ctest -R Sequencer
```

---

## Not yet

These are in the order of the Sequencer plan's phases:

- **P1** The editor window: track tree, timeline, undo, preview, and the
  Content Browser and Details panel hookup. That includes a `.nmseq` asset
  picker for the component's Sequence field.
- **P2** Animation, Audio, Camera cut and Fade tracks.
- **P3** The curve editor, auto-key, pilot camera and camera blends.
- **P4** Cinematic input lock, sub-sequences, and a synced network start.

A bound rigid body is not yet switched to kinematic while a sequence holds
it.
