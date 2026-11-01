# Demo Lab: credits

## Sound and music

Most of Demo Lab's sound is **synthesised in this repository** by
`Tools/Audio/build_audio.py` (numpy only, deterministic). That covers robots,
weapons, machinery, doors, alarms, the beacon, the lab and reactor ambiences,
and the gulls. The rest is made from the recordings below. Every one of them
is **CC0 1.0 Universal** (public domain dedication), so no attribution is
required. We credit them anyway.

Each licence was checked on the source page when the file was fetched: the
OpenGameArt "License(s)" field reads exactly `CC0`, and Kenney's page says
"Creative Commons CC0". `Tools/Audio/fetch_cc0.py` repeats the check every
time it downloads. No account or sign-in was used. The downloads are not
committed. Each game asset holds its own processed source (the `.nmsound`'s
`SSRC` chunk), and the script rebuilds them from a fresh fetch.

Changes common to every CC0-derived asset: decoded with ffmpeg, resampled to
48 kHz (music stays at 44.1 kHz), DC removed, loudness-normalised (BS.1770)
with a -1 dBFS ceiling, then re-encoded. SFX become 16-bit mono FLAC (and PCM)
or mono Ogg Vorbis. Ambience and music become stereo Ogg Vorbis (q3 and q4).

### Music

