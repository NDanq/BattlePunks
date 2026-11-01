"""
make_l3.py -- the L3 Core kit (Content/Meshes/L3/) and the airship (Content/Meshes/Airship/).

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_l3.py
"""

import math
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kitlib as K      # noqa: E402

D = "Meshes/L3/"


def railing(points, kit, mat_post="SteelDark"):
    """Posts every ~1.2 m along a polyline (Blender metres, at deck level), a top rail at
    1.05 m, a mid rail, a toe board."""
    o = []
    from mathutils import Vector
    pts = [Vector(p) for p in points]
    o.append(K.sweep("TopRail", [p + Vector((0, 0, 1.05)) for p in pts], 0.025, "BrassAged", seg=10))
    o.append(K.sweep("MidRail", [p + Vector((0, 0, 0.55)) for p in pts], 0.018, mat_post, seg=8))
    o.append(K.sweep("Toe", [p + Vector((0, 0, 0.06)) for p in pts], 0.012, mat_post, seg=4,
                     profile=[(-0.004, -0.05), (0.004, -0.05), (0.004, 0.05), (-0.004, 0.05)]))
    total = sum((pts[i + 1] - pts[i]).length for i in range(len(pts) - 1))
    n = max(2, int(round(total / 1.2)) + 1)
    acc = 0.0
    seglens = [(pts[i + 1] - pts[i]).length for i in range(len(pts) - 1)]
    for k in range(n):
        d = total * k / (n - 1)
        i, acc = 0, 0.0
        while i < len(seglens) - 1 and acc + seglens[i] < d:
            acc += seglens[i]
            i += 1
        t = (d - acc) / max(seglens[i], 1e-9)
        p = pts[i].lerp(pts[i + 1], min(1.0, t))
        o.append(K.sweep("Post", [p, p + Vector((0, 0, 1.05))], 0.025, mat_post, seg=8))
    return o


