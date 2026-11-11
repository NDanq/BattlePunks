# Replays

A **replay** records what a World looked like over time and plays it back into
a World: kill cams, instant replays, match replays you save and watch later,
the last seconds before a bug. It records **state, not input**. Each frame is
what the recorded entities' components held, written through their
serializers. Nothing is re-simulated, so physics, AI and scripts don't need to
be deterministic.

`Engine/Modules/GameCore/Source/Public/GameCore/Replay.h` is the API, and the
GameCore README's "Replays" section is the short reference. This guide covers
the three ways in (C++, Lua and script graphs) and what to watch for when you
wire them into a game.

Every example here is compiled and run:

| Example | Run by |
|---|---|
| C++ `KillCam` | `Engine/Tests/ReplayTests.cpp`, `TestGuideKillCam` (`NimbleEngine.ReplayTests`) |
| Lua `InstantReplay.lua` | `Engine/Tests/ReplayTests.cpp`, `TestScriptLibrary` |
| Script graph `Replay/InstantReplay.script.nmgraph` | `Engine/Modules/NodeGraph/Tests/NodeGraphTests.cpp`, `TestReplayLibraryGraph` (`NodeGraph.Tests`) |

If you change one, change the other. The short fragments under "A match replay"
and "Choosing what to record" use the same calls but aren't compiled
on their own.

---

## How it works, in one minute

```
 ReplayRecorder ──Capture()──▶ ReplayClip ──SaveReplayClip()──▶ .nmreplay file
   (a TP_PostUpdate system,     keyframes + deltas,
    or your own calls)          entities by GUID
                                    │
                                    ▼
                               ReplayPlayer ──Seek(t)──▶ writes the state into a World
```

- **Recorder.** Every sample (30 a second by default) it serializes each
  recorded entity's components. Every `KeyframeInterval` seconds it stores
  everything (a **keyframe**). Between keyframes it stores only what changed,
  compared byte for byte (a **delta**).
- **Clip.** Plain data. Entities are named by `EntityGuid`, component types by
  name, and entity references by GUID, so a clip plays into another run or
  another machine.
- **Player.** `Seek(t)` rebuilds the state from the keyframe at or before `t`
  and writes it into the World. Playing forward applies one delta at a time.
  Between samples it blends Transforms; every other field jumps from one
  sample to the next.
