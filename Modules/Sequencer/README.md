# Sequencer

Cutscenes for a GameCore `World`. A **sequence** (`.nmseq`) is a timeline. It
names the entities it controls (**bindings**) and what it does to them over
time (**tracks**). A **Sequence Player** component plays one. The World's
`SequenceWorld` advances the players and writes their tracks.

The runtime is complete: every track type, cinematic mode, sub-sequences and
networked playback. Sequences are authored in the editor's Sequencer
(Modules/Editor README, "Sequencer"), or as XML by hand.

---

## Position in the tree

```
Host exe --> NimbleEngine --> Sequencer --> GameCore --> EngineCore
                                    `----> RenderCore (PRIVATE: clips, cameras)
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
  <Track type="CameraCut">
    <Section start="0" end="132" camera="wide"/>
    <Section start="132" end="240" camera="close" blendIn="12"/>
  </Track>
  <Track type="Fade"><Key f="0" v="1"/><Key f="18" v="0"/></Track>
  <Track type="Audio">
    <Section start="0" end="240" sound="Content/Sounds/CellarTheme.nmsound" bus="Music" spatial="2D"/>
  </Track>
  <Track type="BusVolume" bus="Music"><Key f="120" v="1"/><Key f="132" v="0.3"/></Track>
  <Track type="Subtitle">
    <Section start="132" end="200" speaker="Hale" text="Nobody leaves this cellar."/>
  </Track>
  <Marker f="132" name="Reveal"/>
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
  | `Animation` | a binding | Its clips, in sections (`clip`); overlapping sections crossfade over the later one's `blendIn` |
  | `Audio` | anywhere | Sounds in sections (`sound`): at the binding, or at the origin on a track of no binding (heard 2D there unless the section says `spatial="3D"`). A section can put its sound on another mix bus (`bus`) or force it `spatial="2D"`/`"3D"` |
  | `CameraCut` | top level | Which camera binding the game is seen through (`camera`), blending from the previous shot over `blendIn` |
  | `Fade` | top level | The screen to black: one channel, 0 clear .. 1 black |
  | `SubSequence` | top level | Other sequences (`sequence`), each section playing one in its own time |
  | `PostProcess` | top level | Post process stacks (`stack`, a `.pp.nmgraph`) in sections, blended over the level's volumes at `weight` times their fades |
  | `BusVolume` | top level | A gain on one mix bus (`bus`: `Master`, `Music`, `SFX`, `UI`, `Voice`, `Ambient`), 1 as the player set it: ducking music under a line, swelling it for a reveal |
  | `Subtitle` | top level | Lines in sections (`text`, and an optional `speaker`), shown under the letterbox; the later-started section wins where they overlap |
  | `PostProcessSetting` | top level | One setting (`setting`: `Exposure`, `DofFocusDistance`, `DofFocusRange`, `DofMaxBlur`, `BloomIntensity`, `BloomThreshold`, `FogDensity`, `MotionIntensity`, `AoIntensity`, `SsrIntensity`), keyed over everything; keying it turns its effect on |

  Section tracks hold `<Section start end ...>` spans. They also take
  `rate`, `volume`, `offset` (frames into the asset at `start`), `blendIn`,
  `blendOut` and `loop`. Any track can carry `name=""` and `muted="true"`.
  `component` is the component's registered name. `field` is any writable
  field that the component binds for scripts (`Bind.Field` / `Bind.Flag`):
  the same name Lua uses, without Get/Set.
- **Keys.** `interp` controls how a key's value travels to the next key:
  - `Auto` (the default) is a smooth curve. It is flat at the first and last
    keys and at peaks, so it never overshoots a key.
  - `Constant` and `Linear`.
  - `User`, a curve with the key's own tangents (`in`, `out`, in value per
    frame), which the curve editor writes.
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
- **What it holds.** Some values belong to the sequence only while it plays,
  and always go back, whatever Restore State says:
  - An Animation track pauses its entity's Animation component and sets the
    clip and time itself. When the playhead leaves its sections, the
    component plays its own animation again.
  - A rigid body moved by a Transform track is kinematic while held.
- **Sounds** play through helper entities with an Audio Source, parented to
  the binding. They pause, resume and seek with the playhead, and are seeked
  back if they drift more than a quarter second. `blendIn` and `blendOut`
  fade them; `rate` plays them faster (higher). A section's `bus` and
  `spatial` reach the source as `ForceBus` and `ForceSpatial`, over the
  sound's own settings.
