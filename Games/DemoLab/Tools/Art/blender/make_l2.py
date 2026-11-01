"""
make_l2.py -- the L2 Laboratory kit (interior): Content/Meshes/L2/.

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_l2.py

Grid: 4 m. Walls are 4 m wide, 5 m tall, 30 cm thick, their room side facing -Z (Blender
-Y); floors and ceilings 4 x 4 m. Moving parts are separate meshes (see assets.json).
"""

import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kitlib as K      # noqa: E402

KIT = "L2"
D = "Meshes/L2/"
H = 5.0


def wall_dressing(x0, x1, skip=None):
    """Wainscot (walnut panels and rails) to 1.1 m, a brass rail, riveted green plate above,
    a cornice. skip: (xa, xb, top) an opening the dressing stops at."""
    o = []
    def span(a, b, z0, z1, y0, y1, mat, bevel):
        if skip and a < skip[1] and b > skip[0] and z0 < skip[2]:
            if a < skip[0]:
                o.append(K.box_minmax("D", (a, y0, z0), (skip[0], y1, z1), mat, bevel))
            if b > skip[1]:
                o.append(K.box_minmax("D", (skip[1], y0, z0), (b, y1, z1), mat, bevel))
            if z1 > skip[2]:
                o.append(K.box_minmax("D", (max(a, skip[0]), y0, max(z0, skip[2])), (min(b, skip[1]), y1, z1), mat, bevel))
        else:
            o.append(K.box_minmax("D", (a, y0, z0), (b, y1, z1), mat, bevel))
    span(x0, x1, 0.0, 0.15, -0.06, 0.0, "Walnut", 0.01)               # skirting
    span(x0, x1, 0.15, 1.1, -0.03, 0.0, "Walnut", 0.005)              # wainscot board
    n = int(round((x1 - x0) / 1.0))
    for i in range(n):
        a, b = x0 + i * (x1 - x0) / n + 0.08, x0 + (i + 1) * (x1 - x0) / n - 0.08
        if skip and a < skip[1] and b > skip[0]:
            continue
        o.append(K.box_minmax("Panel", (a, -0.05, 0.28), (b, -0.03, 0.98), "Walnut", 0.012))
    span(x0, x1, 1.06, 1.16, -0.07, 0.0, "Walnut", 0.015)             # chair rail
    span(x0, x1, 1.16, 4.55, -0.012, 0.0, "PanelGreen", 0.004)          # plates
    span(x0, x1, 2.6, 2.7, -0.045, 0.0, "BrassAged", 0.01)            # picture rail
    span(x0, x1, 4.55, 4.75, -0.12, 0.0, "Walnut", 0.02)              # cornice
    span(x0, x1, 4.75, 5.0, -0.06, 0.0, "PanelCream", 0.01)
    return o


def walls():
    o = [K.box_minmax("Core", (-2, 0, 0), (2, 0.3, H), "PanelGreen", 0.0)] + wall_dressing(-2, 2)
    # A pilaster at each end (shared with the neighbour: half each).
    for x in (-2, 2):
        o.append(K.box_minmax("Pilaster", (x - 0.15, -0.16, 0), (x + 0.15, 0.0, H), "Walnut", 0.015))
        o.append(K.box_minmax("PilBase", (x - 0.18, -0.19, 0), (x + 0.18, 0.0, 0.25), "Walnut", 0.015))
        o.append(K.box_minmax("PilCap", (x - 0.18, -0.2, 4.4), (x + 0.18, 0.0, 4.6), "BrassAged", 0.015))
    K.export("Wall", o, D + "Wall.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre of the room-side face (z = 0); 4 m along X, 5 m tall, 30 cm thick behind it "
                        "(+Z); pilasters straddle the ends (chain every 400 cm)")
    K.clear()
    # Wall with a doorway, its door leaf and a steel security shutter.
    dw, dh = 1.6, 2.8
    o = [K.prism("Core", [(-2, 0), (2, 0), (2, H), (-2, H)], 0.3, (0, 0.15, 0), "PanelGreen", 0.0,
                 holes=[[(-dw / 2, 0.0001), (dw / 2, 0.0001), (dw / 2, dh), (-dw / 2, dh)]])]
    o += wall_dressing(-2, 2, skip=(-dw / 2 - 0.12, dw / 2 + 0.12, dh + 0.2))
    o.append(K.box_minmax("ArchL", (-dw / 2 - 0.15, -0.1, 0), (-dw / 2, 0.3, dh), "Walnut", 0.015))
    o.append(K.box_minmax("ArchR", (dw / 2, -0.1, 0), (dw / 2 + 0.15, 0.3, dh), "Walnut", 0.015))
    o.append(K.box_minmax("Head", (-dw / 2 - 0.2, -0.12, dh), (dw / 2 + 0.2, 0.3, dh + 0.25), "Walnut", 0.015))
    o.append(K.box_minmax("Sign", (-0.35, -0.135, dh + 0.06), (0.35, -0.12, dh + 0.19), "BrassAged", 0.004))
    o.append(K.box_minmax("ShutterBox", (-dw / 2 - 0.1, -0.35, dh + 0.25), (dw / 2 + 0.1, -0.12, dh + 0.55),
                          "SteelDark", 0.02))
    for x in (-2, 2):
        o.append(K.box_minmax("Pilaster", (x - 0.15, -0.16, 0), (x + 0.15, 0.0, H), "Walnut", 0.015))
    leaf = [K.box_minmax("Leaf", (0, 0.1, 0.0), (dw, 0.16, dh - 0.01), "Walnut", 0.01)]
    for (z0, z1) in ((0.2, 1.2), (1.45, 2.6)):
        leaf.append(K.box_minmax("Panel", (0.15, 0.085, z0), (dw - 0.15, 0.1, z1), "Walnut", 0.01))
    leaf.append(K.box_minmax("Window", (0.35, 0.08, 1.65), (dw - 0.35, 0.09, 2.4), "GlassFrosted", 0.0))
    leaf.append(K.sweep("Handle", [(dw - 0.15, 0.08, 1.0), (dw - 0.15, 0.02, 1.0), (dw - 0.3, 0.02, 1.0)], 0.015,
                        "BrassAged", seg=8))
    leaf.append(K.box_minmax("Kick", (0.05, 0.08, 0.02), (dw - 0.05, 0.1, 0.22), "BrassAged", 0.004))
    for l in leaf:
        l.location.x -= dw / 2
    shutter = []
    for i in range(int(dh / 0.2)):
        z = i * 0.2
        shutter.append(K.box_minmax("Slat", (-dw / 2 - 0.05, -0.3, z), (dw / 2 + 0.05, -0.26, z + 0.19), "SteelDark",
                                    0.01))
    shutter.append(K.box_minmax("Bar", (-dw / 2 - 0.05, -0.32, 0), (dw / 2 + 0.05, -0.25, 0.08), "PaintRed", 0.01))
    parts = [
        K.part("DoorLeaf", leaf, D + "WallDoor_Leaf.nmmesh", (-dw / 2, 0.13, 0),
               moves="swings about Y (up) at its hinge (the opening's left jamb), 0 to 100 degrees into the room "
                     "(-Z)", axis=(0, 0, 1), kit=KIT),
        K.part("Shutter", shutter, D + "WallDoor_Shutter.nmmesh", (0, -0.28, 0),
               moves="rolls up +Y 2.9 m into its box (open); down on the Alarm state", axis=(0, 0, 1), kit=KIT),
    ]
    K.export("WallDoor", o, D + "WallDoor.nmmesh", ground=True, kit=KIT, parts=parts,
             pivot_note="as Wall; the doorway is 1.6 x 2.8 m at its centre")


