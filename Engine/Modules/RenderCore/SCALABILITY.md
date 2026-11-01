# Scalability

There are two settings, and they are independent:

- The **detail mode** (Low, Medium, High, Ultra) decides what is drawn: which
  entities render, and the quality values for shadows, GI, reflections, post
  effects and textures.
- The **power profile** (Power-Saver, Power-Max) decides how each frame is run:
  how many job workers take work, how the frame is paced, and how deep the GPU
  queue is.

Both live in `[Scalability]`:

```ini
[Scalability]
DetailMode=High          ; Low | Medium | High | Ultra
PowerProfile=Auto        ; Auto | PowerSaver | PowerMax
AutoPollSeconds=5        ; how often Auto re-reads the power source; 0 never
```

Both apply live. In the console:

```
scalability                      the tier, the profile and every value each sets
scalability detail low
scalability power saver
scalability detail ultra power max
Scalability.DetailMode Medium    the same, through the config var
```

The code is GameCore's `Scalability.h` (runtime) and `DetailMode.h` (the tier
type). The config layering is CoreConfig's `ConfigSetOverlay`.

## How values stack

```
compiled default  <  project .ini  <  [PowerProfile_*]  <  [Scalability_<Tier>]  <  User.ini / console / command line
```

The tier and the profile are each a **config overlay**: a list of
`Section.Key=Value` lines laid over the project's files.

- **A player's choice wins.** Switching tier moves only the vars that still
  hold the old tier's value. A shadow resolution the player set in a settings
  menu stays where they put it.
- **User.ini stays clean.** An overlay's value counts as the baseline, so
  `ConfigSaveUserFile` writes only what the player changed. It never writes
  the tier's own values.
- **The project file does not override an overlay.** Reloading
  DefaultEngine.ini (Project Settings, a collab pull) records the new value
  but leaves an overlaid var where its overlay put it.

## Tier and profile tables

Each tier and profile has an engine table, in `Scalability.cpp`. **High is
the engine's shipped defaults**, so a project that never mentions scalability
renders exactly as before at the default tier. A project changes the tables
with its own sections:

```ini
[Scalability_Low]
Shadows.Resolution=512
Rendering.AllowVolumetricFog=false

[PowerProfile_Saver]
Display.FrameRateLimit=30
```

Two rules decide which value applies:

1. A line in the project's `[Scalability_<Tier>]` or `[PowerProfile_Saver|Max]`
   replaces the engine's row, or adds a new one. Any config var can go in
   these sections, including a game's own.
2. **A var the project already sets in its own section is left out of the
   engine table.** For example, with `[Display] FrameRateLimit=60`,
   Power-Saver's engine row of 30 is skipped. The engine's guess per tier
   never overrides a choice the project made on purpose. To vary that var by
   tier, name it in the tier sections.

### Detail tiers (engine table)