| Game file | Title | Author | Source | Licence | What we changed |
|---|---|---|---|---|---|
| `Content/Music/Music_Menu.nmsound` | Unexplored Expansion | Bo Jingles (a longer loop of TAD's CC0 "Unexplored (short ver.)") | https://opengameart.org/content/unexplored-expansion (file `tad_-_unexplored_-_expanded_1_by_bo_jingles.mp3`) | CC0 | Loudness to -18 LUFS; 50 ms loop crossfade at the seam; Ogg q4 |
| `Content/Music/Music_Explore.nmsound` | Factory ambiance | yd | https://opengameart.org/content/factory-ambiance (file `Factory.ogg`) | CC0 | DC offset removed; loudness to -21 LUFS (it sits under the ambience); 50 ms loop crossfade; Ogg q4 |
| `Content/Music/Music_Combat.nmsound` | Determined Pursuit (epic orchestra loop) | Emma_MA | https://opengameart.org/content/determined-pursuit-epic-orchestra-loop (file `determined_pursuit_loop.wav`) | CC0 | Loudness to -18 LUFS; Ogg q4; loop length kept exactly (108.0 s) |
| `Content/Music/Music_Extraction.nmsound` | Epic Endgame Cinematic | cynicmusic (pixelsphere.org, cynicmusic.com) | https://opengameart.org/content/epic-endgame-cinematic (file `ChoirChordsBassMaster.wav`) | CC0 | Loudness to -18 LUFS; 0.5 s fade-out; Ogg q4 |

### Nature recordings (ambience)

| Game file | Title | Author | Source | Licence | What we changed |
|---|---|---|---|---|---|
| `Content/Sounds/Ambience/Amb_L1_Coast.nmsound` | Beach Ocean Waves (wave_01..04) | jasinski (Freesound #18363), extracted by qubodup | https://opengameart.org/content/beach-ocean-waves | CC0 | Low-passed at 7 kHz; levelled; scattered every 2.5-5 s over a synthesised sea wash; mixed into a 60 s seamless loop |
| `Content/Sounds/Ambience/Amb_L1_Coast.nmsound` | Park ambiences (`park_ambience_wind.wav`) | Thimras | https://opengameart.org/content/park-ambiences | CC0 | 63 s taken from 1:35; band-limited to 90 Hz-3.5 kHz; levelled; looped with a 3 s crossfade |
| `Content/Sounds/Ambience/Amb_L1_Forest.nmsound` | Park ambiences (`park_ambience_wind.wav`, `park_ambience_birds.wav`) | Thimras | https://opengameart.org/content/park-ambiences | CC0 | Wind: 63 s from 2:15, band-passed 250 Hz-6 kHz (tree rustle). Birds: 63 s from 3:20, high-passed at 1.5 kHz and kept low. Mixed and looped (3 s crossfade) |
| `Content/Sounds/Ambience/Amb_L1_Forest.nmsound` | Tree Creaking | Department64, cleaned up by AntumDeluge | https://opengameart.org/content/tree-creaking (file `tree_creak.flac`) | CC0 | Pitched 0.85-1.1x; low-passed at 4 kHz; three creaks placed in the loop |

The coast's gulls are synthetic. We did not use OpenGameArt's "Solo Seagull
Sound Effects": its page marks it CC0, but it says the sounds came from a
YouTube video, so its provenance is unclear.

### Foley (Kenney audio packs)

All from **Kenney** (kenney.nl), CC0. The packs come from the asset pages, not
from individual file URLs.

| Game file(s) | Pack | Source | Files used | What we changed |
|---|---|---|---|---|
| `Content/Sounds/Footsteps/Step_Stone_01..04` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `footstep_concrete_000..003.ogg` | Mono, 48 kHz; 50 Hz high-pass; 10 ms fade-out; levelled |
| `Content/Sounds/Footsteps/Step_Wood_01..04` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `footstep_wood_000..003.ogg` | Same as above |
| `Content/Sounds/Footsteps/Step_Grass_01..04` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `footstep_grass_000..003.ogg` | Same as above |
| `Content/Sounds/Impacts/Impact_Metal_01..03` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `impactMetal_light_000..002.ogg` | Layered with a synthesised crack, ricochet ping and plate ring; small room reverb |
| `Content/Sounds/Impacts/Impact_Stone_01..03` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `impactMining_000..002.ogg` | At half level, under a synthesised crack, smack and falling grit |
| `Content/Sounds/Impacts/Impact_Robot_01..03` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `impactPlate_medium_000..002.ogg` | Layered with a synthesised hollow body ring and sparks |
| `Content/Sounds/World/Glass_Shatter_01..02` | Impact Sounds | https://kenney.nl/assets/impact-sounds | `impactGlass_heavy_000..001.ogg` | Layered with a synthesised crash and 40 falling shards; room reverb |
| `Content/Sounds/Weapons/Pistol_Reload` | RPG Audio | https://kenney.nl/assets/rpg-audio | `metalLatch.ogg`, `metalClick.ogg` | Placed as the breech opening and shutting, with a synthesised cartridge slide, seat click and pressure hiss |
| `Content/Sounds/Weapons/Pistol_DryFire` | RPG Audio | https://kenney.nl/assets/rpg-audio | `metalClick.ogg` | At 60 % under a synthesised hammer click |
| `Content/Sounds/UI/UI_Click`, `UI_Hover`, `UI_Back`, `UI_Confirm`, `UI_Error` | Interface Sounds | https://kenney.nl/assets/interface-sounds | `click_001`, `tick_001`, `back_001`, `confirmation_001`, `error_004` (.ogg) | Mono, 48 kHz; 10 ms fade-out; levelled |

### Fully synthesised (no recordings)

`Step_MetalGrate_*`, `Step_Tile_*`, `Step_Water_*`, `Pistol_Shot`, `Watchman_*`, `Brute_*` (servos, alert,
hurt, swing, hit), `Robot_Death`, `Shutter_Slam`, `BlastDoor_Open`,
`Steam_Vent_Burst`, `Steam_Vent_Loop`, `Gears_Loop`, `Breaker_Throw`,
`Power_Up_Sweep`, `Power_Hum_Loop`, `Valve_Squeak_*`, `Turbine_SpinUp`,
`Turbine_Loop`, `Lift_Start`, `Lift_Run_Loop`, `Lift_Stop`, `Keycard_Accept`,
`Keycard_Deny`, `Alarm_Klaxon_Loop`, `Beacon_Charge`, `Beacon_Fire`,
`Sprinklers_Loop`, `Airship_Engine_Loop`, `Amb_L2_Calm`, `Amb_L2_Alarm`,
`Amb_L3_Reactor`, and the gulls in `Amb_L1_Coast`. All are made by
`Tools/Audio/build_audio.py` for this project and released with it.

### Fetched file checksums (SHA-256, first 16 hex digits)

| Key | Bytes | SHA-256 |
|---|---|---|
| MusicMenu | 1765271 | a39323b0cbc83dea |
| MusicExplore | 2485366 | c8c1b2b5bad66d29 |
| MusicCombat | 19052830 | e4f3be098b50213b |
| MusicExtraction | 32182224 | 3cf6f2cb4eb4c22d |
| Wave01..04 | 626248, 404529, 367584, 591767 | fcfca178eca2eac5, 53b5c28c0edc36f0, fc795f95f352b354, 3c490e736365fdd6 |
| ParkWind | 73960096 | 5c381856745b4706 |
| ParkBirds | 86190432 | dc1a5ac34b89cab7 |
| TreeCreak | 231924 | 9bab7c7f68850136 |
| Kenney Impact Sounds (zip) | 800850 | 029d734af1582474 |
| Kenney Interface Sounds (zip) | 834536 | f2193d072726d675 |
| Kenney RPG Audio (zip) | 964837 | 6dbeaf8544da958d |
