#!/usr/bin/env python3
"""
make_l1_foliage_meshes.py -- L1 Foothills' foliage meshes, pivots at their base, in
Content/Meshes/Foliage/L1_*, drawn with make_l1_foliage_materials.py's materials
(copied from WorldsLab Tools/Terrain/make_foliage_meshes.py; the bush and rock left out,
the kit has rocks; a wildflower tuft added):

    L1_GrassTuft     four crossed blade cards, 70 cm (L1_Grass)
    L1_Wildflowers   three crossed cards of flowering stems, 60 cm (L1_Wildflower)
    L1_Spruce        a spruce, 9 m: a tapering trunk (L1_SpruceBark) and whorls of
                     drooping branch fronds with hanging side sprays (L1_SpruceNeedles),
                     ~650 cards; L1_Spruce_LOD1 (~140 wider cards, no side sprays)
                     and L1_Spruce_LOD2 (~45)

Cards carry both faces, each with the plant's normal: leaning out from the trunk
(or the bush's middle) and up, so a crown lights as one rounded mass rather than
as flat cards, and its underside as well as its top. Vertex colour RGB is ambient
occlusion (dark inside the crown and low down, open at the branch tips); A is how
far the vertex bends in the wind (0 at the roots, 1 at the top). A frond's UV u
runs 0 at the trunk to 1 at its tip; its outline hugs the frond in the texture, so
its shadow (the Shadows stage casts a card whole) is the frond's shape.

    python Games/DemoLab/Tools/Art/l1_foliage/make_l1_foliage_meshes.py
"""

import math
import random
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import nmformats as nm  # noqa: E402

OUT     = GAME_DIR / "Content" / "Meshes" / "Foliage"
MAT     = "Content/Materials/Foliage/"
NEEDLES = MAT + "L1_SpruceNeedles.material.nmgraph"
BARK    = MAT + "L1_SpruceBark.material.nmgraph"
GRASS   = MAT + "L1_Grass.material.nmgraph"
FLOWERS = MAT + "L1_Wildflower.material.nmgraph"

PINE_HEIGHT = 900.0


def raw_triangle(b, material, verts):
    """verts: three (position, normal, uv, colour), wound as given (counter-clockwise seen
    from the front) -- MeshBuilder.triangle_raw would turn a back face round to its normal."""
    a, c, d = verts
    face = nm.v_cross(nm.v_sub(c[0], a[0]), nm.v_sub(d[0], a[0]))
    if nm.v_dot(face, face) < 1e-10:
        return
    chart = object()
    b._tris(material).append(tuple(v + (chart,) for v in verts))


def two_faced_quad(b, material, v0, v1, v2, v3):
    """A quad v0 v1 v2 v3 and its back, each with the vertices' own normals."""
    raw_triangle(b, material, (v0, v1, v2))
    raw_triangle(b, material, (v0, v2, v3))
    raw_triangle(b, material, (v0, v2, v1))
    raw_triangle(b, material, (v0, v3, v2))


def blend(a, b_, t):
    return nm.v_norm(nm.v_add(nm.v_mul(a, 1.0 - t), nm.v_mul(b_, t)))


# --- Grass and bush -------------------------------------------------------------------------------

def card(b, material, centre, yaw, width, height, lean=0.0, out_from=None, ao_foot=0.45, bend=None):
    """An upright card through centre, turned yaw, its top leaning out. Its normals lean
    out from out_from (the plant's middle) and up; its foot darker (ao_foot)."""
    c, s = math.cos(yaw), math.sin(yaw)
    half = width / 2.0
    lx, lz = -s * lean, c * lean
    p0 = (centre[0] - c * half, centre[1], centre[2] - s * half)
    p1 = (centre[0] + c * half, centre[1], centre[2] + s * half)
    p2 = (p1[0] + lx, centre[1] + height, p1[2] + lz)
    p3 = (p0[0] + lx, centre[1] + height, p0[2] + lz)
    mid = out_from if out_from is not None else (centre[0], centre[1] - height, centre[2])
    top = (bend or (lambda y: 1.0))

    def vert(p, uv, foot):
        out = nm.v_sub(p, mid)
        n = blend((0.0, 1.0, 0.0), nm.v_norm((out[0], 0.0, out[2])) if abs(out[0]) + abs(out[2]) > 1e-3 else (0, 1, 0), 0.45)
        ao = ao_foot if foot else 1.0
        return (p, n, uv, (ao, ao, ao, 0.0 if foot else top(p[1])))

    two_faced_quad(b, material, vert(p0, (0, 1), True), vert(p1, (1, 1), True), vert(p2, (1, 0), False),
                   vert(p3, (0, 0), False))