| Var | Low | Medium | High | Ultra |
|---|---|---|---|---|
| `Shadows.Resolution` | 1024 | 2048 | 2048 | 4096 |
| `Shadows.Filter` (0 hard, 1 PCF 3x3, 2 PCF 5x5) | 0 | 1 | 1 | 2 |
| `Shadows.MaxShadowedLights` | 1 | 2 | 4 | 4 |
| `Shadows.MaxInsetShadows` | 0 | 2 | 4 | 4 |
| `Shadows.DistanceScale` | 0.5 | 0.75 | 1 | 1.5 |
| `Shadows.ContactShadows` | off | off | off | on |
| `Shadows.TranslucentShadows` | off | on | on | on |
| `Rendering.ShadowCacheBudgetMB` (the static shadow cache's atlas, MB; lights it cannot hold draw every frame; RENDERPIPELINE.md, "Shadow cache") | 8 | 16 | 32 | 96 |
| `Rendering.ResolutionScale` (the scene drawn at this fraction of the viewport each axis, then upscaled under the UI; RENDERPIPELINE.md, "Resolution scale") | 0.5 | 0.75 | 1 | 1 |
| `Rendering.ScreenSpaceGI` | off | off | on | on |
| `Rendering.StochasticReflections` | off | off | on | on |
| `Rendering.SubsurfaceScattering` | off | on | on | on |
| `Rendering.DynamicGI` | off | on | on | on |
| `Rendering.GIProbesPerFrame` (only with `CaptureBudgetMs` 0) | 1 | 1 | 1 | 2 |
| `Rendering.CaptureBudgetMs` (GI Volume and reflection probe captures a frame; GLOBAL_ILLUMINATION.md, Phase 7) | 0.5 | 1 | 2 | 3 |
| `Rendering.TextureStreamingPoolMB` | 256 | 384 | 512 | 1024 |
| `Rendering.TextureStreamingMipBias` | 1 | 0 | 0 | 0 |
| `Rendering.Anisotropy` (applies at the next start) | 2 | 4 | 8 | 16 |
| `Rendering.AllowAmbientOcclusion` | off | on | on | on |
| `Rendering.AllowScreenSpaceReflections` | off | off | on | on |
| `Rendering.AllowVolumetricFog` | off | on | on | on |
| `Rendering.AllowDepthOfField` | off | off | on | on |
| `Rendering.AllowMotionBlur` | off | on | on | on |
| `Rendering.AllowBloom` | on | on | on | on |
| `Animation.FullRateDistance` | 1500 | 2000 | 3000 | 5000 |
| `Animation.FreezeDistance` | 8000 | 12000 | 20000 | 30000 |
| `Destruction.MaxDebris` | 64 | 128 | 256 | 512 |

**`Rendering.Allow*` are caps.** They work over every view's blended post
process (`WorldBridge::BuildPostProcess`):

- **false** turns the effect off, whatever the volumes ask for;
- **true** leaves the decision to the volumes.

### Power profiles (engine table)

| Var | Power-Saver | Power-Max | Applies |
|---|---|---|---|
| `Jobs.WorkerFraction` (share of job workers taking work) | 0.5 | 1 | live |
| `Jobs.MinActiveWorkers` (not in the table: its default, a floor under the fraction) | 2 | 2 | live |
| `Jobs.IdleWaitMs` (how often an idle worker wakes) | 8 | 2 | live |
| `Rendering.ParallelMaxThreads` (recording fan-out, render thread included; 0 = all) | 2 | 0 | live |
| `Display.PaceSpinMs` (spin at the end of a paced wait) | 0 | 2 | live |
| `Display.FrameRateLimit` | 30 | 0 | live |
| `Rendering.FramesInFlight` | 2 | 3 | at device creation (next start) |
| `Rendering.SwapchainExtraImages` (present queue depth) | 1 | 2 | next swapchain (Vulkan: also a rebuild) |
| `Rendering.TextureStreamingUploadMB` | 4 | 16 | live |
| `Rendering.TextureStreamingMaxLoads` | 4 | 16 | live |

**Power-Saver** is for laptops and handhelds. It aims for low occupancy and
steady, predictable dispatch:

- half the job pool is **parked**: asleep on a condition variable, not
  polling;
- idle workers wake a quarter as often;
- each render stage's draw recording spreads over two threads at most, every
  frame the same;
- frame pacing sleeps out the whole wait instead of spinning for its last
  2 ms, so the core can idle. The cost is up to 1 ms of overshoot.

**Power-Max** is for desktops. It gives work to every worker, lets the CPU run
three frames ahead of the GPU, adds a second queued swapchain image, and
allows texture uploads in bigger bursts.

`SwapchainExtraImages` means the closest thing on each backend:

- **Vulkan:** the surface's minimum image count plus this many images;
- **D3D12:** this many queued frames (`SetMaximumFrameLatency`, at least 1);
- **Metal:** 2 drawables for 0, otherwise 3 (the most a `CAMetalLayer`
  allows);
- **OpenGL:** the driver decides.

The parallel-recording log line, and the `ParallelRecordingWorkers` stat,
count the **active** workers. The render thread re-evaluates them whenever a
profile parks or wakes workers.

The pool's thread count never changes at runtime: `JobSystem` makes its
threads once. `SetActiveWorkerLimit` only parks and wakes them, so switching
profile is instant and nothing queued is lost.

### Auto

`PowerProfile=Auto` resolves to Power-Saver on a mobile build, or when the
platform reports the machine **on battery**. In every other case, including
plugged in, no battery, and unknown, it resolves to Power-Max.

The launcher reads the power source through `GameWindowSystem::GetPowerSource`
(SDL's `SDL_GetPowerInfo`). It polls every `AutoPollSeconds`, so unplugging a
laptop switches profile within a few seconds. The log notes each switch:

```
[GameCore] Scalability: power PowerSaver (Auto, on battery) (9 values changed)
```

## Entity buckets

Every entity has a **Detail Mode**: the lowest tier it renders at.

| Bucket | Renders at |
|---|---|
| Low (the default) | every tier |
| Medium | Medium, High, Ultra |
| High | High, Ultra |
| Ultra | Ultra only |

Set it in the editor's Details panel (the **Detail Mode** row under Mobility,
which also works on a multi-selection), from Lua with
`e:SetDetailMode("High")`, or from C++ with `Entity::SetDetailMode`.

Detail Mode is saved:

- as `DetailMode="High"` in a text world's `.nmentity` (written only when it
  is not Low);
- in the binary world chunk (version 3);
- in prefabs (chunk version 3, and text), including as a per-instance
  override.

**The bucket only affects rendering.** A bucketed-out entity still collides,
ticks, runs its scripts and plays its sounds. Gameplay must never depend on
the player's quality setting. Use buckets for clutter, dressing, extra local
lights and decorative particles, never for something the player can stand on
or be shot through.

What a bucketed-out entity loses:

- **Meshes, terrain and every other proxy** are culled from every view, every
  shadow view and occluder selection. Extraction marks the proxy
  (`RenderProxy::SetDetailHidden`, read by `IsRenderable`). The proxy keeps its
  GPU resources, so switching back is immediate.
- **Point, spot and area lights** stop lighting. Suns and skies light the
  whole world, so they are never bucketed.
- **Particle systems** stop simulating as well as drawing. Their emitters keep
  their state and resume where they stopped.

The editor's status bar names the tier the viewport renders at, for example
`Detail: High`. It adds **(preview)** while Show > World Detail is previewing
a tier, and **(world)** while World Settings locks the world to one; its
tooltip says which.

In the editor, the Outliner greys out (in italics) every entity the world's
current tier hides. Its tooltip names the tier and the entity's bucket. The
Outliner follows the preview, the world settings and the player's tier.

### The world's tier

`World::GetEffectiveDetailMode` takes the first of these that is set:

1. the editor viewport's **Show > World Detail** preview (Low, Medium, High, Ultra; never saved);
2. **World Settings > Detail mode**, when it locks the world to a tier, for
   example a showcase world at Ultra or a test level at Low;
3. the player's `Scalability.DetailMode`.

The world's lock only selects which entity buckets render. The config values
always follow the player's tier.

## Adding a scalable setting

Declare an ordinary config var that applies live, or note in its comment that
it applies at the next start. Then either add a row to the engine table in
`Scalability.cpp`, or leave it to projects' `[Scalability_*]` sections.
Nothing else needs registering: an overlay can set any var by its path,
including one a game module declares after the overlay was built.
