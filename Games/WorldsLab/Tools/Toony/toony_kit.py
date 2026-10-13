#!/usr/bin/env python3
"""
toony_kit.py -- what the Toony style's generators share: the palette, the
paths, and chunky toy-box geometry.

THE LOOK. Flat, saturated colours in the meshes' vertex colours (one Lit
material, ToonPaint, multiplies them: high roughness, little specular),
chunky shapes with chamfered top edges so the ink outline has corners to
catch, a soft fake occlusion darkening each piece toward its foot, and the
post stack's ink and banding (Toony.pp.nmgraph, make_toony_materials.py).
"""

import math
import platform
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "Steampunk"))
import nmformats as nm   # noqa: E402

GAME_DIR = HERE.parents[1]
REPO_DIR = GAME_DIR.parents[1]
STYLE = "Toony"


def generator():
    """NodeGraphGenerate from the editor-debug preset's binaries."""
    folder = "Windows_Debug" if platform.system() == "Windows" else "Linux_Debug"
    name = "NodeGraphGenerate.exe" if platform.system() == "Windows" else "NodeGraphGenerate"
    return GAME_DIR / "Binaries" / folder / name


def mat(name):
    return f"Content/Materials/{STYLE}/{name}.material.nmgraph"


PAINT = mat("ToonPaint")

# The palette: candy colours, 0-1 sRGB.
GRASS      = (0.46, 0.78, 0.33)
GRASS_DARK = (0.36, 0.66, 0.27)
SAND       = (0.96, 0.84, 0.55)
STONE      = (0.80, 0.78, 0.86)
STONE_DARK = (0.62, 0.60, 0.72)
WOOD       = (0.84, 0.55, 0.32)
WOOD_DARK  = (0.62, 0.38, 0.22)
MUD        = (0.47, 0.33, 0.22)
WATER      = (0.32, 0.68, 0.94)
RED        = (0.93, 0.29, 0.31)
RED_LIGHT  = (1.00, 0.56, 0.52)
BLUE       = (0.25, 0.52, 0.95)
BLUE_LIGHT = (0.55, 0.75, 1.00)
YELLOW     = (1.00, 0.82, 0.25)
PURPLE     = (0.66, 0.45, 0.88)
ORANGE     = (1.00, 0.60, 0.22)
WHITE      = (0.97, 0.96, 0.94)
PIT        = (0.30, 0.24, 0.34)


def shade(colour, k):
    return tuple(max(0.0, min(1.0, c * k)) for c in colour)