- **What a recording includes:** every live entity that isn't Static (and
  passes your filter), its name, parent and mobility, and each component whose
  type has a serializer (and that you haven't excluded).
- **What it leaves out:** events, sounds, particles, prefab membership and
  scripts.

---

## Pick a pattern first

| You want | Play it back | Hold | Record |
|---|---|---|---|
| **Kill cam / instant replay**: the last few seconds, shown in the running game | over the live level | **on** | `RecordOnly({"Transform","Render","Animation"})`, a rolling `MaxDuration` |
| **Match replay** saved to a file and watched from a menu | into the level loaded fresh, with no gameplay running | off | everything (the default), or what is seen |
| **Bug capture**: the last 30 s, attached to a report | anywhere; it's data | off | everything, rolling |

**Hold** decides who wins each frame. If the game's own systems keep running
while you play a clip (a kill cam inside a live match), they move things every
frame. With `SetHold(true)` every seek rewrites every recorded entity in full,
so the clip wins. Without hold, a seek writes only what changed. That's
cheaper, and correct when nothing else is moving the entities.

**Record what is seen** when you play over a live level. Transform, Render,
Animation and lights are enough for the picture. Recording gameplay components
(health, movers, interactables, sequence players) and writing them back would
rewind the game's own state under it.

---

## C++

### A kill cam

```cpp
#include "Public/GameCore/Replay.h"

/* The last 5 seconds always in memory; Show() plays them back over the live
 * level, and Finish() hands the level back and starts recording again. */
class KillCam
{
public:
    void Begin(GameCore::World& InWorld)
    {
        TheWorld = &InWorld;
        StartRecording();
    }

    /* From gameplay code: a game mode, a script, any system. */
    void Show()
    {
        Recorder.reset();   // stops it; the clip stays
        Player = std::make_unique<GameCore::ReplayPlayer>(*TheWorld, Clip);
        Player->SetHold(true);   // the clip wins over the live level every frame
        Time = Clip.GetStartTime();
        bDone = false;
        TheWorld->RegisterSystem("KillCam", GameCore::TP_PostUpdate, &KillCam::Tick, this, GameCore::SO_BeforeScripts);
    }

    bool IsDone() const { return bDone; }

    void Finish()
    {
        TheWorld->UnregisterSystem(&KillCam::Tick, this);
        Player->DestroySpawnedEntities();   // puppets of what had died since
        Player.reset();
        StartRecording();
    }

private:
    void StartRecording()
    {
        GameCore::ReplayRecordSettings Settings;
        Settings.MaxDuration      = 5.0f;   // a rolling buffer
        Settings.SampleRate       = 30.0f;
        Settings.KeyframeInterval = 1.0f;   // short clips: fast seeks over small files
        Recorder = std::make_unique<GameCore::ReplayRecorder>(Clip, Settings);   // clears the clip
        Recorder->RecordOnly({ "Transform", "Render", "Animation" });
        Recorder->Start(*TheWorld);
    }

    /* Late in TP_PostUpdate, ahead of RenderCore.Animation: after every
     * gameplay system, so the replay has the last word on what is drawn. */
    static void Tick(GameCore::World&, const float InSeconds, void* InUserData)
    {
        KillCam& Self = *static_cast<KillCam*>(InUserData);
        Self.Time = std::min(Self.Time + double(InSeconds) * 0.5, Self.Clip.GetEndTime());   // half speed
        Self.Player->Seek(Self.Time);
        Self.bDone = Self.Time >= Self.Clip.GetEndTime();
    }

    GameCore::World*                          TheWorld = nullptr;
    GameCore::ReplayClip                      Clip;
    std::unique_ptr<GameCore::ReplayRecorder> Recorder;
    std::unique_ptr<GameCore::ReplayPlayer>   Player;
    double                                    Time  = 0.0;
    bool                                      bDone = false;
};
```

A game mode owns one. It calls `Begin` in `OnStart`, `Show` when the player
dies, and `Finish` from `OnTick` once `IsDone()`. While `Show` runs, hold
the avatar's input (an input filter that clears the snapshot), or the live
player keeps playing under the replay.

**Where the playback system runs matters.** `TP_PostUpdate` with
`SO_BeforeScripts` comes after every gameplay system (the avatar and its
camera, Character Mover, physics, sequences) and before `RenderCore.Animation`,
so the animation pass sees the replayed Animation parameters. If playback ran
any earlier, gameplay would overwrite it in the same frame. If it ran after
`RenderCore.Animation`, characters would slide around in their live pose.

### A match replay: save, load, watch

```cpp
// At the end of the match (the clip a ReplayRecorder filled):
Clip.LevelPath = "L1_Foothills";                       // free text: what to load before playing
GameCore::SaveReplayClip("Saved/Replays/Match.nmreplay", Clip);

// The viewer, later: load Clip.LevelPath fresh, don't start gameplay, then
GameCore::ReplayClip Saved;
if(GameCore::LoadReplayClip("Saved/Replays/Match.nmreplay", Saved))
{
    Viewer = std::make_unique<GameCore::ReplayPlayer>(World, Saved);   // both must outlive it
    Viewer->Play();
}

// Every frame:
Viewer->Advance(DeltaSeconds);                          // pauses at either end
// A scrub bar at Fraction (0..1):
Viewer->Seek(Saved.GetStartTime() + Fraction * Saved.GetDuration());
// Speed: SetRate(0.25) slow motion, SetRate(-1) rewinds; SetLooping(true)
```

Played into a freshly loaded level, the player takes over the level's own
entities by GUID: a placed door or a lift. It spawns a plain entity (a
**puppet**) for anything the level doesn't have, such as a projectile or a
spawned enemy. `FindEntity(Guid)` gives you the live entity playing a GUID, for
example to aim a camera at the replayed player. Fields that the serializer
skips (render proxies, physics handles) are never written; the live component
keeps its own.

### Recording without the built-in system

`Recorder.Start(World)` samples at `SampleRate` from a `TP_PostUpdate` system,
using the World's own tick time. To decide when frames are taken yourself (on
a fixed network tick, or only while something interesting happens), call
`Capture(World, Time)` instead, with times that increase.

