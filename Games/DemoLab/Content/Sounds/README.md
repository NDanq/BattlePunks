# Demo Lab sounds

Built by `Tools/Audio/build_audio.py` (do not edit the `.nmsound` files by hand; change the script and
rebuild). Sources and licences: `Docs/Credits.md`. Paths below are relative to the project root, as
`Audio.PlaySoundAt("Content/Sounds/...")` and an Audio Source's `Sound` take them.

- **3D**: heard from where it plays (Audio Source or `PlaySoundAt`/`PlaySoundAttached`); distances are
  world units (cm): full volume within Min, inverse rolloff out to Max. **2D**: the same in both ears
  (`PlaySound2D`, or an Audio Source on any entity).
- **Loop** sounds are seamless: set them on an Audio Source with AutoPlay, or `Play`/`FadeTo` from script.
  One-shots: `PlaySoundAt` / `PlaySound2D`. Variants (`_01`..`_04`): pick one at random each time.
- **Load**: Decompressed = PCM in the file (short, frequent); Compressed = Ogg decoded once; Stream = Ogg
  decoded as it plays (beds, music).
- Loudness is normalised (BS.1770): one-shots by their loudest 400 ms, loops/beds/music integrated;
  the LUFS column is the result, so relative levels are already balanced. Bus volumes do the rest.
- Footsteps can be animation-clip notifies naming the sound (Audio README, "Footsteps").

## Footsteps

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/Footsteps/Step_Stone_01..04.nmsound` | Footstep on stone, cobbles, concrete (L1 dock road, L2/L3 floors) | SFX | 3D (100-1500) | one-shot | Decompressed | 0.12 s | -28.1 |
| `Content/Sounds/Footsteps/Step_Wood_01..04.nmsound` | Footstep on wood (L1 pier, L2 archive floor) | SFX | 3D (100-1500) | one-shot | Decompressed | 0.29 s | -24.0 |
| `Content/Sounds/Footsteps/Step_Grass_01..04.nmsound` | Footstep on grass/earth (L1 meadow, forest edge, beach path) | SFX | 3D (100-1200) | one-shot | Decompressed | 0.79 s | -28.0 |
| `Content/Sounds/Footsteps/Step_MetalGrate_01..04.nmsound` | Footstep on a steel grate or catwalk (L3 ring gallery, gantries) | SFX | 3D (100-1800) | one-shot | Decompressed | 0.6 s | -22.0 |

## Weapons

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/Weapons/Pistol_Shot.nmsound` | Pipe pistol shot (player and Watchmen) | SFX | 3D (300-8000) | one-shot | Decompressed | 1.6 s | -12.1 |
| `Content/Sounds/Weapons/Pistol_Reload.nmsound` | Pipe pistol reload: breech opens, cartridge in, breech closes, pressure hiss | SFX | 3D (100-1500) | one-shot | Decompressed | 1.5 s | -19.8 |
| `Content/Sounds/Weapons/Pistol_DryFire.nmsound` | Pipe pistol trigger on an empty chamber | SFX | 3D (100-800) | one-shot | Decompressed | 0.35 s | -25.8 |