def catwalks():
    kit = "L3"
    # Straight: 4 m along Y, 1.5 m wide, deck top at 0.
    o = [K.box_minmax("Deck", (-0.72, 0, -0.04), (0.72, 4.0, 0.0), "Grating", 0.0)]
    for x in (-0.75, 0.75):
        o.append(K.sweep("Channel", [(x, 0, -0.1), (x, 4.0, -0.1)], 0.0, "SteelDark",
                         profile=[(-0.04, -0.1), (0.04, -0.1), (0.04, 0.1), (0.0, 0.1), (0.0, -0.06), (-0.04, -0.06)]))
    for y in (0.05, 2.0, 3.95):
        o.append(K.box_minmax("Cross", (-0.75, y - 0.03, -0.12), (0.75, y + 0.03, -0.04), "SteelDark", 0.005))
    K.export("Catwalk_Straight", o, D + "Catwalk_Straight.nmmesh", kit=kit,
             pivot_note="deck top (y = 0), centre of the near end; 4 m along +Z, 1.5 m wide; chain every 400 cm; "
                        "add Railing_Straight at x = +-75 cm")
    K.clear()
    K.export("Railing_Straight", railing([(0, 0, 0), (0, 4.0, 0)], kit), D + "Railing_Straight.nmmesh", kit=kit,
             pivot_note="bottom of its first post; runs 4 m along +Z")
    K.clear()
    # Arc: 22.5 degrees of the ring gallery, inner radius 7 m, outer 8.5 m, about the shaft's axis.
    import bmesh
    Ri, Ro, A = 7.0, 8.5, 22.5
    n = 8
    bm = bmesh.new()
    vs = []
    for k in range(n + 1):
        a = math.radians(A * k / n)
        row = []
        for r in (Ri, Ro):
            for z in (-0.04, 0.0):
                row.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z)))
        vs.append(row)
    for k in range(n):
        a, b = vs[k], vs[k + 1]
        bm.faces.new([a[1], a[3], b[3], b[1]])    # top
        bm.faces.new([a[0], b[0], b[2], a[2]])    # bottom
        bm.faces.new([a[0], a[1], b[1], b[0]])    # inner edge
        bm.faces.new([a[2], b[2], b[3], a[3]])    # outer edge
    bm.faces.new([vs[0][0], vs[0][2], vs[0][3], vs[0][1]])
    bm.faces.new([vs[n][0], vs[n][1], vs[n][3], vs[n][2]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    o = [K.obj_from_bm("Deck", bm, "Grating", sharp=math.radians(30))]
    for r in (Ri - 0.03, Ro + 0.03):
        arc = [(r * math.cos(math.radians(A * k / 16)), r * math.sin(math.radians(A * k / 16)), -0.1) for k in range(17)]
        o.append(K.sweep("Channel", arc, 0.0, "SteelDark", profile=[(-0.04, -0.1), (0.04, -0.1), (0.04, 0.1), (-0.04, 0.1)]))
    for k in (0, 4, 8):
        a = math.radians(A * k / 8)
        o.append(K.sweep("Cross", [(Ri * math.cos(a), Ri * math.sin(a), -0.08), (Ro * math.cos(a), Ro * math.sin(a), -0.08)],
                         0.03, "SteelDark", seg=6))
    arc_o = [(Ro * math.cos(math.radians(A * k / 16)), Ro * math.sin(math.radians(A * k / 16)), 0) for k in range(17)]
    o += railing(arc_o, kit)
    K.export("Catwalk_Arc", o, D + "Catwalk_Arc.nmmesh", kit=kit,
             pivot_note="the shaft's axis at deck level; 22.5 degrees from +X toward +Z, inner radius 700 cm, outer "
                        "850 cm with a railing on the outer edge (the shaft side is open: add Railing_Straight or "
                        "leave it); 16 make a ring, or step each down for a spiral")


def reactor():
    """The core reactor, 13 m: a stepped, riveted foundation with buttress ribs and bolted
    access hatches, a glass column in a riveted brass frame, the glowing core wound with a
    copper coil on ceramic formers, eight flanged copper conduits clamped to the frame,
    heavy cables to the floor, and a cap crowned by insulator stacks round a central
    electrode (the ReactorArcs particles' anchor points)."""
    kit = "L3"
    from mathutils import Vector
    o = [K.lathe("Base", [(0, 0), (3.4, 0), (3.4, 0.12), (3.3, 0.15), (3.3, 0.55), (3.2, 0.6), (2.7, 0.95),
                          (2.65, 1.05), (2.1, 1.35), (1.65, 1.55), (1.6, 1.6), (0, 1.6)], mat="PanelCream", seg=96,
                 sharp=math.radians(25))]
    o.append(K.lathe("Plinth", [(3.42, 0.0), (3.45, 0.02), (3.45, 0.1), (3.42, 0.12)], mat="SteelDark", seg=96))
    o += K.rivets_on_lathe(lambda z: 3.3, [0.2, 0.5], 72, head=0.018, mat="SteelDark")
    for k in range(16):                                   # buttress ribs up the slope
        a = 360.0 * (k + 0.5) / 16
        rib = K.prism("Rib", [(1.6, 0.55), (3.36, 0.55), (3.36, 0.72), (1.72, 1.62), (1.6, 1.62)], 0.16, (0, 0, 0),
                      "SteelDark", bevel=0.012)
        rib.rotation_euler = (0, 0, math.radians(a))
        o.append(rib)
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        o += K.rivets_along([(3.2 * ca, 3.2 * sa, 0.74), (1.85 * ca, 1.85 * sa, 1.55)], 0.22,
                            (ca * 0.6, sa * 0.6, 1.0), head=0.016, mat="BrassAged")
    for k in range(8):                                    # bolted access hatches round the drum
        a = 360.0 * k / 8
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        hatch = [K.box("Hatch", (0.05, 0.7, 0.3), (0, 0, 0), "PanelGreen", 0.012)]
        hatch.append(K.box("Handle", (0.05, 0.22, 0.03), (0.04, 0, 0.02), "BrassAged", 0.008))
        for (dy, dz) in ((-0.3, -0.11), (0.3, -0.11), (-0.3, 0.11), (0.3, 0.11)):
            b = K.bolt("HatchBolt", 0.016, "SteelDark", washer=False)
            b.location = (0.025, dy, dz)
            b.rotation_euler = (0, math.radians(90), 0)
            hatch.append(b)
        K.group("HatchG", hatch, (3.32 * ca, 3.32 * sa, 0.35), (0, 0, a))
        o += hatch
    for a in (30.0, 210.0):                               # gauges on the drum
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        g = [K.place(K.cyl("GaugeBody", 0.16, 0.08, mat="BrassAged", seg=32), (0, 0, 0), (0, 90, 0)),
             K.place(K.disc("GaugeFace", 0.135, (0, 0, 0), "GaugeFace", seg=32), (0.042, 0, 0), (0, 0, 90)),
             K.place(K.torus("Bezel", 0.145, 0.014, mat="BrassBrushed", seg=32, rseg=8), (0.045, 0, 0), (0, 90, 0))]
        K.group("GaugeG", g, (3.34 * ca, 3.34 * sa, 0.36), (0, 0, a))
        o += g
    # Glass column, core, coil, formers.
    o.append(K.lathe("Glass", [(1.25, 1.6), (1.25, 10.4)], mat="GlassClear", seg=64))
    o.append(K.lathe("Core", [(0, 1.6), (0.5, 1.6), (0.55, 1.7), (0.55, 10.3), (0.5, 10.4), (0, 10.4)],
                     mat="ReactorGlow", seg=40))
    o.append(K.helix("Coil", 0.78, 2.0, 10.0, 24, 0.04, "Copper", per_turn=32, seg=8))
    for k in range(6):
        a = 2 * math.pi * k / 6
        o.append(K.box("Former", (0.08, 0.05, 8.4), (0.86 * math.cos(a), 0.86 * math.sin(a), 6.0), "PanelCream", 0.01,
                       rot=(0, 0, math.degrees(a))))
    for z in (1.75, 10.25):
        o.append(K.band_ring("CoreCollar", 0.9, 0.7, 0.12, "BrassBrushed", seg=64))
        o[-1].location = (0, 0, z)
    # Frame: riveted uprights and bolted bands.
    for k in range(8):
        a = 2 * math.pi * (k + 0.5) / 8
        ca, sa = math.cos(a), math.sin(a)
        up = K.box("Upright", (0.1, 0.22, 8.8), (1.36 * ca, 1.36 * sa, 6.0), "BrassAged", 0.015,
                   rot=(0, 0, math.degrees(a)))
        o.append(up)
        o += K.rivets_along([(1.42 * ca - 0.07 * sa, 1.42 * sa + 0.07 * ca, 1.9),
                             (1.42 * ca - 0.07 * sa, 1.42 * sa + 0.07 * ca, 10.1)], 0.3, (ca, sa, 0), head=0.014)
        o += K.rivets_along([(1.42 * ca + 0.07 * sa, 1.42 * sa - 0.07 * ca, 1.9),
                             (1.42 * ca + 0.07 * sa, 1.42 * sa - 0.07 * ca, 10.1)], 0.3, (ca, sa, 0), head=0.014)
    for z in (1.7, 4.0, 6.3, 8.6, 10.3):
        o.append(K.band_ring("Band", 1.38, 0.14, 0.18, "BrassBrushed", seg=96))
        o[-1].location = (0, 0, z)
        o += K.rivets_on_lathe(lambda zz: 1.45, [z - 0.05, z + 0.05], 24, head=0.012, mat="SteelDark", phase=0.25)
    # Conduits: flanged copper, clamped to the frame.
    for k in range(8):
        a = 2 * math.pi * k / 8
        ca, sa = math.cos(a), math.sin(a)
        pts = [(3.0 * ca, 3.0 * sa, 0.6), (2.1 * ca, 2.1 * sa, 1.45), (1.9 * ca, 1.9 * sa, 2.2),
               (1.9 * ca, 1.9 * sa, 10.55), (1.55 * ca, 1.55 * sa, 10.95)]
        o += K.pipe_run("Conduit", pts, 0.085, "Copper", "BrassAged", seg=14, bolts=6)
        for z in (3.0, 5.2, 7.4, 9.6):
            o.append(K.place(K.torus("Clamp", 0.1, 0.018, mat="SteelDark", seg=16, rseg=6),
                             (1.9 * ca, 1.9 * sa, z)))
            o.append(K.sweep("Stay", [(1.42 * ca, 1.42 * sa, z), (1.81 * ca, 1.81 * sa, z)], 0.022, "SteelDark", seg=6))
    for k in range(6):                                    # heavy cables to the floor
        a = 2 * math.pi * (k + 0.25) / 6
        ca, sa = math.cos(a), math.sin(a)
        o.append(K.cable("Cable", (3.36 * ca, 3.36 * sa, 0.42), (5.6 * ca + 0.6 * sa, 5.6 * sa - 0.6 * ca, 0.06), 0.25,
                         0.075, "Rubber", n=20, seg=10))
        o.append(K.place(K.cyl("Gland", 0.11, 0.12, mat="BrassAged", seg=16), (3.38 * ca, 3.38 * sa, 0.42),
                         (0, 90, math.degrees(a))))
    # Cap, insulator stacks, the central electrode.
    o.append(K.lathe("Cap", [(0, 10.4), (1.6, 10.4), (2.0, 10.7), (2.2, 11.3), (1.9, 11.45), (1.4, 11.55),
                             (0.6, 11.7), (0, 11.7)], mat="PanelCream", seg=96))
    o += K.rivets_on_lathe(lambda z: 2.18, [11.0], 64, head=0.016, mat="SteelDark")
    o.append(K.place(K.torus("CapRim", 2.2, 0.06, mat="BrassAged", seg=96, rseg=10), (0, 0, 11.3)))
    insul = [(0, 0.0), (0.12, 0.0)]
    for i in range(6):
        z = 0.08 + 0.14 * i
        insul += [(0.07, z - 0.04), (0.22, z), (0.07, z + 0.04)]
    insul += [(0.07, 0.95), (0.0, 0.95)]
    for k in range(6):
        a = 2 * math.pi * k / 6
        st = K.lathe("Insulator", insul, mat="Marble", seg=24, sharp=math.radians(50))
        st.location = (1.35 * math.cos(a), 1.35 * math.sin(a), 11.5)
        o.append(st)
        o.append(K.sphere("Terminal", 0.12, (1.35 * math.cos(a), 1.35 * math.sin(a), 12.55), "BrassBrushed", 24, 12))
    o.append(K.lathe("Electrode", [(0, 11.65), (0.3, 11.65), (0.3, 11.8), (0.12, 11.9), (0.1, 12.6), (0.16, 12.7),
                                   (0.0, 12.75)], mat="BrassBrushed", seg=32))
    o.append(K.sphere("Dome", 0.38, (0, 0, 13.0), "BrassBrushed", seg=48, rings=24))
    o.append(K.place(K.torus("Corona", 0.55, 0.05, mat="BrassAged", seg=64, rseg=10), (0, 0, 12.8)))
    rings = []
    for i, (z, R, tilt) in enumerate(((3.6, 2.6, 8), (6.0, 2.9, -10), (8.4, 2.6, 12))):
        import bpy
        holder = bpy.data.objects.new("H", None)
        bpy.context.scene.collection.objects.link(holder)
        holder.location = (0, 0, z)
        holder.rotation_euler = (math.radians(tilt), 0, 0)
        r = [K.band_ring("Ring", R, 0.34, 0.2, "BrassBrushed", seg=128, lip=0.02)]
        r.append(K.torus("Inner", R - 0.2, 0.045, mat="ReactorGlow", seg=128, rseg=8))
        for k in range(16):
            a = 360.0 * k / 16
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            r.append(K.box("Lug", (0.3, 0.14, 0.3), ((R + 0.12) * ca, (R + 0.12) * sa, 0), "SteelDark", 0.02,
                           rot=(0, 0, a)))
            for dz in (-0.09, 0.09):
                b = K.bolt("LugBolt", 0.018, "BrassAged", washer=False)
                b.location = ((R + 0.27) * ca, (R + 0.27) * sa, dz)
                b.rotation_euler = (0, math.radians(90), math.radians(a))
                r.append(b)
        for k in range(4):                                 # electromagnet coils on the ring
            a = 2 * math.pi * (k + 0.5) / 4
            coil = K.helix("RingCoil", 0.13, -0.25, 0.25, 9, 0.022, "Copper", per_turn=20, seg=6)
            core = K.cyl("RingCoilCore", 0.1, 0.56, (0, 0, 0), "SteelDark", seg=16)
            for c in (coil, core):
                c.rotation_euler = (0, math.radians(90), a + math.pi / 2)
                c.location = ((R + 0.02) * math.cos(a), (R + 0.02) * math.sin(a), 0.23)
                r.append(c)
        for ob in r:
            if ob.parent is None:
                ob.parent = holder
        rings.append(K.part(f"Ring{'ABC'[i]}", r, D + f"Reactor_Ring{'ABC'[i]}.nmmesh", (0, 0, z),
                            moves=f"spins about its own (tilted {tilt} degrees) axis, near Y; speed up for the "
                                  "alarm", axis=(0, 0, 1), kit=kit))
    K.export("ReactorCore", o, D + "ReactorCore.nmmesh", kit=kit, parts=rings, ground=True, ao_dist=1.5,
             pivot_note="bottom centre; 6.9 m base, 13.4 m tall; the glowing core (ReactorGlow: EmissiveColor to red "
                        "and PulseSpeed up on the alarm) shows through glass 1.6-10.4 m; ReactorArcs particles: an "
                        "emitter at the dome (0, 1300, 0) cm aimed at the insulator terminals (radius 135 cm, "
                        "1255 cm up)")


def vault():
    kit = "L3"
    o = [K.lathe("Plinth", [(0, 0), (1.3, 0), (1.3, 0.2), (1.1, 0.3), (0.9, 0.9), (1.0, 1.0), (1.0, 1.1), (0, 1.1)],
                 mat="PanelCream", seg=8)]
    o[-1].rotation_euler = (0, 0, math.radians(22.5))
    o.append(K.lathe("Cradle", [(0, 1.1), (0.4, 1.1), (0.4, 1.15), (0.25, 1.2), (0, 1.2)], mat="BrassBrushed", seg=32))
    for k in range(3):
        a = 2 * math.pi * k / 3
        o.append(K.sweep("Clamp", [(0.32 * math.cos(a), 0.32 * math.sin(a), 1.15),
                                   (0.32 * math.cos(a), 0.32 * math.sin(a), 1.45),
                                   (0.2 * math.cos(a), 0.2 * math.sin(a), 1.55)], 0.03, "BrassAged", seg=8))
    for k in range(8):
        a = 2 * math.pi * k / 8 + math.pi / 8
        o.append(K.cyl("Lamp", 0.05, 0.03, (1.02 * math.cos(a), 1.02 * math.sin(a), 0.62), "IntelGlow", seg=12,
                       rot=(0, 90, math.degrees(a))))
    intel = [K.lathe("Tube", [(0, 0), (0.16, 0), (0.16, 0.06), (0.13, 0.08), (0.13, 0.42), (0.16, 0.44), (0.16, 0.5),
                              (0, 0.5)], mat="BrassAged", seg=32)]
    intel.append(K.lathe("Glass", [(0.14, 0.08), (0.14, 0.42)], mat="GlassClear", seg=32))
    intel.append(K.lathe("Fluid", [(0, 0.09), (0.11, 0.09), (0.11, 0.41), (0, 0.41)], mat="IntelGlow", seg=24))
    intel.append(K.lathe("Grip", [(0, 0.5), (0.06, 0.5), (0.06, 0.58), (0.09, 0.6), (0, 0.62)], mat="BrassBrushed", seg=16))
    for ob in intel:
        ob.location.z += 1.2
    parts = [K.part("Intel", intel, D + "Vault_Intel.nmmesh", (0, 0, 1.2),
                    moves="lifted out (C5): rises +Y, then attaches to the player's hand", axis=(0, 0, 1), kit=kit)]
    K.export("VaultPlinth", o, D + "VaultPlinth.nmmesh", kit=kit, parts=parts, ground=True,
             pivot_note="bottom centre; the intel cylinder stands in the cradle at y = 120 cm; IntelGlow lamps round "
                        "the plinth (drop to red on the alarm)")


def locker():
    kit = "L3"
    o = [K.box_minmax("Body", (-0.3, 0.0, 0), (0.3, 0.5, 1.9), "PanelCream", 0.01)]
    o.append(K.box_minmax("Plinth", (-0.3, 0.02, 0), (0.3, 0.5, 0.1), "SteelDark", 0.005))
    o.append(K.box_minmax("Inside", (-0.27, 0.03, 0.12), (0.27, 0.47, 1.85), "SteelDark", 0.0))
    o.append(K.box_minmax("Shelf", (-0.27, 0.03, 1.5), (0.27, 0.47, 1.52), "SteelDark", 0.0))
    o.append(K.box_minmax("Rack", (-0.2, 0.3, 0.9), (0.2, 0.33, 1.3), "Walnut", 0.005))
    door = [K.box_minmax("Door", (0, -0.02, 0.1), (0.58, 0.0, 1.88), "PanelCream", 0.006)]
    for z in (1.55, 1.6, 1.65, 1.7):
        door.append(K.box_minmax("Vent", (0.12, -0.03, z), (0.46, -0.02, z + 0.02), "SteelDark", 0.003))
    door.append(K.box_minmax("Label", (0.2, -0.026, 1.3), (0.38, -0.02, 1.38), "BrassAged", 0.002))
    door.append(K.box_minmax("Handle", (0.5, -0.06, 0.95), (0.53, -0.02, 1.15), "SteelDark", 0.006))
    for d in door:
        d.location.x -= 0.29
    parts = [K.part("Door", door, D + "Locker_Door.nmmesh", (-0.29, 0, 0),
                    moves="swings about Y at its left hinge, 0 to 110 degrees toward -Z", axis=(0, 0, 1), kit=kit)]
    K.export("Locker", o, D + "Locker.nmmesh", kit=kit, parts=parts, ground=True,
             pivot_note="bottom centre of its front; 60 x 50 cm, 1.9 m tall, opening toward -Z; the sidearm hangs on "
                        "the rack at (0, 110, 32) cm")


def coolant_pump():
    kit = "L3"
    o = [K.box_minmax("Skid", (-1.6, -1.0, 0), (1.6, 1.0, 0.3), "SteelDark", 0.02)]
    vol = K.lathe("Volute", [(0, -0.5), (1.0, -0.5), (1.2, -0.3), (1.2, 0.3), (1.0, 0.5), (0, 0.5)], mat="PanelGreen",
                  seg=48)
    vol.rotation_euler = (0, math.radians(90), 0)
    vol.location = (-0.6, 0, 1.5)
    o.append(vol)
    o.append(K.place(K.cyl("Motor", 0.7, 1.4, mat="PanelGreen", seg=48), (0.8, 0, 1.2), (0, 90, 0)))
    for x in (0.25, 0.6, 1.0, 1.35):
        o.append(K.place(K.torus("Fin", 0.72, 0.035, mat="SteelDark", seg=48, rseg=6), (x, 0, 1.2), (0, 90, 0)))
    o.append(K.box_minmax("MotorFoot", (0.2, -0.6, 0.3), (1.4, 0.6, 0.6), "SteelDark", 0.02))
    o.append(K.box_minmax("PumpFoot", (-1.2, -0.7, 0.3), (0.0, 0.7, 0.8), "SteelDark", 0.02))
    o.append(K.sweep("Suction", [(-1.85, 0, 1.5), (-3.0, 0, 1.5)], 0.35, "Copper", seg=24))
    o.append(K.sweep("Discharge", [(-0.6, 0, 2.6), (-0.6, 0, 3.6), (-0.6, 1.0, 4.4), (-0.6, 3.0, 4.4)], 0.3, "Copper",
                     seg=24))
    o.append(K.place(K.cyl("Flange", 0.48, 0.1, mat="BrassAged", seg=32), (-1.85, 0, 1.5), (0, 90, 0)))
    o.append(K.place(K.cyl("Flange", 0.42, 0.1, mat="BrassAged", seg=32), (-0.6, 0, 2.65)))
    o.append(K.place(K.cyl("Gauge", 0.14, 0.08, mat="BrassAged", seg=24), (-0.6, -1.25, 1.5), (90, 0, 0)))
    o.append(K.disc("GaugeFace", 0.12, (-0.6, -1.295, 1.5), "GaugeFace", seg=32))
    o.append(K.place(K.cyl("Guard", 0.75, 0.3, mat="Grating", seg=48), (1.65, 0, 1.2), (0, 90, 0)))
    fan = [K.place(K.cyl("Hub", 0.15, 0.12, mat="BrassBrushed", seg=24), (0, 0, 0), (0, 90, 0))]
    for k in range(6):
        fan.append(K.box("Blade", (0.03, 0.22, 0.6), (0, 0, 0), "BrassBrushed", 0.006, rot=(60 * k, 20, 0)))
        fan[-1].location = (0, 0.32 * math.cos(math.radians(60 * k + 90)), 0.32 * math.sin(math.radians(60 * k + 90)))
    for f in fan:
        f.location.x += 1.62
        f.location.z += 1.2
    parts = [K.part("Fan", fan, D + "CoolantPump_Fan.nmmesh", (1.62, 0, 1.2), moves="spins about X behind its guard",
                    axis=(1, 0, 0), kit=kit)]
    K.export("CoolantPump", o, D + "CoolantPump.nmmesh", kit=kit, parts=parts, ground=True, ao_dist=1.0,
             pivot_note="bottom centre of the skid; 3.2 x 2 m; suction pipe out to -X (300 cm), discharge up and back "
                        "(+Z) at 440 cm")


def beacon():
    kit = "L3"
    o = []
    for k in range(3):
        a = 2 * math.pi * k / 3
        o.append(K.sweep("Leg", [(1.0 * math.cos(a), 1.0 * math.sin(a), 0), (0.15 * math.cos(a), 0.15 * math.sin(a), 1.2)],
                         0.04, "SteelDark", seg=8))
        o.append(K.cyl("Foot", 0.1, 0.04, (1.0 * math.cos(a), 1.0 * math.sin(a), 0.02), "Rubber", seg=12))
    o.append(K.box_minmax("Battery", (-0.35, -0.25, 0.9), (0.35, 0.25, 1.5), "PanelRed", 0.02))
    o.append(K.cyl("Mast", 0.06, 1.6, (0, 0, 2.2), "BrassBrushed", seg=16))
    o.append(K.lathe("Housing", [(0, 2.9), (0.3, 2.9), (0.3, 3.0), (0.28, 3.05), (0.28, 3.45), (0.33, 3.5), (0.05, 3.7),
                                 (0, 3.72)], mat="BrassAged", seg=32))
    o.append(K.lathe("Lens", [(0, 3.05), (0.27, 3.05), (0.29, 3.25), (0.27, 3.45), (0, 3.45)], mat="BeaconLight", seg=32))
    o.append(K.sweep("Aerial", [(0.2, 0, 3.6), (0.2, 0, 4.6)], 0.008, "SteelDark", seg=6))
    o.append(K.box_minmax("Panel", (-0.2, -0.27, 1.05), (0.2, -0.25, 1.35), "SteelDark", 0.004))
    o.append(K.cyl("Button", 0.05, 0.04, (0, -0.29, 1.2), "PaintRed", seg=16, rot=(90, 0, 0)))
    refl = [K.lathe("Mirror", [(0, 0), (0.2, 0.02), (0.24, 0.12), (0.22, 0.13), (0.0, 0.03)], mat="SteelBrushed", seg=24)]
    refl[0].rotation_euler = (math.radians(90), 0, 0)
    refl[0].location = (0, 0.05, 3.25)
    refl.append(K.cyl("Spindle", 0.02, 0.4, (0, 0, 3.25), "SteelDark", seg=8))
    parts = [K.part("Reflector", refl, D + "Beacon_Reflector.nmmesh", (0, 0, 3.25),
                    moves="spins about Y inside the lens when the beacon fires (sweeping the light)", axis=(0, 0, 1),
                    kit=kit)]
    K.export("Beacon", o, D + "Beacon.nmmesh", kit=kit, parts=parts, ground=True,
             pivot_note="bottom centre; a tripod 2 m across, the lamp at 305-345 cm (BeaconLight: raise "
                        "EmissiveIntensity as it charges); the fire button faces -Z at 120 cm")


def freight_lift():
    kit = "L3"
    o = [K.box_minmax("Frame", (-2, -2, -0.3), (2, 2, -0.05), "SteelDark", 0.03)]
    o.append(K.box_minmax("Deck", (-1.9, -1.9, -0.05), (1.9, 1.9, 0.0), "Grating", 0.0))
    for i in range(8):
        x0 = -2 + i * 0.5
        o.append(K.box_minmax("Stripe", (x0, -2.0, -0.05), (x0 + 0.25, -1.9, 0.0), "PanelCream", 0.0))
        o.append(K.box_minmax("Stripe", (x0 + 0.25, -2.0, -0.05), (x0 + 0.5, -1.9, 0.0), "PaintBlack", 0.0))
    o += railing([(-1.95, -1.9, 0), (-1.95, 1.95, 0), (1.95, 1.95, 0), (1.95, -1.9, 0)], kit)
    for sx in (-1, 1):
        o.append(K.box_minmax("Guide", (sx * 2.0 - 0.1, -0.2, -0.3), (sx * 2.0 + 0.1, 0.2, 0.3), "BrassAged", 0.02))
    o.append(K.box_minmax("Control", (1.4, -1.6, 0), (1.7, -1.4, 1.1), "PanelGreen", 0.01))
    o.append(K.cyl("Lamp", 0.05, 0.08, (1.55, -1.5, 1.15), "BeaconLight", seg=12))
    o.append(K.place(K.cyl("Lever", 0.015, 0.3, mat="BrassAged", seg=8), (1.55, -1.62, 0.95), (60, 0, 0)))
    K.export("FreightLift", o, D + "FreightLift.nmmesh", kit=kit, ground=False,
             pivot_note="deck top centre; 4 x 4 m; open side (hazard striped) toward -Z; the whole platform moves on "
                        "Y; guides on +-X for the shaft rails; control post at the front right")


def airship():
    """The extraction airship: a 36 m gored canvas envelope (16 gores pillowing between
    their seams) with brass hoops, a nose cap and mooring ring, framed and ribbed fins, load
    belts down both sides and the rigging that hangs a 12 m gondola from them; the gondola
    a planked hull with a keel, rub rails and portholes, a walnut cabin with framed windows,
    a copper barrel roof with a skylight and a stack, a glazed bridge, railed decks fore and
    aft, lanterns, a winch; two engine nacelles on outriggers and a tail engine, each with
    a four-bladed twisted walnut propeller."""
    import bmesh
    import bpy
    from mathutils import Vector
    kit = "Airship"
    A = "Meshes/Airship/"
    L, R, AXZ = 36.0, 5.5, 9.0

    def radius(t):
        return R * (math.sin(math.pi * t) ** 0.6) * (1.0 - 0.15 * t)

    prof = []
    for k in range(49):
        t = k / 48
        prof.append((radius(t), -L / 2 + L * t))
    prof[0] = (0.0, prof[0][1])
    prof[-1] = (0.0, prof[-1][1])
    gores = 16
    envelope = K.lathe("Envelope", prof, mat="Canvas", seg=gores * 6, sharp=math.radians(70))
    for v in envelope.data.vertices:
        rr = math.hypot(v.co.x, v.co.y)
        if rr < 1e-4:
            continue
        a = math.atan2(v.co.y, v.co.x)
        f = (a * gores / (2 * math.pi)) % 1.0
        s = 1.0 - 0.018 * (1.0 - math.sin(math.pi * f))
        v.co.x *= s
        v.co.y *= s
    envelope.rotation_euler = (0, math.radians(90), 0)
    env = [envelope]
    for t in (0.2, 0.35, 0.5, 0.65, 0.8):
        x = -L / 2 + L * t
        hoop = K.band_ring("Hoop", radius(t) + 0.02, 0.06, 0.22, "BrassAged", seg=128, lip=0.01)
        env.append(K.place(hoop, (x, 0, 0), (0, 90, 0)))
    env.append(K.lathe("NoseCap", [(0.0, L / 2 - 0.9), (1.2, L / 2 - 0.9), (1.05, L / 2 - 0.45), (0.6, L / 2 - 0.1),
                                   (0.0, L / 2 + 0.05)], mat="BrassAged", seg=48))
    env[-1].rotation_euler = (0, math.radians(90), 0)
    env.append(K.place(K.torus("MooringRing", 0.28, 0.04, mat="SteelDark", seg=32, rseg=8), (L / 2 + 0.25, 0, 0),
                       (90, 0, 0)))
    env.append(K.lathe("TailCone", [(0.0, -L / 2 - 0.7), (0.4, -L / 2 - 0.6), (0.75, -L / 2 + 0.3), (0.0, -L / 2 + 0.3)],
                       mat="BrassAged", seg=32))
    env[-1].rotation_euler = (0, math.radians(90), 0)
    # Fins: canvas in a brass frame, ribbed.
    for a in (0, 90, 180, 270):
        fin_out = [(0, 0.5), (5.0, 0.5), (6.2, 5.0), (2.8, 5.0)]
        parts_ = [K.prism("Fin", fin_out, 0.14, (0, 0, 0), "Canvas", 0.02)]
        frame = [(x, 0, z) for (x, z) in fin_out] + [(fin_out[0][0], 0, fin_out[0][1])]
        parts_.append(K.sweep("FinFrame", frame, 0.07, "BrassAged", seg=8))
        for f in (0.3, 0.55, 0.8):
            parts_.append(K.sweep("FinRib", [(5.0 * f, 0, 0.5), (2.8 + 3.4 * f, 0, 5.0)], 0.04, "BrassAged", seg=6))
        parts_.append(K.box("Rudder", (1.4, 0.1, 4.2), (-0.75, 0, 2.85), "Canvas", 0.03))
        parts_.append(K.sweep("Hinge", [(0.0, 0, 0.6), (0.0, 0, 5.0)], 0.06, "SteelDark", seg=8))
        K.group("FinG", parts_, (-L / 2 + 1.6, 0, 0), (a + 90, 0, 0))
        env += parts_
    # Load belts: a riveted strap down each side, with rigging patches.
    belt = {}
    for side in (-1, 1):
        pts = []
        for k in range(15):
            t = 0.16 + 0.68 * k / 14
            x = -L / 2 + L * t
            r = radius(t) + 0.03
            ang = math.radians(52)
            pts.append((x, side * r * math.sin(ang), -r * math.cos(ang)))
        belt[side] = pts
        env.append(K.sweep("Belt", pts, 0.06, "Leather", seg=8))
    for ob in env:
        if ob.parent is None:
            ob.location.z += AXZ
        elif ob.parent.location.z < AXZ - 1:
            ob.parent.location.z += AXZ
    K.export("Airship_Envelope", env, A + "Airship_Envelope.nmmesh", kit=kit, ao=True, ao_dist=3.0,
             pivot_note="the gondola's pivot (see Airship_Hull): the envelope's axis is 9 m above it, along X; "
                        "the nose is +X")
    K.clear()
    # --- Gondola ------------------------------------------------------------------------------------
    bm = bmesh.new()
    n = 24
    rows = []
    for k in range(n + 1):
        t = k / n
        x = -6 + 12 * t
        w = 1.7 * math.sin(math.pi * (0.12 + 0.76 * t)) ** 0.7
        depth = 1.6 * math.sin(math.pi * (0.1 + 0.8 * t)) ** 0.5
        rows.append([bm.verts.new((x, -w, 0.0)), bm.verts.new((x, -w * 0.97, -depth * 0.3)),
                     bm.verts.new((x, -w * 0.85, -depth * 0.62)), bm.verts.new((x, -w * 0.5, -depth * 0.9)),
                     bm.verts.new((x, 0.0, -depth)), bm.verts.new((x, w * 0.5, -depth * 0.9)),
                     bm.verts.new((x, w * 0.85, -depth * 0.62)), bm.verts.new((x, w * 0.97, -depth * 0.3)),
                     bm.verts.new((x, w, 0.0))])
    for k in range(n):
        for j in range(8):
            bm.faces.new([rows[k][j], rows[k + 1][j], rows[k + 1][j + 1], rows[k][j + 1]])
    bm.faces.new(rows[0][::-1])
    bm.faces.new(rows[-1])
    bm.faces.new([rows[k][0] for k in range(n + 1)] + [rows[k][8] for k in range(n, -1, -1)])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    hull = [K.obj_from_bm("Hull", bm, "Planks", sharp=math.radians(40))]
    keel = [(-5.9, 0, -1.6 * math.sin(math.pi * 0.1) ** 0.5)]
    for k in range(1, n):
        t = k / n
        keel.append((-6 + 12 * t, 0, -1.6 * math.sin(math.pi * (0.1 + 0.8 * t)) ** 0.5 - 0.03))
    keel.append((6.0, 0, -1.6 * math.sin(math.pi * 0.9) ** 0.5))
    hull.append(K.strap("Keel", keel, 0.16, 0.12, "WoodBeam"))
    for side in (-1, 1):
        edge = [(-6 + 12 * k / n, side * (1.7 * math.sin(math.pi * (0.12 + 0.76 * k / n)) ** 0.7 + 0.02), 0.0)
                for k in range(n + 1)]
        hull.append(K.sweep("RubRail", edge, 0.06, "BrassAged", seg=8))
        mid = [(p[0], p[1] * 0.98, -0.45) for p in edge]
        hull.append(K.sweep("Wale", mid[2:-2], 0.035, "BrassAged", seg=6))
        for x in (-3.5, -2.0, -0.5, 1.0, 2.5, 4.0):
            t = (x + 6) / 12
            w = 1.7 * math.sin(math.pi * (0.12 + 0.76 * t)) ** 0.7 * 0.985
            hull.append(K.place(K.torus("Porthole", 0.17, 0.035, mat="BrassBrushed", seg=24, rseg=8),
                                (x, side * (w + 0.01), -0.22), (90, 0, 0)))
            hull.append(K.place(K.cyl("PortGlass", 0.15, 0.02, mat="GlassClear", seg=24), (x, side * w, -0.22),
                                (90, 0, 0)))
    hull.append(K.box_minmax("Deck", (-5.6, -1.45, -0.06), (5.6, 1.45, 0.02), "Planks", 0.01))
    # Cabin: walnut walls with real window openings, glazing, brass frames.
    wins = [(x, 0.85, x + 0.8, 1.7) for x in (-2.6, -1.5, -0.4, 0.7, 1.7)]
    for side in (-1, 1):
        hull.append(K.prism("CabinWall", [(-3.0, 0.0), (2.5, 0.0), (2.5, 2.2), (-3.0, 2.2)], 0.1,
                            (0, side * 1.4, 0), "Walnut", 0.01,
                            holes=[[(a, b), (c, b), (c, d), (a, d)] for (a, b, c, d) in wins]))
        for (a, b, c, d) in wins:
            hull.append(K.box_minmax("CabinGlass", (a, side * 1.4 - 0.01, b), (c, side * 1.4 + 0.01, d), "GlassClear", 0.0))
            fr = [(a, side * (1.46), b), (c, side * 1.46, b), (c, side * 1.46, d), (a, side * 1.46, d), (a, side * 1.46, b)]
            hull.append(K.sweep("WinFrame", fr, 0.025, "BrassAged", seg=6))
            hull.append(K.sweep("WinBar", [((a + c) / 2, side * 1.46, b), ((a + c) / 2, side * 1.46, d)], 0.012,
                                "BrassAged", seg=6))
        hull.append(K.box_minmax("Sill", (-3.05, side * 1.4 - 0.08, 0.78), (2.55, side * 1.4 + 0.08, 0.85), "BrassAged",
                                 0.01))
        hull.append(K.box_minmax("Skirt", (-3.05, side * 1.4 - 0.07, 0.0), (2.55, side * 1.4 + 0.07, 0.3), "PanelCream",
                                 0.01))
    back = K.prism("CabinBack", [(-1.45, 0.0), (1.45, 0.0), (1.45, 2.2), (-1.45, 2.2)], 0.1, (0, 0, 0), "Walnut", 0.01,
                   holes=[[(-0.45, 0.0001), (0.45, 0.0001), (0.45, 1.9), (-0.45, 1.9)]])
    back.rotation_euler = (0, 0, math.radians(90))
    back.location = (-3.0, 0, 0)
    hull.append(back)
    hull.append(K.box_minmax("Door", (-3.06, -0.45, 0.0), (-3.02, 0.45, 1.9), "Walnut", 0.01))
    hull.append(K.sphere("DoorKnob", 0.04, (-3.08, 0.32, 1.0), "BrassBrushed", 12, 6))
    # Copper barrel roof with a skylight and a stack.
    roof = K.prism("Roof", [(-1.6 + 3.2 * k / 16, 2.2 + 0.45 * math.sin(math.pi * k / 16)) for k in range(17)][::-1], 5.9, (0, 0, 0), "Copper", 0.02)
    roof.rotation_euler = (0, 0, math.radians(90))
    roof.location = (-0.25, 0, 0)
    hull.append(roof)
    for side in (-1, 1):
        hull += K.rivets_along([(-3.15, side * 1.62, 2.24), (2.65, side * 1.62, 2.24)], 0.25, (0, side, 0.3), head=0.014)
    hull.append(K.box_minmax("Skylight", (-1.6, -0.5, 2.55), (0.4, 0.5, 2.85), "GlassClear", 0.02))
    hull.append(K.sweep("SkylightFrame", [(-1.62, -0.52, 2.86), (0.42, -0.52, 2.86), (0.42, 0.52, 2.86),
                                          (-1.62, 0.52, 2.86), (-1.62, -0.52, 2.86)], 0.025, "BrassAged", seg=6))
    hull.append(K.lathe("Stack", [(0, 2.5), (0.22, 2.5), (0.2, 3.6), (0.26, 3.7), (0.26, 3.8), (0.2, 3.85), (0, 3.85)],
                        mat="Copper", seg=24))
    hull[-1].location = (1.6, 0.6, 0)
    for z in (2.9, 3.3):
        hull.append(K.place(K.torus("StackBand", 0.215, 0.02, mat="BrassAged", seg=24, rseg=6), (1.6, 0.6, z)))
    # Glazed bridge at the bow.
    hull.append(K.box_minmax("BridgeBase", (2.5, -1.2, 0.0), (3.6, 1.2, 0.8), "Walnut", 0.03))
    hull.append(K.box_minmax("BridgeGlass", (2.55, -1.15, 0.8), (3.55, 1.15, 1.7), "GlassClear", 0.02))
    for (y0, y1) in ((-1.17, -1.13), (1.13, 1.17)):
        for x in (2.55, 3.05, 3.55):
            hull.append(K.box_minmax("BridgeMullion", (x - 0.03, y0, 0.8), (x + 0.03, y1, 1.7), "BrassAged", 0.005))
    for y in (-1.15, -0.4, 0.4, 1.15):
        hull.append(K.box_minmax("BridgeMullion", (3.53, y - 0.03, 0.8), (3.58, y + 0.03, 1.7), "BrassAged", 0.005))
    hull.append(K.box_minmax("BridgeRoof", (2.45, -1.28, 1.7), (3.7, 1.28, 1.82), "Copper", 0.03))
    # Railed decks fore and aft.
    for xs in ((-5.4, -3.2), (3.7, 5.4)):
        for side in (-1, 1):
            pts = []
            for k in range(7):
                x = xs[0] + (xs[1] - xs[0]) * k / 6
                t = (x + 6) / 12
                w = 1.7 * math.sin(math.pi * (0.12 + 0.76 * t)) ** 0.7 - 0.08
                pts.append((x, side * w, 0.0))
            hull.append(K.sweep("TopRail", [(p[0], p[1], 1.0) for p in pts], 0.03, "BrassAged", seg=8))
            hull.append(K.sweep("MidRail", [(p[0], p[1], 0.55) for p in pts], 0.018, "BrassAged", seg=6))
            for p in pts[::2]:
                hull.append(K.sweep("Stanchion", [p, (p[0], p[1], 1.0)], 0.025, "BrassBrushed", seg=8))
                hull.append(K.sphere("Knob", 0.04, (p[0], p[1], 1.03), "BrassBrushed", 12, 6))
    for x in (-5.5, 5.5):
        hull.append(K.lathe("Lantern", [(0, 0), (0.1, 0), (0.12, 0.05), (0.12, 0.3), (0.16, 0.34), (0.03, 0.44),
                                        (0, 0.45)], mat="BrassAged", seg=8))
        hull[-1].location = (x, 0, 1.0)
        hull.append(K.lathe("LanternGlow", [(0, 0.06), (0.105, 0.06), (0.105, 0.3), (0, 0.3)], mat="LampGlow", seg=8))
        hull[-1].location = (x, 0, 1.0)
        hull.append(K.sweep("LanternPost", [(x, 0, 0), (x, 0, 1.0)], 0.03, "BrassBrushed", seg=8))
    # Winch.
    hull.append(K.place(K.lathe("Winch", [(0, -0.35), (0.42, -0.35), (0.42, -0.3), (0.3, -0.28), (0.3, 0.28),
                                          (0.42, 0.3), (0.42, 0.35), (0, 0.35)], mat="BrassBrushed", seg=32),
                        (-4.5, 0, -0.4), (90, 0, 0)))
    hull.append(K.place(K.helix("WinchRope", 0.31, -0.26, 0.26, 12, 0.018, "Leather", per_turn=20, seg=6),
                        (-4.5, 0, -0.4), (90, 0, 0)))
    hull.append(K.sweep("WinchLine", [(-4.5, 0, -0.72), (-4.5, 0, -3.0)], 0.015, "SteelDark", seg=5))
    hull.append(K.lathe("Hook", [(0, 0), (0.06, 0.02), (0.08, 0.12), (0.04, 0.2), (0, 0.22)], mat="SteelDark", seg=12))
    hull[-1].location = (-4.5, 0, -3.2)
    # Rigging up to the envelope's belts, with turnbuckles.
    for side in (-1, 1):
        for i, bp in enumerate(belt[side][1:-1:2]):
            top = Vector((bp[0], bp[1], bp[2] + AXZ))
            x = max(-5.0, min(4.8, bp[0] * 0.33))
            low = Vector((x, side * (1.62 if -3.1 < x < 2.6 else 1.4), 2.3 if -3.1 < x < 2.6 else 1.0))
            hull.append(K.sweep("Rigging", [low, top], 0.018, "SteelDark", seg=5))
            tb = low.lerp(top, 0.08)
            c = K.cyl("Turnbuckle", 0.035, 0.28, (0, 0, 0), "BrassAged", seg=8)
            c.location = tb
            K.orient(c, top - low)
            hull.append(c)
    # Outriggers and nacelles.
    for y in (-1, 1):
        hull.append(K.sweep("Outrigger", [(-1.0, y * 1.5, 1.6), (-1.0, y * 4.0, 2.2)], 0.12, "BrassAged", seg=12))
        hull.append(K.sweep("Strut", [(-1.0, y * 1.45, 0.3), (-1.0, y * 3.7, 1.95)], 0.06, "SteelDark", seg=8))
        nac = K.lathe("Nacelle", [(0, -1.0), (0.3, -0.95), (0.45, -0.6), (0.48, 0.0), (0.42, 0.7), (0.3, 0.95),
                                  (0.15, 1.05), (0.0, 1.05)], mat="Copper", seg=32)
        hull.append(K.place(nac, (-1.0, y * 4.0, 2.2), (0, 90, 0)))
        for x in (-1.5, -1.3, -1.1, -0.9, -0.7):
            hull.append(K.place(K.torus("CoolingFin", 0.5, 0.02, mat="SteelDark", seg=32, rseg=6),
                                (x, y * 4.0, 2.2), (0, 90, 0)))
        for k in range(3):
            hull.append(K.sweep("ExhaustStub", [(-0.6, y * 4.0 + 0.3 * math.cos(k), 2.2 + 0.42),
                                                (-0.9, y * 4.0 + 0.3 * math.cos(k), 2.75)], 0.05, "SteelDark", seg=8))
    props = []

    def prop(name, at, radius):
        p = [K.place(K.lathe("Spinner", [(0, 0), (0.2, 0), (0.18, 0.15), (0.1, 0.35), (0, 0.42)], mat="BrassBrushed",
                             seg=24), (0, 0, 0), (0, -90, 0))]
        p.append(K.place(K.cyl("Hub", 0.22, 0.2, mat="SteelDark", seg=24), (0.08, 0, 0), (0, 90, 0)))
        for k in range(4):
            a = 90.0 * k
            bl = K.blade("Blade", radius, 0.42, 0.26, 55, 75, thick=0.14, mat="Walnut", n=8, root=0.15)
            tip = K.blade("Tip", radius * 0.14, 0.27, 0.2, 74.5, 76, thick=0.17, mat="BrassAged", n=3,
                          root=0.15 + radius * 0.87)
            for b in (bl, tip):
                b.rotation_euler = (math.radians(a), 0, 0)
                b.rotation_mode = "XYZ"
                p.append(b)
        for ob in p:
            ob.location = (ob.location.x + at[0], ob.location.y + at[1], ob.location.z + at[2])
        props.append(K.part(name, p, A + f"Airship_{name}.nmmesh", at,
                            moves="spins about X (forward), ~8 turns/s", axis=(1, 0, 0), kit=kit))
    prop("PropLeft", (-2.05, -4.0, 2.2), 1.3)
    prop("PropRight", (-2.05, 4.0, 2.2), 1.3)
    prop("PropTail", (-L / 2 - 0.95, 0, AXZ), 1.6)
    env_part = dict(name="Envelope", mesh="Content/" + A + "Airship_Envelope.nmmesh",
                    materials=["Content/Materials/Kit/Canvas.material.nmgraph",
                               "Content/Materials/Kit/BrassAged.material.nmgraph",
                               "Content/Materials/Kit/SteelDark.material.nmgraph",
                               "Content/Materials/Kit/Leather.material.nmgraph"],
                    offset_cm=[0, 0, 0], moves="rigid with the hull (a gentle bob is fine)")
    K.export("Airship_Hull", hull, A + "Airship_Hull.nmmesh", kit=kit, parts=[env_part] + props, ao_dist=1.5,
             pivot_note="the gondola's deck centre (its rails at 100 cm); nose toward +X; the envelope above "
                        "(9 m to its axis); the winch line hangs from (-450, -300, 0) cm for C6",
             notes="Fly it along +X; C1/C6 in the engine at about 1/1 scale (36 m envelope)")


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv or set(argv) - {"airship"}:
        K.run_kit("L3", [catwalks, reactor, vault, locker, coolant_pump, beacon, freight_lift])
    K.RECORDS.clear()
    if not argv or "airship" in argv:
        K.run_kit("Airship", [airship])


main()
