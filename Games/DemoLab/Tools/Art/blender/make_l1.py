"""
make_l1.py -- the L1 Foothills kit (exterior): Content/Meshes/L1/.

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_l1.py

Modelled facing Blender -Y (the engine's -Z), pivots on the ground (see each asset's
"pivot" in assets.json). Moving parts are separate meshes with their own pivots at the
joint; the manifest gives their offsets from the asset's pivot and how they move.
"""

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kitlib as K      # noqa: E402

KIT = "L1"
D = "Meshes/L1/"


def facade_wall():
    """A 6 m bay of the lab's facade: rusticated granite, pilasters, a cornice, a round
    porthole window in a brass frame. Its front face is y = 0; it runs 1.2 m back (+Y)
    into the cliff."""
    o = []
    o.append(K.box_minmax("Wall", (-3.0, 0.0, 0.0), (3.0, 1.2, 8.0), "StoneBlocks", bevel=0.02))
    o.append(K.box_minmax("Plinth", (-3.0, -0.25, 0.0), (3.0, 0.2, 0.9), "StoneBlocks", bevel=0.04))
    for x in (-2.7, 2.7):
        o.append(K.box_minmax("Pilaster", (x - 0.3, -0.3, 0.9), (x + 0.3, 0.2, 7.2), "StoneBlocks", bevel=0.03))
        o.append(K.box_minmax("Capital", (x - 0.4, -0.4, 7.2), (x + 0.4, 0.2, 7.45), "StoneBlocks", bevel=0.03))
    o.append(K.box_minmax("Cornice", (-3.0, -0.45, 7.45), (3.0, 0.2, 7.75), "StoneBlocks", bevel=0.04))
    o.append(K.box_minmax("CorniceTop", (-3.0, -0.3, 7.75), (3.0, 0.2, 8.0), "StoneBlocks", bevel=0.03))
    # Porthole: a brass ring with bolts, frosted glass, a stone surround.
    o.append(K.place(K.torus("Surround", 1.05, 0.14, mat="StoneBlocks", seg=48, rseg=10), (0, -0.05, 4.2), (90, 0, 0)))
    o.append(K.place(K.torus("Frame", 0.86, 0.07, mat="BrassAged", seg=48, rseg=12), (0, -0.12, 4.2), (90, 0, 0)))
    o.append(K.place(K.cyl("Glass", 0.86, 0.04, mat="GlassFrosted", seg=48), (0, -0.06, 4.2), (90, 0, 0)))
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        o.append(K.box("Mullion", (0.06, 0.05, 1.7), (0, -0.09, 4.2), "BrassAged", 0.01, rot=(0, math.degrees(a), 0)))
    o += K.rivet_ring("Bolt", 0.86, 16, 0, mat="BrassAged", head=0.025)
    for b in o[-16:]:
        b.location = (b.location.x, -0.19, b.location.y + 4.2)
        b.rotation_euler = (math.radians(90), 0, 0)
    # A copper downpipe on the right.
    o.append(K.sweep("Downpipe", [(2.2, -0.12, 0.9), (2.2, -0.12, 7.4)], 0.07, "Copper", seg=14))
    for z in (2.0, 4.5, 6.8):
        o.append(K.box("Bracket", (0.2, 0.12, 0.05), (2.2, -0.04, z), "SteelDark", 0.01))
    K.export("Facade_Wall", o, D + "Facade_Wall.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre of the front face; the bay is 6 m wide and runs 1.2 m back (+Z) into the rock")


