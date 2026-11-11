# Parallel Story / Co-Op mode ("Echo" mode): design

*Written 2026-11-10 against engine merge `5c044a87` (GameCore state replays, NetCore networked level travel).*
*Implementation steps are in [ParallelStoryMode_ImplementationPlan.md](ParallelStoryMode_ImplementationPlan.md).*

## 1. Premise

You play **Story A** through a level and complete its goals while the engine records your run. Finishing A unlocks **Story B**: a new character on a parallel storyline who sees A's run playing out beside them as an "echo". In **co-op**, both storylines run live at the same time.

Decisions this design is built on:

| Question | Decision |
|---|---|
| How does B see A? | **Hybrid.** The echo plays back exactly as recorded. If B disturbs it, AI takes the ghost over until it can rejoin the recording. |
| Can B change what happened in A? | **No.** A's run is fixed history. B's levels are written around A's recorded outcomes. |
| Co-op | Local split-screen first, online later. |

## 2. Concepts

- **Timeline:** A or B. Each chapter belongs to one timeline. A B chapter names the A chapter it runs alongside and a time offset.
- **Run:** what is kept from one completed A chapter: a list of replay segments plus an anchor track (section 5). Only the **canonical run** is kept, meaning the one that reached completion.
- **Anchor:** a story-critical event from A, such as a gate signal, a boss kill or an objective. In B, anchors are re-emitted as signals on schedule, whatever the ghost is doing. This is what makes the past read-only: B's world always ends up matching A's history.
- **Flavor event:** a non-critical event such as a shot, a footstep sound or a taunt. It plays at the ghost and changes nothing.
- **Echo Ghost:** A's avatar as B sees it. It is a replay puppet most of the time, with a real bot avatar standing by for the hybrid takeover (section 6).

## 3. What the engine provides, and what it doesn't

| Need | Engine | Left to BattlePunks |
|---|---|---|
| Record and play back state | `ReplayRecorder`, `ReplayClip`, `ReplayPlayer` in `GameCore/Replay.h`; `.nmreplay` files; entities by `EntityGuid`; Transform blending | Choosing what to record; the run clock |
| Events in a recording | Nothing. Replays record no events, sounds or particles | The anchor track |
| Rewinding a recording at a checkpoint | Nothing. `ReplayClip` has no truncate call | Recording in segments |
| Pausing one entity's playback | Nothing. Hold mode rewrites every recorded entity | The puppet/bot swap |
| Format versioning | Each component's bytes are in a versioned chunk read by its serializer | `PunkVersion.h` for the anchor track and profile |
| Local co-op | Split-screen, up to 4 players, drop-in (`Source/SPLITSCREEN.md`) | Assigning timelines to players |
| Online level travel | The server's `OpenLevel` carries its clients; `IGameMode::OnClientTravelled` | A ready gate, because the server doesn't wait for slow clients |
| Lobby, session discovery | Nothing | Connect by address for now |
| Objectives, unlocks, profile | Nothing | Signals plus `GameState`; a profile file |
| Two Worlds at once | Nothing. `GameSession` has one active level | Chapter pairs that share a World |

The replay system records **state, not input**, so nothing has to be deterministic. That matters here: the gameplay tick is variable and Jolt runs on several threads, so replaying inputs would drift.

## 4. Project and mode structure

The layout follows DemoLab, whose source says it was ported from BattlePunks (`Lab*` there, `Punk*` here).

### Source folders
DemoLab's folders (`Core/`, `Weapons/`, `AI/`, `World/`, `Flow/`, `UI/`, `GameModes/`) plus one new one:

| `Source/Timeline/` file | Holds |
|---|---|
| `PunkRun` | A run on disk: the segment index, the anchor track, load and save |
| `PunkRunRecorder` | Wraps `ReplayRecorder`; owns the run clock, segments and anchor capture |
| `PunkAnchors` | The anchor/flavor event types, their serialization, and re-emission in B |
| `PunkEcho` | Playback in B and the Echo/Live state machine |
| `PunkProfile` | Unlocks and pointers to canonical runs |

### Game modes
One shared `PunkStoryModeBase` carries what DemoLab's `LabStoryMode` does today: objectives, banners, travel state, checkpoints. Three modes are registered on top of it with `NIMBLE_REGISTER_GAME_MODE`:

| Mode | Does |
|---|---|
| `PunkStoryA` | Plays Story A and records the run |
| `PunkStoryB` | Plays A's run back as the echo, re-emits anchors, and records B's run |
| `PunkStoryCoop` | Both players live, both recorded; anchors come from real actions |

A world file names one mode in `<Settings GameMode=...>`. Since B and co-op reuse A's worlds, the world names the base story mode and the travel state (`LabTravel`'s key/value flags) carries `Story=A|B|Coop`, which the mode reads in `OnLoad` to switch behavior. The implementation plan covers the two ways to do this.

