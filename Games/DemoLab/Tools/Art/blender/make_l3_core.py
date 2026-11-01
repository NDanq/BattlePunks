"""
make_l3_core.py -- L3_Core's level-specific dressing meshes (Content/Meshes/L3_Core/L3_Core_*.nmmesh):
the reactor shaft, the pit and its floor, the vault drum, the helix catwalk (and its collider),
the rooms' shells, cover props, alarm fixtures. The kit pieces (Content/Meshes/L3/*) stay
make_l3.py's; these are sized from Tools/Levels/layout.py (L3) so the shells sit on the grey
box's collision.

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_l3_core.py

Big pieces are exported in level space (pivot at the world origin): place them at (0, 0, 0).
Engine centimetres are converted here: E(x, y, z) is the engine point (x east, y up, z north) in
Blender metres.
"""

import math
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "Levels"))
import kitlib as K      # noqa: E402
import layout as LAY    # noqa: E402

D = "Meshes/L3_Core/"
L3 = LAY.L3
T = LAY.WALL_THICKNESS / 100.0
DOOR_H = LAY.DOOR_HEIGHT / 100.0
SPIRAL = L3['spiral']
SHAFT_H = L3['ring']['height'] / 100.0
ARRIVE = L3['arrival_y'] / 100.0
PIT = L3['pit']
TAN = math.tan(math.radians(22.5))


def E(x, y, z):
    """Engine centimetres -> Blender metres."""
    return (x / 100.0, z / 100.0, y / 100.0)


def compass(a, r):
    """(x, z) in metres at compass angle a (0 north, 90 east), radius r metres."""
    return r * math.sin(math.radians(a)), r * math.cos(math.radians(a))


def B(x, z, y):
    """Engine metres (x east, z north) at height y -> Blender."""
    return (x, z, y)


def export(name, objs, **kw):
    K.export(name, objs, D + name + ".nmmesh", record=False, **kw)
    K.clear()


def wall_slab(name, centre_a, apothem, width, y0, y1, mat, holes=(), thick=T):
    """A flat wall of an octagon side: its room face at apothem (metres) facing the axis,
    thick behind it. holes: (u, y0, w, h) doorways, u along the side (clockwise +)."""
    outline = [(-width / 2, y0), (width / 2, y0), (width / 2, y1), (-width / 2, y1)]
    hs = [[(u - w / 2, hy), (u + w / 2, hy), (u + w / 2, hy + h), (u - w / 2, hy + h)] for (u, hy, w, h) in holes]
    if hs:
        # a hole touching the slab's bottom edge: cut the outline instead
        cut = []
        for (u, hy, w, h) in holes:
            if abs(hy - y0) < 1e-4:
                cut.append((u, w, h))
        if cut:
            outline = [(-width / 2, y0)]
            for (u, w, h) in sorted(cut):
                outline += [(u - w / 2, y0), (u - w / 2, y0 + h), (u + w / 2, y0 + h), (u + w / 2, y0)]
            outline += [(width / 2, y0), (width / 2, y1), (-width / 2, y1)]
            hs = [hh for hh, (u, hy, w, h) in zip(hs, holes) if abs(hy - y0) >= 1e-4]
    x, z = compass(centre_a, apothem + thick / 2)
    return K.prism(name, outline, thick, loc=B(x, z, 0), mat=mat, bevel=0.0, rot=(0, 0, -centre_a), holes=hs)