def facade_arch():
    """The blast door's portal: an 8 m stone arch framing a 4 x 5 m opening, keystone, a
    brass name plate and two lamps' brackets."""
    o = []
    outline = [(-4, 0), (4, 0), (4, 8), (-4, 8)]
    hole = [(-2.1, 0.0001)] + [(-2.1 + 0, 5.0)]
    arc = [(2.1 * math.cos(math.pi * k / 16), 5.0 + 1.0 * math.sin(math.pi * k / 16)) for k in range(17)]
    hole = [(-2.1, 0.0001), (2.1, 0.0001)] + arc + []
    o.append(K.prism("Arch", outline, 1.6, (0, 0.8, 0), "StoneBlocks", bevel=0.02, holes=[hole]))
    o.append(K.box("Keystone", (0.7, 0.4, 1.0), (0, -0.15, 6.35), "StoneBlocks", 0.04))
    o.append(K.box("Plate", (2.4, 0.06, 0.45), (0, -0.05, 7.2), "BrassAged", 0.01))
    for x in (-3.3, 3.3):
        o.append(K.box("Pier", (1.2, 0.5, 6.5), (x, -0.25, 3.25), "StoneBlocks", 0.04))
        o.append(K.cyl("Lamp", 0.16, 0.4, (x, -0.75, 5.2), "LampGlow", seg=16))
        o.append(K.cyl("LampCap", 0.22, 0.08, (x, -0.75, 5.44), "Copper", seg=16))
        o.append(K.sweep("Bracket", [(x, -0.5, 5.6), (x, -0.75, 5.6), (x, -0.75, 5.48)], 0.025, "SteelDark"))
    K.export("Facade_Arch", o, D + "Facade_Arch.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre of the opening's front face; opening 4.2 m wide, 5-6 m tall (round top); "
                        "BlastDoor fits it at the same pivot")


def blast_door():
    """Riveted red steel leaves that part sideways, a frame, and a brass cog in the middle
    that turns as they open. Opening 4.2 x 5 m."""
    frame = []
    frame.append(K.box_minmax("JambL", (-2.3, -0.1, 0), (-2.05, 0.6, 5.2), "PanelRed", 0.02))
    frame.append(K.box_minmax("JambR", (2.05, -0.1, 0), (2.3, 0.6, 5.2), "PanelRed", 0.02))
    frame.append(K.box_minmax("Lintel", (-2.3, -0.1, 5.0), (2.3, 0.6, 5.3), "PanelRed", 0.02))
    frame.append(K.box_minmax("Sill", (-2.3, -0.1, 0), (2.3, 0.6, 0.06), "SteelDark", 0.01))
    for x in (-2.18, 2.18):
        frame += K.rivet_row("R", (x, -0.11, 0.3), (x, -0.11, 4.9), 18, (90, 0, 0), "SteelDark", 0.02)
    leaves = []
    for side, name in ((-1, "LeafL"), (1, "LeafR")):
        lo, hi = (-2.05, 0.05) if side < 0 else (0.0, 2.05)
        p = [K.box_minmax(name, (lo, 0.05, 0.06), (hi, 0.35, 5.0), "PanelRed", 0.02)]
        for z in (0.8, 2.5, 4.2):
            p.append(K.box_minmax("Band", (lo + 0.02, -0.02, z - 0.12), (hi - 0.02, 0.05, z + 0.12), "SteelDark", 0.01))
            p += K.rivet_row("R", (lo + 0.12, -0.03, z), (hi - 0.12, -0.03, z), 9, (90, 0, 0), "BrassAged", 0.018)
        leaves.append(p)
    gear = []
    teeth = 24
    prof = []
    gear.append(K.place(K.lathe("Cog", [(0.0, 0.0), (0.7, 0.0), (0.7, 0.14), (0.25, 0.2), (0.0, 0.2)],
                                mat="BrassBrushed", seg=64), (0, 0, 0), (90, 0, 0)))
    for k in range(teeth):
        a = 360.0 * k / teeth
        t = K.box("Tooth", (0.12, 0.14, 0.12), (0, 0, 0), "BrassBrushed", 0.01)
        t.location = (0.76 * math.cos(math.radians(a)), -0.07, 0.76 * math.sin(math.radians(a)))
        t.rotation_euler = (0, -math.radians(a), 0)
        gear.append(t)
    gear.append(K.place(K.cyl("Hub", 0.18, 0.3, mat="SteelDark", seg=24), (0, -0.1, 0), (90, 0, 0)))
    for k in range(6):
        a = 60 * k
        gear.append(K.box("Spoke", (0.5, 0.06, 0.08), (0.38 * math.cos(math.radians(a)), -0.12,
                                                          0.38 * math.sin(math.radians(a))), "BrassAged", 0.01,
                          rot=(0, -a, 0)))
    for g in gear:
        g.location.z += 2.6
        g.location.y -= 0.02
    parts = [
        K.part("LeafL", leaves[0], D + "BlastDoor_LeafL.nmmesh", (0, 0, 0), moves="slides -X 2.1 m to open",
               axis=(-1, 0, 0), kit=KIT),
        K.part("LeafR", leaves[1], D + "BlastDoor_LeafR.nmmesh", (0, 0, 0), moves="slides +X 2.1 m to open",
               axis=(1, 0, 0), kit=KIT),
        K.part("Cog", gear, D + "BlastDoor_Cog.nmmesh", (0, -0.02, 2.6),
               moves="turns about Z (the door's normal) to unlock: 1 turn, then it rides with LeafL as the "
                     "leaves part", axis=(0, 1, 0), kit=KIT),
    ]
    K.export("BlastDoor", frame, D + "BlastDoor_Frame.nmmesh", ground=True, kit=KIT, parts=parts,
             pivot_note="bottom centre of the opening (as Facade_Arch); leaves closed",
             notes="C2: the cog turns, then the leaves slide apart; part offsets are from the frame's pivot")