### Chapters
`LabFlow::Chapter` is `{ Title, World, Entry, Flags }`. It gains:

| Field | Meaning |
|---|---|
| `Timeline` | A or B |
| `ParallelOf` | For a B chapter, the index of the A chapter it runs alongside; -1 for none |
| `OffsetSeconds` | Where on A's run clock this B chapter starts |

B chapters reuse A's worlds through **state groups**, the way `LabLogic::ApplyStateGroups` already drops `Calm_` or `Alarm_` entities. Entities get a timeline prefix (`A_`, `B_`, or none for both).

### The one-World limit
Only one World is active, so a ghost appears only while B is in the same world as A was at that moment. When B is elsewhere, anchors still fire and show up as distant effects or radio chatter. Chapter pairs meant for co-op must share one World.

## 5. Recording

### What is recorded
`ReplayRecorder` with:
- `RecordOnly({"Transform", "Render", "Animation"})`: what is seen, not gameplay state.
- An entity filter (`SetEntityFilter`) that passes only A's avatar, what it carries, and its projectiles.

The level's own doors, lifts and enemies are **not** recorded. B's world rebuilds them by reacting to anchors through its normal gameplay. Writing A's gameplay components back into B's live level would rewind B's game state under it, which `Engine/Docs/Guides/ReplayGuide.md` warns against.

### The run clock
The recorder is driven by `Capture(World, Time)` with the mode's own run clock, not by `Start()`. Anchors are stamped with the same clock, so the two tracks stay aligned. The clock pauses during cinematics and while the game is paused, and is saved in the checkpoint.

### Settings
20–30 Hz samples, a keyframe every 2 seconds, `MaxDuration = 0` (keep everything). The guide measured 22 KB/s for 144 entities; a handful of entities is far smaller.

### Segments
`ReplayClip` cannot be cut back, and a `ReplayRecorder` clears its clip when constructed. So a run is a list of clips:
- Each checkpoint closes the current clip (`SaveReplayClip`) and starts a new recorder on a new clip.
- Reloading a checkpoint discards the open segment and starts a new one from the checkpoint's clock.
- The run index lists segments in order with their start and end times.
- The segment count, run clock and anchor cursor are written into the checkpoint through `OnPreSaveGame` and restored in `OnPostLoadSave`.

### The anchor track
A small file of our own beside the clips, an `ISerializable` versioned with `PunkVersion`. The recorder subscribes to the events the story already uses (`Lab.Signal`, `Lab.Damaged`, `Objective.Set`, `Encounter.*`, pickups, interactables) and writes each with its run-clock time and a tag: anchor or flavor.

Which signals are anchors is authored per chapter, as a list of channel names or a naming rule (for example every `Story.*` channel).

### Stable identity
Anchors name their targets so that another run can find them:
- Entities placed in the level: by `EntityGuid`.
- Enemies spawned at runtime: by encounter Id, wave and slot (`LabEncounter`). `EntityGuid::Generate` is random, and `EntityID` changes from run to run, so neither works for spawned entities.

### Level identity
`ReplayClip::LevelPath` holds the chapter's world name. The run index adds a hash of the level file, so a run recorded against an older layout can be detected.

## 6. Playback: the hybrid ghost

### Echo (default)
- A `ReplayPlayer` on the current segment with `SetHold(true)`, since B's level is live.
- It seeks from a system at `TP_PostUpdate` with `SO_BeforeScripts`. The guide requires this slot: earlier and gameplay overwrites the ghost in the same frame, later and the animation pass has already run.
- A's avatar GUID isn't in B's level, so the player spawns it as a **puppet**: a plain entity with the recorded components and no collider or script.
- `FindEntity(Guid)` gives the puppet, which is where flavor events play.
- An invisible **proxy** (a sensor and a hit target) follows the puppet so B can block or hit it.