- **Sub-sequences** play in their section's time, at its `rate`. A binding a
  sub-sequence shares by id plays the same entity, and their events fire as
  the outer playhead passes them.

### The screen: cameras, fades and cinematic mode

`GameSession` gives each level's `SequenceWorld` a `SequenceHost`:

- **Camera cuts** drive a camera entity the sequence makes (`Sequence
  Camera`). The game is seen through it with `GameSession::SetViewTarget`.
  When several players have a shot, the one started last wins. When none
  has, the view goes back to what it was.
- **The fade** is the darkest any player asks for.
- **Cinematic mode** (the component's **Cinematic**) is on while a cinematic
  player plays or is paused:
  - letterbox bars are drawn;
  - the players' input is held back;
  - Back (Escape, gamepad East) skips every **Skippable** one
    (`Sequence.SkipCinematics()`).

  The fade and the bars are a UI document GameSession builds in code, so
  there is no file to ship.
- **Subtitles**: the line of the player started last that has one, drawn
  over the bars (the same document, bound to `Sequencer.Speaker` and
  `Sequencer.Subtitle`). A game's settings turn them off with
  `Sequence.SetSubtitles(false)` (every level; not saved here).
- **The mix**: every player's Bus Volume gains, multiplied, set on the audio
  device (`AudioDevice::SetBusGain`). They sit under the player's own bus
  volumes, are never saved, and go back to 1 when no player wants them or
  the level unloads.

### Post process

The `PostProcess` and `PostProcessSetting` tracks are laid over the level's
Post Process volumes through RenderCore's `PostProcessOverride` for the
sequence's World:

1. the volumes blend as always;
2. the sequence's stacks blend over them at their weights;
3. its settings are written over the result.

Several players combine, earlier-started first, so a later one's settings win.
The override is cleared when nothing plays. The editor's preview sets it for
the level being edited. A focus pull is a `DofFocusDistance` track; keep
`DofMaxBlur` at 0 where no blur is wanted.

A volume can also be faded on its own: the Post Process component's `Weight`
(0..1) scales its strength. A Float track on `Post Process.Weight` keys it.

### Networking

A server's player writes its playback into the component's `NetState`,
`NetSerial` and `NetSeconds`, which replicate with `Sequence`, `PlayRate`,
`Loop` and `Cinematic`. A client's copy of a replicated player (one with a
Net component) never auto plays. It follows those fields: it starts, pauses,
seeks, skips and stops as the server's did, and plays locally from there, a
round trip behind. Spawn bindings spawn on each machine.

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
Sequence.SeekToMarker(p, "Reveal")     Sequence.SkipCinematics()
Sequence.IsPlaying(p)  Sequence.GetTime(p)  Sequence.GetDuration(p)
Sequence.SetSubtitles(false)  Sequence.GetSubtitles()        -- the player's setting, every level
```

A player placed in a level is driven the same way through its entity.

### C++

```cpp
#include "Public/NimbleEngine/SequenceFacade.h"

const GameCore::EntityID Player = NimbleEngine::PlaySequence(World, "Content/Cinematics/Intro.nmseq");
NimbleEngine::SkipSequence(World, Player);
```

Engine and editor code can use `Sequencer::SequenceInstance` directly. It
applies an asset to a World at any frame, keeps its sounds in step, fires
events over a range, and restores what it changed. The editor's preview uses
it with `SequenceInstanceOptions` that make nothing in the level (no camera
entity, no spawns; sound helpers in a World of its own, `SoundWorld`), so
what the editor shows is what the game plays.

## Cooking

Build > Cook Content cooks every `.nmseq`. A sequence that plays a clip,
sound, sub-sequence or prefab that does not exist fails the cook. Anything
else `Validate` finds is logged as a warning.

---

## Tests

`SequencerTests` covers:

- curve sampling, user tangents;
- XML and binary round trips, sections and markers;
- validation;
- an instance writing transforms and fields and restoring them;
- clips through a real AnimationSystem, crossfades and hand-back;
- shots and camera blends, fades, sub-sequences, sounds, held rigid bodies;
- bus gains, subtitles and a section's bus and 2D/3D, helpers in a separate
  sound World, players' gains multiplying to the host and subtitles off;
- players auto-playing, pausing, seeking, looping, skipping and finishing on
  a bare World ticked by hand;
- the host (view, fade, cinematic, skip) and a replica following a server.

```
ctest -R Sequencer
```

---

## Not yet

- Time dilation (slow motion): the World has no time scale.
- Spawn bindings in the editor's preview: they show nothing until played.
- Lip sync, and subtitles from a string table (a line's text is the line).