def dock():
    """A 4 m dock deck segment (3 m wide): 25 cm boards with gaps on two stringers and
    cross beams. Pivot: deck top, centre of its near (-Y) end; segments chain along +Y."""
    o = []
    # One slab: the Planks texture's 25 cm boards run across the dock (along X).
    o.append(K.box_minmax("Deck", (-1.5, 0.0, -0.07), (1.5, 4.0, 0.0), "Planks", 0.01))
    for x in (-1.2, 1.2):
        o.append(K.box_minmax("Stringer", (x - 0.1, 0.0, -0.36), (x + 0.1, 4.0, -0.06), "WoodBeam", 0.015))
    for y in (0.5, 3.5):
        o.append(K.box_minmax("Beam", (-1.55, y - 0.12, -0.5), (1.55, y + 0.12, -0.36), "WoodBeam", 0.015))
    K.export("Dock_Deck", o, D + "Dock_Deck.nmmesh", ground=False, kit=KIT,
             pivot_note="deck top (y = 0), centre of the near end; 4 m long along +Z, 3 m wide; chain every 400 cm")
    K.clear()
    post = [K.cyl("Pile", 0.16, 4.5, (0, 0, -2.25 + 0.6), "WoodBeam", seg=16, bevel=0.01, swap_uv=True)]
    post.append(K.cyl("Cap", 0.18, 0.04, (0, 0, 0.62), "SteelDark", seg=16))
    for z in (0.3, 0.38, 0.46):
        post.append(K.place(K.torus("Rope", 0.17, 0.02, mat="Leather", seg=24, rseg=6), (0, 0, z)))
    K.export("Dock_Post", post, D + "Dock_Post.nmmesh", kit=KIT,
             pivot_note="at the deck's top on the pile's axis: 60 cm stand above the deck, 3.9 m below (into the water)")
    K.clear()
    b = [K.lathe("Bollard", [(0, 0), (0.2, 0), (0.2, 0.05), (0.13, 0.12), (0.11, 0.35), (0.17, 0.42), (0.17, 0.48),
                             (0.0, 0.5)], mat="SteelDark", seg=24)]
    b.append(K.cyl("Base", 0.26, 0.04, (0, 0, 0.02), "SteelDark", seg=24))
    K.export("Dock_Bollard", b, D + "Dock_Bollard.nmmesh", ground=True, kit=KIT, pivot_note="bottom centre")


