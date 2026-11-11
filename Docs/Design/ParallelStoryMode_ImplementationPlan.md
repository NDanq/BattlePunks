# Parallel Story / Co-Op mode: implementation plan

*Written 2026-11-10 against engine merge `5c044a87`. The design is in [ParallelStoryMode.md](ParallelStoryMode.md).*

Each milestone ends in something you can run. Names starting `Punk` are new; everything else exists in the engine or in DemoLab today. Where a step leans on engine behavior I haven't seen run, it is marked **Check first**.

## Before v0.1: project setup

1. **Create the game project** from `Engine/Templates/Game` (File > New Project, or `NewProject.sh`). `Games/` is untracked in this repo, so decide where the BattlePunks game is versioned before writing code.
2. **Re-unpack DemoLab** from `Engine/Examples` (`NewProject.sh --example DemoLab`). The copy in `Games/DemoLab` predates the replay work and lacks `Source/GameModes/LabReplayDemo.cpp`, which is the engine's own worked example of recording and hold-mode playback.
3. **Bring over DemoLab's story stack** as the starting point, renamed `Lab*` to `Punk*`:
   - `Source/LabVersion.h` → `Source/PunkVersion.h`
   - `Source/Flow/{LabFlow,LabTravel}`
   - `Source/World/{LabSignals,LabLogic,LabEncounter,LabInteractable}`
   - `Source/GameModes/{LabGameModeBase,LabStoryMode,LabInputDriver}`
   - `Source/AI/LabBots`
4. **Split `LabStoryMode`** into `PunkStoryModeBase` (everything it does now) and a thin `PunkStoryA` registered with `NIMBLE_REGISTER_GAME_MODE`. Confirm the game plays exactly as before.
5. **Add source files to `CMakeLists.txt`.** `nimble_add_hot_module` lists every file by hand.

## v0.1: record A, see the ghost in B

Goal: finish one A chapter, start its B chapter, and watch A's avatar run through the level.

### 1. `Source/Timeline/PunkRunRecorder`
- Owns a `GameCore::ReplayClip`, a `std::unique_ptr<GameCore::ReplayRecorder>` and a `double RunClock`.
- `Begin(World, AvatarGuid)`:
  - Build `ReplayRecordSettings` with `SampleRate = 30`, `KeyframeInterval = 2`, `MaxDuration = 0`.
  - Call `RecordOnly({"Transform", "Render", "Animation"})`.
  - Call `SetEntityFilter` with a function that passes the avatar, its children (walk the parent chain) and its projectiles (by owner).
- `Tick(World, DeltaSeconds)`: advance `RunClock`, and at the sample rate call `Capture(World, RunClock)`. Don't use `Start()`; the clock must be yours so anchors can share it.
- `End(Path)`: set `Clip.LevelPath` to the world name and call `SaveReplayClip`.
- Register `Tick` from the mode with `AddSystem<&PunkStoryA::RecordTick>("Punk.Record", TP_PostUpdate)`.

**Check first:** that the avatar entity has an `EntityGuid` at all, and that the Render and Animation components on your character have serializers (`GAMECORE_DEFINE_COMPONENT_SERIALIZER`). A component without one is silently not recorded.

### 2. `Source/Timeline/PunkEcho` (Echo state only)
- Owns the loaded `ReplayClip`, a `ReplayPlayer` and an `EchoClock`.
- `Begin(World, ClipPath, OffsetSeconds)`: `LoadReplayClip`, construct the player, `SetHold(true)`.
- A system registered with `World::RegisterSystem("Punk.Echo", GameCore::TP_PostUpdate, &Tick, this, GameCore::SO_BeforeScripts)`. Each tick: advance `EchoClock` and call `Player.Seek(Clip.GetStartTime() + EchoClock)`.
- `End()`: `DestroySpawnedEntities()`, unregister the system.
- Call `End()` in the mode's `OnUnload` and before any checkpoint save.

**Check first:** what the puppet looks like. It is a plain entity with Transform, Render and Animation. If your character's mesh hangs off child entities, the filter in step 1 must include them and the header (name, parent) must round-trip.

