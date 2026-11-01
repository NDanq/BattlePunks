#!/usr/bin/env python3
"""
build_all.py -- rebuilds every Demo Lab art asset from its generator (see README.md).

    python Games/DemoLab/Tools/Art/build_all.py [steps...] [--sheets] [--bin <dir with NodeGraphGenerate>]

Steps, in order (all by default): textures, steamdummy, armour, l1, l2, l3, materials (and particles), generate,
manifest. --sheets also renders the contact sheets (Saved/ArtCache/Sheets/<kit>.png).
Needs python3 with numpy and Pillow, and Blender 4.2+ on PATH (or $BLENDER).
"""

import glob
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ART = Path(__file__).resolve().parent
GAME = ART.parents[1]
ROOT = GAME.parents[1]
CACHE = GAME / "Saved" / "ArtCache"
BLENDER = os.environ.get("BLENDER", "blender")
KITS = ["Characters", "L1", "L2", "L3", "Airship"]


def run(cmd):
    print("$", " ".join(str(c) for c in cmd), flush=True)
    t = time.time()
    subprocess.run([str(c) for c in cmd], check=True)
    print(f"  ({time.time() - t:.0f} s)")


def blender(script, *args):
    run([BLENDER, "-b", "--factory-startup", "--python", ART / "blender" / script, "--", *args])


def find_generator(explicit):
    if explicit:
        return Path(explicit) / "NodeGraphGenerate"
    roots = [ROOT]
    if ROOT.parent.name == "worktrees":          # a worktree checkout: the main checkout's builds too
        roots.append(ROOT.parents[2])
    found = sorted((f for r in roots for f in glob.glob(str(r / "Games" / "*" / "Binaries" / "*" / "NodeGraphGenerate"))),
                   key=os.path.getmtime)
    if not found:
        sys.exit("NodeGraphGenerate not found: build any editor configuration, or pass --bin")
    return Path(found[-1])


def manifest():
    assets = []
    for kit in KITS:
        path = CACHE / f"manifest_{kit}.json"
        if path.exists():
            assets += json.loads(path.read_text())
    doc = {
        "about": "Demo Lab art assets, written by Tools/Art/build_all.py. Engine frame: left-handed, +Y up, the "
                 "front of an asset faces -Z (as imported characters do); units are centimetres. size_m is the "
                 "mesh's bounds in metres (x, y, z); bounds_cm its min/max. A part is a separate mesh: place it "
                 "at the asset's position plus offset_cm (turned with the asset); 'axis' is what it moves along "
                 "or turns about, in the asset's frame. Every mesh's sections name their materials, so a Render "
                 "component needs no material slots (except the SteamDummy: pick its look in slot 0).",
        "grids": {"L2": "400 cm: walls 400 wide x 500 tall, floors and ceilings 400 x 400",
                  "L3": "catwalks 400 cm straight, 22.5-degree arcs of radius 700-850 cm"},
        "terrain": {"material": "Content/Materials/Terrain/Island.material.nmgraph",
                    "layers": ["Grass", "Dirt (the road)", "Rock", "Sand"],
                    "uv_tile_size": 100,
                    "note": "paint layer 0-3 = vertex colour R G B A; set the Terrain component's UV Tile Size to "
                            "100 (1 m) so each layer repeats at its own size (2, 2, 4, 2 m)"},
        "particles": {
            "SteamVent": "Content/Particles/SteamVent.nmparticle -- a vent's column and wisps, up +Y (L1 vents, L3)",
            "SteamJet": "Content/Particles/SteamJet.nmparticle -- a leaking joint's bursts along +Y every 2.5 s",
            "Sparks": "Content/Particles/Sparks.nmparticle -- spark bursts that bounce at the entity's height, a flash",
            "ReactorArcs": "Content/Particles/ReactorArcs.nmparticle -- four lightning beams (Beam End, or the "
                           "component's Beam Target) and spark showers; put it at ReactorCore's dome",
            "SprinklerSpray": "Content/Particles/SprinklerSpray.nmparticle -- droplets and mist along +Y: turn the "
                              "entity 180 about X under a ceiling head; drops die 450 cm below",
            "DustMotes": "Content/Particles/DustMotes.nmparticle -- motes in a 6 x 4 x 6 m box (light shafts)",
        },
        "assets": assets,
    }
    (ART / "assets.json").write_text(json.dumps(doc, indent=1) + "\n")
    print(f"assets.json: {len(assets)} assets")


def main(argv):
    sheets = "--sheets" in argv
    bin_dir = argv[argv.index("--bin") + 1] if "--bin" in argv else None
    steps = [a for a in argv if not a.startswith("--") and a != bin_dir] or \
        ["textures", "steamdummy", "armour", "l1", "l2", "l3", "materials", "generate", "manifest"]
    for step in steps:
        if step == "textures":
            run([sys.executable, ART / "make_textures.py"])
        elif step == "steamdummy":
            blender("make_steamdummy.py")
        elif step == "armour":
            blender("make_brute_armour.py")
        elif step in ("l1", "l2", "l3"):
            blender(f"make_{step}.py")
        elif step == "materials":
            run([sys.executable, ART / "make_materials.py"])
            run([sys.executable, ART / "make_steamdummy_materials.py"])
            run([sys.executable, ART / "make_particles.py"])
        elif step == "generate":
            graphs = sorted(glob.glob(str(GAME / "Content" / "Materials" / "Kit" / "*.material.nmgraph"))
                            + glob.glob(str(GAME / "Content" / "Materials" / "Terrain" / "*.material.nmgraph"))
                            + glob.glob(str(GAME / "Content" / "Materials" / "Characters" / "SteamDummy_*.material.nmgraph"))
                            + glob.glob(str(GAME / "Content" / "Materials" / "Particles" / "*.material.nmgraph")))
            run([find_generator(bin_dir), *graphs])
        elif step == "manifest":
            manifest()
        else:
            sys.exit(f"unknown step {step}")
    if sheets:
        for kit in KITS:
            blender("preview.py", kit)


if __name__ == "__main__":
    main(sys.argv[1:])