def gatehouse():
    """A 4 x 4 m stone and riveted-steel gatehouse with a copper roof, a window and a door;
    on its road side a big brass crank wheel (it raises the barrier arm)."""
    o = []
    o.append(K.box_minmax("Base", (-2, -2, 0), (2, 2, 1.2), "StoneBlocks", 0.03))
    # Upper walls with openings: front has a window, right side a door.
    o.append(K.box_minmax("WallB", (-1.9, 1.7, 1.2), (1.9, 1.9, 3.6), "PanelGreen", 0.015))
    o.append(K.box_minmax("WallL", (-1.9, -1.9, 1.2), (-1.7, 1.9, 3.6), "PanelGreen", 0.015))
    o.append(K.prism("WallF", [(-1.9, 1.2), (1.9, 1.2), (1.9, 3.6), (-1.9, 3.6)], 0.2, (0, -1.8, 0), "PanelGreen",
                     0.01, holes=[[(-0.8, 1.9), (0.8, 1.9), (0.8, 3.0), (-0.8, 3.0)]]))
    o.append(K.prism("WallR", [(-1.9, 0.0), (1.9, 0.0), (1.9, 3.6), (-1.9, 3.6)], 0.2, (1.8, 0, 0), "PanelGreen",
                     0.01, rot=(0, 0, 90), holes=[[(-0.5, 1.2001), (0.5, 1.2001), (0.5, 3.2), (-0.5, 3.2)]]))
    o.append(K.box_minmax("Floor", (-1.9, -1.9, 1.15), (1.9, 1.9, 1.25), "Planks", 0.01))
    o.append(K.box_minmax("WinGlass", (-0.8, -1.82, 1.9), (0.8, -1.78, 3.0), "GlassClear", 0.0))
    o.append(K.box_minmax("WinSill", (-0.9, -2.05, 1.82), (0.9, -1.7, 1.9), "StoneBlocks", 0.01))
    for x in (-0.82, 0.0, 0.82):
        o.append(K.box_minmax("Mullion", (x - 0.03, -1.86, 1.9), (x + 0.03, -1.76, 3.0), "BrassAged", 0.005))
    o.append(K.box_minmax("Transom", (-0.82, -1.86, 2.42), (0.82, -1.76, 2.48), "BrassAged", 0.005))
    # Cornice and copper pyramid roof with a finial.
    o.append(K.box_minmax("Cornice", (-2.1, -2.1, 3.6), (2.1, 2.1, 3.8), "StoneBlocks", 0.03))
    roof = K.lathe("Roof", [(0, 3.8), (2.95, 3.8), (0.15, 5.3), (0, 5.35)], mat="Copper", seg=4, sharp=math.radians(20))
    roof.rotation_euler = (0, 0, math.radians(45))
    o.append(roof)
    o.append(K.lathe("Finial", [(0, 5.25), (0.08, 5.25), (0.08, 5.5), (0.12, 5.6), (0.0, 5.75)], mat="BrassAged", seg=12))
    # Steps to the door.
    for i in range(4):
        o.append(K.box_minmax("Step", (2.0, -0.6, 0), (2.35 + (3 - i) * 0.3, 0.6, 0.3 * (i + 1)), "StoneBlocks", 0.02))
    # Crank wheel mount on the road side (-X face).
    o.append(K.place(K.cyl("Mount", 0.2, 0.4, mat="SteelDark", seg=20), (-2.2, 0, 1.6), (0, 90, 0)))
    wheel = [K.place(K.torus("Rim", 0.55, 0.05, mat="BrassAged", seg=48, rseg=10), (0, 0, 0), (0, 90, 0))]
    wheel.append(K.place(K.cyl("Hub", 0.1, 0.16, mat="BrassBrushed", seg=20), (0, 0, 0), (0, 90, 0)))
    for k in range(6):
        a = 60 * k
        wheel.append(K.box("Spoke", (0.04, 0.05, 1.1), (0, 0, 0), "BrassAged", 0.01, rot=(a, 0, 0)))
    wheel.append(K.place(K.cyl("Handle", 0.035, 0.25, mat="WoodBeam", seg=12), (-0.12, 0.55, 0), (0, 90, 0)))
    for w in wheel:
        w.location.x -= 2.45
        w.location.z += 1.6
    arm = [K.box_minmax("Arm", (0.0, -0.06, -0.06), (5.0, 0.06, 0.06), "PaintRed", 0.01)]
    for k in range(5):
        arm.append(K.box_minmax("Stripe", (0.5 + k, -0.065, -0.065), (1.0 + k, 0.065, 0.065), "PanelCream", 0.008))
    arm.append(K.box_minmax("Counterweight", (-0.5, -0.12, -0.15), (0.0, 0.12, 0.15), "SteelDark", 0.02))
    for a_ in arm:
        a_.rotation_euler = (0, 0, math.radians(-90))
        a_.location = (a_.location.y - 2.4, -a_.location.x - 2.0, a_.location.z + 1.0)
    parts = [
        K.part("CrankWheel", wheel, D + "Gatehouse_Crank.nmmesh", (-2.45, 0, 1.6),
               moves="turns about X (out of the road-side wall): the player cranks it; 3 turns raise the barrier",
               axis=(1, 0, 0), kit=KIT),
        K.part("BarrierArm", arm, D + "Gatehouse_Barrier.nmmesh", (-2.4, -2.0, 1.0),
               moves="pivots up 80 degrees about X (the road's crossing axis) to open the road", axis=(1, 0, 0),
               kit=KIT),
    ]
    K.export("Gatehouse", o, D + "Gatehouse.nmmesh", ground=True, kit=KIT, parts=parts,
             pivot_note="bottom centre; 4 x 4 m footprint; the road runs along its -X side, the barrier across it "
                        "toward -Z")


