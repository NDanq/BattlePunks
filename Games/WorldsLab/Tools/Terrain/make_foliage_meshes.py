#!/usr/bin/env python3
"""
make_foliage_meshes.py -- meshes for terrain foliage tests, pivots at their
base, in Content/Meshes/Foliage/:

    GrassTuft        three crossed leaf cards, 70 cm (Courtyard Foliage, masked, wind)
    Bush             six cards round a 1.3 m dome
    Pine             a trunk and four stacked needle cones, 9 m; Pine_LOD1 (fewer
                     sides, two cones) and Pine_LOD2 (one cone, five sides)
    Rock             a lumpy, flattened ball, 1.2 m (Courtyard Stone)

    python Games/WorldsLab/Tools/Terrain/make_foliage_meshes.py
"""

import math
import random
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Steampunk"))
import nmformats as nm  # noqa: E402

OUT     = GAME_DIR / "Content" / "Meshes" / "Foliage"
LEAVES  = "Content/Materials/Courtyard/Foliage.material.nmgraph"
STONE   = "Content/Materials/Courtyard/Stone.material.nmgraph"
WOOD    = "Content/Materials/Courtyard/Wood.material.nmgraph"
UP      = (0.0, 1.0, 0.0)


def card(b, centre, yaw, width, height, lean=0.0):
    """A vertical leaf card through centre, turned yaw, leaning outward; normals up (lit like the ground)."""
    c, s = math.cos(yaw), math.sin(yaw)
    half = width / 2.0
    lx, lz = -s * lean, c * lean   # the top leans out, perpendicular to the card
    p0 = (centre[0] - c * half, centre[1], centre[2] - s * half)
    p1 = (centre[0] + c * half, centre[1], centre[2] + s * half)
    p2 = (p1[0] + lx, centre[1] + height, p1[2] + lz)
    p3 = (p0[0] + lx, centre[1] + height, p0[2] + lz)
    # Normals mostly up, leaning off the card: lit like a soft clump, not a flat floor.
    out = nm.v_norm((-s * 0.45, 1.0, c * 0.45))
    b.polygon(LEAVES, [p0, p1, p2, p3], normal=out, uvs=[(0, 1), (1, 1), (1, 0), (0, 0)])


def grass():
    b = nm.MeshBuilder("GrassTuft")
    for k in range(3):
        card(b, (0.0, 0.0, 0.0), k * math.pi / 3.0, 60.0, 70.0)
    return b


def bush():
    b = nm.MeshBuilder("Bush")
    rng = random.Random(4)
    for k in range(6):
        yaw = k * math.pi / 6.0 + rng.uniform(-0.2, 0.2)
        card(b, (rng.uniform(-15, 15), -5.0, rng.uniform(-15, 15)), yaw, 140.0, 130.0, lean=rng.uniform(-20, 20))
    for k in range(4):
        yaw = rng.uniform(0, math.pi)
        r, a = rng.uniform(30, 55), rng.uniform(0, 2 * math.pi)
        card(b, (r * math.cos(a), 10.0, r * math.sin(a)), yaw, 90.0, 80.0, lean=rng.uniform(-25, 25))
    return b


def cone(b, material, y0, y1, radius, sides, skew=0.0):
    """A cone from radius at y0 to a point at y1, its rim wavy; normals leaning out and up."""
    rim = []
    for i in range(sides):
        a = 2 * math.pi * i / sides + skew
        r = radius * (0.85 + 0.15 * math.sin(i * 2.7 + skew * 5))
        rim.append((r * math.cos(a), y0 + 8.0 * math.sin(i * 1.9), r * math.sin(a)))
    tip = (0.0, y1, 0.0)
    slope = radius / max(y1 - y0, 1.0)
    for i in range(sides):
        p, q = rim[i], rim[(i + 1) % sides]
        mid = ((p[0] + q[0]) / 2, 0.0, (p[2] + q[2]) / 2)
        out = nm.v_norm((mid[0], slope * radius, mid[2]))
        # The leaf texture once per face (a card's UVs): world-tiled, its alpha would clip most of it.
        b.polygon(material, [p, q, tip], normal=out, uvs=[(0, 1), (1, 1), (0.5, 0)])
        # Its underside, so the masked needles read from below too.
        b.polygon(material, [q, p, (0.0, y0 + (y1 - y0) * 0.25, 0.0)], normal=nm.v_norm((mid[0], -0.3 * radius, mid[2])),
                  uvs=[(0, 1), (1, 1), (0.5, 0)])


def trunk(b, height, radius, sides):
    b.lathe(WOOD, [(radius, 0.0), (radius * 0.6, height)], segments=sides)


def pine(lod):
    b = nm.MeshBuilder(f"Pine{'' if lod == 0 else f'_LOD{lod}'}")
    sides = (12, 7, 5)[lod]
    trunk(b, 450.0, 22.0, (8, 5, 4)[lod])
    tiers = [(150, 470, 260), (300, 620, 210), (450, 760, 160), (600, 900, 110)]
    if lod == 1:
        tiers = [(150, 620, 260), (430, 900, 170)]
    elif lod == 2:
        tiers = [(150, 900, 250)]
    for k, (y0, y1, r) in enumerate(tiers):
        cone(b, LEAVES, y0, y1, r, sides, skew=k * 0.4)
    return b


def rock():
    b = nm.MeshBuilder("Rock")
    rng = random.Random(9)
    rings, sides = 6, 10
    bumps = [[rng.uniform(0.82, 1.15) for _ in range(sides)] for _ in range(rings + 1)]
    pts = []
    for i in range(rings + 1):
        phi = math.pi * i / rings
        row = []
        for j in range(sides):
            theta = 2 * math.pi * j / sides
            r = 60.0 * bumps[i][j]
            row.append((r * math.sin(phi) * math.cos(theta), 45.0 * math.cos(phi) * bumps[i][j] + 20.0,
                        r * 0.85 * math.sin(phi) * math.sin(theta)))
        pts.append(row)
    for i in range(rings):
        for j in range(sides):
            a, c = pts[i][j], pts[i + 1][j]
            bb, d = pts[i][(j + 1) % sides], pts[i + 1][(j + 1) % sides]
            # Wound to face outward (counter-clockwise seen from outside), so the normals point out.
            b.polygon(STONE, [a, bb, d, c]) if i not in (0, rings - 1) else None
            if i == 0:
                b.polygon(STONE, [a, d, c])
            if i == rings - 1:
                b.polygon(STONE, [a, bb, c])
    return b


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for b in (grass(), bush(), pine(0), pine(1), pine(2), rock()):
        path = OUT / f"{b.name}.nmmesh"
        b.write(path, lightmap=False)
        print(f"  {path.relative_to(GAME_DIR)}: {b.triangle_count()} triangles")


if __name__ == "__main__":
    main()
