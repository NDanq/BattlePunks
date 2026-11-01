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
    """The atrium's brass orrery, 6.5 m tall: a walnut and brass pedestal, a gimbal of
    three rings (each turns on its own axis), a glowing sun and three planets on arms."""
    C = 4.0     # the gimbal's centre height
    base = [K.lathe("Pedestal", [(0, 0), (1.6, 0), (1.6, 0.3), (1.3, 0.4), (0.7, 0.6), (0.45, 1.6), (0.6, 1.8),
                                 (0.25, 2.0), (0.18, 2.6), (0.0, 2.6)], mat="BrassAged", seg=48)]
    base.append(K.lathe("Plinth", [(0, 0), (1.9, 0), (1.9, 0.25), (0, 0.25)], mat="Walnut", seg=8))
    base[-1].rotation_euler = (0, 0, math.radians(22.5))
    for k in range(4):
        a = math.radians(90 * k + 45)
        base.append(K.sweep("Strut", [(1.3 * math.cos(a), 1.3 * math.sin(a), 0.4), (0.3 * math.cos(a),
                                       0.3 * math.sin(a), 2.0)], 0.06, "BrassBrushed", seg=10))
    # Yoke holding the outer ring on an axis along X.
    base.append(K.sweep("Yoke", [(-3.15, 0, C), (-3.15, 0, 2.3), (0, 0, 2.3), (3.15, 0, 2.3), (3.15, 0, C)], 0.09,
                        "BrassBrushed", seg=12))
    base.append(K.sphere("Sun", 0.55, (0, 0, C), "LampGlow", seg=32, rings=16))
    # Gantry ladder up the pedestal to the roof hatch (the player climbs it).
    for z in [0.4 + 0.35 * i for i in range(7)]:
        base.append(K.sweep("Rung", [(1.0, -0.25, z), (1.0, 0.25, z)], 0.02, "SteelDark", seg=6))
    for y in (-0.25, 0.25):
        base.append(K.sweep("Stile", [(1.0, y, 0.3), (1.0, y, 2.9)], 0.03, "SteelDark", seg=6))
    rings = []
    import bpy
    for name, R, axis_rot in (("RingOuter", 3.0, (90, 0, 0)), ("RingMiddle", 2.5, (90, 0, 90)),
                              ("RingInner", 2.0, (55, 0, 30))):
        r = [K.torus(name, R, 0.07, mat="BrassBrushed", seg=96, rseg=12)]
        r.append(K.torus(name + "Inlay", R, 0.075, mat="Walnut", seg=96, rseg=4))
        r[-1].scale = (1, 1, 0.35)
        for k in range(12):
            a = 2 * math.pi * k / 12
            r.append(K.box("Tick", (0.05, 0.22, 0.05), (R * math.cos(a), R * math.sin(a), 0.0), "BrassAged", 0.01,
                           rot=(0, 0, math.degrees(a))))
        # Pivot pins on the ring's own axis.
        for s in (-1, 1):
            r.append(K.place(K.cyl("Pin", 0.06, 0.25, mat="SteelDark", seg=12), (s * (R + 0.12), 0, 0), (0, 90, 0)))
        # Turned as a whole about the gimbal's centre (an empty parent).
        holder = bpy.data.objects.new(name + "Holder", None)
        bpy.context.scene.collection.objects.link(holder)
        holder.location = (0, 0, C)
        holder.rotation_euler = [math.radians(a) for a in axis_rot]
        for ob in r:
            ob.parent = holder
        rings.append((name, r, axis_rot))
    planets = []
    for k, (rad, size, mat) in enumerate(((1.2, 0.18, "Copper"), (1.6, 0.25, "Marble"), (1.85, 0.16, "Enamel"))):
        a = math.radians(120 * k)
        planets.append(K.sweep("Arm", [(0, 0, C - 0.6), (rad * math.cos(a), rad * math.sin(a), C - 0.6),
                                       (rad * math.cos(a), rad * math.sin(a), C - 0.1)], 0.025, "BrassAged", seg=8))
        planets.append(K.sphere("Planet", size, (rad * math.cos(a), rad * math.sin(a), C), mat, seg=24, rings=12))
    planets.append(K.place(K.cyl("Collar", 0.2, 0.25, mat="BrassBrushed", seg=24), (0, 0, C - 0.6)))
    parts = []
    for (name, r, axis_rot) in rings:
        ax = {"RingOuter": (1, 0, 0), "RingMiddle": (0, 1, 0), "RingInner": (0, 0, 1)}[name]
        how = {"RingOuter": "upright in the XY plane; turns about X (its pins)",
               "RingMiddle": "upright in the ZY plane; turns about Z (its pins) inside Outer",
               "RingInner": "tilted 55 degrees; spins in its own plane inside Middle"}[name]
        parts.append(K.part(name, r, D + f"Orrery_{name}.nmmesh", (0, 0, C), moves=how, axis=ax, kit=KIT))
    parts.append(K.part("Planets", planets, D + "Orrery_Planets.nmmesh", (0, 0, C),
                        moves="spins about Y (up) round the sun, slowly", axis=(0, 0, 1), kit=KIT))
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
    o = [K.box_minmax("Base", (-0.3, -0.3, 0), (0.3, 0.3, 0.1), "Walnut", 0.015)]
    o.append(K.box_minmax("Shaft", (-0.22, -0.22, 0.1), (0.22, 0.22, 0.85), "Walnut", 0.015))
    o.append(K.box_minmax("Top", (-0.3, -0.3, 0.85), (0.3, 0.3, 0.92), "Marble", 0.012))
    o.append(K.box_minmax("Label", (-0.12, -0.232, 0.62), (0.12, -0.22, 0.7), "BrassAged", 0.003))
    o.append(K.box_minmax("Band", (-0.225, -0.225, 0.12), (0.225, 0.225, 0.16), "BrassAged", 0.004))
    K.export("SamplePedestal", o, D + "SamplePedestal.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre; a sample sits on its top at y = 92 cm; brass label on its -Z face")


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
    for k in range(5):
        p = K.sphere("Puff", 0.08 + 0.05 * rnd.random(), (rnd.uniform(-0.05, 0.05), rnd.uniform(-0.05, 0.05),
                                                           0.1 + 0.07 * k), "Steam", seg=16, rings=8)
        puff.append(p)
    puff.append(K.lathe("Vent", [(0, 0), (0.06, 0), (0.06, 0.05), (0.04, 0.06), (0, 0.06)], mat="SteelDark", seg=16))
    save("Sample_Steam", puff, "Volume (Translucent): a few steam puffs over a vent; better as particles with the "
                               "Steam material")


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
    """A steam turbine, 4.6 m long on its axis (X): a riveted casing split at the middle, an
    open window on the rotor, brass bands, an inlet and exhaust; the rotor is separate."""
    o = []
    o.append(K.box_minmax("Bed", (-2.4, -1.1, 0), (2.4, 1.1, 0.4), "SteelDark", 0.03))
    case = K.lathe("Casing", [(0.0, -2.0), (0.6, -2.0), (1.1, -1.6), (1.15, 1.4), (0.8, 1.9), (0.0, 1.9)],
                   mat="PanelGreen", seg=48)
    case.rotation_euler = (0, math.radians(90), 0)
    case.location = (0, 0, 1.6)
    o.append(case)
    for x in (-1.5, -0.6, 0.6, 1.3):
        o.append(K.place(K.torus("Band", 1.17, 0.04, mat="BrassAged", seg=64, rseg=8), (x, 0, 1.6), (0, 90, 0)))
    o.append(K.box_minmax("Flange", (-1.6, -1.25, 1.55), (1.4, 1.25, 1.65), "SteelDark", 0.02))
    for x in [-1.4 + 0.2 * i for i in range(14)]:
        for y in (-1.2, 1.2):
            o.append(K.cyl("Bolt", 0.03, 0.06, (x, y, 1.68), "BrassAged", seg=8))
    for x in (-1.6, 1.6):
        o.append(K.box_minmax("Foot", (x - 0.3, -0.9, 0.4), (x + 0.3, 0.9, 0.9), "SteelDark", 0.02))
    o.append(K.sweep("Inlet", [(-1.0, 0, 2.7), (-1.0, 0, 3.6), (-1.0, 0.8, 4.2), (-1.0, 2.0, 4.2)], 0.25, "Copper", seg=20))
    o.append(K.place(K.cyl("InletFlange", 0.36, 0.08, mat="BrassAged", seg=24), (-1.0, 0, 2.72)))
    o.append(K.sweep("Exhaust", [(1.0, 0, 2.7), (1.0, 0, 3.2), (1.0, -1.5, 3.2), (1.0, -1.5, 0.4)], 0.32, "Copper",
                     seg=20))
    o.append(K.place(K.cyl("Gauge", 0.14, 0.08, mat="BrassAged", seg=24), (0.0, -1.18, 2.4), (90, 0, 0)))
    o.append(K.disc("GaugeFace", 0.12, (0.0, -1.225, 2.4), "GaugeFace", seg=32))
    # Shaft ends and a flywheel at the +X end.
    o.append(K.place(K.cyl("Bearing", 0.25, 0.4, mat="BrassBrushed", seg=24), (2.2, 0, 1.6), (0, 90, 0)))
    rotor = [K.place(K.cyl("Shaft", 0.1, 5.4, mat="SteelBrushed", seg=24), (0, 0, 0), (0, 90, 0))]
    for i, x in enumerate([-1.2, -0.6, 0.0, 0.6]):
        r = 0.55 + 0.12 * i
        rotor.append(K.place(K.cyl("Disc", r, 0.06, mat="SteelBrushed", seg=48), (x, 0, 0), (0, 90, 0)))
        for k in range(24):
            a = 360.0 * k / 24
            b = K.box("Blade", (0.035, 0.09, 0.22), (x, 0, 0), "BrassBrushed", 0.004, rot=(a, 25, 0))
            b.location = (x, (r + 0.08) * math.cos(math.radians(a + 90)), (r + 0.08) * math.sin(math.radians(a + 90)))
            rotor.append(b)
    fly = K.place(K.torus("Flywheel", 0.75, 0.1, mat="SteelDark", seg=64, rseg=10), (2.65, 0, 0), (0, 90, 0))
    rotor.append(fly)
    for k in range(6):
        rotor.append(K.box("FlySpoke", (0.06, 0.08, 1.4), (2.65, 0, 0), "SteelDark", 0.01, rot=(30 * k, 0, 0)))
    for r in rotor:
        r.location.z += 1.6
    parts = [K.part("Rotor", rotor, D + "Turbine_Rotor.nmmesh", (0, 0, 1.6),
                    moves="spins about X (its shaft); the flywheel outside the casing shows it", axis=(1, 0, 0),
                    kit=KIT)]
    K.export("Turbine", o, D + "Turbine.nmmesh", ground=True, kit=KIT, parts=parts, ao_dist=1.0,
             pivot_note="bottom centre of the bed; 4.8 x 2.2 m bed, axis along X at 160 cm; inlet up and back "
                        "(+Z), exhaust down to the floor at -Z",
             notes="The casing is closed: the rotor shows through... only the flywheel at +X; open the casing in "
                   "the level by hiding it, or treat the rotor as the flywheel's spin")


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
    K.reset()
    for f in (walls, floor_ceiling, dome_segment, orrery, workbench, pedestal, samples, breaker_panel, bookshelf,
              turbine, valve_wheel, elevator_cage, pipes):
        K.clear()
        f()
    K.save_records(KIT)


main()