## Impacts

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/Impacts/Impact_Metal_01..03.nmsound` | Bullet hits metal (pipes, plates, machinery) | SFX | 3D (200-3000) | one-shot | Decompressed | 0.7 s | -15.7 |
| `Content/Sounds/Impacts/Impact_Stone_01..03.nmsound` | Bullet hits stone, brick or concrete: smack and falling grit | SFX | 3D (200-3000) | one-shot | Decompressed | 0.8 s | -23.3 |
| `Content/Sounds/Impacts/Impact_Robot_01..03.nmsound` | Bullet hits a robot (Watchman enamel, Brute plates): hollow clank and sparks | SFX | 3D (200-3000) | one-shot | Decompressed | 0.7 s | -23.1 |

## Robots

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/Robots/Watchman_Servo_01..03.nmsound` | Watchman joint servo (turn, aim, step accent) | SFX | 3D (100-1500) | one-shot | Decompressed | 0.45 s | -21.0 |
| `Content/Sounds/Robots/Brute_Servo_01..03.nmsound` | Brute hydraulic servo (heavy turn, wind-up) | SFX | 3D (150-2000) | one-shot | Decompressed | 0.9 s | -18.0 |
| `Content/Sounds/Robots/Watchman_Alert.nmsound` | Watchman spots the player: sharp rising steam chirp | SFX | 3D (200-4000) | one-shot | Decompressed | 1.05 s | -14.0 |
| `Content/Sounds/Robots/Watchman_Idle_01..02.nmsound` | Watchman idle / patrol warble (sparse, low) | SFX | 3D (150-2500) | one-shot | Decompressed | 1.2 s | -20.0 |
| `Content/Sounds/Robots/Watchman_Investigate.nmsound` | Watchman hears a noise: questioning chirp (rises at the end) | SFX | 3D (150-3000) | one-shot | Decompressed | 1.1 s | -18.0 |
| `Content/Sounds/Robots/Watchman_Hurt.nmsound` | Watchman takes damage: falling squeal with a steam burst | SFX | 3D (150-3000) | one-shot | Decompressed | 0.8 s | -15.0 |
| `Content/Sounds/Robots/Brute_Alert.nmsound` | Brute spots the player: deep steam-horn growl | SFX | 3D (300-5000) | one-shot | Decompressed | 1.8 s | -13.0 |
| `Content/Sounds/Robots/Brute_Hurt.nmsound` | Brute takes damage / armour plate breaks: grunting horn | SFX | 3D (300-4000) | one-shot | Decompressed | 1.0 s | -14.0 |
| `Content/Sounds/Robots/Brute_Swing_01..02.nmsound` | Brute wrench swing (whoosh) | SFX | 3D (150-2000) | one-shot | Decompressed | 0.55 s | -17.0 |
| `Content/Sounds/Robots/Brute_Hit.nmsound` | Brute wrench connects (player or cover): heavy thud and clank | SFX | 3D (200-3000) | one-shot | Decompressed | 1.1 s | -11.0 |
| `Content/Sounds/Robots/Robot_Death.nmsound` | Watchman or Brute destroyed: power-down whine, steam release, clank cascade | SFX | 3D (200-4000) | one-shot | Compressed | 2.6 s | -12.6 |