### Live (takeover)
Blocking the proxy's path, hitting it, or crowding it switches the ghost to Live:
1. The puppet is hidden. The replay clock keeps running underneath.
2. A real bot avatar is teleported to the puppet's pose (`TeleportCharacter`).
3. The bot is driven through the normal input path (`SteerInput`, or DemoLab's `LabDrive::WalkTo`) toward the recorded position a few seconds ahead.

### Reconvergence
When the bot is back within tolerance of the puppet, the two swap back. If it isn't after a set time, a "temporal flicker" effect covers a snap to the recording.

### Rules that keep the past read-only
- **The ghost cannot die.** Damage makes it flinch or flicker.
- **Anchors fire from the track, not from the ghost.** A door A opened opens at its recorded time even if the ghost is Live and nowhere near it.
- **B waits on A.** If B reaches something A hasn't done yet, it stays locked until its anchor fires: "the door opens when A opens it". Waiting is gated by anchors, never by time alone.

### Limits
- Animation parameters are recorded, not poses, so the ghost's state machine always runs forward. The echo never rewinds, so this is fine.
- Call `DestroySpawnedEntities()` before travel or a checkpoint load, or the puppet leaks into the save.
- B's own checkpoint must save the echo clock, so reloading puts the ghost back where it was.

## 7. Unlocks and profile

`PunkProfile` is a file at `Saved/BattlePunks/Profile.nmprofile`, an `ISerializable` versioned with `PunkVersion`. It holds:
- Whether Story A is complete, and which chapters each timeline has unlocked.
- For each A chapter, the path of its canonical run.

A run becomes canonical when its chapter completes. Until then it lives in a working folder and is thrown away if the player quits to the menu.

**Fallback run:** each chapter ships an authored default run. It is used when the player's run is missing, fails to load, or was recorded against a different level hash.

## 8. Versioning

DemoLab versions every save format through one enum (`Source/LabVersion.h`), with a new entry above `Current` for each release. BattlePunks does the same with `PunkVersion.h`:

- Component serializers, `GameState`, the anchor track, the run index and the profile all use `PunkVersion::GetCurrentVersion()`.
- A new field is read behind `if(Ar.GetVersion() >= N)`.
- Replay clips need no version of their own. Component bytes are stored in versioned chunks and read by the same serializers.
- A run whose anchor track is older than the minimum supported version falls back to the authored run.

### Milestones

| Version | Adds |
|---|---|
| v0.1 | Record A; play the ghost in one A→B chapter pair |
| v0.2 | Anchors, waiting on A, checkpoint segments, profile and unlock |
| v0.3 | Hybrid Live takeover and reconvergence |
| v0.4 | Local split-screen co-op |
| v0.5 | Online co-op |

## 9. Co-op

### Local (v0.4)
- The engine's split-screen: `JoinLocalPlayer`, drop-in.
- Player 0 is A and player 1 is B, each with their own avatar prefab and Player Start.
- The chapter pair shares one World, with both timelines' state groups kept.
- Anchors are live: B waits on A's real actions, and A can wait on B's.
- Both runs are recorded, so a later solo replay can use a partner's run as the echo.

### Online (v0.5)
- A NetCore listen server; the second player connects by address (`-connect=host:port`).
- The server's `OpenLevel` takes clients along, with the `LevelTravel` values that carry the story flags. A connected client cannot change level itself, so travel triggers must run on the authority only.
- `OnClientTravelled` gives each client its timeline and pawn again in the new level. Timeline assignment must survive travel, so it is keyed on the player's index in `PlayerStateComponent`, which the engine carries over.
- **Ready gate:** the server doesn't wait for slow clients, so `PunkStoryCoop` holds a "waiting for partner" phase until every client has travelled. Cinematics and the run clock start after that.
- The server records the run. Playing an echo over the network is out of scope here.

## 10. Risks and open questions

| Risk | Handling |
|---|---|
| The level changed since the run was recorded | Level hash check; fall back to the authored run |
| The puppet looks wrong because Render or Animation serializers changed | Serializer version checks; fall back if the clip fails to load |
| The Live bot can't get back to the recording (blocked for good) | Timed flicker-snap |
| B's pacing: waiting on A is dull | Author B's objectives so A's anchors land while B is busy; `OffsetSeconds` per chapter |
| Recording cost on large encounters | The entity filter keeps the recorded set small; clips are uncompressed |
| A long A chapter means a long run in memory | Segments are loaded one at a time |

Open questions for later:
- Does B's run get an echo too, for a third storyline or for A replayed with B visible?
- In co-op, what happens when one player dies: shared checkpoint or independent respawn?
- Should the player be able to re-record A after B is unlocked, and does that reset B's progress?

## 11. References

- Replays: `Engine/Modules/GameCore/Source/Public/GameCore/Replay.h`, `Engine/Docs/Guides/ReplayGuide.md`, `Engine/Tests/ReplayTests.cpp`.
- Level travel: `Engine/Modules/NetCore/README.md` ("Level travel"), `Engine/Source/Public/NimbleEngine/{GameMode,GameSession,NetHost}.h`.
- Framework: `Engine/Source/GAMEFRAMEWORK.md`, `Engine/Docs/Guides/GameModeGuide.md`, `Engine/Source/SPLITSCREEN.md`, `Engine/Source/Public/NimbleEngine/{Avatar,Input,SaveGame,CharacterMover,AIFacade}.h`.
- DemoLab: `Source/GameModes/LabStoryMode.cpp`, `Source/Flow/{LabFlow,LabTravel}.h`, `Source/World/{LabSignals,LabLogic,LabEncounter}.h`, `Source/GameModes/LabInputDriver.h`, `Source/AI/LabBots.h`, `Source/LabVersion.h`.
- The Replay guide also names DemoLab's `Source/GameModes/LabReplayDemo.cpp`. The unpacked `Games/DemoLab` in this checkout predates it; re-unpack from `Engine/Examples` to get it.