def oct_ring(name, a_in, a_out, y_top, thick, mat, n_side=1):
    """A horizontal octagonal ring (apothems in metres), top at y_top."""
    bm = bmesh.new()
    def ring(apo, y):
        R = apo / math.cos(math.radians(22.5))
        return [bm.verts.new(B(*compass(22.5 + 45 * k, R), y)) for k in range(8)]
    ti, to = ring(a_in, y_top), ring(a_out, y_top)
    bi, bo = ring(a_in, y_top - thick), ring(a_out, y_top - thick)
    for k in range(8):
        j = (k + 1) % 8
        bm.faces.new([ti[k], to[k], to[j], ti[j]])
        bm.faces.new([bi[k], bi[j], bo[j], bo[k]])
        bm.faces.new([to[k], bo[k], bo[j], to[j]])
        bm.faces.new([ti[k], ti[j], bi[j], bi[k]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return K.obj_from_bm(name, bm, mat, sharp=math.radians(30))


def oct_plate(name, apo, y_top, thick, mat):
    bm = bmesh.new()
    R = apo / math.cos(math.radians(22.5))
    top = [bm.verts.new(B(*compass(22.5 + 45 * k, R), y_top)) for k in range(8)]
    bot = [bm.verts.new(B(*compass(22.5 + 45 * k, R), y_top - thick)) for k in range(8)]
    bm.faces.new(top)
    bm.faces.new(bot[::-1])
    for k in range(8):
        j = (k + 1) % 8
        bm.faces.new([top[k], bot[k], bot[j], top[j]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return K.obj_from_bm(name, bm, mat, sharp=math.radians(30))


def oct_loop(apo, y, closed=True, skip=None):
    """Points round an octagon (metres) at height y, through each side's ends."""
    R = apo / math.cos(math.radians(22.5))
    pts = [B(*compass(22.5 + 45 * k, R), y) for k in range(8)]
    return pts + [pts[0]] if closed else pts


def railing(points, mat_post="SteelDark", height=1.05):
    o = []
    pts = [Vector(p) for p in points]
    o.append(K.sweep("TopRail", [p + Vector((0, 0, height)) for p in pts], 0.025, "BrassAged", seg=8))
    o.append(K.sweep("MidRail", [p + Vector((0, 0, height * 0.52)) for p in pts], 0.016, mat_post, seg=6))
    seg = [(pts[i + 1] - pts[i]).length for i in range(len(pts) - 1)]
    total = sum(seg)
    n = max(1, int(round(total / 1.25)))
    for k in range(n + 1):
        d = total * k / n
        i, acc = 0, 0.0
        while i < len(seg) - 1 and acc + seg[i] < d:
            acc += seg[i]
            i += 1
        p = pts[i].lerp(pts[i + 1], min(1.0, (d - acc) / max(seg[i], 1e-9)))
        o.append(K.sweep("Post", [p, p + Vector((0, 0, height))], 0.022, mat_post, seg=6))
    return o


# ==============================================================================================
# The shaft: eight walls round the ring gallery, doors NW (the arrival, high) and S (the tunnel)
# ==============================================================================================

def shaft():
    ring = L3['ring']
    A = ring['apothem'] / 100.0
    side = 2 * (A + T) * TAN
    o = []
    bands = [0.0, 4.0, 8.0, 12.0, 16.0, 20.0, 24.0, SHAFT_H]
    for label, a in K_COMPASS.items():
        holes = []
        if label == 'NW':
            holes.append((0.0, ARRIVE, ring['doors']['NW'] / 100.0, DOOR_H))
        if label == 'S':
            holes.append((0.0, 0.0, ring['doors']['S'] / 100.0, DOOR_H))
        # lower course: green rivet panels; above: dark steel plate
        o.append(wall_slab("Lower", a, A, side, 0.0, 4.0, "PanelGreen",
                           holes=[h for h in holes if h[1] < 4.0]))
        o.append(wall_slab("Upper", a, A, side, 4.0, SHAFT_H, "SteelDark",
                           holes=[h for h in holes if h[1] >= 4.0]))
        # pilasters at the corners and the side's thirds, banded every 4 m
        for u in (-side / 2 + 0.2, -side / 6, side / 6, side / 2 - 0.2):
            if any(abs(u - h[0]) < h[2] / 2 + 0.35 for h in holes):
                continue
            cx, cz = compass(a, A - 0.12)
            tx, tz = math.cos(math.radians(a)), -math.sin(math.radians(a))
            p = (cx + tx * u, cz + tz * u)
            o.append(K.box("Rib", (0.34, 0.24, SHAFT_H), B(p[0], p[1], SHAFT_H / 2), "SteelDark", 0.02, rot=(0, 0, -a)))
            o.append(K.box("RibFace", (0.18, 0.06, SHAFT_H), B(p[0] - math.sin(math.radians(a)) * 0.13,
                                                               p[1] - math.cos(math.radians(a)) * 0.13, SHAFT_H / 2),
                           "BrassAged", 0.01, rot=(0, 0, -a)))
        # door frames
        for (u, hy, w, h) in holes:
            cx, cz = compass(a, A - 0.08)
            tx, tz = math.cos(math.radians(a)), -math.sin(math.radians(a))
            for du in (-w / 2 - 0.15, w / 2 + 0.15):
                p = (cx + tx * (u + du), cz + tz * (u + du))
                o.append(K.box("Jamb", (0.3, 0.2, h + 0.3), B(p[0], p[1], hy + (h + 0.3) / 2), "BrassAged", 0.03,
                               rot=(0, 0, -a)))
            p = (cx + tx * u, cz + tz * u)
            o.append(K.box("Lintel", (w + 0.6, 0.22, 0.35), B(p[0], p[1], hy + h + 0.17), "BrassAged", 0.03,
                           rot=(0, 0, -a)))
            o.append(K.box("Hazard", (w + 0.2, 0.05, 0.12), B(p[0] - math.sin(math.radians(a)) * 0.12,
                                                               p[1] - math.cos(math.radians(a)) * 0.12, hy + h + 0.05),
                           "PaintRed", 0.005, rot=(0, 0, -a)))
    # horizontal bands (brass strap round the shaft) and a walkway ledge at the arrival
    for y in bands[1:-1]:
        o.append(K.sweep("Band", oct_loop(A - 0.06, y), 0.0, "BrassAged",
                         profile=[(-0.05, -0.12), (0.05, -0.12), (0.05, 0.12), (-0.05, 0.12)]))
    o.append(K.sweep("Skirting", oct_loop(A - 0.05, 0.12), 0.0, "SteelDark",
                     profile=[(-0.05, -0.12), (0.05, -0.12), (0.05, 0.12), (-0.05, 0.12)]))
    # vertical coolant risers on four sides
    for a in (30, 120, 200, 250, 340):
        x, z = compass(a, A - 0.45)
        o.append(K.sweep("Riser", [B(x, z, -0.1), B(x, z, SHAFT_H + 0.1)], 0.22, "Copper", seg=16))
        for y in (2.0, 6.0, 10.0, 14.0, 18.0, 22.0, 26.0):
            o.append(K.place(K.torus("Flange", 0.22, 0.05, mat="BrassAged", seg=24, rseg=6), B(x, z, y)))
            cx, cz = compass(a, A - 0.2)
            o.append(K.box("Clamp", (0.5, 0.08, 0.06), B(cx, cz, y + 0.4), "SteelDark", 0.01, rot=(0, 0, -a)))
    # the ceiling: an octagon slab with the vent oculus over the reactor, radial beams
    o.append(oct_ring("Ceiling", 4.2, A + T, SHAFT_H + 0.3, 0.3, "SteelDark"))
    o.append(oct_ring("OculusRim", 3.9, 4.4, SHAFT_H + 0.05, 0.25, "BrassAged"))
    o.append(oct_plate("OculusGrate", 4.2, SHAFT_H + 0.25, 0.04, "Grating"))
    o.append(oct_plate("VentStackBack", 4.6, SHAFT_H + 3.2, 0.1, "SteelDark"))
    for k in range(8):
        a = 22.5 + 45 * k
        x0, z0 = compass(a, 4.2)
        x1, z1 = compass(a, A + 0.1)
        o.append(K.sweep("Beam", [B(x0, z0, SHAFT_H - 0.25), B(x1, z1, SHAFT_H - 0.25)], 0.0, "SteelDark",
                         profile=[(-0.12, -0.25), (0.12, -0.25), (0.12, -0.2), (0.03, -0.2), (0.03, 0.2), (0.12, 0.2),
                                  (0.12, 0.25), (-0.12, 0.25), (-0.12, 0.2), (-0.03, 0.2), (-0.03, -0.2), (-0.12, -0.2)]))
    for k in range(8):   # the vent stack walls above the oculus
        a = 45 * k
        o.append(wall_slab("Stack", a, 4.4, 2 * 4.6 * TAN, SHAFT_H + 0.25, SHAFT_H + 3.2, "SteelDark", thick=0.15))
    export("L3_Core_Shaft", o, ao_dist=1.2)


K_COMPASS = {'N': 0, 'NE': 45, 'E': 90, 'SE': 135, 'S': 180, 'SW': 225, 'W': 270, 'NW': 315}


# ==============================================================================================
# The pit: the gallery floor (an octagonal ring), its rim and railing, the pit's walls down to
# the coolant pool
# ==============================================================================================

def pit():
    A = L3['ring']['apothem'] / 100.0
    Pa = PIT['apothem'] / 100.0
    depth = PIT['depth'] / 100.0
    bridge = PIT['bridge'] / 100.0
    o = [oct_ring("Floor", Pa + 0.4, A + T, 0.0, 0.3, "Concrete")]
    o.append(oct_ring("Rim", Pa, Pa + 0.4, 0.03, 0.33, "SteelDark"))
    o.append(oct_ring("Plates", Pa + 0.4, A - 1.2, 0.012, 0.02, "SteelDark"))
    o.append(oct_ring("PlateEdge", A - 1.25, A - 1.15, 0.016, 0.02, "BrassAged"))
    # pit walls: inward faces of the octagon apothem Pa, down to the pool
    side = 2 * Pa * TAN
    for label, a in K_COMPASS.items():
        o.append(wall_slab("PitWall", a, Pa, side + 0.02, -depth, -0.3, "Concrete", thick=0.4))
        for y in (-3.0, -6.0, -9.0, -12.0):
            o.append(wall_slab("PitBand", a, Pa - 0.05, side, y - 0.15, y + 0.15, "SteelDark", thick=0.05))
    o.append(oct_ring("Pool", 0.0001, Pa + 0.1, -depth + 0.4, 0.1, "ReactorGlow"))
    o.append(oct_ring("PoolRim", Pa - 0.6, Pa, -depth + 0.6, 0.6, "SteelDark"))
    # pit railing round the rim, open at the south bridge
    Rr = Pa + 0.12
    pts = oct_loop(Rr, 0.03, closed=False)
    # start just west of the bridge, run round clockwise (W, N, E) back to just east of it
    corners = [pts[k] for k in (4, 5, 6, 7, 0, 1, 2, 3)]   # 202.5, 247.5, ... 157.5 degrees
    wx = -(bridge / 2 + 0.05)
    ex = bridge / 2 + 2.6   # east of the bridge the helix's landing comes in: leave it open
    zS = -Rr
    path = [B(wx, zS, 0.03)] + corners[:7] + [B(ex, zS, 0.03)]
    o += railing(path)
    export("L3_Core_Pit", o, ao_dist=1.0)


# ==============================================================================================
# The vault drum: its outer wall from the pool to the roof, the vault inside (walnut and
# brass), the roof the reactor stands on
# ==============================================================================================

def drum():
    dr = L3['drum']
    Ai = dr['apothem'] / 100.0
    H = dr['height'] / 100.0
    depth = PIT['depth'] / 100.0
    side_o = 2 * (Ai + T) * TAN
    side_i = 2 * Ai * TAN
    door = dr['doors']['S'] / 100.0
    o = []
    for label, a in K_COMPASS.items():
        holes = [(0.0, 0.0, door, DOOR_H)] if label == 'S' else []
        # outer skin (faces outward): a slab whose room face is at Ai + T, thick inward
        x, z = compass(a, Ai + T / 2)
        outline = [(-side_o / 2, -depth), (side_o / 2, -depth), (side_o / 2, H + 0.4), (-side_o / 2, H + 0.4)]
        hs = []
        if holes:
            outline = [(-side_o / 2, -depth), (side_o / 2, -depth), (side_o / 2, H + 0.4), (-side_o / 2, H + 0.4)]
            hs = [[(-door / 2, 0.0), (door / 2, 0.0), (door / 2, DOOR_H), (-door / 2, DOOR_H)]]
        o.append(K.prism("DrumWall", outline, T, loc=B(x, z, 0), mat="SteelDark", bevel=0.0, rot=(0, 0, -a), holes=hs))
        # vault lining: walnut panels inside
        o.append(wall_slab("Lining", a, Ai - 0.02, side_i, 0.0, H, "PanelRed",
                           holes=[(0.0, 0.0, door, DOOR_H)] if label == 'S' else [], thick=0.02))
        # brass pilasters outside at the corners
        R = (Ai + T) / math.cos(math.radians(22.5))
        cx, cz = compass(a + 22.5, R + 0.04)
        o.append(K.box("Corner", (0.22, 0.22, depth + H + 0.4), B(cx, cz, (H + 0.4 - depth) / 2), "BrassAged", 0.02,
                       rot=(0, 0, -(a + 22.5))))
    for y in (-12.0, -8.0, -4.0, -0.4, 3.8, H + 0.3):
        o.append(K.sweep("Strap", oct_loop(Ai + T + 0.04, y), 0.0, "BrassAged",
                         profile=[(-0.04, -0.14), (0.04, -0.14), (0.04, 0.14), (-0.04, 0.14)]))
    o.append(oct_plate("VaultFloor", Ai, 0.01, 0.02, "MarbleFloor"))
    o.append(oct_plate("VaultCeiling", Ai, H + 0.02, 0.04, "PanelCream"))
    o.append(oct_plate("Roof", Ai + T + 0.1, H + 0.42, 0.4, "SteelDark"))
    o.append(K.place(K.torus("ReactorFlange", 3.3, 0.12, mat="BrassAged", seg=64, rseg=8), B(0, 0, H + 0.45)))
    # door frame (outside)
    for du in (-door / 2 - 0.15, door / 2 + 0.15):
        o.append(K.box("Jamb", (0.3, 0.25, DOOR_H + 0.3), B(du, -(Ai + T + 0.05), (DOOR_H + 0.3) / 2), "BrassAged", 0.03))
    o.append(K.box("Lintel", (door + 0.6, 0.25, 0.35), B(0, -(Ai + T + 0.05), DOOR_H + 0.17), "BrassAged", 0.03))
    o.append(K.box("Sign", (1.2, 0.04, 0.3), B(0, -(Ai + T + 0.2), DOOR_H + 0.55), "PanelRed", 0.01))
    # glow ring in the vault ceiling round the intel's light
    o.append(K.place(K.torus("CeilingRing", 1.6, 0.05, mat="IntelGlow", seg=48, rseg=6), B(0, 0, H - 0.05)))
    export("L3_Core_Drum", o, ao_dist=1.0)


# ==============================================================================================
# The helix: one 22.5-degree piece of the catwalk that spirals round the reactor (deck, both
# railings, stringers, the hangers to the lap above); placed by yaw and height. And the
# walkway's collider (the deck and two rail walls, as one mesh in level space).
# ==============================================================================================

def helix_point(a, r, y):
    x, z = compass(a, r)
    return B(x, z, y)


def helix_piece():
    Ri, Ro = SPIRAL['r_in'] / 100.0, SPIRAL['r_out'] / 100.0
    drop = SPIRAL['drop'] / 100.0
    A = 22.5
    n = 8
    bm = bmesh.new()
    rows = []
    for k in range(n + 1):
        a = A * k / n
        y = -drop * k / n
        row = [bm.verts.new(helix_point(a, r, y + dz)) for r in (Ri, Ro) for dz in (-0.04, 0.0)]
        rows.append(row)
    for k in range(n):
        a, b = rows[k], rows[k + 1]
        bm.faces.new([a[1], b[1], b[3], a[3]])
        bm.faces.new([a[0], a[2], b[2], b[0]])
        bm.faces.new([a[0], b[0], b[1], a[1]])
        bm.faces.new([a[2], a[3], b[3], b[2]])
    bm.faces.new([rows[0][0], rows[0][1], rows[0][3], rows[0][2]])
    bm.faces.new([rows[n][0], rows[n][2], rows[n][3], rows[n][1]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    o = [K.obj_from_bm("Deck", bm, "Grating", sharp=math.radians(30))]
    steps = 16
    for r, off in ((Ri - 0.03, -0.1), (Ro + 0.03, -0.1)):
        pts = [helix_point(A * k / steps, r, -drop * k / steps + off) for k in range(steps + 1)]
        o.append(K.sweep("Stringer", pts, 0.0, "SteelDark", profile=[(-0.04, -0.1), (0.04, -0.1), (0.04, 0.1), (-0.04, 0.1)]))
    for k in (0, 4, 8):
        a = A * k / 8
        y = -drop * k / 8 - 0.08
        o.append(K.sweep("Cross", [helix_point(a, Ri, y), helix_point(a, Ro, y)], 0.03, "SteelDark", seg=6))
    for r in (Ri + 0.04, Ro - 0.04):
        pts = [helix_point(A * k / steps, r, -drop * k / steps) for k in range(steps + 1)]
        o += railing(pts)
    # hangers to the lap above (16 pieces a lap), at the piece's start
    lap = 16 * drop
    for r in (Ri + 0.04, Ro - 0.04):
        o.append(K.sweep("Hanger", [helix_point(0.5, r, 1.05), helix_point(0.5, r, lap - 0.2)], 0.018, "SteelDark", seg=6))
    export("L3_Core_Helix", o, ao_dist=0.6)


def helix_collider():
    """The walkable stretch, in level space: SPIRAL['walk'] pieces from compass 'start'
    clockwise, from arrival_y down by drop each."""
    Ri, Ro = SPIRAL['r_in'] / 100.0, SPIRAL['r_out'] / 100.0
    drop = SPIRAL['drop'] / 100.0
    a0 = SPIRAL['start']
    pieces = SPIRAL['walk']
    top = ARRIVE
    n = pieces * 8
    span = 22.5 * pieces
    bm = bmesh.new()

    def strip(r0, r1, y_lo, y_hi, extend=0.0):
        rows = []
        for k in range(n + 1):
            a = a0 + span * k / n
            y = top - drop * pieces * k / n
            rows.append([bm.verts.new(helix_point(a, r, y + dy)) for r in (r0, r1) for dy in (y_lo, y_hi)])
        for k in range(n):
            a, b = rows[k], rows[k + 1]
            bm.faces.new([a[1], b[1], b[3], a[3]])
            bm.faces.new([a[0], a[2], b[2], b[0]])
            bm.faces.new([a[0], b[0], b[1], a[1]])
            bm.faces.new([a[2], a[3], b[3], b[2]])
        bm.faces.new([rows[0][0], rows[0][1], rows[0][3], rows[0][2]])
        bm.faces.new([rows[n][0], rows[n][2], rows[n][3], rows[n][1]])

    strip(Ri, Ro, -0.12, 0.0)                 # deck
    strip(Ri - 0.06, Ri + 0.02, 0.0, 1.3)     # inner rail wall
    strip(Ro - 0.02, Ro + 0.06, 0.0, 1.3)     # outer rail wall
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = K.obj_from_bm("Collider", bm, "SteelDark", smooth=False)
    K.export("L3_Core_HelixCollider", [ob], D + "L3_Core_HelixCollider.nmmesh", record=False, ao=False, check=False)
    K.clear()


# ==============================================================================================
# Rooms: shells on the grey box's walls (inner faces, doors cut where layout.py has them)
# ==============================================================================================

def rect_shell(name, x0, z0, x1, z1, h, doors, wall_lo="PanelGreen", wall_hi="SteelDark", floor="Concrete",
               ceiling="SteelDark", split=2.6, ribs=0.0, show_h=None):
    """Engine cm in, a level-space mesh out."""
    x0, z0, x1, z1, h = x0 / 100.0, z0 / 100.0, x1 / 100.0, z1 / 100.0, h / 100.0
    vis_h = (show_h or h * 100) / 100.0
    o = []
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    o.append(K.box("Floor", (x1 - x0, z1 - z0, 0.2), B(cx, cz, -0.1), floor, 0.0))
    if ceiling:
        o.append(K.box("Ceiling", (x1 - x0, z1 - z0, 0.2), B(cx, cz, vis_h + 0.1), ceiling, 0.0))

    def wall(label, length, centre, yaw, gaps):
        holes_lo = [(g, 0.0, w, DOOR_H) for g, w in gaps]
        for (mat, ya, yb) in ((wall_lo, 0.0, min(split, vis_h)), (wall_hi, min(split, vis_h), vis_h)):
            if yb - ya < 0.01:
                continue
            outline = [(-length / 2, ya), (length / 2, ya), (length / 2, yb), (-length / 2, yb)]
            hs = []
            if ya < DOOR_H:
                outline = [(-length / 2, ya)]
                for (u, hy, w, hh) in sorted(holes_lo):
                    top = min(hh, yb)
                    outline += [(u - w / 2, ya), (u - w / 2, top), (u + w / 2, top), (u + w / 2, ya)]
                outline += [(length / 2, ya), (length / 2, yb), (-length / 2, yb)]
            o.append(K.prism(label, outline, T, loc=B(centre[0], centre[1], 0), mat=mat, bevel=0.0, rot=(0, 0, yaw)))
            for (u, hy, w, hh) in holes_lo:
                pass
    # N wall: faces south; its local u runs west->east (yaw 180 turns the prism's -Y face to +Y... see below)
    # prism faces -Y (south) unturned: the N wall (room to its south) needs no turn
    wall("WallN", x1 - x0, (cx, z1 - T / 2), 0, [((x / 100.0) - cx, w / 100.0) for x, w in doors.get('N', [])])
    wall("WallS", x1 - x0, (cx, z0 + T / 2), 180, [(-((x / 100.0) - cx), w / 100.0) for x, w in doors.get('S', [])])
    wall("WallE", z1 - z0 - 2 * T, (x1 - T / 2, cz), 90, [((z / 100.0) - cz, w / 100.0) for z, w in doors.get('E', [])])
    wall("WallW", z1 - z0 - 2 * T, (x0 + T / 2, cz), -90, [(-((z / 100.0) - cz), w / 100.0) for z, w in doors.get('W', [])])
    # door frames
    def frame(px, pz, yaw, w):
        for du in (-w / 2 - 0.12, w / 2 + 0.12):
            dx, dz = math.cos(math.radians(yaw)) * du, -math.sin(math.radians(yaw)) * du
            o.append(K.box("Jamb", (0.24, T + 0.12, DOOR_H + 0.2), B(px + dx, pz + dz, (DOOR_H + 0.2) / 2), "BrassAged",
                           0.03, rot=(0, 0, -yaw)))
        o.append(K.box("Lintel", (w + 0.48, T + 0.12, 0.24), B(px, pz, DOOR_H + 0.12), "BrassAged", 0.03,
                       rot=(0, 0, -yaw)))
    for x, w in doors.get('N', []):
        frame(x / 100.0, z1 - T / 2, 0, w / 100.0)
    for x, w in doors.get('S', []):
        frame(x / 100.0, z0 + T / 2, 0, w / 100.0)
    for z, w in doors.get('E', []):
        frame(x1 - T / 2, z / 100.0, 90, w / 100.0)
    for z, w in doors.get('W', []):
        frame(x0 + T / 2, z / 100.0, 90, w / 100.0)
    if ribs > 0:
        n = max(1, int((z1 - z0) / ribs)) if (z1 - z0) > (x1 - x0) else max(1, int((x1 - x0) / ribs))
        along_z = (z1 - z0) > (x1 - x0)
        for k in range(1, n):
            if along_z:
                z = z0 + (z1 - z0) * k / n
                pts = [B(x0 + T, z, 0.0), B(x0 + T, z, vis_h - 0.25), B(x0 + T + 0.3, z, vis_h),
                       B(x1 - T - 0.3, z, vis_h), B(x1 - T, z, vis_h - 0.25), B(x1 - T, z, 0.0)]
            else:
                x = x0 + (x1 - x0) * k / n
                pts = [B(x, z0 + T, 0.0), B(x, z0 + T, vis_h - 0.25), B(x, z0 + T + 0.3, vis_h),
                       B(x, z1 - T - 0.3, vis_h), B(x, z1 - T, vis_h - 0.25), B(x, z1 - T, 0.0)]
            o.append(K.sweep("Rib", pts, 0.0, "SteelDark",
                             profile=[(-0.1, -0.07), (0.1, -0.07), (0.1, 0.07), (-0.1, 0.07)]))
    return o


def rooms():
    rs = L3['rooms']
    x0, z0, x1, z1, h, doors = rs['Tunnel']
    o = rect_shell("Tunnel", x0, z0, x1, z1, h, doors, split=1.4, ribs=3.0)
    # pipe bundle along the tunnel's west wall, a cable tray on the east
    xw, xe = x0 / 100.0 + T + 0.2, x1 / 100.0 - T - 0.15
    for i, (y, r) in enumerate(((2.9, 0.12), (3.25, 0.09), (2.55, 0.07))):
        o.append(K.sweep("Pipe", [B(xw + 0.05 * i, z0 / 100.0 + 0.3, y), B(xw + 0.05 * i, z1 / 100.0 - 0.3, y)], r,
                         "Copper", seg=12))
    o.append(K.sweep("Tray", [B(xe, z0 / 100.0 + 0.3, 3.1), B(xe, z1 / 100.0 - 0.3, 3.1)], 0.0, "SteelDark",
                     profile=[(-0.12, -0.05), (0.12, -0.05), (0.12, 0.05), (-0.12, 0.05)]))
    export("L3_Core_Tunnel", o, ao_dist=0.8)

    x0, z0, x1, z1, h, doors = rs['PumpHall']
    o = rect_shell("PumpHall", x0, z0, x1, z1, h, doors, split=3.0, ribs=4.2)
    # crane rails under the ceiling, piping from the pumps into the south wall
    for x in (x0 / 100.0 + 1.0, x1 / 100.0 - 1.0):
        o.append(K.sweep("CraneRail", [B(x, z0 / 100.0 + T, h / 100.0 - 0.5), B(x, z1 / 100.0 - T, h / 100.0 - 0.5)], 0.0,
                         "SteelDark", profile=[(-0.15, -0.25), (0.15, -0.25), (0.15, 0.25), (-0.15, 0.25)]))
    o.append(K.box("CraneBridge", ((x1 - x0) / 100.0 - 2.0, 0.6, 0.5), B((x0 + x1) / 200.0, -52.0, h / 100.0 - 0.8),
                   "PaintRed", 0.03))
    for (px, pz) in L3['pumps']:
        o.append(K.sweep("Header", [B(px / 100.0, pz / 100.0 - 3.0, 4.4), B(px / 100.0, z0 / 100.0 + 0.1, 4.4)], 0.3,
                         "Copper", seg=16))
    o.append(K.sweep("Manifold", [B(x0 / 100.0 + 0.5, z0 / 100.0 + 0.7, 5.2), B(x1 / 100.0 - 0.5, z0 / 100.0 + 0.7, 5.2)],
                     0.42, "Copper", seg=20))
    export("L3_Core_PumpHall", o, ao_dist=1.0)

    x0, z0, x1, z1, h, doors = rs['FreightLift']
    o = rect_shell("FreightLift", x0, z0, x1, z1, h, doors, split=2.0, ceiling=None, show_h=1600)
    cx, cz = (x0 + x1) / 200.0, (z0 + z1) / 200.0
    for dx in (-2.0, 2.0):   # shaft rails for the platform's guides
        if abs(dx) > (x1 - x0) / 200.0 - T:
            dx = math.copysign((x1 - x0) / 200.0 - T - 0.1, dx)
        o.append(K.sweep("Rail", [B(cx + dx, cz, 0.0), B(cx + dx, cz, 16.0)], 0.0, "SteelDark",
                         profile=[(-0.08, -0.12), (0.08, -0.12), (0.08, 0.12), (-0.08, 0.12)]))
    o.append(K.box("Header", ((x1 - x0) / 100.0, (z1 - z0) / 100.0, 0.4), B(cx, cz, 16.2), "SteelDark", 0.02))
    export("L3_Core_FreightShaft", o, ao_dist=0.8)

    # the elevator room: rotated (its door faces the shaft); level space, at the arrival height
    e = L3['elevator']
    ex = e['distance'] * math.sin(math.radians(e['angle'])) / 100.0
    ez = e['distance'] * math.cos(math.radians(e['angle'])) / 100.0
    w, d = e['size'][0] / 100.0, e['size'][1] / 100.0
    o = rect_shell("Elevator", -w * 50, -d * 50, w * 50, d * 50, e['height'], e['doors'], split=1.2,
                   wall_lo="PanelCream", wall_hi="BrassAged", floor="Grating", ceiling="PanelCream")
    o.append(K.box("Subfloor", (w, d, 0.1), B(0, 0, -0.25), "SteelDark", 0.0))
    root = bpy.data.objects.new("ElevatorRoot", None)
    bpy.context.scene.collection.objects.link(root)
    for ob in o:
        ob.parent = root
    root.location = B(ex, ez, ARRIVE)
    root.rotation_euler = (0, 0, math.radians(-e['angle']))
    export("L3_Core_ElevatorRoom", o, ao_dist=0.8)


# ==============================================================================================
# Props: cover, fixtures, alarm pieces
# ==============================================================================================

def props():
    # crate: walnut-dark planks in a steel frame, 1.2 x 1.1 x 1.2 m; pivot bottom centre
    o = [K.box("Body", (1.14, 1.14, 1.04), (0, 0, 0.53), "Planks", 0.01)]
    for sx in (-1, 1):
        for sy in (-1, 1):
            o.append(K.box("Post", (0.08, 0.08, 1.1), (sx * 0.57, sy * 0.57, 0.55), "SteelDark", 0.01))
    for z in (0.04, 1.06):
        for (sz, rot) in (((1.2, 0.08, 0.06), (0, 0, 0)), ((0.08, 1.2, 0.06), (0, 0, 0))):
            for s in (-1, 1):
                loc = (0, s * 0.57, z) if sz[0] > 1 else (s * 0.57, 0, z)
                o.append(K.box("Edge", sz, loc, "SteelDark", 0.01))
    o.append(K.box("Stencil", (0.5, 0.01, 0.18), (0, -0.575, 0.7), "PaintRed", 0.0))
    export("L3_Core_Crate", o, ground=True)

    # low crate: 1.6 x 0.8 x 0.8 (crouch cover)
    o = [K.box("Body", (1.56, 0.76, 0.76), (0, 0, 0.39), "PanelGreen", 0.03)]
    for x in (-0.6, 0.0, 0.6):
        o.append(K.box("Strap", (0.06, 0.8, 0.8), (x, 0, 0.4), "SteelDark", 0.01))
    o.append(K.box("Lid", (1.6, 0.8, 0.04), (0, 0, 0.78), "SteelDark", 0.01))
    export("L3_Core_CrateLow", o, ground=True)

    # ammo box: 0.9 x 0.6 x 0.6, green with brass latches and a red band
    o = [K.box("Body", (0.88, 0.58, 0.5), (0, 0, 0.25), "PanelGreen", 0.02)]
    o.append(K.box("Lid", (0.9, 0.6, 0.1), (0, 0, 0.55), "SteelDark", 0.02))
    o.append(K.box("Band", (0.9, 0.6, 0.06), (0, 0, 0.36), "PaintRed", 0.005))
    for x in (-0.3, 0.3):
        o.append(K.box("Latch", (0.08, 0.03, 0.1), (x, -0.31, 0.48), "BrassAged", 0.005))
    o.append(K.sweep("Handle", [(-0.2, 0, 0.6), (-0.2, 0, 0.66), (0.2, 0, 0.66), (0.2, 0, 0.6)], 0.012, "BrassAged", seg=6))
    export("L3_Core_AmmoBox", o, ground=True)

    # console: 1.6 wide, 1.2 tall, 0.7 deep, sloped desk with gauges and lamps; faces -Y (engine -Z)
    o = [K.prism("Body", [(-0.35, 0.0), (0.35, 0.0), (0.35, 1.2), (0.05, 1.2), (-0.35, 0.85)], 1.6, mat="PanelGreen",
                 rot=(0, 0, 90))]
    o.append(K.box("Kick", (1.62, 0.72, 0.12), (0, 0, 0.06), "SteelDark", 0.01))
    for i, x in enumerate((-0.5, 0.0, 0.5)):
        g = K.place(K.lathe("Gauge", [(0, 0), (0.11, 0), (0.12, 0.03), (0.11, 0.05), (0, 0.05)], mat="BrassAged", seg=20),
                    (x, -0.2, 1.03), (-45 - 90, 0, 0))
        o.append(g)
        o.append(K.place(K.disc("Dial", 0.095, mat="GaugeFace"), (x, -0.235, 1.0), (45, 0, 0)))
    for i, x in enumerate((-0.65, -0.25, 0.25, 0.65)):
        o.append(K.place(K.sphere("Lamp", 0.03, mat="LampGlow", seg=12, rings=6), (x, -0.33, 0.8)))
    export("L3_Core_Console", o, ground=True)

    # pipe stack: three 70 cm pipes on cradles, 3 m long, along X
    o = []
    for (y, z) in ((-0.36, 0.38), (0.36, 0.38), (0.0, 0.98)):
        o.append(K.sweep("Pipe", [(-1.5, y, z), (1.5, y, z)], 0.34, "Copper", seg=24))
        for x in (-1.45, 1.45):
            o.append(K.place(K.torus("Flange", 0.34, 0.05, mat="BrassAged", seg=24, rseg=6), (x, y, z), (0, 90, 0)))
    for x in (-1.0, 1.0):
        o.append(K.box("Cradle", (0.2, 1.5, 0.2), (x, 0, 0.1), "SteelDark", 0.02))
        o.append(K.box("Chock", (0.16, 0.12, 0.12), (x, 0, 0.58), "WoodBeam", 0.01))
    export("L3_Core_PipeStack", o, ground=True)

    # klaxon beacon: a wall bracket with a caged red dome; pivot at the wall, faces -Y
    o = [K.box("Plate", (0.34, 0.04, 0.34), (0, 0.0, 0), "SteelDark", 0.01)]
    o.append(K.sweep("Arm", [(0, -0.02, 0), (0, -0.25, 0)], 0.04, "SteelDark", seg=10))
    o.append(K.place(K.lathe("Base", [(0, 0), (0.14, 0), (0.14, 0.06), (0.12, 0.08), (0, 0.08)], mat="BrassAged", seg=20),
                     (0, -0.35, -0.12)))
    o.append(K.place(K.sphere("Dome", 0.12, mat="LampGlow", seg=20, rings=10, squash=1.3), (0, -0.35, 0.02)))
    for k in range(4):
        a = math.radians(45 + 90 * k)
        o.append(K.sweep("Cage", [(0.14 * math.cos(a), -0.35 + 0.14 * math.sin(a), -0.04),
                                  (0.13 * math.cos(a), -0.35 + 0.13 * math.sin(a), 0.12), (0, -0.35, 0.2)], 0.008,
                         "SteelDark", seg=5))
    export("L3_Core_Klaxon", o)

    # bulkhead lamp: wall/ceiling mounted cage lamp, pivot at the mount, glass faces -Y
    o = [K.place(K.lathe("Body", [(0, 0), (0.16, 0), (0.16, 0.06), (0.13, 0.1), (0, 0.1)], mat="BrassAged", seg=20),
                 (0, 0, 0), (90, 0, 0))]
    o.append(K.place(K.sphere("Glass", 0.11, mat="LampGlow", seg=16, rings=8, squash=0.7), (0, -0.12, 0)))
    for k in range(3):
        o.append(K.place(K.torus("Cage", 0.12, 0.008, mat="SteelDark", seg=16, rseg=4), (0, -0.12, 0), (0, 60 * k, 0)))
    export("L3_Core_Bulkhead", o)

    # floodlight: a hooded lamp on a yoke (for the ceiling spots); faces -Z blender (down)
    o = [K.place(K.lathe("Hood", [(0.0, 0.3), (0.12, 0.3), (0.28, 0.05), (0.3, 0.0), (0.26, 0.0)], mat="SteelDark", seg=24),
                 (0, 0, -0.3))]
    o.append(K.place(K.disc("Lens", 0.26, mat="LampGlow", seg=24), (0, 0, -0.31), (90, 0, 0)))
    o.append(K.sweep("Yoke", [(-0.32, 0, -0.15), (-0.32, 0, 0.2), (0.32, 0, 0.2), (0.32, 0, -0.15)], 0.025, "SteelDark", seg=6))
    o.append(K.sweep("Stem", [(0, 0, 0.2), (0, 0, 0.6)], 0.03, "SteelDark", seg=8))
    export("L3_Core_Floodlight", o)

    # electrode: a tesla terminal on a ceramic-insulated arm, out from a wall (-Y), copper ball
    o = [K.box("Mount", (0.5, 0.06, 0.6), (0, 0, 0), "SteelDark", 0.01)]
    o.append(K.sweep("Arm", [(0, -0.03, 0), (0, -1.2, 0.0)], 0.06, "BrassAged", seg=12))
    for k in range(6):
        o.append(K.place(K.torus("Insulator", 0.09, 0.035, mat="PanelCream", seg=16, rseg=6), (0, -0.25 - 0.1 * k, 0),
                         (90, 0, 0)))
    o.append(K.sphere("Ball", 0.22, loc=(0, -1.35, 0), mat="Copper", seg=24, rings=12))
    o.append(K.place(K.torus("Corona", 0.3, 0.04, mat="BrassBrushed", seg=32, rseg=6), (0, -1.35, 0), (90, 0, 0)))
    export("L3_Core_Electrode", o)

    # wall vent: a 2 x 1.4 m louvred frame on the wall (pivot bottom centre, faces -Y), and its cover
    o = []
    for x in (-1.05, 1.05):
        o.append(K.box("Side", (0.12, 0.25, 1.5), (x, 0.0, 0.75), "SteelDark", 0.02))
    for z in (0.0, 1.5):
        o.append(K.box("Head", (2.22, 0.25, 0.12), (0, 0.0, z), "SteelDark", 0.02))
    o.append(K.box("Back", (2.0, 0.04, 1.4), (0, 0.16, 0.75), "PaintBlack", 0.0))
    export("L3_Core_Vent", o)
    o = [K.box("Cover", (2.0, 0.06, 1.4), (0, -0.08, 0.75), "SteelDark", 0.01)]
    for k in range(7):
        o.append(K.box("Louvre", (1.9, 0.12, 0.04), (0, -0.14, 0.15 + 0.18 * k), "SteelDark", 0.005, rot=(30, 0, 0)))
    o.append(K.box("Stripe", (2.0, 0.02, 0.1), (0, -0.12, 1.38), "PaintRed", 0.0))
    export("L3_Core_VentCover", o)

    # heavy shutter for a 3 x 3.3 m doorway: corrugated steel, hazard bottom bar (pivot centre)
    o = []
    for k in range(11):
        z = -1.65 + 0.15 + 0.3 * k
        o.append(K.box("Slat", (3.0, 0.1, 0.28), (0, 0, z), "SteelDark", 0.03))
    o.append(K.box("Bar", (3.04, 0.16, 0.14), (0, 0, -1.6), "PaintRed", 0.02))
    for x in (-1.2, -0.4, 0.4, 1.2):
        o.append(K.box("Hazard", (0.3, 0.17, 0.141), (x, 0, -1.6), "PaintBlack", 0.0, rot=(0, 30, 0)))
    export("L3_Core_Shutter", o)


def main():
    K.reset()
    for fn in (shaft, pit, drum, helix_piece, helix_collider, rooms, props):
        K.clear()
        fn()
    print("L3_Core meshes done")


main()
