#!/usr/bin/env python3
"""
make_courtyard_level.py -- writes WorldsLab's Courtyard world,
a GPU test world aiming for photorealism: a Spanish
Colonial Revival house round a courtyard, at sunset.

    Content/Meshes/Courtyard/*.nmmesh   the building, as world-space meshes
    Content/Levels/Courtyard.nmworld    the world: the manifest and its folder
                                        of .nmentity files

    The house: lime-stucco walls 60 thick with deep window reveals, round a
    cloister. A great hall (the sala) on the north, 22 x 8 m and 7.6 m to its
    ceiling of hand-hewn vigas on carved corbels, a Talavera tile wainscot, a
    fireplace and two iron chandeliers; three glazed French doors and two
    windows open onto the courtyard, clerestory windows above the gallery
    roof, and five tall arched windows look north. Single-storey wings on the
    other three sides, the south one pierced by the zaguan: the arched
    entrance with its stone surround and studded doors swung open.

    The courtyard: an arcade of 16 Tuscan columns carrying round arches,
    a gallery behind them under a shed roof of exposed rafters, Saltillo tile
    underfoot; in the open patio, brick pavers, an octagonal fountain clad in
    Talavera tile with a two-tier basin, running (a jet into the small bowl,
    sheets spilling from each rim, splashes: Fountain.nmparticle), and
    potted citrus.

    Roofs of handmade barrel tile, course by course, over exposed rafter
    tails. Windows are green-painted wood -- casements, French doors and
    clerestories -- glazed with old wavy glass (Glass: translucent, its
    opacity rising with Fresnel so it reflects at grazing angles); the
    street-facing ones have wrought-iron rejas.

    Lights: the setting sun, low in the west-south-west and cascaded
    (Stationary: live direct light and shadows, its bounce bakeable); the
    sunset sky's image-based light (make_courtyard_sky.py's HDRI, its disk
    where the sun shines from); reflection probes for the patio, the hall
    and the outside; the hall's two chandeliers and its fire (flames and embers,
    Content/Particles/Courtyard/Hearth.nmparticle; its light flickers), and eight
    lanterns round the gallery, already lit.

The building's static meshes carry lightmap UVs, so Build > Bake Lightmaps
(the world has baked lighting on) adds bounced light; without a bake it draws
live. Materials are Content/Materials/Courtyard (make_courtyard_materials.py),
textures Content/Textures/Courtyard (make_courtyard_textures.py).

Entity GUIDs come from their names, so running it again rewrites the same
files. Usage:

    python Games/WorldsLab/Tools/Courtyard/make_courtyard_level.py
"""

import hashlib
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_courtyard_sky import SUN_PITCH, SUN_YAW  # noqa: E402  -- the sky's sun and the light agree
from courtyard_geometry import (Builder, Opening, Wall, RoofSlope, aabb, beam, box, build_wall, combo, cross, lathe,  # noqa: E402
                                length, mul, norm, polygon_prism, rafters, rand, roof_deck, sub, tile_ridge, tile_slope,
                                tube, X_AXIS, Y_AXIS, Z_AXIS)

GAME_DIR = Path(__file__).resolve().parents[2]
MESH_DIR = GAME_DIR / "Content" / "Meshes" / "Courtyard"
WORLD    = GAME_DIR / "Content" / "Levels" / "Courtyard.nmworld"
FOLDER   = WORLD.with_suffix("")

SKY_TEXTURE = "Content/Textures/Courtyard/SunsetSky.nmtexture"   # make_courtyard_sky.py
POST_STACK  = "Content/PostProcess/Courtyard/Courtyard.pp.nmgraph"


def material(name):
    return f"Content/Materials/Courtyard/{name}.material.nmgraph"


def mesh_path(name):
    return f"Content/Meshes/Courtyard/{name}.nmmesh"


# --- The plan (world units, centimetres; y up, the patio at y = 0) -----------------------------

T        = 60.0      # walls
FL       = 20.0      # the floors indoors and under the gallery, one step above the patio
OUT_X    = 1645.0    # the wings' outer walls, centre lines
IN_X     = 1145.0    # the courtyard walls east and west (and the hall's end walls)
SOUTH_Z  = -1315.0   # the south front
CY_S     = -815.0    # the courtyard's south wall
CY_N     = 815.0     # the hall's courtyard wall
NORTH_Z  = 1715.0    # the north front

WING_TOP   = 575.0   # the wings' walls: their roof deck's underside at the eave walls
HALL_TOP   = 800.0
WING_PITCH = 20.0
HALL_PITCH = 22.0
WING_CEIL  = 500.0
HALL_CEIL  = 760.0
DECK       = 3.0     # roof deck: the tiles sit DECK above a wall top
EAVE       = 45.0    # roof overhang past the walls

ARC_X   = 825.0      # the arcade's lines
ARC_Z   = 495.0
TA      = 50.0       # the arcade wall above the arches
BAY     = 330.0
SPRING  = 280.0      # the arches spring from the capitals
ARC_TOP = 460.0
CAPITAL = 54.0
GAL_WALL = 530.0     # the gallery roof's tile seat where it meets the courtyard walls

TAN_W = math.tan(math.radians(WING_PITCH))
TAN_H = math.tan(math.radians(HALL_PITCH))


def wing_gable(near_face, far_face, peak, y_base=WING_TOP):
    """A wing's gable top between two eave walls' outer faces."""
    run = abs(peak - near_face)
    return [(near_face, y_base), (peak, y_base + run * TAN_W), (far_face, y_base)]


# --- Openings ---------------------------------------------------------------------------------

def win(u, width, sill, top, **kw):
    return Opening(u - width / 2, u + width / 2, sill, top=top, **kw)


def arch(u, width, sill, spring, **kw):
    return Opening(u - width / 2, u + width / 2, sill, spring=spring, **kw)


HALL_NORTH_WINDOWS = [arch(x, 140, 140, 480) for x in (-880, -440, 0, 440, 880)]
HALL_FRENCH_DOORS  = [arch(x, 160, FL, 250) for x in (-660, 0, 660)]
HALL_SOUTH_WINDOWS = [arch(x, 120, 110, 262) for x in (-330, 330)]
HALL_CLERESTORY    = [win(x, 90, 600, 700) for x in (-660, -330, 0, 330, 660)]
HALL_WEST_DOOR     = win(1265, 120, FL, 250)

OUTER_WING_WINDOWS = [win(z, 110, 100, 240) for z in (-1080, -560, -40, 480, 1000, 1480)]
NORTH_WING_WINDOW  = 1395.0
GALLERY_DOOR       = arch(0, 130, FL, 215)
GALLERY_WINDOWS    = [win(z, 110, 95, 235) for z in (-330, 330)] + [win(z, 90, 95, 235) for z in (-640, 640)]
SOUTH_FRONT_WINDOWS = [win(x, 110, 100, 240) for x in (-1395, -900, -560, 560, 900, 1395)]
PORTON_OUT = arch(0, 360, 0, 270)
PORTON_IN  = arch(0, 340, FL, 262)
SOUTH_COURT_WINDOWS = [win(x, 110, 95, 235) for x in (-660, -330, 330, 660)]

COLUMN_X = [-ARC_X + BAY * i for i in range(6)]
COLUMN_Z = [-ARC_Z + BAY * i for i in range(4)]


def arcade_openings(columns):
    return [arch((a + b) / 2, (b - a) - CAPITAL, SPRING, SPRING, segments=18) for a, b in zip(columns, columns[1:])]


# --- The walls --------------------------------------------------------------------------------

def side_sections(exterior_sign, outside="Stucco", inside="Plaster"):
    """(plus, minus) sections for a wall whose outside faces exterior_sign."""
    return (outside, inside) if exterior_sign > 0 else (inside, outside)


def walls_hall():
    walls = []
    plus, minus = side_sections(+1)
    walls.append(Wall("x", NORTH_Z, -1175, 1175, T, 0, HALL_TOP, [Opening(o.u0, o.u1, o.sill, spring=o.spring) for o in HALL_NORTH_WINDOWS],
                      plus, minus, name="HallNorth"))
    plus, minus = side_sections(-1)
    walls.append(Wall("x", CY_N, -1175, 1175, T, 0, HALL_TOP, HALL_FRENCH_DOORS + HALL_SOUTH_WINDOWS + HALL_CLERESTORY,
                      plus, minus, name="HallSouth"))
    gable = [(785, HALL_TOP), (1265, HALL_TOP + 480 * TAN_H), (1745, HALL_TOP)]
    for sx in (-1, 1):
        # Hall's end walls: the hall inside is toward -sx.
        plus, minus = side_sections(sx)
        openings = [HALL_WEST_DOOR] if sx < 0 else []
        walls.append(Wall("z", sx * IN_X, 845, 1685, T, 0, gable, openings, plus, minus, name=f"HallEnd{sx}"))
    return walls


def walls_wing(sx):
    """A side wing (sx -1 west, +1 east)."""
    walls = []
    plus, minus = side_sections(sx)
    walls.append(Wall("z", sx * OUT_X, SOUTH_Z + 30, NORTH_Z - 30, T, 0, WING_TOP, OUTER_WING_WINDOWS, plus, minus, name=f"Outer{sx}"))
    plus, minus = side_sections(-sx)
    walls.append(Wall("z", sx * IN_X, SOUTH_Z + 30, CY_N - 30, T, 0, WING_TOP, [GALLERY_DOOR] + GALLERY_WINDOWS, plus, minus,
                      name=f"Inner{sx}"))
    # The north front's wing part, with the wing's gable.
    outer, inner = sx * 1675.0, sx * 1175.0
    top = wing_gable(outer, sx * 1115.0, sx * 1395.0)
    top = [p for p in top if (p[0] - inner) * sx <= 0] + [(inner, WING_TOP + 60 * TAN_W)]
    top.sort(key=lambda p: p[0])
    lo, hi = sorted((outer, inner))
    plus, minus = side_sections(+1)
    walls.append(Wall("x", NORTH_Z, lo, hi, T, 0, top, [win(sx * NORTH_WING_WINDOW, 110, 100, 240)], plus, minus, name=f"NorthWing{sx}"))
    return walls


def walls_south():
    walls = []
    top = (wing_gable(-1675, -1115, -1395) + wing_gable(1115, 1675, 1395))
    plus, minus = side_sections(-1)
    walls.append(Wall("z" if False else "x", SOUTH_Z, -1675, 1675, T, 0, top, [PORTON_OUT] + SOUTH_FRONT_WINDOWS, plus, minus, name="SouthFront"))
    plus, minus = side_sections(+1)
    walls.append(Wall("x", CY_S, -1115, 1115, T, 0, WING_TOP, [PORTON_IN] + SOUTH_COURT_WINDOWS, plus, minus, name="SouthCourt"))
    # The zaguan's walls, either side of the passage.
    for sx in (-1, 1):
        walls.append(Wall("z", sx * 230.0, SOUTH_Z + 30, CY_S - 30, T, 0, WING_CEIL, [], "Stucco", "Stucco", name=f"Zaguan{sx}"))
    return walls


def walls_arcade():
    walls = []
    for sz in (-1, 1):
        walls.append(Wall("x", sz * ARC_Z, -ARC_X - TA / 2, ARC_X + TA / 2, TA, SPRING, ARC_TOP, arcade_openings(COLUMN_X),
                          "Stucco", "Stucco", ends=(True, True), bottom=True, name=f"ArcadeZ{sz}"))
    for sx in (-1, 1):
        walls.append(Wall("z", sx * ARC_X, -ARC_Z + TA / 2, ARC_Z - TA / 2, TA, SPRING, ARC_TOP, arcade_openings(COLUMN_Z),
                          "Stucco", "Stucco", bottom=True, name=f"ArcadeX{sx}"))
    return walls


# --- Details on the walls -----------------------------------------------------------------------