def grass():
    b = nm.MeshBuilder("L1_GrassTuft")
    for k in range(4):
        card(b, GRASS, (0.0, -2.0, 0.0), k * math.pi / 4.0 + 0.2, 62.0, 72.0, lean=(6.0 if k % 2 else -6.0),
             out_from=(0.0, -30.0, 0.0), ao_foot=0.4)
    return b


def wildflowers():
    b = nm.MeshBuilder("L1_Wildflowers")
    for k in range(3):
        card(b, FLOWERS, (0.0, -2.0, 0.0), k * math.pi / 3.0 + 0.4, 55.0, 62.0, lean=(5.0 if k % 2 else -5.0),
             out_from=(0.0, -30.0, 0.0), ao_foot=0.45)
    return b


# --- Pine ----------------------------------------------------------------------------------------

def trunk_radius(y):
    """Tapering to a point at the top, flared at the roots."""
    return 19.0 * max(1.0 - y / (PINE_HEIGHT * 1.02), 0.0) ** 1.1 + 9.0 * math.exp(-y / 22.0) + 0.6


def bend_weight(y):
    return max(0.0, min(1.0, y / PINE_HEIGHT)) ** 2


def trunk(b, sides, rings):
    """Smooth, its UVs in bark repeats: two around, one every 1.6 m up."""
    ys = [0.0, 12.0, 35.0] + [70.0 + (PINE_HEIGHT - 70.0) * (k / (rings - 1)) ** 0.9 for k in range(rings)]
    ring = []
    for y in ys:
        r = trunk_radius(y)
        slope = (trunk_radius(y + 5.0) - trunk_radius(y - 5.0 if y >= 5.0 else 0.0)) / 10.0
        ao = 0.85 if y < 110.0 else 0.6 + 0.3 * y / PINE_HEIGHT
        row = []
        for j in range(sides + 1):
            a = 2.0 * math.pi * j / sides
            ca, sa = math.cos(a), math.sin(a)
            n = nm.v_norm((ca, -slope, sa))
            row.append(((r * ca, y, r * sa), n, (2.0 * j / sides, -y / 160.0), (ao, ao, ao, bend_weight(y))))
        ring.append(row)
    for i in range(len(ring) - 1):
        for j in range(sides):
            a, c = ring[i][j], ring[i][j + 1]
            d, e = ring[i + 1][j + 1], ring[i + 1][j]
            # Counter-clockwise seen from outside.
            raw_triangle(b, BARK, (a, e, d))
            raw_triangle(b, BARK, (a, d, c))


def frond_outline(s):
    """The frond's half-width in the texture at s along it, 0-1 of the card's half-width
    (make_foliage_textures.py: twigs as long as sin(pi s^0.75), less a margin)."""
    return max(0.12, min(1.0, 1.05 * math.sin(math.pi * min(max(s, 0.0) / 0.98, 1.0) ** 0.75)))


def frond(b, root, azimuth, elevation, length, width, droop, roll, segments, crown_y):
    """A drooping branch card from root along azimuth (radians round Y) rising at elevation,
    its tip sagging by droop, rolled about its own axis by roll; both faces."""
    ca, sa = math.cos(azimuth), math.sin(azimuth)
    along = (ca * math.cos(elevation), math.sin(elevation), sa * math.cos(elevation))
    across = nm.v_norm(nm.v_cross((0.0, 1.0, 0.0), along))
    up = nm.v_cross(along, across)
    # Rolled a little, so the cards are not all flat.
    cr, sr = math.cos(roll), math.sin(roll)
    across, up = nm.v_add(nm.v_mul(across, cr), nm.v_mul(up, sr)), nm.v_add(nm.v_mul(up, cr), nm.v_mul(across, -sr))
    if up[1] < 0.0:
        across, up = nm.v_mul(across, -1.0), nm.v_mul(up, -1.0)
    rows = []
    for k in range(segments + 1):
        s = k / segments
        centre = nm.v_add(root, nm.v_mul(along, length * s))
        centre = (centre[0], centre[1] - droop * s * s, centre[2])
        half = 0.5 * width * frond_outline(s)
        fold = 0.15 * half                      # the edges hang a little below the twig
        u = 0.02 + 0.95 * s
        row = []
        for side, v in ((-1.0, 0.5 - 0.5 * frond_outline(s)), (0.0, 0.5), (1.0, 0.5 + 0.5 * frond_outline(s))):
            p = nm.v_add(centre, nm.v_mul(across, side * half))
            if side != 0.0:
                p = nm.v_add(p, nm.v_mul(up, -fold))
            # Out from the crown's axis, and up: the crown lights as one rounded mass.
            out = (p[0], (p[1] - crown_y) * 0.35, p[2])
            n = blend(nm.v_norm(out) if nm.v_len(out) > 1e-3 else (0, 1, 0), (0.0, 1.0, 0.0), 0.3)
            reach = math.hypot(p[0], p[2])
            ao = min(1.0, 0.42 + 0.58 * min(reach / max(length, 1.0), 1.0) ** 0.8) * (0.82 + 0.18 * p[1] / PINE_HEIGHT)
            row.append((p, n, (u, v), (ao, ao, ao, bend_weight(p[1]))))
        rows.append(row)
    for k in range(segments):
        for j in range(2):
            two_faced_quad(b, NEEDLES, rows[k][j], rows[k][j + 1], rows[k + 1][j + 1], rows[k + 1][j])