def floor_ceiling():
    o = [K.box_minmax("Slab", (-2, -2, -0.2), (2, 2, 0.0), "MarbleFloor", 0.0)]
    K.export("Floor", o, D + "Floor.nmmesh", ground=False, kit=KIT,
             pivot_note="top centre (y = 0); 4 x 4 m, 20 cm thick; chain every 400 cm")
    K.clear()
    o = [K.box_minmax("Slab", (-2, -2, 0.25), (2, 2, 0.4), "PanelCream", 0.0)]
    for x in (-2, 2):
        o.append(K.box_minmax("BeamX", (x - 0.15, -2, 0.0), (x + 0.15, 2, 0.25), "Walnut", 0.015))
    for y in (-2, 2):
        o.append(K.box_minmax("BeamY", (-2, y - 0.15, 0.0), (2, y + 0.15, 0.25), "Walnut", 0.015))
    o.append(K.box_minmax("BeamMid", (-2, -0.1, 0.05), (2, 0.1, 0.25), "SteelDark", 0.01))
    o += K.rivet_row("R", (-1.9, -0.11, 0.15), (1.9, -0.11, 0.15), 20, (90, 0, 0), "BrassAged", 0.012)
    o.append(K.lathe("Rose", [(0, 0.25), (0.25, 0.25), (0.2, 0.2), (0.06, 0.17), (0, 0.17)], mat="BrassAged", seg=24))
    K.export("Ceiling", o, D + "Ceiling.nmmesh", ground=False, kit=KIT,
             pivot_note="bottom centre of the beams (y = 0): place at 500 cm; 4 x 4 m; a ceiling rose at the "
                        "centre for PendantLamp")
    K.clear()
    o = [K.sweep("Rod", [(0, 0, 0), (0, 0, -1.2)], 0.012, "BrassAged", seg=8)]
    o.append(K.lathe("Shade", [(0.02, -1.2), (0.12, -1.22), (0.32, -1.42), (0.34, -1.46), (0.0, -1.46)],
                     mat="Copper", seg=32))
    o.append(K.sphere("Bulb", 0.07, (0, 0, -1.47), "LampGlow", seg=16, rings=8))
    K.export("PendantLamp", o, D + "PendantLamp.nmmesh", ground=False, kit=KIT, ao=False,
             pivot_note="top of the rod (hang it from the ceiling rose); bulb at y = -147 cm (a point light there)")