def segments_between(lo, hi, holes, pad=0.0):
    """[lo, hi] less the holes (u0, u1), padded."""
    out, at = [], lo
    for u0, u1 in sorted(holes):
        if u1 + pad <= at or u0 - pad >= hi:
            continue
        if u0 - pad > at:
            out.append((at, u0 - pad))
        at = max(at, u1 + pad)
    if at < hi:
        out.append((at, hi))
    return out


def face_strip(b, sec, axis, face, normal_sign, u0, u1, y0, y1, out, cap=None):
    """A band on a wall face: axis x (face at z = face) or z; out thick."""
    d0, d1 = sorted((face, face + normal_sign * out))
    if axis == "x":
        aabb(b, sec, (u0, y0, d0), (u1, y1, d1))
    else:
        aabb(b, sec, (d0, y0, u0), (d1, y1, u1))


def zocalo(b, axis, face, sign, spans):
    """The stone base course along an outside face."""
    for u0, u1 in spans:
        face_strip(b, "Stone", axis, face, sign, u0, u1, 0.0, 46.0, 3.0)
        face_strip(b, "Stone", axis, face, sign, u0, u1, 46.0, 50.0, 1.5)


def wainscot(b, axis, face, sign, spans, y0=FL, y1=128.0):
    """The hall's Talavera tile dado, a wood rail along its top."""
    for u0, u1 in spans:
        face_strip(b, "Talavera", axis, face, sign, u0, u1, y0, y1, 1.2)
        face_strip(b, "Wood", axis, face, sign, u0 - 0.5, u1 + 0.5, y1, y1 + 5.0, 2.8)


def stone_surround(b, axis, face, sign, opening, width=28.0, out=7.0, sections=24):
    """A cantera frame round an arched opening on a face: pilasters, then
    the arch's voussoirs."""
    d0, d1 = sorted((face, face + sign * out))

    def place(u, y, d):
        return (u, y, d) if axis == "x" else (d, y, u)

    for u_in, u_out in ((opening.u0, opening.u0 - width), (opening.u1, opening.u1 + width)):
        lo, hi = sorted((u_in, u_out))
        if axis == "x":
            aabb(b, "Stone", (lo, opening.sill, d0), (hi, opening.spring, d1))
        else:
            aabb(b, "Stone", (d0, opening.sill, lo), (d1, opening.spring, hi))
    r_in = opening.width / 2
    r_out = r_in + width
    uc = opening.centre
    along = X_AXIS if axis == "x" else Z_AXIS
    normal = mul(Z_AXIS if axis == "x" else X_AXIS, sign)
    for i in range(sections):
        a0, a1 = math.pi * i / sections, math.pi * (i + 1) / sections
        pts_out = [place(uc + r * math.cos(a), opening.spring + r * math.sin(a), face + sign * out) for r, a in
                   ((r_in, a0), (r_out, a0), (r_out, a1), (r_in, a1))]
        b.face("Stone", pts_out, normal)
        inner = [place(uc + r_in * math.cos(a), opening.spring + r_in * math.sin(a), d) for a, d in ((a0, d0), (a1, d0), (a1, d1), (a0, d1))]
        mid = (a0 + a1) / 2
        b.face("Stone", inner, norm(combo((0, 0, 0), (along, -math.cos(mid)), (Y_AXIS, -math.sin(mid)))))
        outer = [place(uc + r_out * math.cos(a), opening.spring + r_out * math.sin(a), d) for a, d in ((a0, d0), (a1, d0), (a1, d1), (a0, d1))]
        b.face("Stone", outer, norm(combo((0, 0, 0), (along, math.cos(mid)), (Y_AXIS, math.sin(mid)))))
    # The keystone, standing proud.
    if axis == "x":
        aabb(b, "Stone", (uc - 14, opening.spring + r_in - 4, d0), (uc + 14, opening.spring + r_out + 8, d1 + sign * 3 if sign > 0 else d1))
    else:
        aabb(b, "Stone", (d0, opening.spring + r_in - 4, uc - 14), (d1, opening.spring + r_out + 8, uc + 14))


def chimney(b):
    """The hall fireplace's stack, rising through the roof at the east end."""
    aabb(b, "Stucco", (1040, HALL_CEIL, 1200), (1150, 1110, 1330), faces="xXzZ")
    aabb(b, "Stone", (1032, 1110, 1192), (1158, 1122, 1338))
    aabb(b, "Stucco", (1050, 1122, 1210), (1140, 1150, 1320), faces="xXzZ")
    aabb(b, "Stone", (1044, 1150, 1204), (1146, 1158, 1326), faces="xXzZy")
    aabb(b, "Soot", (1060, 1156, 1220), (1130, 1158.5, 1310), faces="Y")


def build_walls():
    meshes = {}
    for name, walls in (("Walls_Hall", walls_hall()), ("Walls_West", walls_wing(-1)), ("Walls_East", walls_wing(1)),
                        ("Walls_South", walls_south()), ("Walls_Arcade", walls_arcade())):
        b = Builder(name, material, lightmap=True)
        for w in walls:
            build_wall(b, w)
        meshes[name] = b

    # The stone base course outside.
    hall, south, west, east = meshes["Walls_Hall"], meshes["Walls_South"], meshes["Walls_West"], meshes["Walls_East"]
    zocalo(hall, "x", NORTH_Z + 30, 1, [(-1175, 1175)])
    zocalo(south, "x", SOUTH_Z - 30, -1, segments_between(-1675, 1675, [(PORTON_OUT.u0 - 28, PORTON_OUT.u1 + 28)]))
    for sx, b in ((-1, west), (1, east)):
        zocalo(b, "z", sx * (OUT_X + 30), sx, [(SOUTH_Z - 30, NORTH_Z + 30)])
        zocalo(b, "x", NORTH_Z + 30, 1, [tuple(sorted((sx * 1675.0, sx * 1175.0)))])

    # The hall's tile dado, round the room but its doors, low windows and fireplace.
    holes_south = [(o.u0, o.u1) for o in HALL_FRENCH_DOORS + HALL_SOUTH_WINDOWS]
    wainscot(hall, "x", CY_N + 30, 1, segments_between(-1115, 1115, holes_south, pad=8))
    wainscot(hall, "x", NORTH_Z - 30, -1, [(-1115, 1115)])
    wainscot(hall, "z", -IN_X + 30, 1, segments_between(845, 1685, [(HALL_WEST_DOOR.u0, HALL_WEST_DOOR.u1)], pad=8))
    wainscot(hall, "z", IN_X - 30, -1, segments_between(845, 1685, [(1130, 1400)]))
    chimney(hall)

    # The entrance's cantera surround, outside and toward the courtyard.
    stone_surround(south, "x", SOUTH_Z - 30, -1, PORTON_OUT)
    stone_surround(south, "x", CY_S + 30, 1, PORTON_IN, width=22, out=5)

    # A stucco cornice along the arcade's courtyard faces.
    arcade = meshes["Walls_Arcade"]
    for sz in (-1, 1):
        face_strip(arcade, "Stucco", "x", sz * (ARC_Z - TA / 2), -sz, -ARC_X + TA / 2 - 7, ARC_X - TA / 2 + 7, ARC_TOP - 16, ARC_TOP - 6, 7)
        face_strip(arcade, "Stucco", "x", sz * (ARC_Z - TA / 2), -sz, -ARC_X + TA / 2 - 4, ARC_X - TA / 2 + 4, ARC_TOP - 6, ARC_TOP, 4)
    for sx in (-1, 1):
        face_strip(arcade, "Stucco", "z", sx * (ARC_X - TA / 2), -sx, -ARC_Z + TA / 2 - 7, ARC_Z - TA / 2 + 7, ARC_TOP - 16, ARC_TOP - 6, 7)
        face_strip(arcade, "Stucco", "z", sx * (ARC_X - TA / 2), -sx, -ARC_Z + TA / 2 - 4, ARC_Z - TA / 2 + 4, ARC_TOP - 6, ARC_TOP, 4)
    return meshes


# --- Floors, the patio and the ground -------------------------------------------------------------

def floor_rect(b, sec, x0, x1, z0, z1, y, chart=None):
    b.face(sec, [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)], Y_AXIS, chart=chart)


def build_floors():
    b = Builder("Floors", material, lightmap=True)
    floor_rect(b, "FloorTile", -1115, 1115, 845, 1685, FL)                                   # the hall
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 1615.0, sx * 1175.0))
        floor_rect(b, "FloorTile", x0, x1, SOUTH_Z + 30, NORTH_Z - 30, FL)                   # the side wings
        x0, x1 = sorted((sx * 1115.0, sx * 260.0))
        floor_rect(b, "FloorTile", x0, x1, SOUTH_Z + 30, CY_S - 30, FL)                      # the south wing
    # The gallery: a ring between the courtyard walls and the stylobate.
    g = (1115.0, 785.0)
    inner = (ARC_X + TA / 2, ARC_Z + TA / 2)
    floor_rect(b, "FloorTile", -g[0], g[0], inner[1], g[1], FL)
    floor_rect(b, "FloorTile", -g[0], g[0], -g[1], -inner[1], FL)
    floor_rect(b, "FloorTile", -g[0], -inner[0], -inner[1], inner[1], FL)
    floor_rect(b, "FloorTile", inner[0], g[0], -inner[1], inner[1], FL)
    # Stone thresholds in the doorways.
    for o in HALL_FRENCH_DOORS:
        aabb(b, "Stone", (o.u0, FL - 4, 785), (o.u1, FL + 1.5, 845))
    aabb(b, "Stone", (-IN_X - 30, FL - 4, HALL_WEST_DOOR.u0), (-IN_X + 30, FL + 1.5, HALL_WEST_DOOR.u1))
    for sx in (-1, 1):
        aabb(b, "Stone", tuple(sorted((sx * 1115.0, sx * 1175.0))[:1]) + (FL - 4, GALLERY_DOOR.u0),
             tuple(sorted((sx * 1115.0, sx * 1175.0))[1:]) + (FL + 1.5, GALLERY_DOOR.u1))
    return b


def build_patio():
    b = Builder("Patio", material, lightmap=True)
    inner = (ARC_X - TA / 2, ARC_Z - TA / 2)
    outer = (ARC_X + TA / 2, ARC_Z + TA / 2)
    floor_rect(b, "Paving", -inner[0], inner[0], -inner[1], inner[1], 0.0)
    # The stylobate the columns stand on: its top level with the gallery, its edge down to the patio.
    floor_rect(b, "Stone", -outer[0], outer[0], inner[1], outer[1], FL)
    floor_rect(b, "Stone", -outer[0], outer[0], -outer[1], -inner[1], FL)
    floor_rect(b, "Stone", -outer[0], -inner[0], -inner[1], inner[1], FL)
    floor_rect(b, "Stone", inner[0], outer[0], -inner[1], inner[1], FL)
    for sz in (-1, 1):
        z = sz * inner[1]
        b.face("Stone", [(-inner[0], 0, z), (inner[0], 0, z), (inner[0], FL, z), (-inner[0], FL, z)], (0, 0, -sz))
    for sx in (-1, 1):
        x = sx * inner[0]
        b.face("Stone", [(x, 0, -inner[1]), (x, 0, inner[1]), (x, FL, inner[1]), (x, FL, -inner[1])], (-sx, 0, 0))
    # The zaguan: stone flags through the south wing, a step down outside.
    floor_rect(b, "Stone", -200, 200, SOUTH_Z - 30, CY_S + 30, FL)
    b.face("Stone", [(-200, 0, SOUTH_Z - 30), (200, 0, SOUTH_Z - 30), (200, FL, SOUTH_Z - 30), (-200, FL, SOUTH_Z - 30)], (0, 0, -1))
    aabb(b, "Stone", (-210, 0, SOUTH_Z - 75), (210, FL * 0.5, SOUTH_Z - 30), faces="xXYz")
    return b