### 3. `PunkStoryB` and the chapter table
- Add `Timeline`, `ParallelOf` and `OffsetSeconds` to `PunkFlow::Chapter`; bump `CHAPTER_COUNT`.
- Register `PunkStoryB`, derived from `PunkStoryModeBase`. In `OnStart` it finds the run for `ParallelOf` and calls `PunkEcho::Begin`.
- **Picking the mode.** A world file names one mode. Two ways to let one world serve A and B:
  - *One registered mode* (`PunkStory`) that reads `Story=A|B` from the travel state (`LabTravel::GetValue`) in `OnLoad` and creates a recorder or an echo. Simplest.
  - *Separate world files* per timeline that share content. More files, but each names its own mode.
  
  Start with the first.
- Extend `ApplyStateGroups` from one bool (`bInAlarm`) to a timeline as well: drop `A_` entities in B and `B_` entities in A.
- B needs its own avatar prefab and its own Player Start entry per chapter.

### 4. Test
An autoplay ctest in the style of DemoLab's `LabAutoPlay` (`-DemoLab.AutoPlay=1`): play the A chapter headless, save the run, start the B chapter, and assert that `ReplayPlayer::FindEntity(AvatarGuid)` is valid and within a metre of the recorded position at three sample times.

## v0.2: anchors, waiting, checkpoints, unlock

Goal: B's world changes when A's did. The run survives checkpoint reloads. B is locked until A is finished.

### 1. `Source/Timeline/PunkAnchors`
- `struct PunkEvent { double Time; uint8_t Kind; bool bAnchor; std::string Channel; bool bValue; PunkTarget Target; }`.
- `PunkTarget` is either an `EntityGuid` (placed entity) or `{ EncounterId, Wave, Slot }` (spawned enemy). Add the slot to `LabEncounter`'s spawn bookkeeping if it isn't kept already.
- `class PunkAnchorTrack : ISerializable` with a sorted `std::vector<PunkEvent>`, versioned with `PunkVersion::GetCurrentVersion()`.
- **Recording:** in `PunkStoryA::OnLoad`, `Subscribe<>` to `Lab.Signal`, `Lab.Damaged`, `Objective.Set`, `Encounter.*` and whatever your pickups emit. Append each with the recorder's `RunClock`.
- **Which are anchors:** a per-chapter list of channel names in `Config/DefaultGame.ini`, or a prefix rule. Keep it data, not code.
- **Playback:** `PunkEcho` keeps a cursor. Each tick it emits every event up to `EchoClock`: anchors through `LabSignals::Emit` on the authority, flavor events as sound and effects at the puppet.

**Check first:** `LabSignals` latches the last value per channel and queues signals emitted during handling (`BeginHandling` / `Flush`). Emit anchors from the mode's tick, not from inside another signal's handler.

### 2. Waiting on A
- For each thing B could reach early, the level's gate listens for the anchor's channel. Nothing new in code: this is how DemoLab's gates already work. It is a level-authoring rule.
- Add a HUD hint ("waiting for…") driven by an authored list of gate → anchor pairs, so the wait is readable.

### 3. Segments (`Source/Timeline/PunkRun`)
- `struct PunkRunIndex { std::string World; uint64_t LevelHash; struct Segment { std::string File; double Start, End; }; std::vector<Segment> Segments; }`, serialized next to the anchor track.
- On each checkpoint (the four places `LabStoryMode` saves one): `SaveReplayClip` the open clip, append it to the index, start a new `ReplayRecorder` on a fresh clip.
- Put `SegmentCount`, `RunClock` and the anchor track's size in the story `GameState` (bump its `GetSerialVersion`, read behind `Ar.GetChunkVersion() >= 2`).
- In `OnPostLoadSave`: truncate the index to `SegmentCount`, truncate the anchor track to its saved size, restore `RunClock`, start a new recorder.
- `PunkEcho` plays segments in order: when `EchoClock` passes a segment's `End`, destroy the player and its puppets, load the next clip, seek.
- B's own checkpoint saves `EchoClock` and the anchor cursor; on load, re-seek and re-latch every anchor up to that time.

