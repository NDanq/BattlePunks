# Demo Lab: design

Demo Lab is NimbleEngine's showcase game and WorldsLab's successor as the sample
project. It is a 15-minute, single-player, third-person vertical slice.
Style: semi-realistic steampunk that pushes the renderer.

The plan with level maps: https://claude.ai/artifact/NLvJnpHhTuUatsx2fbkcgJ

## Premise

The player lands on an island and climbs the foothills to a hidden steampunk
laboratory. They explore down to its core, lift the intel stored there, and
trip the alarm. They fire an extraction beacon and fight back up through the
lab, now on alarm, to an airship waiting over the atrium's glass dome.

The first two thirds are calm exploration, so the rendering gets looked at.
Combat starts only after the alarm. Every cinematic ends by blending its last
shot into the player's own camera (Sequence Player blend-out) and handing back
control.

## Flow

| Order | World | State | Length | Beats |
|---|---|---|---|---|
| 0 | `MainMenu` | | | Live 3D backdrop (lab facade at dusk, slow Sequencer dolly), Continue / New Game / Chapter Select / Settings / Credits / Quit |
| 1 | `L1_Foothills` | | ~3 min | C1 arrival flyover -> dock -> switchback road -> gatehouse crank -> overlook -> C2 blast door |
| 2 | `L2_Laboratory` | Calm | ~4 min | Airlock -> dark foyer, C3 breaker powers the lab -> atrium -> materials workshop -> archive (keycard) -> turbine hall (valves) -> C4 elevator down |
| 3 | `L3_Core` | | ~3 min | Ring gallery -> vault, C5 theft + alarm, sidearm from locker -> W1 reactor floor -> maintenance tunnel -> W2 pump hall while beacon charges -> fire beacon (triggers extraction) -> freight lift up |
| 4 | `L2_Laboratory` | Alarm | ~3 min | Enter at the freight lift (turbine hall), emergency red light, shutters reroute -> W3 turbine hall -> archive/workshop (sprinklers, stragglers, benches shatter) -> W4 atrium 60 s hold -> climb orrery gantry to roof hatch -> C6 extraction, credits, back to menu |

Level 2 is one world with two states: its script reads the travel/checkpoint
`Alarm` flag and swaps tagged entity groups (`Calm` / `Alarm`: lights,
shutters, spawners, debris, sprinkler particles).

Checkpoints (SaveGame) at each level start, after C3, after C5, at the pump
hall, and at the Level 2 alarm entry. Chapter Select starts any of the four
rows above.

## Cinematics

| Id | Where | What |
|---|---|---|
| C1 | L1 start | Airship flyover past the cliffs to the dock; blends into the player |
| C2 | L1 end | Plaza: steam vents, gears, blast door opens; fade to L2 load |
| C3 | L2 foyer | Breaker thrown: lights come on room by room (live DDGI + SSGI bounce) |
| C4 | L2 end | Elevator descent past the reactor shaft; fade to L3 load |
| C5 | L3 vault | Intel lifted: reactor drops to red, shutters slam, vents open; blends to the player at the sidearm locker |
| C6 | L2 alarm end | Winched into the airship, it pulls away over the island at dusk; credits |

## Cast

All characters are the **SteamDummy** (from BattlePunks, Dev-BattlePunks commit
fad1c7d6): a steampunk mesh skinned to CrashDummy's 74-bone rig, so it plays
CrashDummy's clips and anim state unchanged. Its vertex-colour material is
replaced by generated PBR textures; the CRT eyes stay.

| Who | Look | Behaviour |
|---|---|---|
| Player | SteamDummy, field outfit (brass, leather, rubber), amber eyes | Third person over the shoulder. Walk, sprint, crouch, interact. Sidearm (pipe pistol) only after C5 |
| Watchman | SteamDummy, security livery (Clear Coat enamel), red eyes, pipe pistol | Ranged; cover, flank in pairs, investigate noise |
| Brute | SteamDummy scaled ~1.25, riveted Anisotropic steel plates, wrench | Melee, slow; armour plates break off |

Waves: W1 4 Watchmen; W2 Watchmen + first Brute; W3 Watchmen + Brute; W4
mixed, 60 s hold. AICore navmesh + behavior trees + perception.

## Levels

### L1 Foothills Approach (exterior, procedural content)
Island coast at golden hour (TimeOfDay plugin), Weather plugin sea mist.
Sea to the south-west, a pier/dock, beach path, a switchback road climbing
north-east to a plaza under a cliff where the lab facade and blast door are
set into the rock. A spruce forest (north-west), a meadow (east of the road),
scree on the steep slope below the cliff, a signal mast on a ridge.
Procedural Spawner showcase: the road is one spline that also spawns lamps,
rails and fence posts; forest/meadow are area spawners with slope/height/noise
filters; scree uses a slope filter on the terrain.

### L2 The Laboratory (interior, GI and materials)
Rooms: Airlock -> Foyer (breaker panel) -> Atrium (round, glass dome, brass
orrery in the centre; sun crosses the dome) with the Materials Workshop west,
Conservatory east (seen through glass, not entered), Archive north-west
(keycard), Turbine Hall north (valves; freight lift at its west end), Elevator
north-east. Materials Workshop benches show labelled samples of each shading
model: brushed brass (Anisotropic), lacquered walnut and car-paint enamel
(Clear Coat), velvet and leather (Cloth), marble and wax and backlit leaf
(Subsurface), frosted glass (lit translucency), steam (Volume).

### L3 The Core (combat)
Elevator arrival -> ring gallery of catwalks spiralling down around the
reactor shaft (lightning arcs, volumetric shafts, rotating brass rings) ->
bridge to the vault at the centre -> sidearm locker. Escape: maintenance
tunnel south -> Coolant Pump Hall (three big pumps, beacon) -> freight lift.

## UI
Main menu, Settings (Graphics: quality tier, GI, shadows, reflections, fog,
foliage density, VSync/frame limit; Display; Audio buses; Controls:
sensitivity, invert; Accessibility: subtitles), Pause (Resume, Restart
checkpoint, Settings, Main menu, Quit), HUD (objective, health, ammo, wave,
intel marker), loading card between levels, credits. A tech overlay on F1
names the engine features on screen with live `stat unit` numbers.

## Targets and rules
- 1440p at 60 fps on the High tier on the dev machine (perf-testing recipe).
- Art is generated in this repo as far as possible: Python/Blender generators
  under `Tools/`, so every asset can be rebuilt.
- Sounds and music are CC0; each file's source, author and licence goes in
  `Docs/Credits.md` and the credits screen.
- Plugins: TimeOfDay, Weather.
