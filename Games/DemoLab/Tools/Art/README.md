# Demo Lab art generators

Every mesh, texture and material under `Content/Meshes`, `Content/Textures`,
`Content/Materials/{Kit,Terrain}`, `Content/Materials/Characters/SteamDummy_*` and
`Content/Characters/SteamDummy/SteamDummy_*` is written by the scripts here. Rebuild all
of it with one command (from the repository root):

```
python Games/DemoLab/Tools/Art/build_all.py            # everything, ~5 min
python Games/DemoLab/Tools/Art/build_all.py --sheets   # and the contact sheets
python Games/DemoLab/Tools/Art/build_all.py l2 manifest   # just one step or two
```

Needs python3 with numpy and Pillow, Blender 4.2+ on `PATH` (or `$BLENDER`), and a built
`NodeGraphGenerate` (any editor build; found under `Games/*/Binaries/*`, or `--bin <dir>`).
Then cook the shaders as for any material change:
`NimbleCook shaders -project Games/DemoLab -module DemoLabGame [-editor]`.

| Step | Script | Writes |
|---|---|---|
| textures | `make_textures.py` (numpy) | `Content/Textures/Kit/*`, `Content/Textures/Terrain/*` |
| steamdummy | `blender/make_steamdummy.py` | `Content/Characters/SteamDummy/SteamDummy_PBR.nmmesh`, `SteamDummy_{Player,Watchman,Brute}_{BaseColor,Normal,ORM}.nmtexture` |
| l1 | `blender/make_l1.py` | `Content/Meshes/L1/*` (exterior kit) |
| l2 | `blender/make_l2.py` | `Content/Meshes/L2/*` (laboratory kit) |
| l3 | `blender/make_l3.py` | `Content/Meshes/L3/*` (core kit), `Content/Meshes/Airship/*` |
| materials | `make_materials.py`, `make_steamdummy_materials.py` | `Content/Materials/Kit/*`, `Terrain/Island`, `Characters/SteamDummy_*` |
| generate | `NodeGraphGenerate` | fills each graph's `<gen_code>` |
| manifest | `build_all.py` | `assets.json` |

`assets.json` lists every asset for the level generators: mesh, materials, size in metres,
bounds, pivot convention, and its moving parts (separate meshes) with their offsets and how
they move. Frame: the engine's (left-handed, +Y up, centimetres), fronts facing -Z.

## How it works

- **Textures** are tileable numpy noise (`lib/texlib.py`), written as `.nmtexture`
  (RGBA8 + box mips, `lib/nmformats.py`) and as PNGs in `Saved/ArtCache/Textures` for the
  Blender previews. A set is `_BaseColor` (A: height, or a mask the set documents),
  `_Normal` (OpenGL) and `_ORM` (occlusion, roughness, metal; half resolution).
- **Materials** are data (`lib/matdefs.py`) turned into graphs (`lib/graphs.py`): textures
  sampled at UV0 / the metres a repeat covers; vertex colour RGB is baked occlusion, A how
  convex the vertex is (painted plate wears to steel there). Shading models: Clear Coat
  (Walnut, Enamel), Anisotropic (BrassBrushed, SteelBrushed), Cloth (Velvet, Leather,
  Canvas), Subsurface (Marble, Wax, LeafPanel), Volume (Steam), Translucent (glass). Lights
  (LampGlow, ReactorGlow, IntelGlow, BeaconLight) have EmissiveColor / EmissiveIntensity /
  PulseSpeed parameters for per-entity overrides (the alarm turns them red).
- **Meshes** are modelled in Blender Python (`blender/kitlib.py` primitives: bevelled
  boxes, lathes, tori, sweeps, prisms with holes, noise rocks) and written straight to
  `.nmmesh` by `kitlib.export`: Blender (x, y, z) m -> engine (x, z, y) cm, winding
  reversed, MikkTSpace tangents, UV0 in metres (box projection, or the helper's own UVs),
  per-vertex AO by ray casting against the whole asset. Each export checks the tangent frame
  and winding in engine space.
- **SteamDummy**: the BattlePunks mesh (`Content/Characters/SteamDummy/SteamDummy.glb`,
  byte-identical to Dev-BattlePunks fad1c7d6) is unwrapped into one atlas, its ten parts
  (told apart by their vertex colours) get procedural surfaces per look, baked with Cycles
  (colour, roughness, metal, normal, AO, a clear-coat/anisotropy mask). The new mesh keeps
  the import's skin (skeleton path, inverse binds, bones and weights copied by vertex
  position), so it plays CrashDummy's clips. Its materials are BattlePunks' graph with the
  eye glow kept as it was (EyeColor, EyeIntensity, BlinkInterval, ...).

## Checking

- `blender -b --factory-startup --python blender/preview.py -- <kit>` reads the written
  `.nmmesh` files back and renders a contact sheet (`Saved/ArtCache/Sheets/<kit>.png`;
  kits: Characters, L1, L2, L3, Airship).
- `make_review_world.py <Content/Levels folder>` writes two throwaway worlds
  (ArtReview_Characters, ArtReview_Kit) to look at the assets in the editor.