def road_lamp():
    o = [K.lathe("Post", [(0, 0), (0.18, 0), (0.18, 0.08), (0.12, 0.15), (0.1, 0.5), (0.13, 0.6), (0.06, 0.75),
                          (0.055, 3.6), (0.09, 3.7), (0.0, 3.72)], mat="SteelDark", seg=16)]
    o.append(K.sweep("Arm", [(0, 0, 3.5), (0, 0, 3.9), (0, -0.25, 4.1), (0, -0.6, 4.1)], 0.03, "SteelDark", seg=10))
    o.append(K.lathe("Lantern", [(0, 3.6), (0.1, 3.6), (0.16, 3.7), (0.16, 3.98), (0.2, 4.0), (0.04, 4.12), (0, 4.14)],
                     mat="BrassAged", seg=6))
    o.append(K.lathe("Glass", [(0, 3.7), (0.15, 3.7), (0.15, 3.97), (0, 3.97)], mat="GlassFrosted", seg=6))
    o.append(K.lathe("Bulb", [(0, 3.74), (0.05, 3.76), (0.06, 3.84), (0.03, 3.9), (0, 3.91)], mat="LampGlow", seg=12))
    for ob in o[2:]:
        ob.location.y = -0.6
        ob.location.z = -0.05
    K.export("RoadLamp", o, D + "RoadLamp.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre of the post; the lantern hangs 60 cm toward -Z at 3.6-4.1 m (a point light "
                        "at (0, 385, -60) cm)")


def guard_rail():
    o = []
    for x in (-1.9, 0.0, 1.9):
        o.append(K.box_minmax("Post", (x - 0.06, -0.06, 0), (x + 0.06, 0.06, 0.95), "SteelDark", 0.01))
        o.append(K.cyl("Cap", 0.07, 0.04, (x, 0, 0.97), "BrassAged", seg=12))
    for z in (0.55, 0.9):
        o.append(K.sweep("Rail", [(-2.0, -0.08, z), (2.0, -0.08, z)], 0.035, "SteelDark", seg=12))
    o.append(K.box_minmax("Toe", (-2.0, -0.08, 0.05), (2.0, -0.05, 0.2), "SteelDark", 0.005))
    K.export("GuardRail", o, D + "GuardRail.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre; 4 m along X, the rail on its -Z face; chain every 400 cm (posts shared)")


def fence_post():
    o = [K.box_minmax("Post", (-0.06, -0.06, 0), (0.06, 0.06, 1.3), "WoodBeam", 0.012)]
    o.append(K.prism("Top", [(-0.07, 1.3), (0.07, 1.3), (0.0, 1.4)], 0.14, (0, 0, 0), "WoodBeam", 0.005))
    for z in (0.5, 0.85, 1.15):
        o.append(K.box("Staple", (0.03, 0.02, 0.02), (0, -0.065, z), "SteelDark", 0.003))
        o.append(K.sweep("Wire", [(-1.25, -0.07, z), (1.25, -0.07, z - 0.02)], 0.004, "SteelDark", seg=5))
    K.export("FencePost", o, D + "FencePost.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre; wire spans 2.5 m along X (chain every 250 cm)")