### Choosing what to record

```cpp
Recorder.RecordOnly({ "Transform", "Render", "Animation", "Point Light" });   // by type name
Recorder.ExcludeComponentType(MyBookkeepingComponent::StaticType().ID);      // or one by one
Recorder.SetEntityFilter([](GameCore::World& W, GameCore::EntityID E, void*)
{
    return std::strncmp(W.GetEntity(E).GetName(), "Sequence ", 9) != 0;      // not the sequencer's cameras
}, nullptr);
```

Static entities are skipped unless `bRecordStatic`, because level geometry
doesn't move. A component type with no serializer is never recorded. Give it
one (`GAMECORE_DEFINE_COMPONENT_SERIALIZER`) if it affects what is seen.

---

## Lua: the `Replay` library

The `Replay` script library gives every World **one recording and one
playback**, from any script. Playback runs in a `TP_PostUpdate` system ahead
of the phase's scripts, the same place as the C++ example's. The library's
state is released when the World goes away.

| Function | Does |
|---|---|
| `Replay.RecordOnly(types)` | Comma-separated component type names to record from the next `StartRecording`; `""` records everything |
| `Replay.StartRecording(maxSeconds, sampleRate)` | Starts a new recording, replacing the clip. `maxSeconds > 0` keeps only the last that many seconds; a `sampleRate` of `0` means 30 a second |
| `Replay.StopRecording()` | Stops recording. The clip stays, so you can play or save it |
| `Replay.Save(path)` / `Replay.Load(path)` | Write or read a `.nmreplay`; both return `ok`. A failed load leaves no clip |
| `Replay.Play(hold)` | Plays from the start. `hold = true` to play over a live level |
| `Replay.SetRate(rate)` | `0.5` slow motion, `-1` rewinds, `0` freezes |
| `Replay.Seek(seconds)` | Seconds from the clip's start |
| `Replay.Pause()` / `Replay.Resume()` / `Replay.SetLooping(loop)` | |
| `Replay.Stop(destroySpawned)` | Stops playback. `true` removes the puppets it spawned |
| `Replay.IsRecording()`, `Replay.IsPlaying()`, `Replay.GetTime()`, `Replay.GetDuration()`, `Replay.GetSizeBytes()` | Queries |
| event `Replay.ReplayFinished` | Playback reached the end, or the start when rewinding, without looping. It holds that frame until `Stop` or `Play` |

### An instant replay on a goal

`InstantReplay.lua`, a prefab instance's script:

```lua
-- InstantReplay.lua: keeps the last 10 seconds. On "Match.Goal" it saves
-- them and plays them back at half speed over the live level, then goes
-- back to recording when the replay ends.
return {
    OnStart = function(self)
        Replay.RecordOnly("Transform,Render,Animation")   -- what is seen, not gameplay state
        Replay.StartRecording(10, 30)                     -- the last 10 s, 30 samples a second
        self.OnGoal = Events.On("Match.Goal", function() self:ShowReplay() end)
        self.OnDone = Events.On(Replay.ReplayFinished, function() self:BackToPlay() end)
    end,

    OnDestroy = function(self)
        Events.Off(self.OnGoal)
        Events.Off(self.OnDone)
    end,

    ShowReplay = function(self)
        Replay.StopRecording()
        Replay.Save("Saved/Replays/LastGoal.nmreplay")
        Replay.Play(true)       -- hold: the clip wins over the live level every frame
        Replay.SetRate(0.5)
    end,

    BackToPlay = function(self)
        Replay.Stop(true)       -- and remove anything playback spawned
        Replay.SetRate(1)
        Replay.StartRecording(10, 30)
    end,
}
```

Anything can raise `Match.Goal`: C++ (`World.GetEvents().Emit("Match.Goal")`),
another script (`Events.Emit("Match.Goal")`), a trigger, or a Sequencer Event
key. The game still has to stop the live player from acting during the replay,
as in the C++ example.

---

## Script graphs

The same library is a set of nodes under **Replay** in the Script palette:
every function above is a statement node, every query a value node, and
**On ReplayFinished** an event node. Nothing extra needs to be set up; a
library's nodes come with it (GameCore `ScriptLibrary.h`).