def build_ground():
    b = Builder("Ground", material)
    G = 25000.0
    fx, fz0, fz1 = 1675.0, SOUTH_Z - 30, NORTH_Z + 30
    path_end = SOUTH_Z - 1500.0
    for rect in ((-G, -fx, -G, G), (fx, G, -G, G), (-fx, fx, fz1, G), (-fx, -210, -G, fz0), (210, fx, -G, fz0), (-210, 210, -G, path_end)):
        floor_rect(b, "Ground", rect[0], rect[1], rect[2], rect[3], 0.0)
    floor_rect(b, "Paving", -210, 210, path_end, SOUTH_Z - 75, 0.0)
    # The path's brick edging, a little proud.
    for sx in (-1, 1):
        aabb(b, "Stone", (sx * 210 - 6, 0, path_end), (sx * 210 + 6, 3, SOUTH_Z - 75), faces="xXYzZ")
    return b


# --- Ceilings: vigas on corbels ----------------------------------------------------------------------

def corbel(b, x, wall_face, direction, top, width=24.0, reach=68.0, drop=50.0):
    """A carved zapata under a beam's end: from the wall face, reach into the
    room, its underside an ogee from drop below the beam up to its tip."""
    curve = [(0.0, 0.0), (0.12, 0.04), (0.3, 0.2), (0.48, 0.42), (0.62, 0.55), (0.75, 0.62), (0.86, 0.72), (1.0, 0.84)]
    prof = [(reach * f, top - drop + drop * h) for f, h in curve]
    zfor = lambda d: wall_face + direction * d
    for sxw in (-1, 1):
        xs = x + sxw * width / 2
        pts = [(xs, top, zfor(0))] + [(xs, top, zfor(reach))] + [(xs, y, zfor(d)) for d, y in reversed(prof)]
        b.face("Wood", pts, (sxw, 0, 0))
    for (d0, y0), (d1, y1) in zip(prof, prof[1:]):
        dd, dy = d1 - d0, y1 - y0
        n = norm((0, -dd, direction * dy))
        b.face("Wood", [(x - width / 2, y0, zfor(d0)), (x + width / 2, y0, zfor(d0)), (x + width / 2, y1, zfor(d1)), (x - width / 2, y1, zfor(d1))], n)
    tip = prof[-1]
    b.face("Wood", [(x - width / 2, tip[1], zfor(reach)), (x + width / 2, tip[1], zfor(reach)), (x + width / 2, top, zfor(reach)),
                    (x - width / 2, top, zfor(reach))], (0, 0, direction))


def build_ceilings():
    b = Builder("Ceilings", material, lightmap=True)
    # The hall: a plank deck on 21 vigas, each on a corbel at either wall.
    b.face("Wood", [(-1115, HALL_CEIL, 845), (1115, HALL_CEIL, 845), (1115, HALL_CEIL, 1685), (-1115, HALL_CEIL, 1685)], (0, -1, 0),
           uv=lambda p: (p[0] / 100.0, p[2] / 100.0))
    for k in range(21):
        x = -1050.0 + 105.0 * k
        jitter = rand(f"viga{k}", -1.5, 1.5)
        aabb(b, "Wood", (x - 12, HALL_CEIL - 30 + jitter, 845), (x + 12, HALL_CEIL, 1685), faces="xXy")
        corbel(b, x, 845.0, 1, HALL_CEIL - 30 + jitter)
        corbel(b, x, 1685.0, -1, HALL_CEIL - 30 + jitter)
    # The wings: lower, lighter beams.
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 1615.0, sx * 1175.0))
        b.face("Wood", [(x0, WING_CEIL, SOUTH_Z + 30), (x1, WING_CEIL, SOUTH_Z + 30), (x1, WING_CEIL, NORTH_Z - 30), (x0, WING_CEIL, NORTH_Z - 30)],
               (0, -1, 0), uv=lambda p: (p[2] / 100.0, p[0] / 100.0))
        z = SOUTH_Z + 80
        while z < NORTH_Z - 60:
            aabb(b, "Wood", (x0, WING_CEIL - 22, z - 9), (x1, WING_CEIL, z + 9), faces="zZy")
            z += 95.0
    b.face("Wood", [(-1115, WING_CEIL, SOUTH_Z + 30), (1115, WING_CEIL, SOUTH_Z + 30), (1115, WING_CEIL, CY_S - 30), (-1115, WING_CEIL, CY_S - 30)],
           (0, -1, 0), uv=lambda p: (p[0] / 100.0, p[2] / 100.0))
    x = -1070.0
    while x < 1080:
        aabb(b, "Wood", (x - 9, WING_CEIL - 22, SOUTH_Z + 30), (x + 9, WING_CEIL, CY_S - 30), faces="xXy")
        x += 95.0
    return b


# --- Columns ----------------------------------------------------------------------------------------

def column(b, x, z):
    y = FL
    aabb(b, "Stone", (x - 23, y, z - 23), (x + 23, y + 12, z + 23), faces="xXYzZ")
    base = [(21, 12), (23.5, 15), (23.5, 19), (20.5, 22.5), (18.2, 24)]
    shaft = [(18.2, 24), (18.0, 90), (17.2, 170), (16.4, 236)]
    neck = [(16.4, 236), (18.6, 238), (18.6, 241.5), (16.4, 243.5), (16.4, 250), (19.5, 253), (23.5, 256.5), (25.5, 258)]
    for profile in (base, shaft, neck):
        lathe(b, "Stone", (x, y, z), profile, segments=22, hard_angle=50)
    aabb(b, "Stone", (x - CAPITAL / 2, y + 258, z - CAPITAL / 2), (x + CAPITAL / 2, SPRING, z + CAPITAL / 2), faces="xXyzZ")


def build_columns():
    b = Builder("Columns", material)
    spots = {(x, z) for x in COLUMN_X for z in (-ARC_Z, ARC_Z)} | {(x, z) for x in (-ARC_X, ARC_X) for z in COLUMN_Z}
    for x, z in sorted(spots):
        column(b, x, z)
    return b


# --- Roofs --------------------------------------------------------------------------------------------

def gable_slopes(axis, centre, half_width, lo, hi, base, pitch, eave=EAVE):
    """The two slopes of a gable roof whose ridge runs along axis ('x' or
    'z') at centre, its eave walls' outer faces half_width either side, from
    lo to hi along the ridge (overhangs included). base: the tile seat at the
    eave walls' outer faces."""
    tan = math.tan(math.radians(pitch))
    run = half_width + eave
    y_eave = base - eave * tan
    ls = run / math.cos(math.radians(pitch))
    slopes = []
    for sign in (-1, 1):
        if axis == "x":
            E0 = (lo, y_eave, centre + sign * run)
            slopes.append(RoofSlope(E0, X_AXIS, (0, 0, -sign), pitch, hi - lo, ls))
        else:
            E0 = (centre + sign * run, y_eave, lo)
            slopes.append(RoofSlope(E0, Z_AXIS, (-sign, 0, 0), pitch, hi - lo, ls))
    ridge_y = base + half_width * tan
    return slopes, ridge_y


def dress_slope(b, slope, seed, rake=(True, True), tails=True, wall_run=None):
    tile_slope(b, "RoofTile", slope, seed=seed)
    roof_deck(b, "Wood", slope, rake=rake)
    if tails:
        s_wall = (wall_run if wall_run is not None else EAVE) / math.cos(math.radians(slope.pitch))
        rafters(b, "Wood", slope, 0.0, s_wall + 12.0)


def build_roofs():
    meshes = {}
    # The hall.
    b = Builder("Roof_Hall", material)
    slopes, ridge_y = gable_slopes("x", 1265.0, 480.0, -1175 - EAVE, 1175 + EAVE, HALL_TOP + DECK, HALL_PITCH)
    for i, s in enumerate(slopes):
        dress_slope(b, s, f"hall{i}")
    tile_ridge(b, "RoofTile", (-1175 - EAVE, ridge_y, 1265.0), (1175 + EAVE, ridge_y, 1265.0), Z_AXIS, seed="hallridge")
    meshes["Roof_Hall"] = b

    # The side wings: the courtyard-side slope stops at the hall's end wall.
    b = Builder("Roof_Wings", material)
    for sx in (-1, 1):
        cx = sx * 1395.0
        slopes, ridge_y = gable_slopes("z", cx, 280.0, SOUTH_Z - 30 - EAVE, NORTH_Z + 30 + EAVE, WING_TOP + DECK, WING_PITCH)
        for s in slopes:
            outer = (s.E0[0] - cx) * sx > 0
            if outer:
                dress_slope(b, s, f"wing{sx}out")
            else:
                cut = CY_N - 30.0
                s1 = RoofSlope(s.E0, Z_AXIS, s.H, WING_PITCH, cut - s.E0[2], s.Ls)
                dress_slope(b, s1, f"wing{sx}in", rake=(True, False))
                # Beside the hall: from its end wall up to the ridge.
                start_x = sx * 1175.0
                run = abs(cx - start_x)
                y0 = WING_TOP + DECK + (abs(start_x) - 1115.0) * math.tan(math.radians(WING_PITCH))
                s2 = RoofSlope((start_x, y0, cut), Z_AXIS, s.H, WING_PITCH, (NORTH_Z + 30 + EAVE) - cut,
                               run / math.cos(math.radians(WING_PITCH)))
                tile_slope(b, "RoofTile", s2, seed=f"wing{sx}hall")
                roof_deck(b, "Wood", s2, rake=(False, True), edge=False)
        tile_ridge(b, "RoofTile", (cx, ridge_y, SOUTH_Z - 30 - EAVE), (cx, ridge_y, NORTH_Z + 30 + EAVE), X_AXIS, seed=f"wingridge{sx}")
    meshes["Roof_Wings"] = b

    # The south wing, its ridge meeting the side wings' at a T.
    b = Builder("Roof_South", material)
    slopes, ridge_y = gable_slopes("x", -1065.0, 280.0, -1395.0, 1395.0, WING_TOP + DECK, WING_PITCH)
    for i, s in enumerate(slopes):
        tile_slope(b, "RoofTile", s, seed=f"south{i}")
        roof_deck(b, "Wood", s, rake=(False, False))
        rafters(b, "Wood", s, 0.0, EAVE / math.cos(math.radians(WING_PITCH)) + 12.0, t_range=(1395 - 1115, 1395 + 1115))
    tile_ridge(b, "RoofTile", (-1395.0, ridge_y, -1065.0), (1395.0, ridge_y, -1065.0), Z_AXIS, seed="southridge")
    meshes["Roof_South"] = b

    # The gallery: shed roofs on exposed rafters, from the courtyard walls down over the arcade.
    b = Builder("Roof_Gallery", material)
    wall_face = (1115.0, 785.0)
    arcade_back = (ARC_X + TA / 2, ARC_Z + TA / 2)
    seat_at_arcade = ARC_TOP + 14.0 + DECK
    for axis, (wf, ab) in (("x", (wall_face[0], arcade_back[0])), ("z", (wall_face[1], arcade_back[1]))):
        tan = (GAL_WALL - seat_at_arcade) / (wf - ab)
        pitch = math.degrees(math.atan(tan))
        eave_at = ab - TA - 40.0
        y_eave = seat_at_arcade - (ab - eave_at) * tan
        ls = (wf - eave_at) / math.cos(math.radians(pitch))
        for sign in (-1, 1):
            if axis == "x":     # the east and west galleries, eaves along Z
                E0 = (sign * eave_at, y_eave, -wall_face[1])
                s = RoofSlope(E0, Z_AXIS, (sign, 0, 0), pitch, 2 * wall_face[1], ls)
            else:
                E0 = (-wall_face[0], y_eave, sign * eave_at)
                s = RoofSlope(E0, X_AXIS, (0, 0, sign), pitch, 2 * wall_face[0], ls)
            tile_slope(b, "RoofTile", s, seed=f"gallery{axis}{sign}")
            roof_deck(b, "Wood", s, rake=(False, False))
            rafters(b, "Wood", s, 0.0, ls, spacing=58.0, width=9.0, depth=15.0)
            # Blocking between the rafters on the arcade wall.
            s_arc = (ab - eave_at) / math.cos(math.radians(pitch))
            beam(b, "Wood", s.at(0, s_arc - TA * 0.5, -3 - 8), s.at(s.La, s_arc - TA * 0.5, -3 - 8), TA - 4, 16, up=s.N)
    meshes["Roof_Gallery"] = b
    return meshes


