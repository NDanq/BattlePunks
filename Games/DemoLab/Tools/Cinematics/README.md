# Demo Lab cinematics

The six cutscenes of Docs/Design.md ("Cinematics"), generated:

```
python3 Tools/Cinematics/make_cinematics.py     (from Games/DemoLab; the same as Tools/Levels/make_levels.py)
```

`make_levels.py` calls `make_cinematics.build(lv)` before it writes L1, L2 and L3. That adds each
level's cinematic entities and writes its `Content/Cinematics/C*.nmseq`:

| Entity | What |
|---|---|
| `CineCam_<Cn>_<k>` (`Calm_` / `Alarm_` in L2) | one camera per shot, a binding of the sequence's CameraCut |
| `CineMark_*` | where the player is stood: `Lab.PlacePlayer` keys (the hand-over pose) and `-DemoLab.Cinematic` |
| `Airship_Flight` (L1, `dress_l1.airship`) | the rig C1 flies; the ambient loop bobs the hull on it |
| `CineShaft_*`, `Calm_CineCage_Under` (L2) | the well under the elevator cage that C4 drops into |
| `Alarm_CineAirship*` (L2) | C6's airship, propellers, lamps, dusk key / fill / rim spots and winch line; parked below the world until C6 |
| `Alarm_CineBackdrop_*` (L2) | L1's terrain and sea under the lab (`L2_ON_L1`): the island C6 shows from the roof |
| `CineIntel` (L3) | the canister C5 lifts (the interactable hides the real one when it is used) |

`seq.py` writes the XML: cameras aimed at targets, cuts with blends, Animation sections
(CrashDummy's clips), Transform/Float/Bool tracks, audio, bus volumes, subtitles, Event keys.

## The cinematics

| Id | Length | Beats | Hand-over |
|---|---|---|---|
| C1 Arrival | 20 s | the airship in low over the sea into the sun; alongside it, the island behind; a sweep past the facade and the blast door; the airship mooring off the pier; the player walks down the pier; behind him | Blend Out 1.5 s, control at Blend Start; the menu theme hands over to the level's music |
| C2 Blast door | 16 s | the plaza and its steam; the cog turns once; the leaves part (`L1.BlastDoor`); behind the player into the dark doorway | fade, `L1.Done` (travel) |
| C3 Power-up | 16 s | the dark foyer; the lever; the 13 power signals over ~6 s, a cut per room (foyer, atrium orrery, workshop samples, turbine hall); the atrium held while the bounce light arrives; the player turns to the open door | Blend Out 1.5 s, `L2.Powered`, `Checkpoint.Powered` |
| C4 Elevator | 15 s | into the cage; it drops into the shaft past the lamp bands; down past the player into the core's blue glow | fade; the elevator panel's signal travels |
| C5 Theft | 18 s | the intel lifts; the reactor drops to red, klaxons, shutters and vents (`L3.Alarm`); a vent jetting; the run to the locker | `Lab.PlacePlayer CineMark_C5_End`, Blend Out 1.5 s, `Checkpoint.Theft` |
| C6 Extraction | 22 s | the roof hatch slides; the airship over the dome; the line drops; the winch; the airship climbs away into the dusk, the music up | fade, `Game.Complete` (credits) |

The Event keys the flow listens for keep their meaning and order; C3's cascade asserts it
matches `dress_l2_laboratory.CASCADE`.

## The player in a cutscene

A sequence's binding `player` is the local character: `LabGameModeBase::OnSequenceStarted` binds it.
Its Transform track moves it (the Character Mover reads its position back, so where it ends is
where play resumes); keep keyed yaws `facing(yaw)` (the mover's MeshYaw is 180). Where the
hand-over needs the mover's own yaw and camera to agree, end with `Lab.PlacePlayer` and a
`CineMark_*`.

## Watching one

```
Tools/Cinematics/record.sh C3 <folder>        (from Games/DemoLab, an Editor-target build)
```

runs the game in a window with `-DemoLab.Cinematic=C3` (the level, the signals the story would
have set, the player on `CineMark_C3`, the cutscene, quit) and `-record`, then writes
`<folder>/C3.mp4` and a sheet of a frame a second. The video starts at the cutscene: the level load
and the settling before it are cut (the run log's times; `KEEP_LEAD=1` keeps them). The recorder
takes pictures only, no sound.