def signal_mast():
    """A 12 m lattice signal mast on a concrete pad: tapering legs, X bracing, a platform,
    a beacon lamp and a rotating anemometer on top."""
    o = [K.box_minmax("Pad", (-1.2, -1.2, 0), (1.2, 1.2, 0.3), "Concrete", 0.03)]
    top = 0.45
    for sx in (-1, 1):
        for sy in (-1, 1):
            o.append(K.sweep("Leg", [(sx * 0.9, sy * 0.9, 0.3), (sx * top, sy * top, 11.0)], 0.05, "SteelDark", seg=8))
    levels = [0.3 + i * 1.6 for i in range(7)]
    for i in range(len(levels) - 1):
        z0, z1 = levels[i], levels[i + 1]
        w0 = 0.9 + (top - 0.9) * (z0 - 0.3) / 10.7
        w1 = 0.9 + (top - 0.9) * (z1 - 0.3) / 10.7
        for (ax, ay), (bx, by) in [((-1, -1), (1, -1)), ((1, -1), (1, 1)), ((1, 1), (-1, 1)), ((-1, 1), (-1, -1))]:
            o.append(K.sweep("Brace", [(ax * w0, ay * w0, z0), (bx * w1, by * w1, z1)], 0.022, "SteelDark", seg=6))
            o.append(K.sweep("Brace", [(bx * w0, by * w0, z0), (ax * w1, ay * w1, z1)], 0.022, "SteelDark", seg=6))
            o.append(K.sweep("Ring", [(ax * w1, ay * w1, z1), (bx * w1, by * w1, z1)], 0.025, "SteelDark", seg=6))
    o.append(K.box_minmax("Platform", (-0.9, -0.9, 11.0), (0.9, 0.9, 11.08), "Grating", 0.0))
    for (ax, ay), (bx, by) in [((-1, -1), (1, -1)), ((1, -1), (1, 1)), ((1, 1), (-1, 1)), ((-1, 1), (-1, -1))]:
        o.append(K.sweep("Railing", [(ax * 0.9, ay * 0.9, 12.0), (bx * 0.9, by * 0.9, 12.0)], 0.025, "SteelDark",
                         seg=6))
        o.append(K.sweep("Stanchion", [(ax * 0.9, ay * 0.9, 11.08), (ax * 0.9, ay * 0.9, 12.0)], 0.025, "SteelDark",
                         seg=6))
    o.append(K.lathe("BeaconHousing", [(0, 11.08), (0.35, 11.08), (0.35, 11.3), (0.3, 11.3), (0.3, 11.75),
                                       (0.38, 11.8), (0.1, 12.05), (0, 12.1)], mat="BrassAged", seg=24))
    o.append(K.lathe("BeaconLens", [(0, 11.3), (0.29, 11.3), (0.31, 11.52), (0.29, 11.75), (0, 11.75)],
                     mat="BeaconLight", seg=24))
    o.append(K.cyl("Spindle", 0.025, 1.2, (0, 0, 12.6), "SteelDark", seg=8))
    cups = [K.cyl("Hub", 0.06, 0.1, (0, 0, 0), "BrassAged", seg=12)]
    for k in range(3):
        a = 120 * k
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        cups.append(K.sweep("CupArm", [(0, 0, 0), (0.45 * ca, 0.45 * sa, 0)], 0.012, "SteelDark", seg=6))
        cup = K.lathe("Cup", [(0, 0), (0.09, 0.02), (0.11, 0.1), (0.1, 0.1)], mat="Copper", seg=16)
        cup.location = (0.45 * ca, 0.45 * sa, 0)
        cup.rotation_euler = (0, math.radians(90), math.radians(a + 90))
        cups.append(cup)
    for c in cups:
        c.location.z += 13.2
    parts = [K.part("Anemometer", cups, D + "SignalMast_Anemometer.nmmesh", (0, 0, 13.2),
                    moves="spins about Y (up), ~1 turn/s in the wind", axis=(0, 0, 1), kit=KIT)]
    K.export("SignalMast", o, D + "SignalMast.nmmesh", ground=True, kit=KIT, parts=parts, ao_dist=0.8,
             pivot_note="bottom centre of the pad; beacon lens at 11.3-11.75 m (BeaconLight: blink it with "
                        "EmissiveIntensity, add a red point light at (0, 1150, 0) cm)")