**Check first:** the puppet's GUID is the same in every segment, but each new `ReplayPlayer` spawns its own puppet. Confirm there is no visible pop at a segment boundary; if there is, overlap by one frame or keep the old puppet until the new one exists.

### 4. `Source/Timeline/PunkProfile`
- `ISerializable` at `Saved/BattlePunks/Profile.nmprofile`, saved with `SaveArchiveToFile`.
- Fields: `bStoryAComplete`, unlocked chapter bitmask per timeline, canonical run folder per A chapter.
- Runs record into `Saved/BattlePunks/Runs/_working/`. On chapter complete, move the folder to `Runs/<Chapter>/` and write the profile.
- Chapter Select reads the profile to show or lock B's chapters.
- Authored fallback runs ship in `Content/Runs/<Chapter>/`. `PunkRun::Load` tries the profile's path, checks the level hash, then falls back.

### 5. Test
- Autoplay: A opens gate G at time T. In B, assert G is closed before T and open after.
- Autoplay: reload a checkpoint mid-chapter in A, finish, and assert the run's duration matches the kept play time, not the total.
- Unit: anchor track and run index round-trip through an `Archive`.

## v0.3: hybrid takeover

Goal: block or shoot the ghost and it reacts, then rejoins its recording.

1. **Proxy.** A prefab with a sensor `RigidBody` and a damageable component. Each tick, after `Seek`, copy the puppet's transform to it. It emits `Punk.GhostDisturbed` on damage, on a blocking overlap, or when the puppet's recorded step passes through something solid (a short sweep along the recorded movement).
2. **Bot avatar.** One instance of A's real character prefab, hidden and inactive, spawned at `PunkEcho::Begin`.
3. **State machine in `PunkEcho`:**
   - `Echo → Live` on `Punk.GhostDisturbed`: hide the puppet (it keeps being written; hide by a parent or a Render flag the clip doesn't record, or filter it out of rendering), `TeleportCharacter` the bot to the puppet's pose, show it.
   - `Live`: each tick take the recorded position at `EchoClock + Lookahead` (read from `Player.FindEntity` after a second, hidden player seeks ahead, or sample the clip's frames directly) and drive the bot with `LabDrive::WalkTo` through the same input path the bots use.
   - `Live → Echo` when the bot is within tolerance of the puppet for half a second.
   - `Live → Flicker → Echo` after a timeout: play the effect, hide the bot, show the puppet.
4. **Invulnerability.** The bot takes no damage; hits trigger a flinch.
5. **Anchors are unaffected.** They already fire from the track.

**Check first:** hold mode rewrites every recorded entity on every seek, including Render. If "hidden" is a Render field, the player will un-hide the puppet each frame. Hide it some way the clip doesn't own, such as a non-recorded component or parenting it under a hidden root.

**Test:** autoplay B, walk into the ghost's path, assert the state goes Live and returns to Echo within the timeout, and that the gate anchor still fires on time.

## v0.4: local split-screen co-op

Goal: two players on one machine, A and B at once.

1. **`PunkStoryCoop`** (or `Story=Coop` in the travel state). Keeps both timelines' state groups.
2. **Players.** Read `Engine/Source/SPLITSCREEN.md` first. In `OnLocalPlayerJoin`, give player 0 the A prefab and A's entry, player 1 the B prefab and B's entry. Override `CanPossess` so they can't swap.
3. **No echo.** Don't create `PunkEcho`. Anchors come from the live `Lab.Signal` stream, which both players' gates already hear.
4. **Two recorders.** One `PunkRunRecorder` per avatar, each with its own filter, sharing one `RunClock` and one anchor track with a `Timeline` field per event.
5. **Checkpoints and death.** Decide the rule (the design lists it as open). The simplest is a shared checkpoint: either death reloads both.
6. **Drop-out.** If player 1 leaves mid-chapter, either pause, or swap B to the authored fallback run's echo from the current `RunClock`.
7. **Chapter pairs.** Only chapters whose A and B share a World are offered in co-op. Add a `bCoop` flag to the chapter table.

**Test:** autoplay with two input drivers; assert both avatars reach their goals and two runs are written.

## v0.5: online co-op

Goal: the same as v0.4 across two machines.

1. **Config.** Enable NetCore in `[Modules]`. Build the server launcher (`nimble_add_server_launcher`) or run a listen server with `-listen`; the partner uses `-connect=host:port`.
2. **Authority.** Go through the story code and put `HasAuthority()` in front of everything that changes state: signal handling, travel, checkpoints, recording. Signals' effects must be on replicated components.
3. **Travel.** `LabTravel::Travel` calls `OpenLevel`; on the server that now takes clients along with the travel values. A client's own call is refused and logged, so make sure no trigger runs it client-side.
4. **`OnClientTravelled(Client, Player)`.** Assign the timeline from the carried-over `PlayerStateComponent` index, then spawn the right prefab at the right entry. Don't rely on the default (`OnClientConnected`) if joining and travelling should differ.
5. **Ready gate.** Add a phase ahead of play. Enter it in `OnLoad`; leave it when `OnClientTravelled` or `OnClientConnected` has fired for every expected client. Start the opening cinematic and `RunClock` only then. Add a timeout that falls back to solo with the echo.
6. **Recording** runs on the server only.
7. **Prediction.** Each client's own avatar is predicted as DemoLab's already is; nothing new.
8. **Not included:** a lobby, invites, or replaying an echo to remote clients.

**Test:** a loopback test in the style of `Engine/Modules/NetCore/Tests/LoopbackTests.cpp`: server and one client, travel between two chapters, assert the client arrives with the same timeline and that the run clock didn't start before it was ready.

## Order of risk

Do these early, because each can change the design:

| Check | Why | Milestone |
|---|---|---|
| The puppet renders and animates correctly from Transform/Render/Animation alone | The whole echo depends on it | v0.1, step 2 |
| Hiding a puppet under hold mode | Decides how the takeover swap works | v0.3, before step 3 |
| Segment boundaries don't pop | Decides whether segments are per checkpoint or need overlap | v0.2, step 3 |
| Spawned enemies can be named stably | Kill anchors depend on it | v0.2, step 1 |
| Two local players can have different prefabs and entries | Co-op depends on it | v0.4, step 2 |

## File summary

| New file | Milestone |
|---|---|
| `Source/PunkVersion.h` | setup |
| `Source/GameModes/PunkStoryModeBase.{h,cpp}`, `PunkStoryA.cpp` | setup |
| `Source/Timeline/PunkRunRecorder.{h,cpp}` | v0.1 |
| `Source/Timeline/PunkEcho.{h,cpp}` | v0.1, extended in v0.2 and v0.3 |
| `Source/GameModes/PunkStoryB.cpp` | v0.1 |
| `Source/Timeline/PunkAnchors.{h,cpp}` | v0.2 |
| `Source/Timeline/PunkRun.{h,cpp}` | v0.2 |
| `Source/Timeline/PunkProfile.{h,cpp}` | v0.2 |
| `Source/GameModes/PunkStoryCoop.cpp` | v0.4 |
| `Content/Runs/<Chapter>/` (authored fallback runs) | v0.2 |

| Changed file (from DemoLab's equivalents) | Change |
|---|---|
| `Flow/PunkFlow.{h,cpp}` | `Timeline`, `ParallelOf`, `OffsetSeconds`, `bCoop` on `Chapter` |
| `World/PunkLogic.{h,cpp}` | `ApplyStateGroups` takes a timeline |
| `World/PunkEncounter.{h,cpp}` | Stable wave/slot IDs for spawned enemies |
| Story `GameState` | Run clock, segment count, anchor size, echo clock; serial version 2 |
| `Config/DefaultEngine.ini` | Worlds for B chapters; NetCore in v0.5 |
| `Config/DefaultGame.ini` | Anchor channel lists per chapter |
| `CMakeLists.txt` | Every new source file |
