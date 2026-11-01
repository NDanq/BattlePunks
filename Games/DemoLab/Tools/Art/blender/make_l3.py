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
    kit = "L3"
    o = [K.lathe("Base", [(0, 0), (3.2, 0), (3.2, 0.6), (2.6, 0.9), (2.0, 1.4), (1.6, 1.6), (0, 1.6)],
                 mat="PanelCream", seg=64)]
    o.append(K.lathe("Glass", [(1.25, 1.6), (1.25, 10.4)], mat="GlassClear", seg=48))
    o.append(K.lathe("Core", [(0, 1.6), (0.75, 1.6), (0.75, 10.4), (0, 10.4)], mat="ReactorGlow", seg=32))
    o.append(K.lathe("Cap", [(0, 10.4), (1.6, 10.4), (2.0, 10.7), (2.2, 11.3), (1.4, 11.8), (0.5, 12.2), (0, 12.2)],
                     mat="PanelCream", seg=64))
    for k in range(8):
        a = 2 * math.pi * k / 8
        o.append(K.sweep("Cage", [(1.4 * math.cos(a), 1.4 * math.sin(a), 1.6), (1.4 * math.cos(a), 1.4 * math.sin(a), 10.4)],
                         0.06, "BrassAged", seg=8))
        o.append(K.sweep("Conduit", [(2.9 * math.cos(a), 2.9 * math.sin(a), 0.6), (2.0 * math.cos(a), 2.0 * math.sin(a), 1.4),
                                     (1.9 * math.cos(a), 1.9 * math.sin(a), 10.6)], 0.09, "Copper", seg=12))
    for z in (1.7, 4.0, 6.3, 8.6, 10.3):
        o.append(K.place(K.torus("Band", 1.32, 0.07, mat="BrassAged", seg=64, rseg=8), (0, 0, z)))
    rings = []
    for i, (z, R, tilt) in enumerate(((3.6, 2.6, 8), (6.0, 2.9, -10), (8.4, 2.6, 12))):
        holder = bpy.data.objects.new("H", None)
        bpy.context.scene.collection.objects.link(holder)
        holder.location = (0, 0, z)
        holder.rotation_euler = (math.radians(tilt), 0, 0)
        r = [K.torus("Ring", R, 0.14, mat="BrassBrushed", seg=96, rseg=12)]
        r.append(K.torus("Inner", R - 0.18, 0.05, mat="ReactorGlow", seg=96, rseg=8))
        for k in range(16):
            a = 360.0 * k / 16
            r.append(K.box("Lug", (0.32, 0.12, 0.34), ((R + 0.1) * math.cos(math.radians(a)),
                                                       (R + 0.1) * math.sin(math.radians(a)), 0), "SteelDark", 0.02,
                           rot=(0, 0, a)))
        for ob in r:
            ob.parent = holder
        rings.append(K.part(f"Ring{'ABC'[i]}", r, D + f"Reactor_Ring{'ABC'[i]}.nmmesh", (0, 0, z),
                            moves=f"spins about its own (tilted {tilt} degrees) axis, near Y; speed up for the "
                                  "alarm", axis=(0, 0, 1), kit=kit))
    K.export("ReactorCore", o, D + "ReactorCore.nmmesh", kit=kit, parts=rings, ground=True, ao_dist=1.5,
             pivot_note="bottom centre; 6.4 m base, 12.2 m tall; the glowing core (ReactorGlow: EmissiveColor to red "
                        "and PulseSpeed up on the alarm) shows through glass 1.6-10.4 m")


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
    """The extraction airship: a 36 m canvas envelope with brass rings and fins, a 12 m
    walnut and brass gondola slung under it, two side propellers and a tail one."""
    kit = "Airship"
    A = "Meshes/Airship/"
    L, R = 36.0, 5.5
    prof = []
    for k in range(25):
        t = k / 24
        x = -L / 2 + L * t
        r = R * (math.sin(math.pi * t) ** 0.6) * (1.0 - 0.15 * t)
        prof.append((r, x))
    prof[0] = (0.0, prof[0][1])
    prof[-1] = (0.0, prof[-1][1])
    env = [K.lathe("Envelope", prof, mat="Canvas", seg=32, sharp=math.radians(70))]
    env[0].rotation_euler = (0, math.radians(90), 0)
    for t in (0.2, 0.35, 0.5, 0.65, 0.8):
        x = -L / 2 + L * t
        r = R * (math.sin(math.pi * t) ** 0.6) * (1.0 - 0.15 * t)
        env.append(K.place(K.torus("Hoop", r + 0.03, 0.06, mat="BrassAged", seg=64, rseg=6), (x, 0, 0), (0, 90, 0)))
    for a in (0, 90, 180, 270):
        fin = K.prism("Fin", [(0, 0.5), (5.0, 0.5), (6.2, 5.0), (2.8, 5.0)], 0.18, (0, 0, 0), "Canvas", 0.02)
        fin.location = (-L / 2 + 1.0, 0, 0)
        fin.rotation_euler = (math.radians(a + 90), 0, 0)
        fin.location = (-L / 2 + 1.0 + 0, 0, 0)
        env.append(fin)
    for ob in env:
        ob.location.z += 9.0
    K.export("Airship_Envelope", env, A + "Airship_Envelope.nmmesh", kit=kit, ao=True, ao_dist=3.0,
             pivot_note="the gondola's pivot (see Airship_Hull): the envelope's axis is 9 m above it, along X; "
                        "the nose is +X")
    K.clear()
    # Gondola: a boat-shaped hull, walnut planks, brass rails, a cabin with windows.
    import bmesh
    bm = bmesh.new()
    n = 16
    rows = []
    for k in range(n + 1):
        t = k / n
        x = -6 + 12 * t
        w = 1.7 * math.sin(math.pi * (0.12 + 0.76 * t)) ** 0.7
        depth = 1.6 * math.sin(math.pi * (0.1 + 0.8 * t)) ** 0.5
        row = [bm.verts.new((x, -w, 0.0)), bm.verts.new((x, -w * 0.85, -depth * 0.6)),
               bm.verts.new((x, 0.0, -depth)), bm.verts.new((x, w * 0.85, -depth * 0.6)), bm.verts.new((x, w, 0.0))]
        rows.append(row)
    for k in range(n):
        for j in range(4):
            bm.faces.new([rows[k][j], rows[k + 1][j], rows[k + 1][j + 1], rows[k][j + 1]])
    bm.faces.new(rows[0][::-1])
    bm.faces.new(rows[-1])
    bm.faces.new([rows[k][0] for k in range(n + 1)] + [rows[k][4] for k in range(n, -1, -1)])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    hull = [K.obj_from_bm("Hull", bm, "Planks", sharp=math.radians(50))]
    hull.append(K.box_minmax("Cabin", (-3.0, -1.4, 0.0), (2.5, 1.4, 2.2), "Walnut", 0.04))
    hull.append(K.box_minmax("CabinRoof", (-3.2, -1.55, 2.2), (2.7, 1.55, 2.4), "Copper", 0.04))
    for x in (-2.2, -1.0, 0.2, 1.4):
        for y in (-1.41, 1.41):
            hull.append(K.box_minmax("Window", (x, y - 0.02, 0.9), (x + 0.8, y + 0.02, 1.7), "GlassClear", 0.0))
    hull.append(K.box_minmax("Bridge", (2.5, -1.2, 0.0), (3.6, 1.2, 1.4), "GlassFrosted", 0.04))
    for y in (-1.75, 1.75):
        hull.append(K.sweep("Rail", [(-5.4, y * 0.75, 1.0), (-3.5, y, 1.0), (3.5, y, 1.0), (5.4, y * 0.75, 1.0)], 0.035,
                            "BrassAged", seg=8))
    for (x, y) in ((-4.0, -1), (-4.0, 1), (4.0, -1), (4.0, 1), (0.0, -1), (0.0, 1)):
        hull.append(K.sweep("Cable", [(x, y * 1.4, 2.4), (x * 1.3, y * 3.5, 9.0 - 4.6)], 0.02, "SteelDark", seg=5))
    hull.append(K.place(K.lathe("Winch", [(0, -0.3), (0.35, -0.3), (0.35, 0.3), (0, 0.3)], mat="BrassBrushed", seg=24),
                        (-4.5, 0, -0.4), (90, 0, 0)))
    hull.append(K.sweep("WinchLine", [(-4.5, 0, -0.75), (-4.5, 0, -3.0)], 0.015, "SteelDark", seg=5))
    # Propeller mounts.
    for y in (-1, 1):
        hull.append(K.sweep("Outrigger", [(-1.0, y * 1.5, 1.6), (-1.0, y * 4.0, 2.2)], 0.12, "BrassAged", seg=10))
        hull.append(K.place(K.lathe("Nacelle", [(0, -0.9), (0.35, -0.7), (0.45, 0.0), (0.35, 0.8), (0.0, 1.0)],
                                    mat="Copper", seg=24), (-1.0, y * 4.0, 2.2), (0, 90, 0)))
    props = []
    def prop(name, at, axis_rot, radius):
        p = [K.place(K.lathe("Spinner", [(0, 0), (0.18, 0), (0.12, 0.3), (0, 0.4)], mat="BrassBrushed", seg=16),
                     (0, 0, 0), (0, -90, 0))]
        for k in range(4):
            b = K.box("Blade", (0.06, 0.28, radius), (0, 0, radius / 2 + 0.15), "Walnut", 0.02)
            holder = bpy.data.objects.new("PH", None)
            bpy.context.scene.collection.objects.link(holder)
            holder.rotation_euler = (math.radians(90 * k), 0, 0)
            b.rotation_euler = (0, math.radians(18), 0)
            b.parent = holder
            p.append(b)
        root = bpy.data.objects.new("PR", None)
        bpy.context.scene.collection.objects.link(root)
        root.location = at
        root.rotation_euler = [math.radians(a) for a in axis_rot]
        for ob in p:
            top = ob
            while top.parent is not None:
                top = top.parent
            if top is not root:
                top.parent = root
        props.append(K.part(name, p, A + f"Airship_{name}.nmmesh", at,
                            moves="spins about X (forward), ~8 turns/s", axis=(1, 0, 0), kit=kit))
    prop("PropLeft", (-1.95, -4.0, 2.2), (0, 0, 0), 1.3)
    prop("PropRight", (-1.95, 4.0, 2.2), (0, 0, 0), 1.3)
    prop("PropTail", (-L / 2 - 0.6, 0, 9.0), (0, 0, 0), 1.6)
    env_part = dict(name="Envelope", mesh="Content/" + A + "Airship_Envelope.nmmesh",
                    materials=["Content/Materials/Kit/Canvas.material.nmgraph",
                               "Content/Materials/Kit/BrassAged.material.nmgraph"],
                    offset_cm=[0, 0, 0], moves="rigid with the hull (a gentle bob is fine)")
    K.export("Airship_Hull", hull, A + "Airship_Hull.nmmesh", kit=kit, parts=[env_part] + props, ao_dist=1.5,
             pivot_note="the gondola's deck centre (its rails at 100 cm); nose toward +X; the envelope above "
                        "(9 m to its axis); the winch line hangs from (-450, -300, 0) cm for C6",
             notes="Fly it along +X; C1/C6 in the engine at about 1/1 scale (36 m envelope)")


def main():
    K.reset()
    for f in (catwalks, reactor, vault, locker, coolant_pump, beacon, freight_lift):
        K.clear()
        f()
    K.save_records("L3")
    K.RECORDS.clear()
    K.clear()
    airship()
    K.save_records("Airship")


main()