def toy_box(mb, lo, hi, colour, bevel=8.0, top=None, occlusion=0.72, skip_bottom=True, material=PAINT):
    """A box with its top edges chamfered by bevel: the top inset, four slopes
    down to the sides, the sides to the floor. Darker toward its foot (a fake
    occlusion, occlusion at y0), top tinted by top if given."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    b = max(0.0, min(bevel, (x1 - x0) / 3.0, (z1 - z0) / 3.0, (y1 - y0) / 2.0))
    top_colour = top or colour
    yb = y1 - b
    height = max(y1 - y0, 1.0)

    def at(p, base):
        t = (p[1] - y0) / height
        k = occlusion + (1.0 - occlusion) * min(1.0, t * 1.6)
        return shade(base, k) + (1.0,)

    def face(points, normal, base):
        mb.polygon(material, points, normal=normal, colours=[at(p, base) for p in points])

    # Top.
    face([(x0 + b, y1, z0 + b), (x1 - b, y1, z0 + b), (x1 - b, y1, z1 - b), (x0 + b, y1, z1 - b)], (0, 1, 0), top_colour)
    if b > 0.0:
        s = 1.0 / math.sqrt(2.0)
        face([(x0, yb, z0), (x1, yb, z0), (x1 - b, y1, z0 + b), (x0 + b, y1, z0 + b)], (0, s, -s), top_colour)
        face([(x1, yb, z1), (x0, yb, z1), (x0 + b, y1, z1 - b), (x1 - b, y1, z1 - b)], (0, s, s), top_colour)
        face([(x0, yb, z1), (x0, yb, z0), (x0 + b, y1, z0 + b), (x0 + b, y1, z1 - b)], (-s, s, 0), top_colour)
        face([(x1, yb, z0), (x1, yb, z1), (x1 - b, y1, z1 - b), (x1 - b, y1, z0 + b)], (s, s, 0), top_colour)
    # Sides.
    face([(x0, y0, z0), (x1, y0, z0), (x1, yb, z0), (x0, yb, z0)], (0, 0, -1), colour)
    face([(x1, y0, z1), (x0, y0, z1), (x0, yb, z1), (x1, yb, z1)], (0, 0, 1), colour)
    face([(x0, y0, z1), (x0, y0, z0), (x0, yb, z0), (x0, yb, z1)], (-1, 0, 0), colour)
    face([(x1, y0, z0), (x1, y0, z1), (x1, yb, z1), (x1, yb, z0)], (1, 0, 0), colour)
    if not skip_bottom:
        face([(x0, y0, z1), (x1, y0, z1), (x1, y0, z0), (x0, y0, z0)], (0, -1, 0), shade(colour, occlusion))


def toy_cylinder(mb, centre, radius, y0, y1, colour, segments=12, cap=True, occlusion=0.72, material=PAINT):
    """A faceted post: segments sides, a flat cap."""
    cx, cz = centre
    height = max(y1 - y0, 1.0)
    for i in range(segments):
        a0 = 2.0 * math.pi * i / segments
        a1 = 2.0 * math.pi * (i + 1) / segments
        p0 = (cx + math.sin(a0) * radius, y0, cz + math.cos(a0) * radius)
        p1 = (cx + math.sin(a1) * radius, y0, cz + math.cos(a1) * radius)
        p2 = (p1[0], y1, p1[2])
        p3 = (p0[0], y1, p0[2])
        mid = (a0 + a1) / 2.0
        normal = (math.sin(mid), 0.0, math.cos(mid))
        cols = [shade(colour, occlusion if p[1] == y0 else 1.0) + (1.0,) for p in (p0, p1, p2, p3)]
        mb.polygon(material, [p1, p0, p3, p2], normal=normal, colours=cols)
    if cap:
        ring = [(cx + math.sin(2.0 * math.pi * i / segments) * radius, y1, cz + math.cos(2.0 * math.pi * i / segments) * radius)
                for i in range(segments)]
        mb.polygon(material, list(reversed(ring)), normal=(0, 1, 0), colours=[colour + (1.0,)] * segments)


def ramp(mb, x0, x1, z0, z1, y_at_x0, y_at_x1, colour, along="x", side_colour=None, material=PAINT):
    """A solid wedge: its top slopes along X (or Z) from y_at_x0 to y_at_x1,
    filled down to y=0, with side walls."""
    side_colour = side_colour or shade(colour, 0.82)
    if along == "x":
        top = [(x0, y_at_x0, z0), (x1, y_at_x1, z0), (x1, y_at_x1, z1), (x0, y_at_x0, z1)]
        mb.polygon(material, top, normal=nm.v_norm((-(y_at_x1 - y_at_x0) / max(x1 - x0, 1e-3), 1.0, 0.0)),
                   colours=[colour + (1.0,)] * 4)
        for z, sign in ((z0, -1.0), (z1, 1.0)):
            pts = [(x0, 0.0, z), (x1, 0.0, z), (x1, y_at_x1, z), (x0, y_at_x0, z)]
            mb.polygon(material, pts if sign < 0 else list(reversed(pts)), normal=(0, 0, sign),
                       colours=[shade(side_colour, 0.75 if p[1] == 0.0 else 1.0) + (1.0,) for p in (pts if sign < 0 else list(reversed(pts)))])
        for x, y, sign in ((x0, y_at_x0, -1.0), (x1, y_at_x1, 1.0)):
            if y <= 0.5:
                continue
            pts = [(x, 0.0, z1), (x, 0.0, z0), (x, y, z0), (x, y, z1)] if sign < 0 else [(x, 0.0, z0), (x, 0.0, z1), (x, y, z1), (x, y, z0)]
            mb.polygon(material, pts, normal=(sign, 0, 0), colours=[shade(side_colour, 0.75 if p[1] == 0.0 else 1.0) + (1.0,) for p in pts])
    else:
        # Along Z: y_at_x0 is the height at z0, y_at_x1 at z1.
        top = [(x0, y_at_x0, z0), (x1, y_at_x0, z0), (x1, y_at_x1, z1), (x0, y_at_x1, z1)]
        mb.polygon(material, top, normal=nm.v_norm((0.0, 1.0, -(y_at_x1 - y_at_x0) / max(z1 - z0, 1e-3))),
                   colours=[colour + (1.0,)] * 4)
        for x, sign in ((x0, -1.0), (x1, 1.0)):
            pts = [(x, 0.0, z1), (x, 0.0, z0), (x, y_at_x0, z0), (x, y_at_x1, z1)]
            if sign > 0:
                pts = list(reversed(pts))
            mb.polygon(material, pts, normal=(sign, 0, 0), colours=[shade(side_colour, 0.75 if p[1] == 0.0 else 1.0) + (1.0,) for p in pts])
        for z, y, sign in ((z0, y_at_x0, -1.0), (z1, y_at_x1, 1.0)):
            if y <= 0.5:
                continue
            pts = [(x0, 0.0, z), (x1, 0.0, z), (x1, y, z), (x0, y, z)]
            if sign > 0:
                pts = list(reversed(pts))
            mb.polygon(material, pts, normal=(0, 0, sign), colours=[shade(side_colour, 0.75 if p[1] == 0.0 else 1.0) + (1.0,) for p in pts])