# --- Windows and doors ---------------------------------------------------------------------------------

FRAME = 7.0          # a window frame's face width
FRAME_DEPTH = 9.0
MUNTIN = 2.6


class Placed:
    """An opening in a wall, and which side of it is outside."""

    def __init__(self, wall, opening, outside, kind, reja=False, lites=(2, 3), depth=14.0):
        self.w, self.o, self.outside, self.kind, self.reja, self.lites, self.depth = wall, opening, outside, kind, reja, lites, depth

    @property
    def dc(self):
        """The frame's centre, across the wall from its centre line."""
        return self.outside * (self.w.T / 2 - self.depth)

    def p(self, u, y, d):
        return self.w.point(u, y, d)

    def n_out(self):
        return mul(self.w.perp(), self.outside)


def frame_box(b, sec, pl, u0, u1, y0, y1, d0, d1):
    a, c = pl.p(u0, y0, d0), pl.p(u1, y1, d1)
    aabb(b, sec, tuple(min(a[i], c[i]) for i in range(3)), tuple(max(a[i], c[i]) for i in range(3)))


def arch_ring(b, sec, pl, uc, spring, r_out, r_in, d0, d1, segments=16, a_from=0.0, a_to=math.pi):
    along = pl.w.along()
    for i in range(segments):
        a0 = a_from + (a_to - a_from) * i / segments
        a1 = a_from + (a_to - a_from) * (i + 1) / segments
        mid = (a0 + a1) / 2
        ring = lambda r, a, d: pl.p(uc + r * math.cos(a), spring + r * math.sin(a), d)
        for d, sgn in ((d0, -1.0), (d1, 1.0)):
            b.face(sec, [ring(r_in, a0, d), ring(r_out, a0, d), ring(r_out, a1, d), ring(r_in, a1, d)], mul(pl.w.perp(), sgn))
        b.face(sec, [ring(r_in, a0, d0), ring(r_in, a1, d0), ring(r_in, a1, d1), ring(r_in, a0, d1)],
               norm(combo((0, 0, 0), (along, -math.cos(mid)), (Y_AXIS, -math.sin(mid)))))
        b.face(sec, [ring(r_out, a0, d0), ring(r_out, a1, d0), ring(r_out, a1, d1), ring(r_out, a0, d1)],
               norm(combo((0, 0, 0), (along, math.cos(mid)), (Y_AXIS, math.sin(mid)))))


def glass_pane(b, pl, outline, d):
    pts = [pl.p(u, y, d) for u, y in outline]
    b.face("Glass", pts, pl.n_out())


def muntin(b, pl, u0, y0, u1, y1, d, width=MUNTIN, depth=4.0, sec="GreenPaint"):
    beam(b, sec, pl.p(u0, y0, d), pl.p(u1, y1, d), width, depth, up=pl.w.perp())


def sash_grid(b, pl, u0, u1, y0, y1, cols, rows, d):
    for i in range(1, cols):
        u = u0 + (u1 - u0) * i / cols
        muntin(b, pl, u, y0, u, y1, d)
    for j in range(1, rows):
        y = y0 + (y1 - y0) * j / rows
        muntin(b, pl, u0, y, u1, y, d)


def arch_muntins(b, pl, uc, spring, radius, d, spokes=(math.pi / 4, math.pi / 2, 3 * math.pi / 4)):
    for a in spokes:
        muntin(b, pl, uc, spring, uc + radius * math.cos(a), spring + radius * math.sin(a), d)
    arch_ring(b, "GreenPaint", pl, uc, spring, radius * 0.42 + MUNTIN / 2, radius * 0.42 - MUNTIN / 2, d - 2, d + 2, segments=10)


def casement(b, pl):
    """A pair of casements in a square or round-headed opening."""
    o, dc = pl.o, pl.dc
    d0, d1 = dc - FRAME_DEPTH / 2, dc + FRAME_DEPTH / 2
    cols, rows = pl.lites
    # The fixed frame round the opening.
    frame_box(b, "GreenPaint", pl, o.u0, o.u0 + FRAME, o.sill, o.head_at(o.u0), d0, d1)
    frame_box(b, "GreenPaint", pl, o.u1 - FRAME, o.u1, o.sill, o.head_at(o.u1), d0, d1)
    frame_box(b, "GreenPaint", pl, o.u0, o.u1, o.sill, o.sill + FRAME + 2, d0 - 1.5, d1)
    top = o.spring if o.kind != "rect" else o.top - FRAME
    if o.kind == "rect":
        frame_box(b, "GreenPaint", pl, o.u0, o.u1, o.top - FRAME, o.top, d0, d1)
    else:
        arch_ring(b, "GreenPaint", pl, o.centre, o.spring, o.width / 2, o.width / 2 - FRAME, d0, d1)
        frame_box(b, "GreenPaint", pl, o.u0 + FRAME, o.u1 - FRAME, o.spring - 2.5, o.spring + 2.5, d0 + 1, d1 - 1)
    inner = (o.u0 + FRAME, o.u1 - FRAME, o.sill + FRAME + 2, top)
    glass_pane(b, pl, [(inner[0], inner[2]), (inner[1], inner[2]), (inner[1], inner[3]), (inner[0], inner[3])], dc)
    if o.kind != "rect":
        r = o.width / 2 - FRAME
        glass_pane(b, pl, [(o.centre + r * math.cos(math.pi * i / 16), o.spring + r * math.sin(math.pi * i / 16)) for i in range(17)], dc)
        arch_muntins(b, pl, o.centre, o.spring, r, dc)
    # Two leaves, each with its own stiles and its lites.
    mid = o.centre
    frame_box(b, "GreenPaint", pl, mid - 3.5, mid + 3.5, inner[2], inner[3], d0 + 0.5, d1 - 0.5)
    for lo, hi in ((inner[0], mid - 3.5), (mid + 3.5, inner[1])):
        frame_box(b, "GreenPaint", pl, lo, lo + 4, inner[2], inner[3], d0 + 1, d1 - 1)
        frame_box(b, "GreenPaint", pl, hi - 4, hi, inner[2], inner[3], d0 + 1, d1 - 1)
        frame_box(b, "GreenPaint", pl, lo, hi, inner[2], inner[2] + 6, d0 + 1, d1 - 1)
        frame_box(b, "GreenPaint", pl, lo, hi, inner[3] - 4, inner[3], d0 + 1, d1 - 1)
        sash_grid(b, pl, lo + 4, hi - 4, inner[2] + 6, inner[3] - 4, cols // 2 if cols > 2 else 1, rows, dc)


def french_door(b, pl):
    o, dc = pl.o, pl.dc
    d0, d1 = dc - FRAME_DEPTH / 2, dc + FRAME_DEPTH / 2
    F = 8.0
    frame_box(b, "GreenPaint", pl, o.u0, o.u0 + F, o.sill, o.spring, d0, d1)
    frame_box(b, "GreenPaint", pl, o.u1 - F, o.u1, o.sill, o.spring, d0, d1)
    arch_ring(b, "GreenPaint", pl, o.centre, o.spring, o.width / 2, o.width / 2 - F, d0, d1)
    frame_box(b, "GreenPaint", pl, o.u0 + F, o.u1 - F, o.spring - 8, o.spring, d0, d1)    # the transom bar
    r = o.width / 2 - F
    glass_pane(b, pl, [(o.centre + r * math.cos(math.pi * i / 16), o.spring + r * math.sin(math.pi * i / 16)) for i in range(17)], dc)
    arch_muntins(b, pl, o.centre, o.spring, r, dc, spokes=[math.pi * k / 6 for k in range(1, 6)])
    mid = o.centre
    top = o.spring - 8
    for lo, hi in ((o.u0 + F, mid - 0.5), (mid + 0.5, o.u1 - F)):
        frame_box(b, "GreenPaint", pl, lo, lo + 7, o.sill, top, d0 + 1, d1 - 1)
        frame_box(b, "GreenPaint", pl, hi - 7, hi, o.sill, top, d0 + 1, d1 - 1)
        frame_box(b, "GreenPaint", pl, lo, hi, o.sill, o.sill + 20, d0 + 1, d1 - 1)        # bottom rail
        frame_box(b, "GreenPaint", pl, lo + 7, hi - 7, o.sill + 20, o.sill + 62, dc - 1.5, dc + 1.5)   # the kick panel
        frame_box(b, "GreenPaint", pl, lo, hi, o.sill + 62, o.sill + 69, d0 + 1, d1 - 1)   # lock rail
        frame_box(b, "GreenPaint", pl, lo, hi, top - 7, top, d0 + 1, d1 - 1)
        glass_pane(b, pl, [(lo + 7, o.sill + 69), (hi - 7, o.sill + 69), (hi - 7, top - 7), (lo + 7, top - 7)], dc)
        sash_grid(b, pl, lo + 7, hi - 7, o.sill + 69, top - 7, 2, 4, dc)
        # A brass-dark iron lever at the meeting stiles.
        u = mid - 6 if lo < mid else mid + 6
        for side in (-1, 1):
            frame_box(b, "Iron", pl, u - 1, u + 1, o.sill + 95, o.sill + 106, dc + side * (FRAME_DEPTH / 2 + 0.2), dc + side * (FRAME_DEPTH / 2 + 2.5))


def clerestory(b, pl):
    o, dc = pl.o, pl.dc
    d0, d1 = dc - FRAME_DEPTH / 2, dc + FRAME_DEPTH / 2
    frame_box(b, "GreenPaint", pl, o.u0, o.u0 + FRAME, o.sill, o.top, d0, d1)
    frame_box(b, "GreenPaint", pl, o.u1 - FRAME, o.u1, o.sill, o.top, d0, d1)
    frame_box(b, "GreenPaint", pl, o.u0, o.u1, o.sill, o.sill + FRAME, d0, d1)
    frame_box(b, "GreenPaint", pl, o.u0, o.u1, o.top - FRAME, o.top, d0, d1)
    glass_pane(b, pl, [(o.u0 + FRAME, o.sill + FRAME), (o.u1 - FRAME, o.sill + FRAME), (o.u1 - FRAME, o.top - FRAME), (o.u0 + FRAME, o.top - FRAME)], dc)
    sash_grid(b, pl, o.u0 + FRAME, o.u1 - FRAME, o.sill + FRAME, o.top - FRAME, 2, 2, dc)


def stone_sill(b, pl):
    o = pl.o
    face = pl.outside * pl.w.T / 2
    frame_box(b, "Stone", pl, o.u0 - 7, o.u1 + 7, o.sill - 6, o.sill + 1.5, face - pl.outside * 12, face + pl.outside * 5)


def reja(b, pl):
    """A wrought-iron grille standing off the outside face: round bars on
    flat rails, returned into the wall at the corners."""
    o = pl.o
    face = pl.outside * pl.w.T / 2
    d = face + pl.outside * 10.0
    u0, u1 = o.u0 - 9, o.u1 + 9
    y0, y1 = o.sill - 12, o.head + 10
    count = max(3, round((u1 - u0) / 12.0))
    for i in range(count + 1):
        u = u0 + (u1 - u0) * i / count
        tube(b, "Iron", pl.p(u, y0, d), pl.p(u, y1, d), 1.1, segments=6)
        # The pointed finials above the top rail.
        tube(b, "Iron", pl.p(u, y1, d), pl.p(u, y1 + 6, d), 1.1, segments=6, radius_end=0.15, caps=False)
    for y in (y0 + 4, y0 + (y1 - y0) * 0.5, y1 - 4):
        frame_box(b, "Iron", pl, u0 - 2, u1 + 2, y - 2, y + 2, d - 1.6, d + 1.6)
    for u in (u0 - 1, u1 + 1):
        for y in (y0 + 4, y1 - 4):
            frame_box(b, "Iron", pl, u - 1.5, u + 1.5, y - 1.5, y + 1.5, face - pl.outside * 4, d)


def wood_door(b, pl, leaves=2, studs=True):
    """A plank door of dark wood in its jamb, iron straps and nail heads on the outside."""
    o, dc = pl.o, pl.dc
    J = 9.0
    t = 7.0
    head = o.head_at(o.u0)
    frame_box(b, "Wood", pl, o.u0, o.u0 + J, o.sill, head, dc - 8, dc + 8)
    frame_box(b, "Wood", pl, o.u1 - J, o.u1, o.sill, head, dc - 8, dc + 8)
    if o.kind == "rect":
        frame_box(b, "Wood", pl, o.u0, o.u1, o.top - J, o.top + 4, dc - 8, dc + 8)
    else:
        arch_ring(b, "Wood", pl, o.centre, o.spring, o.width / 2, o.width / 2 - J, dc - 8, dc + 8)
    inner = o.outline(inset=J)
    inner = [(u, max(y, o.sill + 1.0)) for u, y in inner]
    width = o.width - 2 * J
    out = pl.outside
    for k in range(leaves):
        lo = o.u0 + J + width * k / leaves + (0.4 if k else 0)
        hi = o.u0 + J + width * (k + 1) / leaves - (0.4 if k + 1 < leaves else 0)
        shape = [(min(max(u, lo), hi), y) for u, y in inner]
        cleaned = []
        for p in shape:
            if not cleaned or abs(p[0] - cleaned[-1][0]) > 1e-3 or abs(p[1] - cleaned[-1][1]) > 1e-3:
                cleaned.append(p)
        for d, sgn in ((dc - t / 2, -1.0), (dc + t / 2, 1.0)):
            b.face("Wood", [pl.p(u, y, d) for u, y in cleaned], mul(pl.w.perp(), sgn),
                   uv=lambda p, lo=lo: ((p[1]) / 100.0 + lo * 0.013, 0.0))
        # Battens across the inside face, straps and studs across the outside.
        y_bottom = o.sill + 1
        y_top = max(y for _, y in cleaned)
        for frac in (0.12, 0.5, 0.86):
            y = y_bottom + (min(y_top, o.head_at((lo + hi) / 2)) - y_bottom) * frac
            if y > o.head_at(lo) - 6:
                continue
            frame_box(b, "Iron", pl, lo + (2 if k == 0 else (hi - lo) * 0.3), hi - (2 if k else (hi - lo) * 0.3) if leaves > 1 else hi - 2,
                      y - 2.5, y + 2.5, dc + out * t / 2, dc + out * (t / 2 + 0.7))
        if studs:
            rows = 6
            for r in range(rows):
                y = y_bottom + 18 + (min(o.head_at(lo), o.head_at(hi)) - y_bottom - 34) * r / (rows - 1)
                for c in range(3):
                    u = lo + (hi - lo) * (c + 1) / 4
                    frame_box(b, "Iron", pl, u - 1.2, u + 1.2, y - 1.2, y + 1.2, dc + out * t / 2, dc + out * (t / 2 + 1.4))


def build_windows():
    """Every window and glazed door, one mesh per front (each its own
    translucent draw, so they sort)."""
    out = {}
    hall_n = walls_hall()[0]
    hall_s = walls_hall()[1]
    hall_w = walls_hall()[2]

    b = Builder("Windows_HallNorth", material)
    for o in hall_n.openings:
        pl = Placed(hall_n, o, +1, "casement", lites=(2, 4), depth=16)
        casement(b, pl)
        stone_sill(b, pl)
    out[b.name] = b

    b = Builder("Windows_HallSouth", material)
    for o in hall_s.openings:
        if o in HALL_FRENCH_DOORS:
            french_door(b, Placed(hall_s, o, -1, "french", depth=14))
        elif o in HALL_CLERESTORY:
            clerestory(b, Placed(hall_s, o, -1, "clerestory", depth=14))
        else:
            pl = Placed(hall_s, o, -1, "casement", lites=(2, 3), depth=14)
            casement(b, pl)
            stone_sill(b, pl)
    out[b.name] = b

    for sx, name in ((-1, "Windows_West"), (1, "Windows_East")):
        b = Builder(name, material)
        outer, inner, north = walls_wing(sx)
        for o in outer.openings:
            pl = Placed(outer, o, sx, "casement", reja=True, lites=(2, 3), depth=22)
            casement(b, pl)
            stone_sill(b, pl)
            reja(b, pl)
        for o in north.openings:
            pl = Placed(north, o, +1, "casement", reja=True, depth=22)
            casement(b, pl)
            stone_sill(b, pl)
            reja(b, pl)
        for o in inner.openings:
            if o is GALLERY_DOOR:
                continue
            pl = Placed(inner, o, -sx, "casement", lites=(2, 3), depth=14)
            casement(b, pl)
            stone_sill(b, pl)
        out[name] = b

    b = Builder("Windows_South", material)
    front, court = walls_south()[0], walls_south()[1]
    for o in front.openings:
        if o is PORTON_OUT:
            continue
        pl = Placed(front, o, -1, "casement", reja=True, depth=22)
        casement(b, pl)
        stone_sill(b, pl)
        reja(b, pl)
    for o in court.openings:
        if o is PORTON_IN:
            continue
        pl = Placed(court, o, +1, "casement", depth=14)
        casement(b, pl)
        stone_sill(b, pl)
    out[b.name] = b

    # Doors: the gallery doors, the hall's west door, and the entrance's leaves standing open.
    b = Builder("Doors", material)
    for sx in (-1, 1):
        inner = walls_wing(sx)[1]
        wood_door(b, Placed(inner, GALLERY_DOOR, -sx, "door", depth=12))
    wood_door(b, Placed(hall_w, HALL_WEST_DOOR, -1, "door", depth=12), leaves=1)
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 200.0, sx * 191.0))
        z0, z1 = SOUTH_Z + 32, SOUTH_Z + 32 + 176
        aabb(b, "Wood", (x0, FL + 1, z0), (x1, 318, z1))
        face = x0 if sx > 0 else x1
        for y in (60, 190, 290):
            aabb(b, "Iron", (min(face, face - sx * 0.8), y - 3, z0 + 2), (max(face, face - sx * 0.8), y + 3, z1 - 30))
        for r in range(8):
            for c in range(4):
                y = FL + 30 + r * 36
                z = z0 + 25 + c * 42
                aabb(b, "Iron", (min(face, face - sx * 1.6), y - 1.6, z - 1.6), (max(face, face - sx * 1.6), y + 1.6, z + 1.6))
    out[b.name] = b
    return out