## World

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/World/Shutter_Slam.nmsound` | Security shutter drops and slams (C5, alarm reroutes) | SFX | 3D (300-5000) | one-shot | Compressed | 2.6 s | -11.0 |
| `Content/Sounds/World/BlastDoor_Open.nmsound` | Plaza blast door: latches, steam release, gears and motor, travel, stop (C2) | SFX | 3D (500-8000) | one-shot | Compressed | 8.0 s | -12.0 |
| `Content/Sounds/World/Steam_Vent_Burst.nmsound` | Steam vent bursts (C2 plaza, C5 vents open, pipes in L2/L3) | SFX | 3D (200-3500) | one-shot | Compressed | 2.4 s | -14.0 |
| `Content/Sounds/World/Steam_Vent_Loop.nmsound` | Steady steam leak / vent hiss (emitters on pipes) | SFX | 3D (150-2000) | loop | Compressed | 6.0 s | -22.0 |
| `Content/Sounds/World/Gears_Loop.nmsound` | Big brass gears turning (C2 plaza, orrery, machinery) | SFX | 3D (150-2500) | loop | Compressed | 4.0 s | -21.1 |
| `Content/Sounds/World/Breaker_Throw.nmsound` | Main breaker lever thrown: creak, heavy clack, arc and relays (C3) | SFX | 3D (200-3500) | one-shot | Decompressed | 1.8 s | -15.6 |
| `Content/Sounds/World/Power_Up_Sweep.nmsound` | Lab powers up after the breaker: hum and flywheel whine rising (C3) | SFX | 2D | one-shot | Compressed | 5.5 s | -14.0 |
| `Content/Sounds/World/Power_Hum_Loop.nmsound` | Steady powered-machinery hum after power-up (breaker panel, junction boxes) | SFX | 3D (100-1500) | loop | Compressed | 4.0 s | -24.0 |
| `Content/Sounds/World/Valve_Squeak_01..02.nmsound` | Valve wheel turned: metal squeak and ratchet (L2 turbine hall valves) | SFX | 3D (150-2000) | one-shot | Decompressed | 1.5 s | -17.0 |
| `Content/Sounds/World/Turbine_Loop.nmsound` | Steam turbine running (L2 turbine hall, both states) | SFX | 3D (300-4000) | loop | Compressed | 8.0 s | -20.0 |
| `Content/Sounds/World/Turbine_SpinUp.nmsound` | Steam turbine spins up from rest to running; crossfade into Turbine_Loop | SFX | 3D (300-4000) | one-shot | Compressed | 6.0 s | -17.0 |
| `Content/Sounds/World/Lift_Run_Loop.nmsound` | Elevator / freight lift running: motor, cable, rail joints, cage rattle (C4, L3 exit) | SFX | 3D (300-4000) | loop | Compressed | 6.0 s | -19.0 |
| `Content/Sounds/World/Lift_Start.nmsound` | Elevator / freight lift starts: brake release, motor winds up | SFX | 3D (300-4000) | one-shot | Compressed | 2.2 s | -16.0 |
| `Content/Sounds/World/Lift_Stop.nmsound` | Elevator / freight lift stops: wind-down, brake squeal, clunk, chain settle | SFX | 3D (300-4000) | one-shot | Compressed | 2.6 s | -14.1 |
| `Content/Sounds/World/Keycard_Accept.nmsound` | Keycard reader accepts: two-tone beep and lock relay (L2 archive) | SFX | 3D (100-1500) | one-shot | Decompressed | 0.6 s | -18.0 |
| `Content/Sounds/World/Keycard_Deny.nmsound` | Keycard reader refuses: low double buzz | SFX | 3D (100-1500) | one-shot | Decompressed | 0.55 s | -18.0 |
| `Content/Sounds/World/Alarm_Klaxon_Loop.nmsound` | Lab alarm klaxon (L3 after C5, L2 alarm state) | SFX | 3D (500-6000) | loop | Compressed | 3.0 s | -14.0 |
| `Content/Sounds/World/Beacon_Charge.nmsound` | Extraction beacon charging (W2 pump hall, ~8 s); Beacon_Fire follows | SFX | 3D (400-6000) | one-shot | Compressed | 8.0 s | -19.4 |
| `Content/Sounds/World/Beacon_Fire.nmsound` | Extraction beacon fires: crack, boom and rising flare (end of W2) | SFX | 3D (600-12000) | one-shot | Compressed | 4.5 s | -9.0 |
| `Content/Sounds/World/Glass_Shatter_01..02.nmsound` | Glass breaks (benches, display cases, conservatory panes in the alarm state) | SFX | 3D (200-3500) | one-shot | Decompressed | 1.8 s | -17.3 |
| `Content/Sounds/World/Sprinklers_Loop.nmsound` | Fire sprinklers spraying, water on the floor (L2 alarm: archive, workshop) | Ambient | 2D | loop | Compressed | 8.0 s | -22.0 |
| `Content/Sounds/World/Airship_Engine_Loop.nmsound` | Airship engines: propeller thrum and steam-engine chuff (C1, C6) | SFX | 3D (1000-20000) | loop | Compressed | 8.0 s | -16.1 |

## UI

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/UI/UI_Click.nmsound` | Menu button pressed | UI | 2D | one-shot | Decompressed | 0.12 s | -32.2 |
| `Content/Sounds/UI/UI_Hover.nmsound` | Menu item highlighted (hover / focus move) | UI | 2D | one-shot | Decompressed | 0.07 s | -32.1 |
| `Content/Sounds/UI/UI_Back.nmsound` | Back / close a menu page | UI | 2D | one-shot | Decompressed | 0.08 s | -26.8 |
| `Content/Sounds/UI/UI_Confirm.nmsound` | Confirm (New Game, apply settings, objective complete) | UI | 2D | one-shot | Decompressed | 0.31 s | -19.0 |
| `Content/Sounds/UI/UI_Error.nmsound` | Not allowed (locked chapter, invalid setting) | UI | 2D | one-shot | Decompressed | 0.12 s | -20.7 |