def rocks():
    """Boulders: sculpted (faceted hull, chipped cells, fracture cracks, rills, softened
    tops), box-mapped in metres so the Rock material's textures and its moss on the tops
    line up whatever way they are turned."""
    specs = [("Rock_A", (0.8, 0.6, 0.5), 11, 3000), ("Rock_B", (1.6, 1.2, 0.9), 12, 5000),
             ("Rock_C", (2.6, 2.0, 1.4), 13, 7000), ("Rock_D", (1.0, 0.9, 1.3), 14, 4500),
             ("Rock_E", (3.5, 2.6, 2.2), 15, 9000)]
    for name, size, seed, tris in specs:
        K.clear()
        r = K.sculpted_rock(name, size, seed, mat="Rock", target_tris=tris)
        K.export(name, [r], D + f"{name}.nmmesh", ground=True, kit=KIT, ao_dist=0.5 * max(size),
                 pivot_note="bottom centre, sunk ~6% into the ground; rotate freely about Y")


def cliffs():
    """Cliff pieces: the same sculpting with bedding -- soft beds weathered back, hard ones
    lipped, dipping a few degrees, split by joints. The Cliff_Wall pieces have a flat-ish
    front (-Z) to stand behind the lab facade and along the plaza."""
    specs = [("Cliff_A", (9.0, 4.0, 10.0), 21, 0.55, 0.8, False, 26000),
             ("Cliff_B", (7.0, 3.5, 7.0), 22, 0.5, 0.65, False, 20000),
             ("Cliff_C", (12.0, 5.0, 5.0), 23, 0.45, 0.6, False, 22000),
             ("Cliff_Wall_A", (16.0, 5.0, 12.0), 24, 0.6, 0.75, True, 36000),
             ("Cliff_Wall_B", (10.0, 4.5, 14.0), 25, 0.6, 0.9, True, 30000)]
    for name, size, seed, strata, thick, front, tris in specs:
        K.clear()
        r = K.sculpted_rock(name, size, seed, mat="Cliff", voxel=min(size) / 40.0, target_tris=tris, facets=30,
                            fracture=0.7, strata=strata, strata_thick=thick, dip=5.0, flat_front=front,
                            base_sink=0.03)
        note = ("bottom centre; a stratified cliff piece: overlap them, rotate and sink into the terrain"
                if not front else "bottom centre; a cliff wall, its flatter face toward -Z: stand it behind "
                                  "Facade_Wall / Facade_Arch (their backs run 120-160 cm in) and overlap pieces")
        K.export(name, [r], D + f"{name}.nmmesh", ground=False, kit=KIT, ao_dist=2.0, pivot_note=note)


def sea():
    """The sea: a 200 m tile, 1.5625 m quads (enough for the Sea material's swell, World
    Position Offset), chainable; a 4 km far plane for the horizon; and a 20 m foam strip
    for the shore line (V 0 at the sand, 1 out to sea)."""
    import bmesh
    from mathutils import Vector

    def grid(name, half, n, mat, z=0.0):
        bm = bmesh.new()
        bmesh.ops.create_grid(bm, x_segments=n, y_segments=n, size=half)
        for v in bm.verts:
            v.co.z = z
        return K.obj_from_bm(name, bm, mat, smooth=False)
    K.export("Sea_Plane", [grid("Sea", 100.0, 128, "Sea")], D + "Sea_Plane.nmmesh", ao=False, kit=KIT,
             pivot_note="the water level at the tile's centre; 200 x 200 m, chain every 20000 cm; the Sea "
                        "material swells it about +-45 cm (Max Offset 60)")
    K.clear()
    K.export("Sea_Far", [grid("SeaFar", 2000.0, 16, "Sea", -0.05)], D + "Sea_Far.nmmesh", ao=False, kit=KIT,
             pivot_note="4 x 4 km, 5 cm under the water level: under and round the Sea_Plane tiles to the horizon")
    K.clear()
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    nx, ny = 20, 6
    vs = [[bm.verts.new((-10.0 + 20.0 * i / nx, 6.0 * j / ny, 0.02)) for i in range(nx + 1)] for j in range(ny + 1)]
    for j in range(ny):
        for i in range(nx):
            f = bm.faces.new([vs[j][i], vs[j][i + 1], vs[j + 1][i + 1], vs[j + 1][i]])
            for lp in f.loops:
                lp[uv].uv = ((lp.vert.co.x + 10.0) / 6.0, lp.vert.co.y / 6.0)
    strip = K.obj_from_bm("Foam", bm, "SeaFoam", smooth=False, keep_uv=True)
    K.export("Sea_FoamStrip", [strip], D + "Sea_FoamStrip.nmmesh", ao=False, kit=KIT,
             pivot_note="the middle of its shore edge, 2 cm over the water; 20 m along X, 6 m out to sea (+Z); "
                        "lay it along the beach (V runs from the sand out), chain every 2000 cm")