# --- The fountain ------------------------------------------------------------------------------------

def build_fountain():
    b = Builder("Fountain", material)
    c = (0.0, 0.0, 0.0)
    ph = math.pi / 8
    polygon_prism(b, "Stone", c, 240.0, 8, 0.0, 6.0, phase=ph)
    polygon_prism(b, "Talavera", c, 232.0, 8, 6.0, 50.0, phase=ph, top=False)
    # The cap: a ring of slabs, rounded off by a second, slightly smaller step.
    outer_c = [(242 * math.cos(ph + k * math.pi / 4), 242 * math.sin(ph + k * math.pi / 4)) for k in range(8)]
    inner_c = [(204 * math.cos(ph + k * math.pi / 4), 204 * math.sin(ph + k * math.pi / 4)) for k in range(8)]
    for k in range(8):
        a, bq = outer_c[k], outer_c[(k + 1) % 8]
        ia, ib = inner_c[k], inner_c[(k + 1) % 8]
        b.face("Stone", [(a[0], 61, a[1]), (bq[0], 61, bq[1]), (ib[0], 61, ib[1]), (ia[0], 61, ia[1])], Y_AXIS)
        b.face("Stone", [(a[0], 50, a[1]), (bq[0], 50, bq[1]), (bq[0], 61, bq[1]), (a[0], 61, a[1])],
               norm(((a[0] + bq[0]) / 2, 0, (a[1] + bq[1]) / 2)))
        b.face("Stone", [(a[0], 50, a[1]), (bq[0], 50, bq[1]), (bq[0] * 232 / 242, 50, bq[1] * 232 / 242), (a[0] * 232 / 242, 50, a[1] * 232 / 242)],
               (0, -1, 0))
    polygon_prism(b, "StoneWet", c, 204.0, 8, 10.0, 61.0, phase=ph, top=False, inner=True)
    polygon_prism(b, "StoneWet", c, 204.0, 8, 9.0, 10.0, phase=ph, top=True)
    b.face("Water", [(204 * math.cos(ph + k * math.pi / 4), 46.0, 204 * math.sin(ph + k * math.pi / 4)) for k in range(8)], Y_AXIS)
    # The centrepiece: a pedestal, the great bowl, a column, the small bowl and a finial.
    lathe(b, "StoneWet", c, [(44, 10), (44, 22), (32, 28), (22, 38), (20, 46)], segments=24)
    lathe(b, "Stone", c, [(20, 46), (19, 100), (21, 108), (26, 116), (28, 122)], segments=24)
    bowl = [(28, 122), (44, 126), (66, 135), (82, 146), (88, 153), (88, 159), (82, 159), (78, 153), (60, 144), (30, 140), (16, 141)]
    lathe(b, "Stone", c, bowl[:6], segments=32)
    lathe(b, "StoneWet", c, bowl[5:], segments=32)
    b.face("Water", [(79 * math.cos(2 * math.pi * k / 32), 154.5, 79 * math.sin(2 * math.pi * k / 32)) for k in range(32)], Y_AXIS)
    lathe(b, "Stone", c, [(16, 141), (13, 175), (12, 190), (15, 194), (17, 198)], segments=20)
    small = [(17, 198), (28, 201), (36, 207), (38, 212), (38, 215), (34, 215), (28, 210), (14, 207)]
    lathe(b, "Stone", c, small[:5], segments=24)
    lathe(b, "StoneWet", c, small[4:], segments=24)
    b.face("Water", [(34 * math.cos(2 * math.pi * k / 24), 212.0, 34 * math.sin(2 * math.pi * k / 24)) for k in range(24)], Y_AXIS)
    lathe(b, "Stone", c, [(9, 207), (9, 220), (13, 225), (11, 233), (5, 240), (2.5, 246), (0.01, 249)], segments=16)
    return b


# --- The hall's furnishings, lanterns and planters ------------------------------------------------------

def turned_leg(b, x, z, y0, height):
    f = height / 70.0
    prof = [(5.0, 0), (5.0, 10), (6.6, 13), (4.0, 18), (4.6, 34), (6.2, 40), (4.2, 46), (4.8, 62), (5.6, 66), (5.6, 70)]
    lathe(b, "Wood", (x, y0, z), [(r, y * f) for r, y in prof], segments=12)