def dome_segment():
    """One eighth (45 degrees) of the atrium's 12 m glass dome: glass panes, brass ribs at its
    start edge and on the rings. Eight, turned 45 degrees each about Y, make the dome."""
    R, Hd = 6.0, 4.0
    o = []
    import bmesh
    from mathutils import Vector
    bm = bmesh.new()
    na, nz = 6, 10
    verts = []
    for j in range(nz + 1):
        t = j / nz * (math.pi / 2) * 0.96
        r, z = R * math.cos(t), Hd * math.sin(t)
        row = []
        for i in range(na + 1):
            a = math.radians(45) * i / na
            row.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z)))
        verts.append(row)
    for j in range(nz):
        for i in range(na):
            bm.faces.new([verts[j][i], verts[j][i + 1], verts[j + 1][i + 1], verts[j + 1][i]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    o.append(K.obj_from_bm("Glass", bm, "GlassClear", sharp=math.radians(89)))
    # Ribs: the meridian at the start edge, and every ring.
    mer = [(R * math.cos(t) * 1.002, 0, Hd * math.sin(t) * 1.002)
           for t in [k / 20 * (math.pi / 2) * 0.96 for k in range(21)]]
    o.append(K.sweep("Rib", mer, 0.06, "BrassAged", seg=8))
    mid = [(R * math.cos(t) * math.cos(math.radians(22.5)), R * math.cos(t) * math.sin(math.radians(22.5)),
            Hd * math.sin(t)) for t in [k / 20 * (math.pi / 2) * 0.96 for k in range(21)]]
    o.append(K.sweep("Mullion", mid, 0.025, "BrassAged", seg=6))
    for j in (0, 3, 6, 9):
        t = j / nz * (math.pi / 2) * 0.96
        r, z = R * math.cos(t), Hd * math.sin(t)
        arc = [(r * math.cos(math.radians(a)), r * math.sin(math.radians(a)), z) for a in range(0, 46, 5)]
        o.append(K.sweep("Ring", arc, 0.05 if j == 0 else 0.03, "BrassAged", seg=8))
    o.append(K.place(K.torus("Base", R, 0.15, mat="BrassAged", seg=96, rseg=10, arc=45), (0, 0, 0.0)))
    K.export("DomeSegment", o, D + "DomeSegment.nmmesh", ground=False, kit=KIT, ao_dist=1.0,
             pivot_note="the dome's centre at its base ring; 6 m radius, 4 m high; one 45-degree slice starting on "
                        "+X turning toward +Z; place 8 copies turned 0, 45, ... 315 degrees about Y. The crown (top "
                        "4%) is open: the roof hatch goes there")


def orrery():
    """The atrium's brass orrery, 6.5 m tall: an octagonal walnut plinth with a brass
    compass rose, a fluted brass column on four scrolled struts, a riveted yoke with bearing
    housings, a gimbal of three graduated hoops (each turns on its own pins), a glowing sun
    in a brass cage on a spindle, and four planets on jointed arms driven by a spoked crown
    gear and a pinion."""
    import bpy
    from mathutils import Vector
    C = 4.0     # the gimbal's centre height
    base = []
    plinth = K.lathe("Plinth", [(0, 0), (2.0, 0), (2.0, 0.08), (1.96, 0.1), (1.96, 0.22), (2.0, 0.24), (2.0, 0.28),
                                (1.9, 0.33), (1.88, 0.35), (0, 0.35)], mat="Walnut", seg=8, sharp=math.radians(30))
    plinth.rotation_euler = (0, 0, math.radians(22.5))
    base.append(plinth)
    kick = K.lathe("Kick", [(2.005, 0.015), (2.012, 0.02), (2.012, 0.075), (2.005, 0.08)], mat="BrassAged", seg=8)
    kick.rotation_euler = (0, 0, math.radians(22.5))
    base.append(kick)
    base.append(K.lathe("Inlay", [(0, 0.35), (1.62, 0.35), (1.62, 0.358), (0, 0.358)], mat="MarbleFloor", seg=48))
    base.append(K.place(K.torus("InlayRim", 1.63, 0.015, mat="BrassBrushed", seg=96, rseg=6), (0, 0, 0.358)))
    for k in range(8):                                     # compass rose: long and short points
        L = 1.55 if k % 2 == 0 else 1.05
        star = K.prism("Rose", [(-0.09, 0), (0, L), (0.09, 0), (0, -0.05)], 0.008, (0, 0, 0), "BrassBrushed",
                       bevel=0.0)
        star.rotation_euler = (math.radians(90), 0, math.radians(45 * k))
        star.location = (0, 0, 0.362)
        base.append(star)
    base += K.rivets_on_lathe(lambda z: 1.99, [0.16], 32, head=0.014, mat="BrassAged")
    # Fluted column.
    col = K.lathe("Column", [(0, 0.35), (1.0, 0.35), (1.0, 0.45), (0.92, 0.5), (0.7, 0.58), (0.62, 0.66),
                             (0.66, 0.7), (0.66, 0.76), (0.5, 0.84), (0.42, 0.95), (0.36, 2.2), (0.42, 2.25),
                             (0.42, 2.32), (0.55, 2.4), (0.6, 2.48), (0.6, 2.55), (0.3, 2.6), (0, 2.6)],
                  mat="BrassAged", seg=96, sharp=math.radians(30))
    for v in col.data.vertices:
        if 0.97 < v.co.z < 2.18:
            a = math.atan2(v.co.y, v.co.x)
            t = min(1.0, (v.co.z - 0.97) / 0.06, (2.18 - v.co.z) / 0.06)
            flute = max(0.0, math.cos(16 * a)) ** 2
            s = 1.0 - 0.055 * flute * t
            v.co.x *= s
            v.co.y *= s
    base.append(col)
    for z, r in ((0.95, 0.43), (2.2, 0.37), (0.66, 0.665)):
        base.append(K.place(K.torus("Astragal", r, 0.025, mat="BrassBrushed", seg=96, rseg=8), (0, 0, z)))
    # Four scrolled struts.
    for k in range(4):
        a = math.radians(90 * k + 45)
        ca, sa = math.cos(a), math.sin(a)
        pts = [(1.55 * ca, 1.55 * sa, 0.37), (1.5 * ca, 1.5 * sa, 0.7), (1.05 * ca, 1.05 * sa, 1.25),
               (0.6 * ca, 0.6 * sa, 1.55), (0.38 * ca, 0.38 * sa, 1.75)]
        base.append(K.sweep("Strut", pts, 0.05, "BrassBrushed", seg=14))
        base.append(K.sphere("Finial", 0.08, (1.55 * ca, 1.55 * sa, 0.43), "BrassAged", seg=16, rings=8))
    # Yoke: a riveted channel carrying the outer ring's pins in bearing housings.
    yoke = [(-3.15, 0, C - 0.2), (-3.15, 0, 2.5), (-2.8, 0, 2.3), (2.8, 0, 2.3), (3.15, 0, 2.5), (3.15, 0, C - 0.2)]
    base.append(K.strap("Yoke", yoke, 0.2, 0.09, "BrassBrushed"))
    for y in (-0.046, 0.046):
        base += K.rivets_along([(p[0], y, p[2]) for p in yoke[1:-1]], 0.25, (0, math.copysign(1, y), 0), head=0.012)
    for x in (-3.15, 3.15):
        hb = K.lathe("Bearing", [(0, -0.14), (0.15, -0.14), (0.17, -0.1), (0.17, 0.1), (0.15, 0.14), (0, 0.14)],
                     mat="BrassAged", seg=32)
        hb.rotation_euler = (0, math.radians(90), 0)
        hb.location = (x, 0, C)
        base.append(hb)
        base += K.bolt_circle("BearingBolt", 0.12, 6, (x + math.copysign(0.142, x), 0, C),
                              (0, math.copysign(90, x), 0), head=0.016)
        base.append(K.strap("Cheek", [(x, 0, C - 0.25), (x, 0, C - 0.12)], 0.22, 0.12, "BrassBrushed"))
        sg = math.copysign(1, x)
        arc = [(x - sg * 0.55 + sg * 0.55 * math.sin(t * math.pi / 8), 0, 2.85 - 0.55 * math.cos(t * math.pi / 8))
               for t in range(5)]
        base.append(K.strap("Scroll", arc, 0.07, 0.04, "BrassAged"))
    base.append(K.lathe("Saddle", [(0, 2.2), (0.48, 2.2), (0.5, 2.25), (0.5, 2.38), (0.46, 2.42), (0, 2.42)],
                        mat="BrassBrushed", seg=48))
    # Spindle, sun and its cage.
    base.append(K.cyl("Spindle", 0.07, 0.95, (0, 0, 3.05), "SteelBrushed", seg=24))
    base.append(K.sphere("Sun", 0.55, (0, 0, C), "LampGlow", seg=48, rings=24))
    cage = []
    for k in range(3):
        cage.append(K.place(K.torus("Cage", 0.64, 0.014, mat="BrassBrushed", seg=64, rseg=6), (0, 0, C),
                            (90, 0, 60 * k)))
    cage.append(K.place(K.torus("Equator", 0.64, 0.02, mat="BrassAged", seg=64, rseg=8), (0, 0, C)))
    base += cage
    base.append(K.lathe("SunCup", [(0, C - 0.66), (0.08, C - 0.66), (0.2, C - 0.6), (0.24, C - 0.52),
                                   (0.22, C - 0.5), (0.0, C - 0.56)], mat="BrassAged", seg=32))
    # Pinion meshing with the planets' crown gear, on a bracket off the column top.
    pin = K.gear("Pinion", 0.2, 14, 0.08, "BrassAged", bore=0.03)
    pin.rotation_euler = (math.radians(90), 0, 0)
    pin.location = (0.79, 0, C - 0.75)
    base.append(pin)
    base.append(K.cyl("PinionShaft", 0.03, 0.85, (0.79, 0, C - 1.15), "SteelBrushed", seg=12))
    base.append(K.strap("PinionArm", [(0.3, 0, 2.52), (0.79, 0, 2.52)], 0.08, 0.06, "BrassBrushed"))
    base.append(K.cyl("PinionBoss", 0.06, 0.1, (0.79, 0, 2.52), "BrassAged", seg=16))
    # Gantry ladder up the plinth to the column (the player climbs it), on stand-offs.
    for z in [0.45 + 0.3 * i for i in range(9)]:
        base.append(K.sweep("Rung", [(1.05, -0.24, z), (1.05, 0.24, z)], 0.018, "SteelDark", seg=8))
    for y in (-0.26, 0.26):
        base.append(K.sweep("Stile", [(1.05, y, 0.35), (1.05, y, 3.1), (0.95, y, 3.35)], 0.028, "SteelDark", seg=8))
        for z in (1.0, 2.0, 2.9):
            base.append(K.sweep("Standoff", [(1.05, y, z), (0.5 if z < 2.5 else 0.62, y * 0.5, z)], 0.02,
                                "SteelDark", seg=6))
    rings = []
    for name, R, axis_rot in (("RingOuter", 3.0, (90, 0, 0)), ("RingMiddle", 2.5, (90, 0, 90)),
                              ("RingInner", 2.0, (55, 0, 30))):
        hoop = K.lathe(name, [(R - 0.035, -0.08), (R - 0.01, -0.085), (R + 0.005, -0.085), (R + 0.014, -0.075),
                              (R + 0.014, -0.058), (R + 0.004, -0.052), (R + 0.004, 0.052), (R + 0.014, 0.058),
                              (R + 0.014, 0.075), (R + 0.005, 0.085), (R - 0.01, 0.085), (R - 0.035, 0.08),
                              (R - 0.035, -0.08)], mat="BrassBrushed", seg=160, sharp=math.radians(40))
        r = [hoop]
        r.append(K.lathe(name + "Inlay", [(R + 0.005, -0.022), (R + 0.007, -0.02), (R + 0.007, 0.02),
                                          (R + 0.005, 0.022)], mat="Walnut", seg=160))
        for k in range(72):
            a = 2 * math.pi * k / 72
            major = k % 6 == 0
            t = K.box("Tick", (0.006, 0.01, 0.045 if major else 0.022), (0, 0, 0), "BrassAged", 0.0)
            t.location = ((R + 0.007) * math.cos(a), (R + 0.007) * math.sin(a), 0.026 if major else 0.037)
            t.rotation_euler = (0, 0, a)
            r.append(t)
        for k in range(4):                                  # rivet plates where the hoop was joined
            a = 2 * math.pi * k / 4 + math.pi / 4
            pl = K.box("Splice", (0.012, 0.16, 0.15), ((R + 0.012) * math.cos(a), (R + 0.012) * math.sin(a), 0),
                       "BrassAged", 0.004, rot=(0, 0, math.degrees(a)))
            r.append(pl)
            for dz in (-0.045, 0.045):
                for dt in (-0.05, 0.05):
                    rv = K.rivet(0.01, "BrassAged")
                    rv.location = ((R + 0.018) * math.cos(a) - dt * math.sin(a), (R + 0.018) * math.sin(a) + dt * math.cos(a), dz)
                    rv.rotation_euler = (0, math.radians(90), a)
                    r.append(rv)
        for s in (-1, 1):
            r.append(K.place(K.cyl("Pin", 0.055, 0.25, mat="SteelBrushed", seg=16), (s * (R + 0.12), 0, 0), (0, 90, 0)))
            r.append(K.place(K.cyl("Boss", 0.09, 0.06, mat="BrassAged", seg=24), (s * (R + 0.03), 0, 0), (0, 90, 0)))
        holder = bpy.data.objects.new(name + "Holder", None)
        bpy.context.scene.collection.objects.link(holder)
        holder.location = (0, 0, C)
        holder.rotation_euler = [math.radians(a) for a in axis_rot]
        for ob in r:
            if ob.parent is None:
                ob.parent = holder
        rings.append((name, r, axis_rot))
    planets = []
    crown = K.gear("Crown", 0.55, 44, 0.06, "BrassBrushed", spokes=5, hub_r=0.14)
    crown.rotation_euler = (math.radians(90), 0, 0)
    crown.location = (0, 0, C - 0.75)
    planets.append(crown)
    planets.append(K.lathe("Collar", [(0.075, C - 0.82), (0.2, C - 0.82), (0.22, C - 0.78), (0.22, C - 0.5),
                                      (0.18, C - 0.46), (0.075, C - 0.46)], mat="BrassAged", seg=32))
    specs = ((1.05, 0.17, "Copper", 0), (1.4, 0.24, "Marble", 1), (1.72, 0.15, "Enamel", 2), (0.75, 0.1, "SteelBrushed", 3))
    for rad, size, mat, k in specs:
        a = math.radians(95 * k + 10)
        ca, sa = math.cos(a), math.sin(a)
        zarm = C - 0.55 - 0.06 * k
        arm = [(0.2 * ca, 0.2 * sa, zarm), (rad * ca, rad * sa, zarm), (rad * ca, rad * sa, C - size - 0.03)]
        planets.append(K.strap("Arm", arm[:2], 0.05, 0.025, "BrassAged"))
        planets.append(K.sweep("Post", arm[1:], 0.014, "BrassBrushed", seg=8))
        planets.append(K.sphere("Knuckle", 0.03, arm[1], "BrassAged", seg=12, rings=6))
        planets.append(K.sphere("Planet", size, (rad * ca, rad * sa, C), mat, seg=40, rings=20))
        if k == 1:
            sat = K.band_ring("Rings", size * 1.65, size * 0.7, 0.008, "BrassBrushed", seg=64, lip=0.002, groove=False)
            sat.location = (rad * ca, rad * sa, C)
            sat.rotation_euler = (math.radians(24), math.radians(10), 0)
            planets.append(sat)
        if k == 0:
            planets.append(K.sweep("MoonArm", [(rad * ca, rad * sa, C + size), (rad * ca, rad * sa, C + size + 0.06),
                                               (rad * ca + 0.3 * sa, rad * sa - 0.3 * ca, C + size + 0.06)], 0.008,
                                   "BrassBrushed", seg=6))
            planets.append(K.sphere("Moon", 0.05, (rad * ca + 0.3 * sa, rad * sa - 0.3 * ca, C + size + 0.06),
                                    "Marble", seg=16, rings=8))
    parts = []
    for (name, r, axis_rot) in rings:
        ax = {"RingOuter": (1, 0, 0), "RingMiddle": (0, 1, 0), "RingInner": (0, 0, 1)}[name]
        how = {"RingOuter": "upright in the XY plane; turns about X (its pins)",
               "RingMiddle": "upright in the ZY plane; turns about Z (its pins) inside Outer",
               "RingInner": "tilted 55 degrees; spins in its own plane inside Middle"}[name]
        parts.append(K.part(name, r, D + f"Orrery_{name}.nmmesh", (0, 0, C), moves=how, axis=ax, kit=KIT))
    parts.append(K.part("Planets", planets, D + "Orrery_Planets.nmmesh", (0, 0, C),
                        moves="spins about Y (up) round the sun, slowly (the crown gear under it turns with it; "
                              "the pinion on the column is static)", axis=(0, 0, 1), kit=KIT))
    K.export("Orrery", base, D + "Orrery.nmmesh", ground=True, kit=KIT, parts=parts, ao_dist=1.0,
             pivot_note="bottom centre of the plinth; the gimbal's centre (every part's pivot) is 400 cm up; the "
                        "Sun is LampGlow (put a point light at its centre); a ladder on the +X side",
             notes="The ring meshes are exported in their rest pose (already turned); spin each about its pivot. "
                   "For a true gimbal parent Inner to Middle to Outer")


def workbench():
    o = [K.box_minmax("Top", (-1.0, -0.45, 0.88), (1.0, 0.45, 0.95), "Walnut", 0.012)]
    for x in (-0.92, 0.92):
        for y in (-0.38, 0.38):
            o.append(K.box_minmax("Leg", (x - 0.04, y - 0.04, 0), (x + 0.04, y + 0.04, 0.88), "SteelDark", 0.008))
    for x in (-0.92, 0.92):
        o.append(K.box_minmax("Rail", (x - 0.03, -0.38, 0.15), (x + 0.03, 0.38, 0.2), "SteelDark", 0.006))
    o.append(K.box_minmax("Shelf", (-0.95, -0.4, 0.18), (0.95, 0.4, 0.21), "Planks", 0.006))
    o.append(K.box_minmax("Drawers", (0.2, -0.43, 0.6), (0.9, 0.42, 0.88), "Walnut", 0.01))
    for z in (0.67, 0.81):
        o.append(K.sweep("Pull", [(0.45, -0.45, z), (0.65, -0.45, z)], 0.008, "BrassAged", seg=6))
    # A vice on the left corner, a tool rack and a lamp.
    o.append(K.box_minmax("ViceBody", (-0.95, -0.5, 0.95), (-0.7, -0.3, 1.08), "PaintRed", 0.01))
    o.append(K.box_minmax("ViceJaw", (-0.95, -0.55, 0.95), (-0.7, -0.5, 1.1), "SteelDark", 0.005))
    o.append(K.place(K.cyl("ViceScrew", 0.012, 0.3, mat="SteelDark", seg=8), (-0.825, -0.65, 1.02), (90, 0, 0)))
    o.append(K.box_minmax("Rack", (-0.95, 0.38, 0.95), (0.95, 0.45, 1.6), "Planks", 0.006))
    for i, x in enumerate([-0.7, -0.45, -0.2, 0.1, 0.4]):
        o.append(K.box_minmax("Tool", (x - 0.015, 0.35, 1.15 + 0.05 * (i % 2)), (x + 0.015, 0.37, 1.5), "SteelDark",
                              0.004))
    o.append(K.sweep("LampArm", [(0.8, 0.3, 0.95), (0.8, 0.3, 1.45), (0.6, 0.1, 1.6)], 0.012, "BrassAged", seg=8))
    o.append(K.lathe("LampShade", [(0.01, 0), (0.05, -0.02), (0.11, -0.12), (0.0, -0.12)], mat="Copper", seg=20))
    o[-1].location = (0.6, 0.1, 1.62)
    K.export("Workbench", o, D + "Workbench.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre; 2 x 0.9 m, top at 95 cm; the user's side faces -Z; a benchtop "
                        "sample stands at (x, 95, 0) cm")


def pedestal():
    """A museum exhibit plinth: a stepped walnut base on a brass kick plate, a body with
    raised and fielded panels and brass corner guards, a moulded cornice, a marble slab and a
    velvet display mat, and an angled brass label plate on a bracket at the front."""
    o = [K.box_minmax("KickPlate", (-0.33, -0.33, 0.0), (0.33, 0.33, 0.025), "BrassAged", 0.004)]
    o.append(K.box_minmax("Base", (-0.32, -0.32, 0.025), (0.32, 0.32, 0.085), "Walnut", 0.012))
    o.append(K.box_minmax("BaseStep", (-0.29, -0.29, 0.085), (0.29, 0.29, 0.11), "Walnut", 0.008))
    o.append(K.box_minmax("Body", (-0.25, -0.25, 0.11), (0.25, 0.25, 0.8), "Walnut", 0.006))
    for (ax, s) in (("y", -1), ("y", 1), ("x", -1), ("x", 1)):
        if ax == "y":
            o.append(K.box_minmax("Field", (-0.18, s * 0.25 - 0.012, 0.19), (0.18, s * 0.25 + 0.012, 0.72), "Walnut", 0.01))
            o.append(K.box_minmax("Bead", (-0.205, s * 0.25 - 0.006, 0.165), (0.205, s * 0.25 + 0.006, 0.745),
                                  "Walnut", 0.003))
        else:
            o.append(K.box_minmax("Field", (s * 0.25 - 0.012, -0.18, 0.19), (s * 0.25 + 0.012, 0.18, 0.72), "Walnut", 0.01))
            o.append(K.box_minmax("Bead", (s * 0.25 - 0.006, -0.205, 0.165), (s * 0.25 + 0.006, 0.205, 0.745),
                                  "Walnut", 0.003))
    for sx in (-1, 1):
        for sy in (-1, 1):
            o.append(K.box("Guard", (0.035, 0.035, 0.67), (sx * 0.252, sy * 0.252, 0.455), "BrassBrushed", 0.006))
            o.append(K.sphere("GuardCap", 0.014, (sx * 0.27, sy * 0.27, 0.8), "BrassAged", 12, 6))
    o.append(K.box_minmax("CorniceCove", (-0.27, -0.27, 0.8), (0.27, 0.27, 0.83), "Walnut", 0.01))
    o.append(K.box_minmax("Cornice", (-0.305, -0.305, 0.83), (0.305, 0.305, 0.865), "Walnut", 0.012))
    o.append(K.box_minmax("Slab", (-0.295, -0.295, 0.865), (0.295, 0.295, 0.9), "Marble", 0.008))
    o.append(K.box_minmax("Mat", (-0.21, -0.21, 0.9), (0.21, 0.21, 0.92), "Velvet", 0.008))
    o.append(K.box_minmax("MatPiping", (-0.215, -0.215, 0.9), (0.215, 0.215, 0.906), "BrassAged", 0.002))
    plate = [K.box("Plate", (0.2, 0.008, 0.08), (0, 0, 0), "BrassBrushed", 0.003),
             K.box("Engraving", (0.16, 0.004, 0.045), (0, -0.005, 0.0), "PaintBlack", 0.0)]
    for x in (-0.085, 0.085):
        r = K.rivet(0.005, "BrassAged")
        r.location = (x, -0.004, 0.028)
        r.rotation_euler = (math.radians(90), 0, 0)
        plate.append(r)
    K.group("PlateG", plate, (0, -0.33, 0.78), (-35, 0, 0))
    o += plate
    o.append(K.box_minmax("PlateBracket", (-0.02, -0.33, 0.745), (0.02, -0.25, 0.765), "BrassAged", 0.004))
    K.export("SamplePedestal", o, D + "SamplePedestal.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre; a sample sits on its velvet mat at y = 92 cm; an angled brass label plate "
                        "on its -Z face at 78 cm")


def samples():
    """One object per shading model, each about 30 cm, pivot at its base."""
    def save(name, obs, notes):
        K.export(name, obs, D + f"{name}.nmmesh", ground=True, kit=KIT, pivot_note="bottom centre (sits on a "
                 "SamplePedestal at y = 92 cm or a bench)", notes=notes)
        K.clear()
    save("Sample_BrushedBrass", [K.lathe("Vase", [(0, 0), (0.08, 0), (0.09, 0.02), (0.06, 0.05), (0.13, 0.16),
                                                    (0.12, 0.24), (0.06, 0.3), (0.07, 0.34), (0.0, 0.34)],
                                          mat="BrassBrushed", seg=64)], "Anisotropic: brushed brass, grain round it")
    w = [K.box_minmax("Box", (-0.16, -0.11, 0), (0.16, 0.11, 0.14), "Walnut", 0.01)]
    w.append(K.box_minmax("Lid", (-0.165, -0.115, 0.14), (0.165, 0.115, 0.18), "Walnut", 0.012))
    w.append(K.box_minmax("Hasp", (-0.02, -0.12, 0.1), (0.02, -0.112, 0.16), "BrassAged", 0.002))
    save("Sample_Walnut", w, "Clear Coat: lacquered walnut")
    e = [K.sphere("Body", 0.13, (0, 0, 0.14), "Enamel", seg=48, rings=24, squash=0.85)]
    e.append(K.lathe("Foot", [(0, 0), (0.08, 0), (0.06, 0.04), (0.0, 0.04)], mat="BrassAged", seg=32))
    e.append(K.sweep("Spout", [(0.1, 0, 0.14), (0.2, 0, 0.2), (0.25, 0, 0.27)], 0.022, "Enamel", seg=12))
    e.append(K.place(K.torus("Handle", 0.07, 0.014, mat="Enamel", seg=24, rseg=8), (-0.14, 0, 0.17), (90, 0, 0)))
    e.append(K.lathe("Knob", [(0, 0.24), (0.04, 0.24), (0.02, 0.28), (0, 0.29)], mat="BrassAged", seg=16))
    save("Sample_Enamel", e, "Clear Coat: car-paint enamel (a kettle), Tint via BaseColor")
    import bmesh
    from mathutils import Vector
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges, cuts=6, use_grid_fill=True)
    for v in bm.verts:
        x, y, z = v.co
        v.co = Vector((x * 0.36 * (1 - 0.25 * z * z), y * 0.36 * (1 - 0.25 * z * z), z * 0.12 * (1 - 0.6 * (x * x + y * y))))
        v.co.z += 0.07
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    cushion = K.obj_from_bm("Cushion", bm, "Velvet", sharp=math.radians(70))
    tassels = [K.sphere("Tassel", 0.02, (sx * 0.17, sy * 0.17, 0.07), "BrassAged", 12, 6) for sx in (-1, 1)
               for sy in (-1, 1)]
    save("Sample_Velvet", [cushion] + tassels, "Cloth: crushed velvet cushion, pink sheen")
    b = [K.box_minmax("Book", (-0.12, -0.16, 0), (0.12, 0.16, 0.07), "Leather", 0.012)]
    b.append(K.box_minmax("Pages", (-0.115, -0.155, 0.006), (0.125, 0.155, 0.064), "PanelCream", 0.003))
    for y in (-0.08, 0.08):
        b.append(K.box_minmax("Strap", (-0.125, y - 0.015, -0.002), (0.125, y + 0.015, 0.072), "Leather", 0.003))
        b.append(K.box_minmax("Buckle", (0.12, y - 0.02, 0.025), (0.13, y + 0.02, 0.05), "BrassAged", 0.002))
    save("Sample_Leather", b, "Cloth: a leather-bound journal")
    m = [K.box_minmax("Plinth", (-0.09, -0.09, 0), (0.09, 0.09, 0.05), "Marble", 0.008)]
    m.append(K.lathe("Column", [(0, 0.05), (0.07, 0.05), (0.06, 0.07), (0.055, 0.25), (0.07, 0.27), (0.0, 0.27)],
                     mat="Marble", seg=24))
    m.append(K.sphere("Orb", 0.09, (0, 0, 0.36), "Marble", seg=32, rings=16))
    save("Sample_Marble", m, "Subsurface: white marble (an orb on a column)")
    c = [K.lathe("Dish", [(0, 0), (0.14, 0), (0.15, 0.02), (0.13, 0.025), (0, 0.025)], mat="BrassAged", seg=32)]
    for (x, y, h, r) in ((0, 0, 0.25, 0.035), (0.07, 0.04, 0.17, 0.03), (-0.06, 0.05, 0.12, 0.032)):
        c.append(K.lathe("Candle", [(0, 0.02), (r, 0.02), (r, h - 0.01), (r * 0.8, h), (0.005, h - 0.005), (0, h - 0.005)],
                         mat="Wax", seg=24))
        c[-1].location = (x, y, 0)
        c.append(K.sweep("Wick", [(x, y, h - 0.006), (x, y, h + 0.012)], 0.002, "PaintBlack", seg=4))
    save("Sample_Wax", c, "Subsurface: wax candles (light through them)")
    f = [K.box_minmax("Frame", (-0.2, -0.03, 0), (0.2, 0.03, 0.04), "BrassAged", 0.006)]
    for x in (-0.19, 0.19):
        f.append(K.box_minmax("Side", (x - 0.012, -0.012, 0.04), (x + 0.012, 0.012, 0.5), "BrassAged", 0.004))
    f.append(K.box_minmax("TopBar", (-0.2, -0.012, 0.5), (0.2, 0.012, 0.525), "BrassAged", 0.004))
    leaf = K.uv_box("Leaf", (0.36, 0.006, 0.46), (0, 0, 0.27), "LeafPanel",
                    faces={"-y": (0, 0, 1, 1), "+y": (0, 0, 1, 1), "*": (0, 0, 0.01, 0.01)})
    f.append(leaf)
    f.append(K.box_minmax("Lamp", (-0.18, 0.03, 0.04), (0.18, 0.06, 0.08), "LampGlow", 0.005))
    save("Sample_LeafPanel", f, "Subsurface, two-sided: a pressed fern between glass, lit from behind")
    j = [K.lathe("BellJar", [(0.0, 0.0), (0.12, 0.0), (0.12, 0.22), (0.1, 0.28), (0.04, 0.31), (0.03, 0.34),
                             (0.0, 0.345)], mat="GlassFrosted", seg=48)]
    j.append(K.lathe("JarBase", [(0, 0), (0.14, 0), (0.14, 0.02), (0, 0.02)], mat="Walnut", seg=32))
    save("Sample_FrostedGlass", j, "Translucent Lit: a frosted bell jar")
    rnd = random.Random(4)
    puff = []
    # A plume: small dense puffs at the nozzle swelling and leaning as they rise, overlapping (the
    # Steam material's world-space clouds eat them into wisps, so no ball keeps its outline)
    x = y = 0.0
    for k in range(11):
        t = k / 10.0
        x += rnd.uniform(-0.012, 0.03)
        y += rnd.uniform(-0.02, 0.02)
        r = 0.035 + 0.11 * t ** 1.3 + 0.015 * rnd.random()
        p = K.sphere("Puff", r, (x, y, 0.09 + 0.62 * t ** 1.15), "Steam", seg=20, rings=10,
                     squash=1.15 + 0.3 * rnd.random())
        puff.append(p)
    puff.append(K.lathe("Vent", [(0, 0), (0.07, 0), (0.07, 0.03), (0.035, 0.05), (0.025, 0.08), (0.02, 0.08), (0, 0.075)],
                        mat="SteelDark", seg=24))
    puff.append(K.place(K.torus("Collar", 0.03, 0.008, mat="BrassAged", seg=24, rseg=6), (0, 0, 0.065)))
    save("Sample_Steam", puff, "Volume (Translucent): a plume of steam over a nozzle; the Steam material's clouds "
                               "break it into wisps")


def breaker_panel():
    o = [K.box_minmax("Cabinet", (-0.5, 0.0, 0.0), (0.5, 0.35, 1.8), "PanelGreen", 0.02)]
    o.append(K.box_minmax("Face", (-0.45, -0.02, 0.1), (0.45, 0.0, 1.7), "SteelDark", 0.005))
    o.append(K.box_minmax("Plate", (-0.3, -0.035, 1.45), (0.3, -0.02, 1.6), "BrassAged", 0.004))
    for i, x in enumerate((-0.25, 0.0, 0.25)):
        o.append(K.cyl("GaugeBody", 0.1, 0.06, (x, -0.04, 1.25), "BrassAged", seg=32, rot=(90, 0, 0)))
        o.append(K.disc("GaugeFace", 0.085, (x, -0.0705, 1.25), "GaugeFace", seg=32))
        o.append(K.box("Needle", (0.006, 0.004, 0.07), (x - 0.02, -0.074, 1.27), "PaintRed", 0.0, rot=(0, -40 + i * 35, 0)))
    for x in (-0.3, -0.15, 0.15, 0.3):
        o.append(K.box_minmax("Fuse", (x - 0.04, -0.06, 0.45), (x + 0.04, -0.02, 0.7), "PaintBlack", 0.006))
        o.append(K.cyl("Lamp", 0.018, 0.02, (x, -0.035, 0.78), "LampGlow", seg=12, rot=(90, 0, 0)))
    o.append(K.box_minmax("LeverMount", (-0.08, -0.12, 0.85), (0.08, -0.02, 1.0), "SteelDark", 0.01))
    lever = [K.box_minmax("Blade", (-0.025, -0.02, 0.0), (0.025, 0.02, 0.38), "Copper", 0.004)]
    lever.append(K.place(K.cyl("Grip", 0.035, 0.18, mat="Rubber", seg=16), (0, 0, 0.38), (0, 90, 0)))
    lever.append(K.place(K.cyl("Pivot", 0.03, 0.18, mat="BrassAged", seg=16), (0, 0, 0), (0, 90, 0)))
    for l in lever:
        l.location.y -= 0.14
        l.location.z += 0.92
        l.rotation_euler.x += math.radians(-35)
    parts = [K.part("Lever", lever, D + "BreakerPanel_Lever.nmmesh", (0, -0.14, 0.92),
                    moves="throws about X (the panel's width): -35 degrees (off, tilted toward -Z... down) to +35 "
                          "(on); C3 throws it", axis=(1, 0, 0), kit=KIT)]
    for z in (0.1, 1.0):
        o.append(K.sweep("Conduit", [(0.35, 0.2, 1.8), (0.35, 0.2, 2.4)], 0.04, "Copper", seg=10))
        break
    K.export("BreakerPanel", o, D + "BreakerPanel.nmmesh", ground=True, kit=KIT, parts=parts,
             pivot_note="bottom centre of its back (against a wall, facing -Z); 1 x 1.8 m, conduit up to 2.4 m")


def bookshelf():
    o = [K.box_minmax("Back", (-1.0, 0.3, 0), (1.0, 0.34, 2.4), "Walnut", 0.005)]
    for x in (-1.0, 1.0):
        o.append(K.box_minmax("Side", (x - 0.03, -0.05, 0), (x + 0.03, 0.34, 2.45), "Walnut", 0.01))
    shelves = [0.08, 0.6, 1.05, 1.5, 1.95, 2.4]
    for z in shelves:
        o.append(K.box_minmax("Shelf", (-0.98, -0.02, z - 0.03), (0.98, 0.3, z), "Walnut", 0.006))
    o.append(K.box_minmax("Crown", (-1.08, -0.1, 2.4), (1.08, 0.36, 2.5), "Walnut", 0.015))
    o.append(K.box_minmax("Kick", (-1.0, -0.04, 0), (1.0, 0.3, 0.08), "Walnut", 0.008))
    rnd = random.Random(9)
    books = []
    for si, z in enumerate(shelves[:-1]):
        x = -0.95
        while x < 0.92:
            w = rnd.uniform(0.03, 0.065)
            h = rnd.uniform(0.25, 0.38)
            d = rnd.uniform(0.18, 0.24)
            if x + w > 0.95:
                break
            if rnd.random() < 0.06:
                x += 0.08
                continue
            cell = rnd.randrange(8)
            lean = 0 if rnd.random() > 0.08 else rnd.uniform(-12, 12)
            books.append(K.uv_box("Book", (w, d, h), (x + w / 2, 0.28 - d / 2, z + h / 2), "Books",
                                  faces={"-y": (cell / 8, 0.5, (cell + 1) / 8, 1.0), "*": (cell / 8, 0.0, (cell + 1) / 8, 0.5)},
                                  rot=(0, lean, 0)))
            x += w + 0.003
    K.export("Bookshelf", o + books, D + "Bookshelf.nmmesh", ground=True, kit=KIT, ao_dist=0.4,
             pivot_note="bottom centre of its front; 2 m wide, 2.5 m tall, 36 cm deep behind (+Z); books included")


def turbine():
    """A steam turbine, 4.8 m on its axis (X): a riveted green casing between flared inlet
    and exhaust bells, cut open front-top behind a brass-framed glass window so the rotor
    (separate: five rows of twisted blades on bolted discs, shrouded, and the flywheel)
    shows as it spins; bearing pedestals with oil cups, a fly-ball governor on the exhaust
    bell (separate, it spins), a valved inlet and a gauge."""
    import bpy
    o = []
    o.append(K.box_minmax("Bed", (-2.4, -1.1, 0), (2.4, 1.1, 0.4), "SteelDark", 0.03))
    o.append(K.box_minmax("BedLip", (-2.46, -1.16, 0), (2.46, 1.16, 0.08), "SteelDark", 0.015))
    for y in (-1.02, 1.02):
        for x in [-2.2 + 0.55 * i for i in range(9)]:
            b = K.bolt("Anchor", 0.03, "SteelDark")
            b.location = (x, y, 0.4)
            o.append(b)
    Z = 1.6

    def about_x(ob):
        ob.rotation_euler = (0, math.radians(90), 0)
        ob.location = (0, 0, Z)
        return ob
    o.append(about_x(K.lathe("InletBell", [(0.0, -2.0), (0.62, -2.0), (0.68, -1.95), (1.05, -1.7), (1.2, -1.5),
                                            (1.2, -1.45), (1.15, -1.45)], mat="PanelGreen", seg=64)))
    o.append(about_x(K.lathe("ExhaustBell", [(1.15, 1.25), (1.2, 1.25), (1.2, 1.32), (1.1, 1.55), (0.85, 1.85),
                                              (0.8, 1.9), (0.0, 1.9)], mat="PanelGreen", seg=64)))
    # The open middle: a 250-degree shell; the window faces front and up (-Y, +Z).
    a0, a1 = 325.0, 575.0          # open 215-325 degrees: the window faces -Y, the pipes stay on top
    for a in (225.0, 315.0):       # inspection lamps inside the window's edges light the rotor
        ar = math.radians(a)
        o.append(K.sweep("InspectionLamp", [(-1.35, 1.0 * math.sin(ar), Z - 1.0 * math.cos(ar)),
                                             (1.15, 1.0 * math.sin(ar), Z - 1.0 * math.cos(ar))], 0.025, "LampGlow",
                         seg=8))
    o.append(about_x(K.shell_sector("Casing", [(1.15, -1.45), (1.15, 1.25)], a0, a1, 0.06, "PanelGreen", seg=72,
                                    inner_mat="SteelDark")))
    o.append(about_x(K.shell_sector("Pane", [(1.13, -1.45), (1.13, 1.25)], a1 - 360.0, a0, 0.012, "GlassClear",
                                    seg=72)))
    for x in (-1.47, 1.27):                                # rolled bands at the joints
        o.append(K.place(K.torus("Band", 1.2, 0.05, mat="BrassAged", seg=96, rseg=10), (x, 0, Z), (0, 90, 0)))
        o += K.rivets_on_lathe(lambda z: 1.21, [0.0], 40, head=0.014, mat="BrassAged",
                               centre=(x + (0.07 if x < 0 else -0.07), 0, Z), rot=(0, 90, 0))
    # Window frame: brass rails along the cut edges and two glazing bars across it.
    for a in (a0, a1):
        ar = math.radians(a)
        y, z = math.sin(ar), -math.cos(ar)
        rail = [(-1.45, 1.19 * y, Z + 1.19 * z), (1.25, 1.19 * y, Z + 1.19 * z)]
        o.append(K.strap("WindowRail", rail, 0.14, 0.05, "BrassAged"))
        o += K.rivets_along([(p[0], 1.22 * y, Z + 1.22 * z) for p in rail], 0.16, (0, y, z), head=0.012)
    for x in (-0.55, 0.35):
        bar = K.torus("GlazingBar", 1.15, 0.025, mat="BrassBrushed", seg=96, rseg=6, arc=a0 - (a1 - 360.0))
        bar.rotation_euler = (0, 0, math.radians(a1 - 360.0))
        K.group("BarG", [bar], (x, 0, Z), (0, 90, 0))
        o.append(bar)
    # Riveted straps round the closed side.
    for x in (-1.0, -0.2, 0.6):
        ring = [(x, 1.18 * math.sin(math.radians(a)), Z - 1.18 * math.cos(math.radians(a))) for a in range(int(a0), int(a1) + 1, 10)]
        o.append(K.strap("Strap", ring, 0.12, 0.03, "PanelGreen"))
    # Bearing pedestals, oil cups.
    for x, w in ((-2.2, 0.5), (2.2, 0.5)):
        o.append(K.box_minmax("Pedestal", (x - w / 2, -0.4, 0.4), (x + w / 2, 0.4, Z - 0.2), "PanelGreen", 0.03))
        cap = K.lathe("BearingCap", [(0, -0.22), (0.3, -0.22), (0.32, -0.18), (0.32, 0.18), (0.3, 0.22), (0, 0.22)],
                      mat="BrassAged", seg=32)
        o.append(about_x(cap))
        cap.location = (x, 0, Z)
        for y in (-0.33, 0.33):
            b = K.bolt("CapBolt", 0.028, "SteelDark")
            b.location = (x, y, Z - 0.2)
            o.append(b)
        o.append(K.lathe("OilCup", [(0, 0), (0.03, 0), (0.03, 0.1), (0.07, 0.16), (0.07, 0.24), (0.05, 0.26),
                                    (0, 0.26)], mat="GlassClear", seg=16))
        o[-1].location = (x, 0, Z + 0.3)
        o.append(K.lathe("OilCupLid", [(0, 0), (0.075, 0), (0.06, 0.04), (0.01, 0.06), (0, 0.06)], mat="BrassBrushed",
                         seg=16))
        o[-1].location = (x, 0, Z + 0.54)
    # Inlet with a valve, exhaust down to the floor.
    o += K.pipe_run("Inlet", [(-1.0, 0, 2.62), (-1.0, 0, 3.6), (-1.0, 0.8, 4.2), (-1.0, 2.0, 4.2)], 0.22, "Copper",
                    "BrassAged", seg=24, bolts=10)
    o.append(K.place(K.torus("ValveWheel", 0.32, 0.03, mat="PaintRed", seg=48, rseg=8), (-1.0, 0.0, 3.25)))
    for k in range(4):
        o.append(K.box("Spoke", (0.6, 0.03, 0.02), (-1.0, 0, 3.25), "PaintRed", 0.006, rot=(0, 0, 45 * k)))
    o += K.pipe_run("Exhaust", [(1.0, 0, 2.6), (1.0, 0, 3.2), (1.0, -1.5, 3.2), (1.0, -1.5, 0.4)], 0.28, "Copper",
                    "BrassAged", seg=24, bolts=10)
    o.append(K.place(K.cyl("Gauge", 0.14, 0.08, mat="BrassAged", seg=24), (-1.75, -0.95, 2.1), (90, 0, 0)))
    o.append(K.disc("GaugeFace", 0.12, (-1.75, -0.995, 2.1), "GaugeFace", seg=32))
    o.append(K.sweep("GaugeStem", [(-1.75, -0.9, 2.1), (-1.75, -0.6, 2.1), (-1.75, -0.6, 2.4)], 0.015, "Copper", seg=6))
    # Governor stand on the exhaust bell.
    o.append(K.lathe("GovernorStand", [(0, 0), (0.2, 0), (0.2, 0.05), (0.08, 0.12), (0.06, 0.3), (0.1, 0.34),
                                       (0, 0.34)], mat="BrassAged", seg=24))
    o[-1].location = (1.55, 0.35, Z + 0.95)
    gov = [K.cyl("Spindle", 0.025, 0.7, (0, 0, 0.35), "SteelBrushed", seg=12),
           K.lathe("Sleeve", [(0, 0.15), (0.06, 0.15), (0.06, 0.25), (0, 0.25)], mat="BrassBrushed", seg=16),
           K.sphere("Top", 0.05, (0, 0, 0.72), "BrassAged", seg=16, rings=8)]
    for s in (-1, 1):
        gov.append(K.sweep("Arm", [(0, 0, 0.66), (s * 0.28, 0, 0.36)], 0.012, "SteelBrushed", seg=6))
        gov.append(K.sweep("Link", [(s * 0.14, 0, 0.51), (0, 0, 0.22)], 0.009, "SteelBrushed", seg=6))
        gov.append(K.sphere("Ball", 0.08, (s * 0.31, 0, 0.33), "BrassBrushed", seg=24, rings=12))
    for g in gov:
        g.location.x += 1.55
        g.location.y += 0.35
        g.location.z += Z + 1.29
    # Rotor.
    rotor = [K.place(K.cyl("Shaft", 0.1, 5.4, mat="SteelBrushed", seg=24), (0, 0, 0), (0, 90, 0))]
    for i, x in enumerate([-1.2, -0.65, -0.1, 0.45, 0.95]):
        r = 0.42 + 0.06 * i
        rotor.append(K.place(K.lathe("Disc", [(0.1, -0.05), (r, -0.03), (r, 0.03), (0.1, 0.05), (0.18, 0.08),
                                              (0.1, 0.08)], mat="SteelBrushed", seg=48), (x, 0, 0), (0, 90, 0)))
        bolts = K.bolt_circle("DiscBolt", 0.2, 8, (x - 0.07, 0, 0), (0, -90, 0), head=0.014, washer=False)
        rotor += bolts
        n = 28
        L = 0.2 + 0.03 * i
        for k in range(n):
            a = 360.0 * k / n
            bl = K.blade("Blade", L, 0.11, 0.07, 30, 55, thick=0.18, mat="BrassBrushed", n=4, root=r - 0.02)
            bl.rotation_euler = (math.radians(a), 0, 0)
            bl.location = (x, 0, 0)
            rotor.append(bl)
        rotor.append(K.place(K.lathe("Shroud", [(r + L - 0.03, -0.06), (r + L + 0.01, -0.06), (r + L + 0.01, 0.06),
                                                (r + L - 0.03, 0.06)], mat="BrassAged", seg=64), (x, 0, 0), (0, 90, 0)))
    fly = K.band_ring("Flywheel", 0.72, 0.14, 0.2, "SteelDark", seg=64)
    rotor.append(K.place(fly, (2.65, 0, 0), (0, 90, 0)))
    rotor.append(K.place(K.lathe("FlyHub", [(0.08, -0.12), (0.22, -0.12), (0.22, 0.12), (0.08, 0.12)], mat="SteelDark",
                                 seg=24), (2.65, 0, 0), (0, 90, 0)))
    for k in range(6):
        sp = K.sweep("FlySpoke", [(0, 0, 0.2), (0, 0.06, 0.4), (0, 0, 0.64)], 0.035, "SteelDark", seg=8)
        sp.rotation_euler = (math.radians(60 * k), 0, 0)
        sp.location = (2.65, 0, 0)
        rotor.append(sp)
    for r_ in rotor:
        if r_.parent is None:
            r_.location.z += Z
        else:
            r_.parent.location.z = Z if abs(r_.parent.location.z) < 1e-6 else r_.parent.location.z
    parts = [K.part("Rotor", rotor, D + "Turbine_Rotor.nmmesh", (0, 0, Z),
                    moves="spins about X (its shaft): seen through the casing's window and at the flywheel (+X)",
                    axis=(1, 0, 0), kit=KIT),
             K.part("Governor", gov, D + "Turbine_Governor.nmmesh", (1.55, 0.35, Z + 1.29),
                    moves="spins about Y (up), its balls flung out: turn it with the rotor (about 1/4 its speed)",
                    axis=(0, 0, 1), kit=KIT)]
    K.export("Turbine", o, D + "Turbine.nmmesh", ground=True, kit=KIT, parts=parts, ao_dist=1.0,
             pivot_note="bottom centre of the bed; 4.8 x 2.2 m bed, axis along X at 160 cm; inlet up and back "
                        "(+Z), exhaust down to the floor at -Z",
             notes="The casing is open on its front (-Z) behind glass: a brass-framed window from x -145 to +125 cm, "
                   "110 degrees of the drum, lit inside by two LampGlow strips; the rotor's five blade rows show "
                   "as it spins")


def valve_wheel():
    o = [K.sweep("Pipe", [(-1.0, 0, 0.9), (1.0, 0, 0.9)], 0.12, "Copper", seg=20)]
    for x in (-1.0, -0.35, 0.35, 1.0):
        o.append(K.place(K.cyl("Flange", 0.19, 0.05, mat="BrassAged", seg=24), (x, 0, 0.9), (0, 90, 0)))
    o.append(K.box_minmax("Body", (-0.25, -0.2, 0.7), (0.25, 0.2, 1.12), "SteelDark", 0.04))
    o.append(K.cyl("Stem", 0.035, 0.45, (0, 0, 1.35), "SteelBrushed", seg=12))
    for x in (-0.8, 0.8):
        o.append(K.box_minmax("Stand", (x - 0.08, -0.15, 0), (x + 0.08, 0.15, 0.78), "SteelDark", 0.01))
    w = [K.torus("Rim", 0.32, 0.03, mat="PaintRed", seg=48, rseg=10)]
    w.append(K.cyl("Hub", 0.06, 0.08, mat="BrassAged", seg=16))
    for k in range(5):
        a = 72 * k
        w.append(K.box("Spoke", (0.62, 0.035, 0.025), (0, 0, 0), "PaintRed", 0.006, rot=(0, 0, a)))
    for p in w:
        p.location.z += 1.6
    parts = [K.part("Wheel", w, D + "ValveWheel_Wheel.nmmesh", (0, 0, 1.6),
                    moves="turns about Y (up); 2 turns open the valve", axis=(0, 0, 1), kit=KIT)]
    K.export("ValveWheel", o, D + "ValveWheel.nmmesh", ground=True, kit=KIT, parts=parts,
             pivot_note="bottom centre; a 2 m pipe run along X at 90 cm with the valve; wheel at 160 cm")


def elevator_cage():
    o = [K.box_minmax("Floor", (-1.25, -1.25, 0), (1.25, 1.25, 0.12), "SteelDark", 0.02)]
    o.append(K.box_minmax("Deck", (-1.2, -1.2, 0.12), (1.2, 1.2, 0.14), "Grating", 0.0))
    o.append(K.box_minmax("Roof", (-1.25, -1.25, 2.9), (1.25, 1.25, 3.0), "BrassAged", 0.02))
    for sx in (-1, 1):
        for sy in (-1, 1):
            o.append(K.box_minmax("Corner", (sx * 1.25 - 0.06, sy * 1.25 - 0.06, 0), (sx * 1.25 + 0.06,
                                  sy * 1.25 + 0.06, 3.0), "BrassBrushed", 0.01))
    for side in ("x+", "x-", "y+"):
        for z in (1.0, 2.0):
            if side[0] == "x":
                x = 1.25 if side[1] == "+" else -1.25
                o.append(K.sweep("Rail", [(x, -1.2, z), (x, 1.2, z)], 0.025, "BrassAged", seg=8))
                for y in [-1.0 + 0.25 * i for i in range(9)]:
                    o.append(K.sweep("Bar", [(x, y, 0.14), (x, y, 2.9)], 0.012, "SteelDark", seg=6))
            else:
                o.append(K.sweep("Rail", [(-1.2, 1.25, z), (1.2, 1.25, z)], 0.025, "BrassAged", seg=8))
                for x in [-1.0 + 0.25 * i for i in range(9)]:
                    o.append(K.sweep("Bar", [(x, 1.25, 0.14), (x, 1.25, 2.9)], 0.012, "SteelDark", seg=6))
            break
    o.append(K.box_minmax("ControlPost", (1.0, 1.0, 0.14), (1.15, 1.15, 1.2), "PanelGreen", 0.01))
    o.append(K.place(K.cyl("Lever", 0.012, 0.25, mat="BrassAged", seg=8), (1.07, 0.98, 1.3), (20, 0, 0)))
    o.append(K.lathe("Hook", [(0, 3.0), (0.25, 3.0), (0.1, 3.2), (0.1, 3.5), (0, 3.5)], mat="SteelDark", seg=16))
    gate = []
    for i in range(10):
        x = -1.15 + 0.25 * i
        gate.append(K.sweep("Lattice", [(x, -1.27, 0.15), (x + 0.25, -1.27, 2.6)], 0.01, "BrassAged", seg=5))
        gate.append(K.sweep("Lattice", [(x + 0.25, -1.27, 0.15), (x, -1.27, 2.6)], 0.01, "BrassAged", seg=5))
    gate.append(K.sweep("GateTop", [(-1.2, -1.27, 2.6), (1.2, -1.27, 2.6)], 0.02, "BrassAged", seg=8))
    gate.append(K.sweep("GateBottom", [(-1.2, -1.27, 0.15), (1.2, -1.27, 0.15)], 0.02, "BrassAged", seg=8))
    parts = [K.part("Gate", gate, D + "ElevatorCage_Gate.nmmesh", (0, -1.27, 0),
                    moves="a scissor gate on the -Z side: scale X from 1 to 0.15 about its left end (-120 cm) to open",
                    axis=(1, 0, 0), kit=KIT)]
    K.export("ElevatorCage", o, D + "ElevatorCage.nmmesh", ground=True, kit=KIT, parts=parts,
             pivot_note="bottom centre of the floor; 2.5 x 2.5 m, 3.5 m with its hook; the whole cage moves on Y "
                        "(C4); the gate faces -Z")


def pipes():
    def flange(x, y, z, rot):
        return K.place(K.cyl("Flange", 0.11, 0.04, mat="BrassAged", seg=20), (x, y, z), rot)
    o = [K.sweep("Pipe", [(-1.0, 0, 0), (1.0, 0, 0)], 0.075, "Copper", seg=16), flange(-0.98, 0, 0, (0, 90, 0)),
         flange(0.98, 0, 0, (0, 90, 0))]
    K.export("Pipe_Straight", o, D + "Pipe_Straight.nmmesh", kit=KIT, ao=False,
             pivot_note="the pipe's axis at its centre; 2 m along X, 15 cm bore; flanged ends (chain every 200 cm)")
    K.clear()
    pts = [(-0.6, 0, 0)] + [(0.3 * math.sin(math.radians(a)) - 0.3, 0, 0.3 - 0.3 * math.cos(math.radians(a)))
                            for a in range(0, 91, 10)] + [(0, 0, 0.6)]
    pts = [(-0.6, 0, 0), (-0.3, 0, 0)] + [(-0.3 + 0.3 * math.sin(math.radians(a)), 0, 0.3 - 0.3 * math.cos(math.radians(a)))
                                           for a in range(10, 81, 10)] + [(0, 0, 0.3), (0, 0, 0.6)]
    o = [K.sweep("Elbow", pts, 0.075, "Copper", seg=16), flange(-0.58, 0, 0, (0, 90, 0)), flange(0, 0, 0.58, (0, 0, 0))]
    K.export("Pipe_Elbow", o, D + "Pipe_Elbow.nmmesh", kit=KIT, ao=False,
             pivot_note="the bend's corner (the two axes' meeting point); legs to -X 60 cm and +Y 60 cm")
    K.clear()
    o = [K.sweep("Run", [(-0.6, 0, 0), (0.6, 0, 0)], 0.075, "Copper", seg=16),
         K.sweep("Branch", [(0, 0, 0), (0, 0, 0.6)], 0.075, "Copper", seg=16),
         flange(-0.58, 0, 0, (0, 90, 0)), flange(0.58, 0, 0, (0, 90, 0)), flange(0, 0, 0.58, (0, 0, 0)),
         K.sphere("Joint", 0.1, (0, 0, 0), "BrassAged", 24, 12)]
    K.export("Pipe_Tee", o, D + "Pipe_Tee.nmmesh", kit=KIT, ao=False,
             pivot_note="the axes' meeting point; run along X (1.2 m), branch up +Y 60 cm")
    K.clear()
    o = [K.cyl("Body", 0.13, 0.08, (0, 0, 0), "BrassAged", seg=32, rot=(90, 0, 0)),
         K.disc("Face", 0.11, (0, -0.0405, 0), "GaugeFace", seg=32),
         K.disc("Glass", 0.115, (0, -0.045, 0), "GlassClear", seg=32),
         K.sweep("Stem", [(0, 0.04, 0), (0, 0.04, -0.2), (0, 0.04, -0.25)], 0.015, "BrassAged", seg=8),
         K.place(K.torus("Bezel", 0.12, 0.012, mat="BrassAged", seg=32, rseg=8), (0, -0.045, 0), (90, 0, 0))]
    needle = [K.box_minmax("Needle", (-0.004, -0.05, -0.01), (0.004, -0.047, 0.085), "PaintRed", 0.0),
              K.cyl("Cap", 0.01, 0.006, (0, -0.05, 0), "BrassAged", seg=12, rot=(90, 0, 0))]
    parts = [K.part("Needle", needle, D + "Gauge_Needle.nmmesh", (0, -0.048, 0),
                    moves="turns about Z (the dial's normal): -135 degrees (0) to +135 (full scale); the face's red "
                          "zone starts at +81", axis=(0, 1, 0), kit=KIT)]
    K.export("Gauge", o, D + "Gauge.nmmesh", kit=KIT, parts=parts, ao=False,
             pivot_note="the dial's centre; it faces -Z; a stem runs 25 cm down to a pipe; GaugeFace glows "
                        "(EmissiveIntensity)")


def main():
    K.run_kit(KIT, [walls, floor_ceiling, dome_segment, orrery, workbench, pedestal, samples, breaker_panel,
                    bookshelf, turbine, valve_wheel, elevator_cage, pipes])


main()