[`Replay/InstantReplay.script.nmgraph`](Replay/InstantReplay.script.nmgraph)
is the Lua example built as a graph. Open it in the editor, or set it as a
prefab's script. It has three chains:

```
 On Start ─▶ Record Only ("Transform,Render,Animation") ─▶ Start Recording (10, 30)

 On Custom Event "Match.Goal" ─▶ Stop Recording ─▶ Save ("Saved/Replays/LastGoal.nmreplay")
                               ─▶ Play (Hold = true) ─▶ Set Rate (0.5)

 On ReplayFinished ─▶ Stop (Destroy Spawned = true) ─▶ Set Rate (1) ─▶ Start Recording (10, 30)
```

Its generated Lua (the `<gen_code>` in the file) makes the same calls as the
hand-written script, in the same order.

---

## Directing a replay with the Sequencer

For a presented replay (a highlight reel, an attract mode, a trailer), let a
sequence drive playback, so timing is authored rather than coded:

- Give a small component of your own `Rate` and overlay-opacity fields, bind
  them for scripts (`GAMECORE_DEFINE_COMPONENT_SCRIPT`), and put it on an
  entity the sequence possesses by name.
- Key them with **Float** tracks: eased ramps into slow motion, a rewind, a
  freeze.
- Your playback system reads `Rate` each frame and advances its own clock.
- **Event** keys jump the playhead and put up captions. **Subtitle** tracks
  narrate, and a **Fade** track covers the cut into the replay.

`Games/DemoLab` does exactly this:

- `Source/GameModes/LabReplayDemo.cpp` runs the demo, with
  `Content/Cinematics/ReplayDemo.nmseq` directing it and
  `Content/UI/ReplayOverlay.nmui` as its overlay.
- To run it:
  `DemoLab -Game.StartLevel=L1_Foothills -DemoLab.AutoPlay=1 -DemoLab.ReplayDemo=1`.
- An Event key's string arguments hold 63 characters each, so long captions
  are split across `arg1`..`arg3`.

---

## Size and cost

Measured in DemoLab's L1: 144 entities recording Transform, Render, Animation
and lights at 30 Hz, with a keyframe every 2 s, came to **448 KB for 20 s**,
about 22 KB/s. That is uncompressed, and the file is the same size.

| Setting | Effect |
|---|---|
| `KeyframeInterval` up | Keyframes cost less, in proportion; a seek applies more deltas |
| `SampleRate` down | Deltas cost less, in proportion; movement still blends, other fields jump between samples |
| `RecordOnly` / filters | Fewer bytes in every keyframe. Render's mesh and material paths are long strings |
| `MaxDuration` | Caps memory at roughly that many seconds of the above |

Every keyframe stores every recorded entity in full. Between keyframes, an
entity that doesn't change costs nothing. The format isn't compressed yet.

With hold on, every seek decodes every recorded entity. That is fine for the
few hundred entities a kill cam needs. For a whole large level, prefer
playback into a fresh load with hold off.

---

## Pitfalls

- **Play from the right place.** Hold-mode playback belongs in `TP_PostUpdate`
  with `SO_BeforeScripts`. Any earlier and gameplay overwrites it in the same
  frame; any later and the animation pass has already run.
- **Starting or stopping from inside a system is fine.** `Stop()` (or
  `World::UnregisterSystem`) from inside any system, in any phase, takes
  effect at once: a recorder stopped in `TP_PostUpdate` takes no frame later
  in that tick. One started during a phase's walk first samples on the next
  tick.
- **Hold the live player.** Playback over a live level doesn't pause the
  game. Clear the local player's input while a replay plays, and keep AI from
  hurting the player in the meantime.
- **Characters run their animation forwards when you rewind.** Animation
  *parameters* are recorded, not poses, so the state machine keeps playing
  forwards while the character moves backwards.
- **Puppets are plain entities.** They have the recorded components and no
  prefab script. Call `DestroySpawnedEntities()` (or `Replay.Stop(true)`)
  before handing the World back.
- **One clip per World in Lua and script graphs.** For several at once (a
  rolling bug buffer as well as a kill cam), use the C++ classes.
- **Play a clip into the level it was recorded in.** An entity the clip never
  mentions is left alone. Playing into a different level only shows the
  puppets.
