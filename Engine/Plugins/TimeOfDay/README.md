# TimeOfDay

A day/night cycle, and the pilot plugin for
[Engine/Docs/PLUGINS.md](../../Docs/PLUGINS.md).

Enable it in a project's `Config/DefaultEngine.ini`:

```ini
[Plugins]
TimeOfDay=true
```

Then add a **Time Of Day** component (Lighting) to the entity that has the
level's **Directional Light** and **Sky Light**. Each frame the component:

- moves its **Hour** forward and turns the entity along the sun's arc (the
  moon's at night);
- sets the sun's colour and intensity, and the sky's and ground's colours,
  from **Preset** at that hour.

| Field | |
|---|---|
| Hour | 0 to 24 |
| Day Length (min) | real minutes per game day; 0 uses `[TimeOfDay] DayLengthMinutes` |
| Running | whether the clock moves |
| North Yaw | which way the sun's arc faces |
| Latitude | the noon sun stands 90 - Latitude degrees up |
| Sun Intensity Scale | multiplies the preset's sun intensity, for levels exposed for a brighter sun |
| Preset | `TimeOfDay:/Presets/Default.todpreset`, or the project's own `Content/....todpreset` |
| Draw Sky | draw the sky (below) and take the light colours from it |
| Haze | aerosol density: more gives a whiter sky and wider, redder sunsets |
| Sky Brightness | multiplies the drawn sky |
| Sun Disc Size | the sun disc's radius in degrees (the real one is about 0.27) |
| Star Brightness | 0 turns the stars off |

**Hour replicates:** the server's clock is the real clock, and a client runs
its own copy between updates.

**Scripts:** `TimeOfDay.GetHour()`, `TimeOfDay.SetHour(h)`,
`TimeOfDay.SetRunning(b)`, `TimeOfDay.IsDaytime()`.
`Events.On("TimeOfDayHour", function(hour, clock) ... end)` fires as each
hour begins.

**Config** (`[TimeOfDay]`): `DayLengthMinutes`; `Speed`, which scales every
clock (console: `TimeOfDay.Speed 10`); and `Hour`, which pins every clock at
that hour, for previews and screenshots (`-TimeOfDay.Hour=18.5`).

## The sky

With **Draw Sky** on, a render pass draws the sky behind everything
([`TimeOfDaySky.fsh`](Content/Shaders/TimeOfDaySky.fsh)). Per pixel it
computes single scattering through a planet's atmosphere:

- Rayleigh scattering gives the blue sky and the red horizons.
- Mie scattering gives the haze and the glow around the sun.
- Ozone absorption keeps the blue hour blue.

The sun and the moon (opposite the sun) both light it. On top of that: a
limb-darkened sun disc, the moon disc, twinkling stars, and a faint airglow
so that night is deep blue rather than black. Sunsets, the pink band
opposite the sun and the blue hour all come from the sun's angle; nothing is
keyed by hand.

The exposure adapts as the zenith dims, up to 8x, so twilight stays readable
and night stays dark.

**Reflections and sky light see it.** A second pass of the sky is
registered at `ERenderHook::Sky`, so image-based lighting captures the drawn
sky into its sky cube in place of the Sky Light's colours
(RENDERPIPELINE.md, "Image-based lighting"): metals and glass reflect it,
screen-space reflections fall back to it, and the diffuse sky light takes
its colour and direction from it -- the warm side of a sunset sky lights the
side of a building that faces it. It is captured again each time the sun
moves about a degree (at most every `[Rendering] SkyCaptureFrames` frames),
and Reflection Probes are then captured again in turn; they and Scene
Capture Probes draw the sky behind what they see. These copies leave out the
sun and moon discs and the stars: the Directional Light is the sun's
highlight already. Below the horizon the sky shows the sunlit ground's light
(`TOD_Bounce`), so a mirror ball's lower half is not black.

**In the editor** the sky shows while editing too, at the component's
**Hour** (the clock runs, and sets the lights, in Play mode only).

The same model runs on the CPU to set the sun's colour by day and the Sky
Light's sky and ground colours, so the lighting matches what you see. The
preset still sets every intensity, and the moonlight colour at night.

Cook the sky shader after changing it:

```bash
NimbleCook shaders -project <game>
```

## Presets

A `.todpreset` is a list of keys. Values blend linearly between keys, and
across midnight from the last key back to the first:

```ini
[Key_Noon]
Hour=12
SunColor=255,250,240
SunIntensity=3.2
SkyColor=130,150,190
GroundColor=65,60,55
SkyIntensity=0.65
```

## Example

WorldsLab's **DayCycle** world is Hills with a two-minute day.

## Limits

- **Reflections see the sky in steps.** The sky cube is recaptured as the
  sun moves a degree, so a fast clock (a two-minute day) steps the
  reflections a few times a second; the sky behind them moves smoothly.
- **No clouds yet.**
- **In edit mode the lights stay as saved.** Only the sky follows Hour while
  editing; the Directional Light and Sky Light follow it in Play mode.
- **The sky draws over a Sky Light's HDRI background.** Turn Draw Sky off to
  keep the HDRI.
- **Rotation is applied to the entity**, so make its Mobility Dynamic.
