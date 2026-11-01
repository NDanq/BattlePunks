#!/usr/bin/env python3
"""
make_materials.py -- Demo Lab's kit and terrain material graphs (without generated code):

    Content/Materials/Kit/<name>.material.nmgraph       every matdefs.py definition
    Content/Materials/Terrain/Island.material.nmgraph   Grass, Dirt, Rock, Sand by height

The SteamDummy looks are make_steamdummy_materials.py's. Then fill <gen_code> (build_all.py
does both):

    python Games/DemoLab/Tools/Art/make_materials.py
    <bin>/NodeGraphGenerate Games/DemoLab/Content/Materials/Kit/*.material.nmgraph ...
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import graphs                 # noqa: E402
from matdefs import MATS, KIT_TEX   # noqa: E402

GAME_DIR = Path(__file__).resolve().parents[2]
KIT_DIR = GAME_DIR / "Content" / "Materials" / "Kit"
TERRAIN_DIR = GAME_DIR / "Content" / "Materials" / "Terrain"


def main():
    written = []
    for d in MATS.values():
        g = graphs.kit_material(d, KIT_TEX)
        path = KIT_DIR / f"{d['name']}.material.nmgraph"
        g.write(path)
        written.append(path)
    # Layer order is the Terrain component's paint layers 0-3 (vertex colour R G B A).
    terrain = "Content/Textures/Terrain"
    g = graphs.terrain_material("Island", [(terrain, "Grass", 2.0, 0.9, True), (terrain, "Dirt", 2.0, 0.85, True),
                                           (KIT_TEX, "Rock", 4.0, 0.82, True), (terrain, "Sand", 2.0, 0.8, False)])
    path = TERRAIN_DIR / "Island.material.nmgraph"
    g.write(path)
    written.append(path)
    for p in written:
        print(f"  {p.relative_to(GAME_DIR)}")
    return written


if __name__ == "__main__":
    main()
