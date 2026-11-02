# Example projects

Whole games, packed, that File > New Project unpacks into `Games/<Name>`:
pick **Example: Demo Lab** next to Blank Game. They are here as archives
rather than as folders under `Games/` so the repository carries one copy
of their assets and none of their builds.

| Example | What it is |
|---|---|
| DemoLab | A 15-minute third-person slice in three levels: dynamic GI, procedural foliage, the TimeOfDay and Weather plugins, the Sequencer, AICore and the UI module. Its `Docs/Guide.md` and `Docs/Tutorial.md` walk through it. |

## Files

| File | What it is |
|---|---|
| `Examples.json` | The index: per example its title and description (written by hand), its parts with their sizes and SHA-256s, the unpacked size and the engine version it was packed against (written by `pack_example.py`) |
| `<Name>.Source.tar.zst` | Everything but the assets: code, config, scripts, tools, docs. Small; repacked whenever the code changes |
| `<Name>.Content.<N>.tar.zst` | The assets (`Content/`, the docs' images), split so each part stays under 50 MB. Repacked only when the art changes |
| `pack_example.py` | Packs a working `Games/<Name>` into the above |
| `UnpackExample.cmake` | Unpacks one, for File > New Project and the command line (`Engine/Binaries/NewProject.cmake -DEXAMPLE=`) |

Nothing a build makes goes in: no `Build/`, `Binaries/`, `Saved/`, IDE
folders or Python caches.

## Unpacking without the editor

```bash
Engine/Binaries/NewProject.sh --example DemoLab
Engine/Binaries/NewProject.sh --example DemoLab --build --jobs 6 MyLab
```

or `NewProject.bat` on Windows, or `cmake -DEXAMPLE=DemoLab [-DNAME=MyLab]
[-DLOCATION=<folder>] [-DBUILD=ON] [-DJOBS=6] -P Engine/Binaries/NewProject.cmake`.

- Each part is checked against `Examples.json` first; a damaged or stale
  archive is refused.
- It unpacks into a hidden staging folder and moves it into place, so a
  failure never leaves a half-made project.
- Another name renames the project: the example's name (and the same in
  capitals) is replaced in the text files, and in file names outside
  `Content/` -- the module, classes, include guards, settings and docs.
  Assets keep their names and binary files are never touched.
- `--build` (`-DBUILD=ON`) then runs `cmake --preset editor-relwithdebinfo`,
  `cmake --build --preset editor-relwithdebinfo` and `NimbleCook shaders`.
  A failed step can be run again on the kept project with
  `-DSTEPS="build;cook"` (any of `unpack;configure;build;cook`).

## Repacking

Work in an unpacked copy (`Games/DemoLab`), then:

```bash
python3 Engine/Examples/pack_example.py Games/DemoLab                  # all parts
python3 Engine/Examples/pack_example.py Games/DemoLab --parts source   # code only
```

Commit the parts that changed together with `Examples.json`. Needs Python
3.14 (`compression.zstd`) or `zstd` on PATH.

- **The packing is reproducible**: files sorted, every entry owned by 0:0,
  with one fixed time and mode 644/755. Packing an unchanged project gives
  the same bytes, so git sees no change in parts you did not touch. (The
  same zstd version gives the same bytes.)
- **Code changes**: repack `--parts source` (well under 1 MB). An engine
  change that breaks an example's code updates the source part in the same
  commit, the way it would update any other caller.
- **Art changes**: repack `--parts content`. Every content repack adds the
  full parts (~90 MB for DemoLab) to the repository's history for good, so
  batch art changes, and keep source art (`.blend`, layered images) out of
  this repository.
- **A new example**: add its entry to `Examples.json` with a Title and a
  Description, then pack it. `NimbleEditorProjectTests` covers the
  machinery with a tiny example (`Engine/Modules/Editor/Tests/Data`), not
  the real ones.
