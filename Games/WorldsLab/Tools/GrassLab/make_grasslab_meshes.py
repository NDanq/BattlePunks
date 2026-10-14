#!/usr/bin/env python3
"""
make_grasslab_meshes.py -- GrassLab's grass, Content/Meshes/GrassLab/:

    GrassClump       a clump of 20 blades, 26-48 cm, pivot at its base: each a
                     thin tapered strip in five segments, leaning out from the
                     clump's middle and curving over toward its tip
    GrassClump_Far   the same idea, 12 wider blades in two segments (the
                     spawner's far mesh, past its LOD distance): as much
                     ground covered from afar for a fifth of the triangles

Geometry, not cards: no texture to clip, so the blades stay sharp at any
distance and the ball's push (make_grasslab_materials.py) bends real blades.
UV v runs from 1 at a blade's root to 0 at its tip, the weight the grass
material bends and shades by; u across it. Vertex colour varies each blade
a little (some yellower, some darker), times the spawner's per-instance
tint. Normals lean toward up, so a clump lights as one soft tuft rather
than as flat strips.

    python Games/WorldsLab/Tools/GrassLab/make_grasslab_meshes.py
"""

import math
import random
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Steampunk"))
import nmformats as nm  # noqa: E402

OUT      = GAME_DIR / "Content" / "Meshes" / "GrassLab"
MATERIAL = "Content/Materials/GrassLab/GrassBlade.material.nmgraph"


def blade(b, rng, segments, widen=1.0):
    """One blade: a strip from a root near the clump's middle, tapering to a point."""
    radius = rng.uniform(0.0, 7.0)
    around = rng.uniform(0.0, 2.0 * math.pi)
    root = (math.cos(around) * radius, 0.0, math.sin(around) * radius)
    height = rng.uniform(26.0, 48.0)
    width = rng.uniform(1.6, 2.6) * widen
    # Leans away from the middle (plus some scatter), and curves over more near the tip.
    lean_dir = around + rng.uniform(-0.6, 0.6)
    lean = rng.uniform(0.15, 0.5)            # how far the tip ends up out, as a share of the height
    curl = rng.uniform(1.6, 2.4)             # how late the curve comes
    face = lean_dir + math.pi / 2 + rng.uniform(-0.5, 0.5)   # the blade's flat side faces roughly along the lean
    side = (math.cos(face), 0.0, math.sin(face))
    out = (math.cos(lean_dir), 0.0, math.sin(lean_dir))
    shade = rng.uniform(0.82, 1.08)
    yellow = rng.uniform(0.0, 0.18)
    colour = (shade * (1.0 + yellow * 0.6), shade, shade * (1.0 - yellow), 1.0)

    def centre(t):
        return (root[0] + out[0] * lean * height * t ** curl,
                height * t * (1.0 - 0.25 * lean * t ** curl),
                root[2] + out[2] * lean * height * t ** curl)

    rows = []
    for k in range(segments + 1):
        t = k / segments
        c = centre(t)
        ahead = centre(min(t + 0.02, 1.0))
        behind = centre(max(t - 0.02, 0.0))
        along = nm.v_norm(nm.v_sub(ahead, behind))
        normal = nm.v_norm(nm.v_cross(side, along))
        # Mostly up: the tuft lights softly, as grass seen together does.
        normal = nm.v_norm(nm.v_add(nm.v_mul(normal, 0.45), (0.0, 1.0, 0.0)))
        half = 0.5 * width * (1.0 - t) ** 0.8
        v = 1.0 - t
        rows.append(((nm.v_add(c, nm.v_mul(side, -half)), normal, (0.0, v), colour),
                     (nm.v_add(c, nm.v_mul(side, half)), normal, (1.0, v), colour)))
    for k in range(segments):
        (a, b0), (c, d) = rows[k], rows[k + 1]
        if k == segments - 1:
            # The tip: one triangle to a point.
            tip = (nm.v_mul(nm.v_add(c[0], d[0]), 0.5), c[1], (0.5, 0.0), colour)
            b.triangle_raw(MATERIAL, (a, b0, tip))
        else:
            b.triangle_raw(MATERIAL, (a, b0, d))
            b.triangle_raw(MATERIAL, (a, d, c))


def clump(name, blades, segments, seed, widen=1.0):
    b = nm.MeshBuilder(name)
    rng = random.Random(seed)
    for _ in range(blades):
        blade(b, rng, segments, widen)
    return b


def main():
    for mesh in (clump("GrassClump", 20, 5, 31), clump("GrassClump_Far", 12, 2, 32, widen=1.7)):
        path = OUT / f"{mesh.name}.nmmesh"
        lo, hi = mesh.write(path, lightmap=False)
        print(f"  {path.relative_to(GAME_DIR)}: {mesh.triangle_count()} triangles, "
              f"{lo[1]:.0f}..{hi[1]:.0f} cm tall, {hi[0] - lo[0]:.0f} cm across")


if __name__ == "__main__":
    main()