def build_hall_furnishings():
    b = Builder("Hall_Furnishings", material)
    cz = 1265.0
    # A long refectory table on turned legs, benches either side.
    aabb(b, "Wood", (-310, FL + 70, cz - 52), (310, FL + 78, cz + 52))
    aabb(b, "Wood", (-296, FL + 62, cz - 44), (296, FL + 70, cz + 44), faces="xXyzZ")
    for x in (-280, 0, 280):
        for sz in (-1, 1):
            turned_leg(b, x, cz + sz * 36, FL, 62)
    aabb(b, "Wood", (-280, FL + 14, cz - 4), (280, FL + 22, cz + 4))
    for sz in (-1, 1):
        z = cz + sz * 88
        aabb(b, "Wood", (-275, FL + 40, z - 17), (275, FL + 46, z + 17))
        for x in (-225, 225):
            aabb(b, "Wood", (x - 4, FL, z - 14), (x + 4, FL + 40, z + 14))
            aabb(b, "Wood", (x - 4, FL, z - 16), (x + 4, FL + 6, z + 16))
        aabb(b, "Wood", (-225, FL + 12, z - 3), (225, FL + 18, z + 3))

    # The fireplace on the east wall: plastered piers and lintel, a cantera
    # surround, a wood mantel, a sooty firebox with logs on its hearth.
    wall = IN_X - 30
    front = wall - 80.0
    z0, z1 = 1145.0, 1385.0
    o0, o1 = 1205.0, 1325.0
    top = 255.0
    aabb(b, "Plaster", (front, FL, z0), (wall, top, o0), faces="xyzZ")
    aabb(b, "Plaster", (front, FL, o1), (wall, top, z1), faces="xyzZ")
    aabb(b, "Plaster", (front, 160, o0), (wall, top, o1), faces="xy")
    aabb(b, "Soot", (wall - 12, FL, o0), (wall, 160, o1), faces="x")
    b.face("Soot", [(front, FL, o0), (wall - 12, FL, o0), (wall - 12, 160, o0), (front, 160, o0)], (0, 0, 1))
    b.face("Soot", [(front, FL, o1), (wall - 12, FL, o1), (wall - 12, 160, o1), (front, 160, o1)], (0, 0, -1))
    b.face("Soot", [(front, 160, o0), (wall - 12, 160, o0), (wall - 12, 160, o1), (front, 160, o1)], (0, -1, 0))
    for zz0, zz1 in ((o0 - 16, o0), (o1, o1 + 16)):
        aabb(b, "Stone", (front - 6, FL, zz0), (front, 160, zz1), faces="xyzZY")
    aabb(b, "Stone", (front - 6, 160, o0 - 16), (front, 178, o1 + 16), faces="xyzZY")
    aabb(b, "Wood", (front - 16, top, z0 - 18), (wall, top + 12, z1 + 18))
    aabb(b, "Plaster", (wall - 58, top + 12, 1172), (wall, HALL_CEIL, 1358), faces="xzZ")
    aabb(b, "Stone", (front - 70, FL, z0 + 10), (wall, FL + 5, z1 - 10), faces="xXYzZ")
    for i, (za, zb, y) in enumerate(((o0 + 18, o1 - 14, FL + 13), (o0 + 10, o1 - 22, FL + 25))):
        tube(b, "Wood", (front + 34 + i * 6, y, za), (front + 30 + i * 4, y + 2, zb), 7.0 - i, segments=10)
    aabb(b, "Embers", (front + 28, FL + 1, o0 + 22), (front + 50, FL + 4, o1 - 22), faces="Y")
    tube(b, "Iron", (front + 12, FL + 6, o0 + 10), (front + 12, FL + 6, o1 - 10), 1.2, segments=6)

    # Two iron chandeliers hung from the vigas.
    for x in (-525.0, 525.0):
        ring_y = 520.0
        tube(b, "Iron", (x, HALL_CEIL - 30, cz), (x, ring_y + 55, cz), 1.0, segments=6)
        lathe(b, "Iron", (x, ring_y, cz), [(4, 45), (6, 50), (6, 56), (3, 60), (0.01, 62)], segments=10)
        for k in range(4):
            a = math.pi / 4 + k * math.pi / 2
            tube(b, "Iron", (x, ring_y + 52, cz), (x + 68 * math.cos(a), ring_y + 2, cz + 68 * math.sin(a)), 0.7, segments=5)
        for k in range(16):
            a0, a1 = 2 * math.pi * k / 16, 2 * math.pi * (k + 1) / 16
            tube(b, "Iron", (x + 70 * math.cos(a0), ring_y, cz + 70 * math.sin(a0)), (x + 70 * math.cos(a1), ring_y, cz + 70 * math.sin(a1)),
                 1.4, segments=6, caps=False)
        for k in range(8):
            a = 2 * math.pi * k / 8 + math.pi / 8
            px, pz = x + 70 * math.cos(a), cz + 70 * math.sin(a)
            lathe(b, "Iron", (px, ring_y, pz), [(1.5, 0), (4.5, 3), (4.5, 6.5), (3.2, 6.5)], segments=10)
            tube(b, "Wax", (px, ring_y + 6, pz), (px, ring_y + 24 + rand(f"candle{x}{k}", -3, 3), pz), 2.4, segments=8)
            tip = ring_y + 24 + rand(f"candle{x}{k}", -3, 3)
            lathe(b, "Flame", (px, tip + 0.5, pz), [(0.01, 0), (1.2, 1.5), (1.5, 3.2), (1.0, 5.5), (0.01, 8.5)], segments=8)
    return b


LANTERNS = []    # (position of the flame, wall normal) -- filled by build_lanterns


