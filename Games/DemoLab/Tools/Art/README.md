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
| armour | `blender/make_brute_armour.py` | `Content/Characters/SteamDummy/Armour/Brute_*.nmmesh` (the Brute's breakable plates) |
| l1 | `blender/make_l1.py` | `Content/Meshes/L1/*` (exterior kit) |
| l2 | `blender/make_l2.py` | `Content/Meshes/L2/*` (laboratory kit) |
| l3 | `blender/make_l3.py` | `Content/Meshes/L3/*` (core kit), `Content/Meshes/Airship/*` |
| materials | `make_materials.py`, `make_steamdummy_materials.py`, `make_particles.py` | `Content/Materials/Kit/*`, `Terrain/Island`, `Characters/SteamDummy_*`, `Materials/Particles/P_*`, `Content/Particles/*.nmparticle` |
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

## Second pass (detail)

- **Kit helpers** (`kitlib.py`, after `sphere`): bolts and bolt circles, rivets on lathes and
  along lines (shared prototypes: one mesh, many copies), band rings, spur gears with spoked
  windows, hanging cables, coils, flanged pipe runs with collars, open shells
  (`shell_sector`: a casing with a window cut in it), twisted airfoil blades, straps, and
  `sculpted_rock`. A kit script runs some of its functions with
  `blender ... --python make_l2.py -- orrery turbine` (the manifest keeps the others).
- **Hero pieces**: the orrery (fluted column, scrolled struts, riveted yoke and bearings,
  graduated hoops, caged sun, crown gear and pinion, four planets), the reactor (buttressed
  riveted foundation, hatches, coil-wound core, riveted frame, flanged conduits, cables,
  insulator crown and electrode), the turbine (open casing behind a glass window, lit
  inside, bladed and shrouded rotor, fly-ball governor part) and the airship (gored envelope,
  framed fins, load belts and rigging, a gondola with portholes, framed cabin windows,
  barrel roof, bridge, railed decks, lanterns, twisted propellers).
- **Rocks and cliffs** are sculpted: a block-like convex hull, voxel remeshed, displaced by
  chipped Voronoi facets, fracture cracks, rills and softened tops; cliffs add bedding (soft
  beds weathered back, hard beds lipped, dipping, jointed) and use the `Cliff` material
  (CliffStrata set, 6 m). Box-mapped in metres; `Rock` and `Cliff` grow moss on upward faces
  (world normal) from the terrain's Grass. `Cliff_Wall_A/B` have a flatter -Z face for the
  facade.
- **Weathering** (`graphs.weathering`, matdefs `weather`): every opaque set can take the
  shared `Grime_Mask` (grime clouds and occluded corners, rust / verdigris / water streaks
  down V, chipped edge wear or polish on convex edges) and the `Detail_Normal`, 5-6 texture
  samples a material in all.
- **The sea** (`Sea`): Translucent Lit, World Position Offset swell (three sine trains, Max
  Offset 60), two panned `Sea_Normal`s, Fresnel opacity and colour, crest foam; the shore's
  foam is a strip mesh with its own material (`SeaFoam`), since only post process materials
  read the scene depth. Meshes `Sea_Plane` (200 m, chainable), `Sea_Far`, `Sea_FoamStrip`.
- **Particles**: see `make_particles.py` (SteamVent, SteamJet, Sparks, ReactorArcs,
  SprinklerSpray, DustMotes) and assets.json's `particles`.
- **Brute armour**: `make_brute_armour.py` cuts eight plates from the body's shell by bone,
  in each bone's frame, for Anim Socket children (assets.json `Brute_Armour_*`, `bone`).
- Static meshes have no LODs in the engine and the Demo Lab lights live (DDGI/SSGI), so no
  lightmap UVs (UV1) are written.

## Checking

- `blender -b --factory-startup --python blender/preview.py -- <kit>` reads the written
  `.nmmesh` files back and renders a contact sheet (`Saved/ArtCache/Sheets/<kit>.png`;
  kits: Characters, L1, L2, L3, Airship).
- `make_review_world.py <Content/Levels folder>` writes two throwaway worlds
  (ArtReview_Characters, ArtReview_Kit) to look at the assets in the editor.
