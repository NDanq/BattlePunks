---
name: perf-testing
description: Measure NimbleEngine's GPU cost for a rendering change and write it up -- on RelWithDebInfo builds, interleaved A/B, medians. Use when asked for a perf report, a cost, a budget check, or before/after GPU timings of a feature.
---

# Performance testing in NimbleEngine

## Rules

- **RelWithDebInfo only.** Every number in a perf report comes from a
  RelWithDebInfo build (`Games/WorldsLab/Build/editor-relwithdebinfo`, binaries
  in `Games/WorldsLab/Binaries/Windows_RelWithDebInfo`). Debug builds are for
  correctness checks and captures, never for timings: their costs are not the
  engine's and their ratios differ.
- **A/B in one build, interleaved.** A feature's cost is the span with its
  switch on less the same span with it off, measured in the same build. Run
  the configs interleaved -- every config once per round, view by view -- never
  one config after another: the GPU's clock cannot be locked here and drifts
  over the minutes a measurement takes, which turned sequential runs into
  negative "costs" (2026-10-23).
- **Medians of many samples.** At least two launches per config, view and
  backend, three samples a launch (frames 480, 600, 720), fixed render clock.
  Report the median and say what the noise is (the spread between samples of
  one config).
- **No readback while timing.** A frame capture stalls the GPU and skews the
  spans: time with stats-only runs, capture in separate runs.
- **Both backends** (Vulkan and D3D12); OpenGL cannot start the game here,
  Metal cannot be built.
- Cross-session numbers are not comparable (the whole GPU ran 35-40 % faster
  one session than the last): compare only within one measurement.

## Build and stage RelWithDebInfo

From PowerShell, with VS's bundled cmake on PATH (the PATH cmake 3.20 cannot
regenerate these trees):

```
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"" x86_amd64 && set PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH% && cmake --build Games\WorldsLab\Build\editor-relwithdebinfo --target WorldsLab"
```

Shaders: `Games/WorldsLab/Binaries/Windows_Debug/NimbleCook.exe shaders -project Games/WorldsLab -editor`
cooks Engine and game shaders into `Saved/Cooked`, which both trees' games
mount first; a build then stages the cooked `.material` files beside the exe.
After changing a material graph, run `NodeGraphGenerate` on it first.

## Measure (TranslucencyLab)

`Games/WorldsLab/Tools/TranslucencyLab/perf.ps1` launches the lab's four views
(`WorldsLab.TranslucencyLabView` 0-3) with `stat frame` on and logs each
span's GPU time; it defaults to the RelWithDebInfo exe and interleaves its
configs:

```
& Games\WorldsLab\Tools\TranslucencyLab\perf.ps1 -Out Games\WorldsLab\Saved\TLab\perf.csv `
    -Configs "all=", "no-forward-reflections=-Rendering.ForwardReflections=0"
py -3 Games\WorldsLab\Tools\TranslucencyLab\perf.py Games\WorldsLab\Saved\TLab\perf.csv --base all
```

A config is `name=` plus the `-Section.Key=Value` arguments that make it;
`all=` is the defaults. Run long measurements in the background (about 25 s a
launch); game windows take focus, so warn the user first.

For another level, add a stats hook like `WorldsLabTranslucencyLab.cpp`'s
(`TranslucencyLabStats`, `...StatsCount`: log `RenderStats::GpuMs` per span at
fixed frames) to that level's game mode.

## Write it up

A `<FEATURE>_PERF.md` beside the feature's design doc, as
`Engine/Modules/RenderCore/GLOBAL_ILLUMINATION_PERF.md` and
`LIT_TRANSLUCENCY_PERF.md`: setup (GPU, build, resolution, level, method,
noise), a cost table per feature and switch, the heaviest view frame by
frame, what to do about it, and what it does not cover. Bold only what is
clearly above noise.