def steam_vent():
    o = [K.cyl("Collar", 0.35, 0.3, (0, 0, 0.15), "SteelDark", seg=24, bevel=0.01)]
    o.append(K.place(K.torus("Lip", 0.33, 0.04, mat="BrassAged", seg=32, rseg=8), (0, 0, 0.3)))
    o.append(K.cyl("Grate", 0.31, 0.02, (0, 0, 0.29), "Grating", seg=24))
    for k in range(8):
        a = 45 * k
        o.append(K.box("Bolt", (0.04, 0.04, 0.03), (0.42 * math.cos(math.radians(a)), 0.42 * math.sin(math.radians(a)),
                                                    0.015), "BrassAged", 0.005))
    o.append(K.cyl("Flange", 0.46, 0.03, (0, 0, 0.015), "SteelDark", seg=24))
    K.export("SteamVent", o, D + "SteamVent.nmmesh", ground=True, kit=KIT,
             pivot_note="bottom centre; emit steam particles from (0, 30, 0) cm upward")


def balustrade():
    """The plaza's edge: a 4 m run of stone balustrade over a retaining wall's coping -- a
    plinth, eight turned balusters, a moulded rail, a pier at the run's start (chain every
    400 cm; the next run's pier closes it) -- and the retaining wall face below, 3 m of
    rusticated stone with a weathered batter, to sit into the slope under the edge."""
    o = [K.box_minmax("Wall", (-2.0, -0.05, -3.0), (2.0, 0.55, 0.0), "StoneBlocks", bevel=0.02)]
    o.append(K.box_minmax("Batter", (-2.0, -0.35, -3.0), (2.0, 0.0, -2.2), "StoneBlocks", bevel=0.04))
    o.append(K.box_minmax("Coping", (-2.0, -0.12, 0.0), (2.0, 0.55, 0.12), "StoneBlocks", bevel=0.02))
    o.append(K.box_minmax("Plinth", (-2.0, -0.02, 0.12), (2.0, 0.34, 0.27), "StoneBlocks", bevel=0.02))
    prof = [(0.0, 0.0), (0.075, 0.0), (0.075, 0.04), (0.05, 0.07), (0.045, 0.16), (0.085, 0.3), (0.09, 0.36),
            (0.06, 0.45), (0.04, 0.5), (0.04, 0.53), (0.07, 0.56), (0.07, 0.6), (0.0, 0.6)]
    for k in range(8):
        x = -1.5 + k * 0.43
        o.append(K.lathe("Baluster", prof, (x, 0.16, 0.27), "StoneBlocks", seg=14))
    o.append(K.box_minmax("Rail", (-2.0, -0.04, 0.87), (2.0, 0.36, 0.97), "StoneBlocks", bevel=0.02))
    o.append(K.box_minmax("RailCap", (-2.0, -0.07, 0.97), (2.0, 0.39, 1.04), "StoneBlocks", bevel=0.025))
    o.append(K.box_minmax("Pier", (-2.0, -0.08, 0.12), (-1.6, 0.4, 1.12), "StoneBlocks", bevel=0.02))
    o.append(K.box_minmax("PierCap", (-2.05, -0.13, 1.12), (-1.55, 0.45, 1.2), "StoneBlocks", bevel=0.02))
    o.append(K.place(K.sphere("Finial", 0.11, mat="BrassAged", seg=16, rings=8), (-1.8, 0.16, 1.3)))
    K.export("Balustrade", o, D + "Balustrade.nmmesh", ground=False, kit=KIT, ao_dist=0.6,
             pivot_note="the coping's top at its centre line's -Z edge (y = 0 is the plaza's surface); 4 m along X, "
                        "the plaza on +Z; the retaining wall runs 3 m down; chain every 400 cm")


def main():
    K.run_kit(KIT, [facade_wall, facade_arch, blast_door, dock, gatehouse, road_lamp, guard_rail, fence_post,
                    signal_mast, rocks, cliffs, sea, steam_vent, balustrade])


main()