def crown_length(y):
    """How far a branch at height y reaches: the spruce's cone, a little rounded at the foot."""
    rel = (y - 120.0) / (PINE_HEIGHT - 120.0)
    return 300.0 * (1.0 - rel) ** 0.95 * (1.0 - 0.15 * max(0.0, 0.12 - rel) / 0.12) + 22.0


def pine(lod):
    b = nm.MeshBuilder(f"L1_Spruce{'' if lod == 0 else f'_LOD{lod}'}")
    rng = random.Random(23)
    trunk(b, (10, 7, 5)[lod], (12, 7, 4)[lod])
    spacing = (30.0, 52.0, 95.0)[lod]
    widen = (1.0, 1.3, 1.7)[lod]
    segments = (4, 3, 2)[lod]
    y, turn = 125.0, 0.0
    while y < PINE_HEIGHT - 50.0:
        rel = (y - 120.0) / (PINE_HEIGHT - 120.0)
        count = max(3, round((8, 6, 4)[lod] - (4, 2, 1)[lod] * rel))
        turn += 2.39996                              # the golden angle, whorl to whorl
        for j in range(count):
            az = turn + 2.0 * math.pi * j / count + rng.uniform(-0.25, 0.25)
            length = crown_length(y) * rng.uniform(0.88, 1.08)
            elevation = math.radians(-6.0 + 34.0 * rel + rng.uniform(-6, 6))
            droop = length * (0.32 - 0.22 * rel)
            root = (math.cos(az) * trunk_radius(y) * 0.6, y + rng.uniform(-4, 4), math.sin(az) * trunk_radius(y) * 0.6)
            width = length * 0.68 * widen
            roll = math.radians(rng.uniform(-22, 22))
            frond(b, root, az, elevation, length, width, droop, roll, segments, y)
            if lod == 0 and length > 90.0:
                # Side sprays off the branch, splayed out and hanging: the crown filled in.
                for s, side in ((0.25, 1.0), (0.45, -1.0), (0.65, 1.0 if rng.random() < 0.5 else -1.0)):
                    off = nm.v_add(root, (math.cos(az) * length * s * math.cos(elevation), -droop * s * s,
                                          math.sin(az) * length * s * math.cos(elevation)))
                    sub = length * (1.0 - s) * rng.uniform(0.55, 0.75)
                    frond(b, off, az + side * rng.uniform(0.4, 0.75), elevation - math.radians(rng.uniform(10, 30)), sub,
                          sub * 0.65, sub * 0.3, side * math.radians(rng.uniform(15, 50)), 2, y)
        y += spacing * rng.uniform(0.85, 1.15) * (1.0 - 0.3 * rel)
    # The leader: two narrow sprays standing up from the tip, crossed.
    for j in range(2):
        frond(b, (0.0, PINE_HEIGHT - 70.0, 0.0), 1.2 + j * math.pi / 2.0, math.radians(82.0), 65.0, 22.0 * widen, 0.0,
              0.0, 2, PINE_HEIGHT - 20.0)
    return b


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for b in (grass(), wildflowers(), pine(0), pine(1), pine(2)):
        path = OUT / f"{b.name}.nmmesh"
        b.write(path, lightmap=False)
        print(f"  {path.relative_to(GAME_DIR)}: {b.triangle_count()} triangles")


if __name__ == "__main__":
    main()