## Ambience

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Sounds/Ambience/Amb_L1_Coast.nmsound` | L1 coast: sea on the rocks, wind, distant gulls (dock, beach, lower road) | Ambient | 2D | loop | Stream | 60.0 s | -26.0 |
| `Content/Sounds/Ambience/Amb_L1_Forest.nmsound` | L1 spruce forest: wind in the trees, birds, creaking trunks (forest, overlook) | Ambient | 2D | loop | Stream | 60.0 s | -26.0 |
| `Content/Sounds/Ambience/Amb_L2_Calm.nmsound` | L2 laboratory, calm: electrical hum, ventilation, clock ticks, distant pipes | Ambient | 2D | loop | Stream | 60.0 s | -26.0 |
| `Content/Sounds/Ambience/Amb_L2_Alarm.nmsound` | L2 laboratory on alarm: distant klaxon, sparking wiring, stressed metal | Ambient | 2D | loop | Stream | 60.0 s | -23.0 |
| `Content/Sounds/Ambience/Amb_L3_Reactor.nmsound` | L3 core: reactor drone, rotating rings, lightning arcs in a huge shaft | Ambient | 2D | loop | Stream | 60.0 s | -22.0 |

## Music

| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |
|---|---|---|---|---|---|---|---|
| `Content/Music/Music_Menu.nmsound` | Main menu theme (loops) | Music | 2D | loop | Stream | 88.01 s | -18.0 |
| `Content/Music/Music_Explore.nmsound` | Exploration bed: L1 and L2 calm, under the ambience (loops; keep it low) | Music | 2D | loop | Stream | 122.95 s | -21.0 |
| `Content/Music/Music_Combat.nmsound` | Combat loop: waves W1-W4 (loops seamlessly) | Music | 2D | loop | Stream | 108.0 s | -18.0 |
| `Content/Music/Music_Extraction.nmsound` | Extraction and credits cue: C6 and the credits roll (plays once) | Music | 2D | one-shot | Stream | 91.21 s | -18.0 |

## Wiring notes

- **Level 1**: `Amb_L1_Coast` near the dock/beach, crossfade to `Amb_L1_Forest` up the road (two 2D
  Audio Sources and `FadeTo`), `Music_Explore` low under both. C1/C6: `Airship_Engine_Loop` attached to
  the airship. C2: `Steam_Vent_Burst`, `Gears_Loop`, `BlastDoor_Open` on the door.
- **Level 2 calm**: `Amb_L2_Calm`; C3 is `Breaker_Throw` then `Power_Up_Sweep`, then `Power_Hum_Loop`
  on panels; `Turbine_Loop` in the turbine hall (`Turbine_SpinUp` if a valve starts it), `Valve_Squeak`
  per valve, `Keycard_Accept`/`Keycard_Deny` at the archive, `Lift_Start` + `Lift_Run_Loop` + `Lift_Stop`
  for C4.
- **Level 3**: `Amb_L3_Reactor`; C5: `Shutter_Slam`, `Steam_Vent_Burst`, then `Alarm_Klaxon_Loop` and
  `Music_Combat`. W2: `Beacon_Charge` then `Beacon_Fire`. Freight lift: the Lift sounds.
- **Level 2 alarm**: `Amb_L2_Alarm` (it already carries a distant klaxon; place `Alarm_Klaxon_Loop`
  emitters only near the horns), `Sprinklers_Loop` in the archive/workshop, `Glass_Shatter` on benches,
  `Music_Combat`; C6: `Music_Extraction` (one-shot; the credits run under it), then back to `Music_Menu`.
- **Robots**: Watchman `_Idle` every 6-12 s on patrol, `_Investigate` on a noise, `_Alert` on sight,
  `_Hurt` on damage, `Watchman_Servo` on turns; Brute the same with `Brute_*`, `Brute_Swing` at the
  wind-up, `Brute_Hit` on contact; `Robot_Death` for both. `Impact_Robot` when a round hits either.
- **Cinematics**: Sequencer Audio tracks can play any of these (Force Bus/Spatial as needed); duck
  `Music` with a Bus Volume track under C3/C5 stingers.
