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

**Hour replicates:** the server's clock is the real clock, and a client runs
its own copy between updates.

**Scripts:** `TimeOfDay.GetHour()`, `TimeOfDay.SetHour(h)`,
`TimeOfDay.SetRunning(b)`, `TimeOfDay.IsDaytime()`.
`Events.On("TimeOfDayHour", function(hour, clock) ... end)` fires as each
hour begins.

**Config** (`[TimeOfDay]`): `DayLengthMinutes`, and `Speed`, which scales
every clock (console: `TimeOfDay.Speed 10`).

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

- **No sky background colour.** The Sky Light draws only an HDRI behind the
  scene, so the sky colour lights surfaces but the background stays dark. A
  Sky Light that has an HDRI `Environment` lights by the HDRI, which ignores
  the preset's sky colours.
- **Rotation is applied to the entity**, so make its Mobility Dynamic.