def lantern(b, at, n):
    """A wall lantern: an iron bracket, a glazed square cage, a candle."""
    side = norm(cross(Y_AXIS, n))
    wall = at
    centre = combo(wall, (n, 30.0))
    tube(b, "Iron", combo(wall, (Y_AXIS, 22)), combo(centre, (Y_AXIS, 22)), 1.2, segments=6)
    tube(b, "Iron", combo(wall, (Y_AXIS, 2)), combo(centre, (n, -10), (Y_AXIS, 22)), 0.9, segments=6)
    aabb(b, "Iron", combo(wall, (Y_AXIS, -6), (side, -5), (n, 0)) if False else tuple(min(a, c) for a, c in zip(combo(wall, (Y_AXIS, -6), (side, -5)), combo(wall, (Y_AXIS, 30), (side, 5), (n, 2)))),
         tuple(max(a, c) for a, c in zip(combo(wall, (Y_AXIS, -6), (side, -5)), combo(wall, (Y_AXIS, 30), (side, 5), (n, 2)))))
    h0, h1 = -14.0, 16.0
    hw = 9.0
    corners = [combo(centre, (side, sa * hw), (n, sb * hw)) for sa, sb in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    for p in corners:
        tube(b, "Iron", combo(p, (Y_AXIS, h0)), combo(p, (Y_AXIS, h1)), 0.8, segments=5)
    for k in range(4):
        p, q = corners[k], corners[(k + 1) % 4]
        out_n = norm(combo((0, 0, 0), (sub(lerp_mid(p, q), centre), 1.0)))
        b.face("Glass", [combo(p, (Y_AXIS, h0 + 2)), combo(q, (Y_AXIS, h0 + 2)), combo(q, (Y_AXIS, h1 - 1)), combo(p, (Y_AXIS, h1 - 1))], out_n)
        for y in (h0, h1):
            tube(b, "Iron", combo(p, (Y_AXIS, y)), combo(q, (Y_AXIS, y)), 0.9, segments=5, caps=False)
    lathe(b, "Iron", combo(centre, (Y_AXIS, h0 - 3)), [(2, 0), (hw + 1.5, 2.5), (hw + 1.5, 3.5), (0.01, 3.6)], segments=4, phase=math.pi / 4, hard_angle=10)
    lathe(b, "Iron", combo(centre, (Y_AXIS, h1 - 0.5)), [(hw + 2.5, 0), (hw + 2.5, 1.2), (4, 9), (1.5, 11), (1.5, 15), (0.01, 15.5)],
          segments=4, phase=math.pi / 4, hard_angle=10)
    tube(b, "Wax", combo(centre, (Y_AXIS, h0)), combo(centre, (Y_AXIS, h0 + 12)), 1.8, segments=8)
    lathe(b, "Flame", combo(centre, (Y_AXIS, h0 + 12.4)), [(0.01, 0), (1.0, 1.2), (1.3, 2.8), (0.8, 4.6), (0.01, 7)], segments=8)
    LANTERNS.append((combo(centre, (Y_AXIS, h0 + 15)), n))


def lerp_mid(a, c):
    return ((a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2)


LANTERN_SPOTS = ([((sx * 1115.0, 232.0, z), (-sx, 0.0, 0.0)) for sx in (-1, 1) for z in (-112.0, 112.0)]
                 + [((x, 232.0, 785.0), (0.0, 0.0, -1.0)) for x in (-125.0, 125.0)]
                 + [((x, 232.0, -785.0), (0.0, 0.0, 1.0)) for x in (-245.0, 245.0)])


def build_lanterns():
    b = Builder("Lanterns", material)
    LANTERNS.clear()
    for at, n in LANTERN_SPOTS:
        lantern(b, at, n)
    return b


PLANTERS = ([("tree", (sx * 655.0, 0.0, sz * 330.0)) for sx in (-1, 1) for sz in (-1, 1)]
            + [("shrub", (sx * 1050.0, FL, z)) for sx in (-1, 1) for z in (-480.0, 480.0)]
            + [("shrub", (x, FL, 720.0)) for x in (-480.0, 480.0)])


def pot(b, at, big):
    s = 1.0 if big else 0.72
    prof = [(28, 0), (30, 4), (39, 30), (45, 52), (47, 58), (51, 60), (51, 68), (46, 68), (44, 63)]
    lathe(b, "Terracotta", at, [(r * s, y * s) for r, y in prof], segments=28)
    b.face("Soil", [combo(at, (X_AXIS, 44 * s * math.cos(2 * math.pi * k / 20)), (Z_AXIS, 44 * s * math.sin(2 * math.pi * k / 20)), (Y_AXIS, 62 * s))
                    for k in range(20)], Y_AXIS)
    return 62 * s


def crown(b, centre, radii, count, size, seed):
    """Leaf cards scattered through an ellipsoid, each facing its own way,
    their normals bent out from the crown's middle so it lights as a mass."""
    for k in range(count):
        key = f"{seed}/{k}"
        u, v, w = rand(key + "u"), rand(key + "v"), rand(key + "w")
        theta, phi = 2 * math.pi * u, math.acos(2 * v - 1)
        r = w ** 0.45
        offset = (radii[0] * r * math.sin(phi) * math.cos(theta), radii[1] * r * math.cos(phi), radii[2] * r * math.sin(phi) * math.sin(theta))
        p = add3(centre, offset)
        a, bb_ = rand(key + "a", 0, 2 * math.pi), rand(key + "b", -1.2, 1.2)
        n = norm((math.cos(a) * math.cos(bb_), math.sin(bb_), math.sin(a) * math.cos(bb_)))
        side = norm(cross(Y_AXIS if abs(n[1]) < 0.9 else X_AXIS, n))
        up = cross(n, side)
        spin = rand(key + "s", 0, 2 * math.pi)
        e1 = combo((0, 0, 0), (side, math.cos(spin)), (up, math.sin(spin)))
        e2 = cross(n, e1)
        half = size * rand(key + "z", 0.75, 1.15) * 0.5
        quad = [combo(p, (e1, -half), (e2, -half)), combo(p, (e1, half), (e2, -half)), combo(p, (e1, half), (e2, half)), combo(p, (e1, -half), (e2, half))]
        bent = [norm(add3(mul(norm(sub(q, centre)), 1.0), mul(n, 0.35))) for q in quad]
        avg = norm(add3(add3(bent[0], bent[1]), add3(bent[2], bent[3])))
        b.face("Foliage", quad, avg, uv=[(0, 1), (1, 1), (1, 0), (0, 0)], normals=bent)


def add3(a, c):
    return (a[0] + c[0], a[1] + c[1], a[2] + c[2])


def build_planters():
    pots = Builder("Planters", material)
    leaves = Builder("Foliage", material)
    for i, (kind, at) in enumerate(PLANTERS):
        big = kind == "tree"
        soil = pot(pots, at, big)
        if big:
            base = add3(at, (0, soil, 0))
            lean = (rand(f"lean{i}", -6, 6), 0, rand(f"leanz{i}", -6, 6))
            mid = add3(base, (lean[0] * 0.5, 55, lean[2] * 0.5))
            top = add3(base, (lean[0], 105, lean[2]))
            tube(pots, "Wood", base, mid, 4.5, segments=8, radius_end=3.8)
            tube(pots, "Wood", mid, top, 3.8, segments=8, radius_end=2.6)
            for k in range(4):
                a = k * math.pi / 2 + rand(f"br{i}{k}", -0.4, 0.4)
                tube(pots, "Wood", top, add3(top, (34 * math.cos(a), 30, 34 * math.sin(a))), 2.2, segments=6, radius_end=0.8)
            crown(leaves, add3(top, (0, 48, 0)), (72, 58, 72), 64, 48, f"tree{i}")
        else:
            crown(leaves, add3(at, (0, soil + 32, 0)), (40, 34, 40), 30, 36, f"shrub{i}")
    return pots, leaves


# --- The fire in the hearth ----------------------------------------------------------------------------------

PARTICLE_DIR = GAME_DIR / "Content" / "Particles" / "Courtyard"
HEARTH_SYSTEM = "Content/Particles/Courtyard/Hearth.nmparticle"
HEARTH_AT = (IN_X - 30 - 80 + 36, FL + 17, 1265.0)     # on the logs, in the firebox's middle

EMITTER_DEFAULTS = [
    ("Enabled", "true"), ("Type", "Sprite"), ("Modules", "Shape Velocity Forces"), ("Mesh", ""), ("Material", ""),
    ("Offset", "0,0,0"), ("Billboard", "FaceCamera"), ("VelocityStretch", "0"), ("MeshOrientation", "RandomSpin"),
    ("RandomRotation", "false"), ("RotationRate", "0"), ("SpawnMode", "Continuous"), ("MaxParticles", "128"),
    ("Shape", "Box"), ("ShapeSurface", "false"), ("BoxSize", "100,100,100"), ("ConeAngle", "25"), ("RingWidth", "0"),
    ("ShapeSpeed", "0"), ("SpawnRate", "10"), ("BurstCount", "0"), ("BurstInterval", "0"), ("BurstCycles", "1"),
    ("Lifetime", "1"), ("LifetimeVariance", "0"), ("EmitterRadius", "0"), ("Seed", "0"), ("CullDistance", "0"),
    ("PauseOffscreen", "true"), ("Velocity", "0,0,0"), ("VelocitySpread", "0"), ("Acceleration", "0,0,0"), ("Drag", "0"),
    ("Turbulence", "0"), ("CollisionResponse", "None"), ("CollisionHeight", "0"), ("Bounce", "0.4"), ("Friction", "0.2"),
    ("FlipbookColumns", "1"), ("FlipbookRows", "1"), ("FlipbookFrames", "0"), ("FlipbookRate", "0"), ("FlipbookCycles", "1"),
    ("FlipbookRandomStart", "false"), ("BeamEnd", "0,200,0"), ("BeamSegments", "16"), ("BeamNoise", "0"), ("BeamNoiseSpeed", "4"),
    ("SizeMode", "StartEnd"), ("StartSize", "10"), ("EndSize", "10"), ("SizeKeys", ""), ("ColorMode", "StartEnd"),
    ("StartColor", "255,255,255,255"), ("EndColor", "255,255,255,0"), ("ColorCurve", "1"), ("FadeIn", "0"), ("FadeOut", "0"),
    ("ColorKeys", ""),
]


def emitter(name, **settings):
    lines = [f"Emitter {name}"]
    for key, default in EMITTER_DEFAULTS:
        value = settings.pop(key, default)
        lines.append(f"    {key} {value}".rstrip())
    assert not settings, f"{name}: unknown settings {sorted(settings)}"
    return "\n".join(lines) + "\n"


def write_hearth_particles():
    """Hearth.nmparticle: flame tongues rising off the logs, the odd ember
    drifting up toward the flue. Their light is the Fire point light,
    flickered by WorldsLabExplore (WorldsLab.Flicker)."""
    flames = emitter("Flames", Material=material("HearthFlame"), Billboard="Vertical", MaxParticles="160",
                     BoxSize="24,3,78", SpawnRate="64", Lifetime="0.7", LifetimeVariance="0.4", Seed="3",
                     Velocity="0,62,0", VelocitySpread="10", Acceleration="0,55,0", Drag="0.9", Turbulence="38",
                     SizeMode="Curve", SizeKeys="0:20 0.3:30 0.75:22 1:9",
                     ColorMode="Gradient", ColorKeys="0:255,244,190,0 0.1:255,226,140,255 0.4:255,148,52,230 0.75:235,72,20,140 1:130,24,8,0")
    embers = emitter("Embers", Material="Content/Materials/Courtyard/ParticleStreak.material.nmgraph", Billboard="VelocityAligned",
                     VelocityStretch="0.03", MaxParticles="48", BoxSize="20,2,70", SpawnRate="5", Lifetime="1.6",
                     LifetimeVariance="0.5", Seed="11", Velocity="0,105,0", VelocitySpread="45", Acceleration="0,25,0",
                     Drag="0.8", Turbulence="90", SizeMode="Curve", SizeKeys="0:1.6 0.5:1.2 1:0.5",
                     ColorMode="Gradient", ColorKeys="0:255,240,190,255 0.3:255,170,70,255 0.8:255,90,25,200 1:180,40,10,0")
    PARTICLE_DIR.mkdir(parents=True, exist_ok=True)
    (PARTICLE_DIR / "Hearth.nmparticle").write_text("NimbleParticleSystem 1\n\n" + flames + "\n" + embers, newline="\n")


FOUNTAIN_SYSTEM = "Content/Particles/Courtyard/Fountain.nmparticle"

# The fountain's water, by build_fountain's heights (its entity at the origin,
# so each emitter's Offset and kill plane are world heights): the finial's
# tip, the small bowl's rim and water, the great bowl's, the basin's water.
FINIAL_TIP, SMALL_RIM, SMALL_WATER = (249.0, (38.0, 215.0), 212.0)
GREAT_RIM, GREAT_WATER, BASIN_WATER = (88.0, 159.0), 154.5, 46.0


def write_fountain_particles():
    """Fountain.nmparticle: the fountain running. A jet from the finial
    falling back into the small bowl; the small bowl overflowing its rim in a
    thin sheet into the great bowl, and the great bowl into the basin --
    streaks along their velocity, each killed at the water it falls into --
    and droplets splashing up where each lands."""
    water = material("WaterSpray")
    falling = dict(Material=water, Billboard="VelocityAligned", CollisionResponse="Kill", Modules="Shape Velocity Forces Collision",
                   Acceleration="0,-981,0", ColorMode="Gradient", PauseOffscreen="true")

    def sheet(name, rim, water_y, outward, rate, life, width, seed):
        return emitter(name, Shape="Ring", ShapeSurface="true", EmitterRadius=num(rim[0]), Offset=f"0,{num(rim[1])},0",
                       ShapeSpeed=num(outward), Velocity="0,-12,0", VelocitySpread="6", SpawnRate=num(rate),
                       Lifetime=num(life), LifetimeVariance="0.1", MaxParticles=str(int(rate * life * 1.25)),
                       CollisionHeight=num(water_y), VelocityStretch="0.045", Seed=str(seed),
                       # Wide and faint at the lip, where the sheet still holds together; strands lower down.
                       SizeMode="Curve", SizeKeys=f"0:{num(width * 1.9)} 0.25:{num(width * 1.2)} 1:{num(width * 0.7)}",
                       ColorKeys="0:255,255,255,0 0.06:255,255,255,200 0.8:255,255,255,170 1:255,255,255,120", **falling)

    def splash(name, centre_r, width, water_y, rate, seed, up=70.0):
        return emitter(name, Shape="Ring", EmitterRadius=num(centre_r), RingWidth=num(width), Offset=f"0,{num(water_y + 1)},0",
                       ShapeSpeed="18", Velocity=f"0,{num(up)},0", VelocitySpread="38", SpawnRate=num(rate),
                       Lifetime="0.38", LifetimeVariance="0.4", MaxParticles=str(int(rate * 0.5)), CollisionHeight=num(water_y),
                       VelocityStretch="0.03", Seed=str(seed), SizeMode="StartEnd", StartSize="1.3", EndSize="0.8",
                       ColorKeys="0:255,255,255,210 0.7:255,255,255,150 1:255,255,255,0", **falling)

    jet = emitter("Jet", Shape="Cone", ConeAngle="9", EmitterRadius="1.2", Offset=f"0,{num(FINIAL_TIP)},0", ShapeSpeed="232",
                  VelocitySpread="8", SpawnRate="170", Lifetime="0.9", LifetimeVariance="0.1", MaxParticles="200",
                  CollisionHeight=num(SMALL_WATER), VelocityStretch="0.05", Seed="21", SizeMode="Curve", SizeKeys="0:1.4 1:1.8",
                  ColorKeys="0:255,255,255,230 0.7:255,255,255,190 1:255,255,255,140", **falling)
    systems = [
        jet,
        splash("JetSplash", 12.0, 16.0, SMALL_WATER, 110, 22, up=40.0),
        sheet("UpperSheet", SMALL_RIM, GREAT_WATER, 22.0, 1350, 0.42, 1.7, 23),
        splash("UpperSplash", 46.0, 10.0, GREAT_WATER, 160, 24, up=50.0),
        sheet("LowerSheet", GREAT_RIM, BASIN_WATER, 30.0, 2700, 0.56, 1.9, 25),
        splash("LowerSplash", 102.0, 14.0, BASIN_WATER, 320, 26),
    ]
    PARTICLE_DIR.mkdir(parents=True, exist_ok=True)
    (PARTICLE_DIR / "Fountain.nmparticle").write_text("NimbleParticleSystem 1\n\n" + "\n".join(systems), newline="\n")


def particles(system):
    return ("  <Component Type=\"Particle System\" Version=\"3\">\n"
            f"    <System Value=\"{system}\" />\n"
            "    <Emitting Value=\"true\" />\n"
            "    <Tint R=\"255\" G=\"255\" B=\"255\" A=\"255\" />\n"
            "    <SpawnRateScale Value=\"1\" />\n"
            "    <TimeScale Value=\"1\" />\n"
            "    <Seed Value=\"0\" />\n"
            "    <UseBeamTarget Value=\"false\" />\n"
            "    <BeamTarget X=\"0\" Y=\"0\" Z=\"0\" />\n"
            "  </Component>\n")


# --- The world ----------------------------------------------------------------------------------------------

def guid(name):
    return hashlib.md5(("Courtyard/" + name).encode()).hexdigest()


def num(value):
    text = repr(float(round(value, 4)))
    return text[:-2] if text.endswith(".0") else text


def transform(position, rotation=(0, 0, 0), scale=(1, 1, 1)):
    return ("  <Component Type=\"Transform\" Version=\"1\">\n"
            f"    <Position X=\"{num(position[0])}\" Y=\"{num(position[1])}\" Z=\"{num(position[2])}\" />\n"
            f"    <Rotation X=\"{num(rotation[0])}\" Y=\"{num(rotation[1])}\" Z=\"{num(rotation[2])}\" />\n"
            f"    <Scale X=\"{num(scale[0])}\" Y=\"{num(scale[1])}\" Z=\"{num(scale[2])}\" />\n"
            "  </Component>\n")


def render(mesh, sections, casts=True, occluder=False, params=()):
    """A Render component: the mesh, each slot its section's material."""
    lines = ["  <Component Type=\"Render\" Version=\"5\">\n", "    <Visible Value=\"true\" />\n"]
    for slot in range(8):
        value = material(sections[slot]) if slot < len(sections) else ""
        lines.append(f"    <Material{slot} Value=\"{value}\" />\n")
    lines.append(f"    <Mesh Value=\"{mesh}\" />\n")
    lines.append(f"    <ParameterCount Value=\"{len(params)}\" />\n")
    for index, (slot, name, value) in enumerate(params):
        lines.append(f"    <Parameter{index}Name Value=\"{name}\" />\n")
        lines.append(f"    <Parameter{index}Slot Value=\"{slot}\" />\n")
        for axis, component in zip("XYZW", tuple(value) + (0,) * (4 - len(value))):
            lines.append(f"    <Parameter{index}{axis} Value=\"{num(component)}\" />\n")
    lines.append(f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n")
    lines.append("    <CastInsetShadow Value=\"false\" />\n")
    lines.append(f"    <Occluder Value=\"{'true' if occluder else 'false'}\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def colour(r, g, b):
    return f"R=\"{r}\" G=\"{g}\" B=\"{b}\" A=\"255\""


def shadow_fields(casts, bias, normal_bias, technique=0, tint=(40, 50, 80)):
    return (f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n"
            f"    <ShadowTechnique Value=\"{technique}\" />\n"
            f"    <ShadowTint {colour(*tint)} />\n"
            f"    <ShadowBias Value=\"{bias}\" />\n"
            f"    <ShadowNormalBias Value=\"{normal_bias}\" />\n")


def point_light(rgb, intensity, radius, casts, source=4.0):
    return ("  <Component Type=\"Point Light\" Version=\"2\">\n"
            f"    <Color {colour(*rgb)} />\n"
            f"    <Intensity Value=\"{intensity}\" />\n"
            f"    <Radius Value=\"{radius}\" />\n"
            "    <Active Value=\"true\" />\n"
            + shadow_fields(casts, 0.004, 2) +
            f"    <SourceRadius Value=\"{source}\" />\n"
            "  </Component>\n")


def spot_light(rgb, intensity, range_, inner, outer, casts, source=4.0):
    """A Spot Light (version 1): it shines along its entity's forward, Inner and Outer half-angles in degrees."""
    return ("  <Component Type=\"Spot Light\" Version=\"1\">\n"
            f"    <Color {colour(*rgb)} />\n"
            f"    <Intensity Value=\"{intensity}\" />\n"
            f"    <Range Value=\"{range_}\" />\n"
            f"    <InnerConeAngle Value=\"{inner}\" />\n"
            f"    <OuterConeAngle Value=\"{outer}\" />\n"
            "    <Active Value=\"true\" />\n"
            + shadow_fields(casts, 0.002, 1.5) +
            f"    <SourceRadius Value=\"{source}\" />\n"
            "  </Component>\n")


def probe(radius, extent=(0, 0, 0), resolution=256):
    return ("  <Component Type=\"Reflection Probe\" Version=\"1\">\n"
            f"    <Radius Value=\"{radius}\" />\n"
            f"    <BoxExtent X=\"{num(extent[0])}\" Y=\"{num(extent[1])}\" Z=\"{num(extent[2])}\" />\n"
            "    <Intensity Value=\"1\" />\n"
            f"    <Resolution Value=\"{resolution}\" />\n"
            "    <Frequency Value=\"2\" />\n"     # SCF_Once: the first frame it is seen (0 is every frame)
            "    <Interval Value=\"1\" />\n"
            "    <NearClip Value=\"5\" />\n"
            "    <FarClip Value=\"20000\" />\n"
            "    <CaptureShadows Value=\"true\" />\n"
            "    <Enabled Value=\"true\" />\n"
            "  </Component>\n")


def entity(name, components, mobility=None):
    attr = f" Mobility=\"{mobility}\"" if mobility else ""
    body = "".join(text for _, text in sorted(components))
    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            f"<Entity Guid=\"{guid(name)}\" Name=\"{name}\"{attr}>\n{body}</Entity>\n")


# Per-section looks set on the Render component (a slot's Tint parameter).
# Foliage: its clip is not run in the shadow pass (whole cards would cast). Lanterns:
# their cages sit round their own lights, which cast shadows (bars striped over the walls).
SHADOWLESS = {"Foliage", "Lanterns"}
OCCLUDERS = {"Walls_Hall", "Walls_West", "Walls_East", "Walls_South"}


def main():
    MESH_DIR.mkdir(parents=True, exist_ok=True)
    builders = {}
    builders.update(build_walls())
    builders["Floors"] = build_floors()
    builders["Patio"] = build_patio()
    builders["Ground"] = build_ground()
    builders["Ceilings"] = build_ceilings()
    builders["Columns"] = build_columns()
    builders.update(build_roofs())
    builders.update(build_windows())
    builders["Fountain"] = build_fountain()
    builders["Hall_Furnishings"] = build_hall_furnishings()
    builders["Lanterns"] = build_lanterns()
    pots, leaves = build_planters()
    builders["Planters"] = pots
    builders["Foliage"] = leaves

    entities = []
    written = set()
    total_bytes = 0
    for name, b in builders.items():
        if name != "Foliage":    # bent normals on two-sided cards: no single facing to check
            b.check()
        data, vertex_count, triangle_count, sections = b.serialize()
        assert len(sections) <= 8, f"{name}: {len(sections)} sections, a Render component has 8 slots"
        path = MESH_DIR / f"{name}.nmmesh"
        path.write_bytes(data)
        written.add(path.name)
        total_bytes += len(data)
        print(f"  {name}: {vertex_count} vertices, {triangle_count} triangles, {len(data) / 1e6:.2f} MB, sections {', '.join(sections)}"
              + (" (lightmapped)" if b.lightmap else ""))
        entities.append((name, entity(name, [("Render", render(mesh_path(name), sections, casts=name not in SHADOWLESS,
                                                               occluder=name in OCCLUDERS)),
                                             ("Transform", transform((0, 0, 0)))], "Static")))
    for stale in MESH_DIR.glob("*.nmmesh"):
        if stale.name not in written:
            stale.unlink()
    print(f"  meshes: {total_bytes / 1e6:.1f} MB")

    # --- Lights. The sun: setting in the west-south-west, deep orange, cascaded (its disk is in the sky's HDRI).
    sun = ("  <Component Type=\"Directional Light\" Version=\"3\">\n"
           f"    <Color {colour(255, 146, 76)} />\n"
           "    <Intensity Value=\"4.2\" />\n"
           "    <Active Value=\"true\" />\n"
           + shadow_fields(True, 0.0012, 1.5, technique=2) +
           "    <ShadowDistance Value=\"2600\" />\n"
           "    <CascadeCount Value=\"4\" />\n"
           "    <CascadeSplitLambda Value=\"0.88\" />\n"
           "    <SourceAngle Value=\"0.6\" />\n"
           "  </Component>\n")
    entities.append(("Sun", entity("Sun", [("Directional Light", sun), ("Transform", transform((0, 3000, 0), (SUN_PITCH, SUN_YAW, 0)))], "Stationary")))

    sky = ("  <Component Type=\"Sky Light\" Version=\"2\">\n"
           f"    <SkyColor {colour(96, 104, 160)} />\n"
           f"    <GroundColor {colour(70, 54, 46)} />\n"
           "    <Intensity Value=\"0.4\" />\n"
           "    <Active Value=\"true\" />\n"
           f"    <Environment Value=\"{SKY_TEXTURE}\" />\n"
           "    <EnvironmentIntensity Value=\"1\" />\n"
           "    <EnvironmentRotation Value=\"0\" />\n"
           "    <DrawBackground Value=\"true\" />\n"
           "  </Component>\n")
    entities.append(("Sky", entity("Sky", [("Sky Light", sky), ("Transform", transform((0, 0, 0)))], "Stationary")))

    # Reflection probes: the courtyard and its galleries, the hall, and the outside.
    for name, at, radius, extent in (("ProbeCourtyard", (0, 220, 0), 1300, (1115, 300, 785)),
                                     ("ProbeHall", (0, 390, 1265), 1200, (1115, 370, 420)),
                                     ("ProbeOutside", (0, 300, -2400), 4000, (0, 0, 0))):
        entities.append((name, entity(name, [("Reflection Probe", probe(radius, extent)), ("Transform", transform(at))], "Static")))

    # The hall's chandeliers and the gallery's lanterns: shadowed, or each would shine through
    # the walls onto the floors beyond (the level shadows the four nearest). A chandelier's light
    # hangs just under its ring, so the ring and candles shadow only the ceiling.
    for i, x in enumerate((-525.0, 525.0)):
        name = f"Chandelier{i}"
        entities.append((name, entity(name, [("Point Light", point_light((255, 168, 98), 11, 1100, True, source=30)),
                                             ("Transform", transform((x, 506, 1265)))], "Stationary")))
    # The fountain running.
    write_fountain_particles()
    entities.append(("FountainWater", entity("FountainWater", [("Particle System", particles(FOUNTAIN_SYSTEM)),
                                                               ("Transform", transform((0, 0, 0)))])))

    # The fire: flames and embers on the logs, and their light (flickered by WorldsLab.Flicker, so Dynamic).
    write_hearth_particles()
    entities.append(("Hearth", entity("Hearth", [("Particle System", particles(HEARTH_SYSTEM)), ("Transform", transform(HEARTH_AT))])))
    # The firelight: a spot aimed out of the firebox (forward -X, a little up), its apex on the
    # logs -- nothing behind it is lit, so none of it reaches through the wall into the east wing.
    entities.append(("Fire", entity("Fire", [("Spot Light", spot_light((255, 132, 56), 7, 700, 55, 86, False, source=20)),
                                            ("Transform", transform((IN_X - 30 - 80 + 40, FL + 45, 1265), (-8, -90, 0)))])))
    for i, (at, n) in enumerate(LANTERNS):
        name = f"Lantern{i}"
        entities.append((name, entity(name, [("Point Light", point_light((255, 176, 104), 3.2, 560, True, source=6)),
                                             ("Transform", transform(at))], "Stationary")))

    # --- The player, in the south gallery's corner, looking across the fountain to the hall.
    camera = ("  <Component Type=\"Camera\" Version=\"2\">\n"
              "    <ZNear Value=\"5\" />\n"
              "    <ZFar Value=\"30000\" />\n"
              "    <Active Value=\"true\" />\n"
              "    <Projection Value=\"1\" />\n"
              "    <FovY Value=\"62\" />\n"
              "  </Component>\n")
    player_input = ("  <Component Type=\"Input\" Version=\"1\">\n"
                    "    <Map Value=\"Default\" />\n"
                    "    <PlayerIndex Value=\"0\" />\n"
                    "    <Enabled Value=\"true\" />\n"
                    "  </Component>\n")
    entities.append(("Player", entity("Player", [("Camera", camera), ("Input", player_input),
                                                 ("Transform", transform((-930, 175, -690), (4, 38, 0)))])))
    post = ("  <Component Type=\"Post Process\" Version=\"2\">\n"
            f"    <Stack Value=\"{POST_STACK}\" />\n"
            "    <Priority Value=\"0\" />\n"
            "    <Enabled Value=\"true\" />\n"
            "    <BlendDistance Value=\"0\" />\n"
            "  </Component>\n")
    entities.append(("PostProcess", entity("PostProcess", [("Post Process", post), ("Transform", transform((0, 0, 0)))])))

    FOLDER.mkdir(parents=True, exist_ok=True)
    for stale in list(FOLDER.glob("*.nmentity")) + list(FOLDER.glob("*.nmprefabinst")):
        stale.unlink()
    manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n",
                "  <Settings>\n",
                "    <Lighting BakedLighting=\"true\" TexelsPerUnit=\"0.12\" Bounces=\"3\" ProbeSpacing=\"150\" />\n",
                "  </Settings>\n"]
    for name, text in entities:
        (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
        manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
    manifest.append("</World>\n")
    WORLD.write_text("".join(manifest), newline="\n")
    print(f"{WORLD}: {len(entities)} entities")


if __name__ == "__main__":
    main()
