#!/usr/bin/env python3
"""
make_toon_arena.py -- writes the Toon Arena: Content/Levels/ToonArena.nmworld
and its entities, Content/Meshes/Toony/ and Content/PostProcess/Toony/, and
the navigation test fixture Engine/Modules/AICore/Tests/Data/ToonArena.obj.

AICore's demo world (Engine/Modules/AICore/README.md, "The Toon Arena"): two
teams of crash dummies fight across a 60 x 60 m candy-coloured yard. Every
feature is a navigation or AI case:

    bases        Red in the south-west corner, Blue in the north-east: flags,
                 crates for cover, three spawn points each
    plaza        a raised square (1.2 m) in the middle: a gentle ramp up its
                 west side, 30 cm steps up its east, a too-steep slope on its
                 north (no path), a sheer ledge on its south (no path)
    trench       a pit across the south-west, crossed by a plank bridge -- the
                 pit floor is an island nothing reaches
    mud          a High Cost patch in the south-east (Nav Modifier, cost 6):
                 paths go round it
    cover        low walls (1 m: seen over, shot over) and high ones (2.6 m:
                 line of sight cut), crate stacks
    maze         a grid of pillars between the plaza and Blue's base: crowd
                 avoidance in narrow lanes
    doors        two doors in a wall across the north-west, which the mode
                 opens and shuts: Nav Obstacles, cut from the navmesh while shut
    patrol       Patrol_N markers round the yard, spawn markers Spawn_<Team>_N

Collision is JoltPhysics' (Config/DefaultEngine.ini [Modules]): static
triangle meshes on layer 1; the doors kinematic boxes. A Nav Mesh Volume
covers the yard: Build Navigation in the editor bakes ToonArena.nmnav beside
the level, and without one the level builds its own as it starts.

Run after make_toony_textures.py and make_toony_materials.py, from the
repository root:

    python Games/WorldsLab/Tools/Toony/make_toon_arena.py
"""

import math
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import toony_kit as kit          # noqa: E402
from toony_kit import nm, toy_box, toy_cylinder, ramp, shade   # noqa: E402
from make_toony_textures import SUN_ELEVATION, SUN_AZIMUTH      # noqa: E402

GAME_DIR = kit.GAME_DIR
SCOPE = "ToonArena"
WORLD = GAME_DIR / "Content" / "Levels" / "ToonArena.nmworld"
FOLDER = GAME_DIR / "Content" / "Levels" / "ToonArena"
NAVMESH = "Content/Levels/ToonArena/ToonArena.nmnav"
MESHES = GAME_DIR / "Content" / "Meshes" / kit.STYLE
MESH_PATH = f"Content/Meshes/{kit.STYLE}/"
POST = GAME_DIR / "Content" / "PostProcess" / kit.STYLE
STACK = f"Content/PostProcess/{kit.STYLE}/Toony.pp.nmgraph"
SKY = f"Content/Textures/{kit.STYLE}/Sky_HDR.nmtexture"
FIXTURE = kit.REPO_DIR / "Engine" / "Modules" / "AICore" / "Tests" / "Data" / "ToonArena.obj"

# --- The yard (cm) -------------------------------------------------------------------------------
HALF = 3000.0                    # inside faces of the outer wall
WALL_T, WALL_H = 60.0, 320.0
GROUND_T = 340.0                 # the ground is a slab this deep: the trench's walls
PLAZA = 800.0                    # half its side
PLAZA_H = 120.0
TRENCH = (-3000.0, -500.0, -1300.0, -900.0)   # x0, x1, z0, z1
PIT_Y = -300.0
BRIDGE_X = (-2350.0, -2050.0)
MUD = (900.0, 2300.0, -2300.0, -1500.0)
MUD_COST = 6.0
MAZE = (900.0, 2100.0, 1150.0, 2150.0)
DIVIDER_Z = 1100.0
DOORS_X = (-2400.0, -1400.0)
DOOR_W, DOOR_H = 240.0, 280.0

LAYER_WORLD = 1


def guid(name):
    return nm.guid(SCOPE, name)


# ==============================================================================================
# Entities
# ==============================================================================================

class Level:
    def __init__(self):
        self.entities, self.names = [], set()
        self.meshes = {}
        self.collision = []          # MeshBuilders whose triangles walk (the fixture)

    def unique(self, name):
        base, k = name, 1
        while name in self.names:
            k += 1
            name = f"{base}_{k}"
        self.names.add(name)
        return name

    def add(self, name, components, mobility="Static"):
        name = self.unique(name)
        self.entities.append((name, nm.entity_xml(guid(name), name, components, mobility)))
        return name

    def mesh(self, key, builder, collide=True, casts=True, position=(0, 0, 0), materials=()):
        builder.write(MESHES / f"{key}.nmmesh", lightmap=False)
        path = MESH_PATH + f"{key}.nmmesh"
        components = [("Render", render_xml(path, casts=casts, materials=materials)),
                      ("Transform", nm.transform_xml(position))]
        if collide:
            components.append(("Rigid Body", rigid_body_xml("Static")))
            self.collision.append((builder, position))
        return self.add(key, components)

    def marker(self, name, position, yaw=0.0):
        return self.add(name, [("Transform", nm.transform_xml(position, (0, yaw, 0)))], mobility=None)

    def write(self):
        FOLDER.mkdir(parents=True, exist_ok=True)
        for stale in list(FOLDER.glob("*.nmentity")) + list(FOLDER.glob("*.nmprefabinst")):
            stale.unlink()
        manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n"]
        for name, text in self.entities:
            (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
            manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
        manifest.append("</World>\n")
        WORLD.write_text("".join(manifest), newline="\n")


def render_xml(mesh, casts=True, materials=()):
    lines = ["  <Component Type=\"Render\" Version=\"5\">\n", "    <Visible Value=\"true\" />\n"]
    lines += [f"    <Material{slot} Value=\"{materials[slot] if slot < len(materials) else ''}\" />\n" for slot in range(8)]
    lines.append(f"    <Mesh Value=\"{mesh}\" />\n")
    lines.append("    <ParameterCount Value=\"0\" />\n")
    lines.append(f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n")
    lines.append("    <CastInsetShadow Value=\"false\" />\n")
    lines.append("    <Occluder Value=\"false\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def rigid_body_xml(motion, shape=0, source=0, layer=LAYER_WORLD):
    motion_value = {"Dynamic": 0, "Kinematic": 1, "Static": 2}[motion]
    return nm.component_xml("Rigid Body", 1, [
        ("Enabled", True), ("Motion", motion_value), ("Shape", shape), ("ShapeSource", source), ("Mass", 0),
        ("Friction", 0.6), ("Restitution", 0), ("LinearDamping", 0.05), ("AngularDamping", 0.05),
        ("GravityFactor", 1), ("Continuous", False), ("Sensor", False), ("AllowSleeping", True),
        ("Layer", layer), ("Mask", -1)]).replace(
        "  </Component>\n",
        "    <InitialLinearVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n"
        "    <InitialAngularVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n  </Component>\n")


def bounds_xml(lo, hi, static=True):
    return ("  <Component Type=\"Bounds\" Version=\"1\">\n"
            f"    <Local MinX=\"{nm.num(lo[0])}\" MinY=\"{nm.num(lo[1])}\" MinZ=\"{nm.num(lo[2])}\" "
            f"MaxX=\"{nm.num(hi[0])}\" MaxY=\"{nm.num(hi[1])}\" MaxZ=\"{nm.num(hi[2])}\" />\n"
            f"    <Static Value=\"{'true' if static else 'false'}\" />\n"
            "  </Component>\n")


def vec_xml(name, v):
    return f"    <{name} X=\"{nm.num(v[0])}\" Y=\"{nm.num(v[1])}\" Z=\"{nm.num(v[2])}\" />\n"


# ==============================================================================================
# The yard
# ==============================================================================================

def checker(x, z, a, b, size=400.0):
    return a if (int(math.floor(x / size)) + int(math.floor(z / size))) % 2 == 0 else b


def make_ground(level):
    mb = nm.MeshBuilder("Ground")
    tx0, tx1, tz0, tz1 = TRENCH
    pieces = [(-HALF - WALL_T, HALF + WALL_T, tz1, HALF + WALL_T),       # north of the trench
              (-HALF - WALL_T, HALF + WALL_T, -HALF - WALL_T, tz0),      # south of it
              (tx1, HALF + WALL_T, tz0, tz1)]                            # east of its end
    # Tiles 4 m a side in two greens, so the yard reads as a lawn.
    step = 400.0
    for x0, x1, z0, z1 in pieces:
        x = x0
        while x < x1 - 1e-3:
            xe = min(x1, math.floor(x / step) * step + step)
            z = z0
            while z < z1 - 1e-3:
                ze = min(z1, math.floor(z / step) * step + step)
                colour = checker(x + 1, z + 1, kit.GRASS, kit.GRASS_DARK)
                mb.polygon(kit.PAINT, [(x, 0, z), (xe, 0, z), (xe, 0, ze), (x, 0, ze)], normal=(0, 1, 0),
                           colours=[colour + (1.0,)] * 4)
                z = ze
            x = xe
    # The trench: its walls and the pit floor (water-blue, an island).
    for x, sign in ((tx1, 1.0),):
        mb.polygon(kit.PAINT, [(x, PIT_Y, tz0), (x, PIT_Y, tz1), (x, 0, tz1), (x, 0, tz0)], normal=(-sign, 0, 0),
                   colours=[shade(kit.SAND, 0.7) + (1,), shade(kit.SAND, 0.7) + (1,), kit.SAND + (1,), kit.SAND + (1,)])
    for z, nz in ((tz0, 1.0), (tz1, -1.0)):
        mb.polygon(kit.PAINT, [(tx0, PIT_Y, z), (tx1, PIT_Y, z), (tx1, 0, z), (tx0, 0, z)], normal=(0, 0, nz),
                   colours=[shade(kit.SAND, 0.7) + (1,), shade(kit.SAND, 0.7) + (1,), kit.SAND + (1,), kit.SAND + (1,)])
    mb.polygon(kit.PAINT, [(tx0, PIT_Y, tz0), (tx1, PIT_Y, tz0), (tx1, PIT_Y, tz1), (tx0, PIT_Y, tz1)], normal=(0, 1, 0),
               colours=[kit.WATER + (1.0,)] * 4)
    level.mesh("Ground", mb, casts=False)


def make_walls(level):
    mb = nm.MeshBuilder("OuterWall")
    h, t = WALL_H, WALL_T
    stripes = (kit.STONE, kit.STONE_DARK)
    # Each side in 6 m blocks, alternating shades: chunky toy bricks.
    for k in range(10):
        a, b = -HALF + k * 600.0, -HALF + (k + 1) * 600.0
        c = stripes[k % 2]
        toy_box(mb, (a, 0, HALF), (b, h, HALF + t), c, bevel=12)
        toy_box(mb, (a, 0, -HALF - t), (b, h, -HALF), c, bevel=12)
        toy_box(mb, (HALF, 0, a), (HALF + t, h, b), c, bevel=12)
        toy_box(mb, (-HALF - t, 0, a), (-HALF, h, b), c, bevel=12)
    for x in (-HALF - t, HALF):
        for z in (-HALF - t, HALF):
            toy_box(mb, (x - 20, 0, z - 20), (x + t + 20, h + 60, z + t + 20), kit.PURPLE, bevel=14)
    level.mesh("OuterWall", mb)


def make_plaza(level):
    mb = nm.MeshBuilder("Plaza")
    p, top = PLAZA, PLAZA_H
    toy_box(mb, (-p, 0, -p), (p, top, p), kit.SAND, bevel=10, top=shade(kit.SAND, 1.04))
    # A tile ring on top, a shade lighter, for the outline to catch.
    for k in range(8):
        a = -p + 100 + k * 175
        mb.polygon(kit.PAINT, [(a, top + 0.5, -p + 100), (a + 150, top + 0.5, -p + 100), (a + 150, top + 0.5, -p + 130),
                               (a, top + 0.5, -p + 130)], normal=(0, 1, 0), colours=[kit.WHITE + (1,)] * 4)
    # West: the ramp, 11 m at ~6 degrees.
    ramp(mb, -p - 1100, -p, -300, 300, 0.0, top, kit.WOOD, side_colour=kit.WOOD_DARK)
    # East: four 30 cm steps.
    for k in range(4):
        x0 = p + k * 60.0
        toy_box(mb, (x0, 0, -300), (x0 + 60.0, top - 30.0 * (k + 1), 300), kit.STONE, bevel=4)
    # North: a slope too steep to walk (56 degrees).
    ramp(mb, -300, 300, p, p + 80, top, 0.0, kit.ORANGE, along="z")
    # On top: a low block in the middle, a post at each corner.
    toy_box(mb, (-110, top, -50), (110, top + 100, 50), kit.STONE_DARK, bevel=10)
    for sx in (-1, 1):
        for sz in (-1, 1):
            toy_cylinder(mb, (sx * (p - 120), sz * (p - 120)), 45, top, top + 260, kit.PURPLE, segments=10)
    level.mesh("Plaza", mb)


def make_trench_bridge(level):
    mb = nm.MeshBuilder("Bridge")
    x0, x1 = BRIDGE_X
    tz0, tz1 = TRENCH[2], TRENCH[3]
    toy_box(mb, (x0, -14, tz0 - 60), (x1, 10, tz1 + 60), kit.WOOD, bevel=3)
    for k in range(9):          # planks: darker seams
        z = tz0 - 60 + k * 60
        mb.polygon(kit.PAINT, [(x0 + 4, 10.4, z), (x1 - 4, 10.4, z), (x1 - 4, 10.4, z + 6), (x0 + 4, 10.4, z + 6)],
                   normal=(0, 1, 0), colours=[kit.WOOD_DARK + (1,)] * 4)
    for x in (x0, x1 - 12):     # rails
        toy_box(mb, (x, 10, tz0 - 60), (x + 12, 70, tz1 + 60), kit.RED, bevel=3)
    level.mesh("Bridge", mb)


def make_cover(level):
    mb = nm.MeshBuilder("Cover")
    low = [((-2100, 1700), 300, 0), ((-1300, 2300), 300, 90), ((-700, 1500), 260, 0),
           ((0, -1700), 320, 0), ((-1100, -2300), 300, 90), ((1500, -800), 300, 90), ((-1400, 650), 260, 90),
           ((1500, 200), 300, 0), ((400, 2300), 300, 0)]
    for (cx, cz), length, yaw in low:
        hx, hz = (length / 2, 22) if yaw == 0 else (22, length / 2)
        toy_box(mb, (cx - hx, 0, cz - hz), (cx + hx, 100, cz + hz), kit.STONE, bevel=8, top=kit.WHITE)
    high = [((-1600, 2000), 400, 90), ((1900, -400), 400, 0), ((-500, -2200), 400, 0), ((2300, 800), 300, 90),
            ((-2400, -300), 400, 0)]
    for (cx, cz), length, yaw in high:
        hx, hz = (length / 2, 30) if yaw == 0 else (30, length / 2)
        toy_box(mb, (cx - hx, 0, cz - hz), (cx + hx, 260, cz + hz), kit.BLUE if cx > 0 else kit.RED, bevel=12,
                top=kit.YELLOW)
    # Crate stacks by each base.
    for (cx, cz), colour in (((-1900, -2500), kit.WOOD), ((-2500, -1800), kit.WOOD), ((1900, 2500), kit.WOOD),
                             ((2500, 1800), kit.WOOD), ((300, -500 - PLAZA), kit.ORANGE)):
        toy_box(mb, (cx - 60, 0, cz - 60), (cx + 60, 120, cz + 60), colour, bevel=8, top=shade(colour, 1.1))
        toy_box(mb, (cx + 60, 0, cz - 50), (cx + 160, 100, cz + 50), shade(colour, 0.92), bevel=8)
        toy_box(mb, (cx - 40, 120, cz - 40), (cx + 40, 200, cz + 40), shade(colour, 1.05), bevel=6)
    level.mesh("Cover", mb)


def make_maze(level):
    mb = nm.MeshBuilder("Maze")
    x0, x1, z0, z1 = MAZE
    row = 0
    z = z0
    while z <= z1 + 1e-3:
        x = x0 + (120.0 if row % 2 else 0.0)
        while x <= x1 + 1e-3:
            colour = (kit.YELLOW, kit.ORANGE, kit.PURPLE)[(row + int(x / 240)) % 3]
            toy_box(mb, (x - 35, 0, z - 35), (x + 35, 280, z + 35), colour, bevel=8, top=kit.WHITE)
            x += 240.0
        z += 250.0
        row += 1
    level.mesh("Maze", mb)


def make_divider(level):
    """A wall across the north-west with two door gaps; the doors are their own entities."""
    mb = nm.MeshBuilder("Divider")
    edges = [-HALF]
    for dx in DOORS_X:
        edges += [dx - DOOR_W / 2, dx + DOOR_W / 2]
    edges.append(-PLAZA - 100.0)    # its east end, short of the plaza: the way round
    for k in range(0, len(edges), 2):
        a, b = edges[k], edges[k + 1]
        toy_box(mb, (a, 0, DIVIDER_Z - 20), (b, 300, DIVIDER_Z + 20), kit.STONE_DARK, bevel=8, top=kit.PURPLE)
    for dx in DOORS_X:          # lintels over the doors
        toy_box(mb, (dx - DOOR_W / 2 - 20, 300, DIVIDER_Z - 30), (dx + DOOR_W / 2 + 20, 340, DIVIDER_Z + 30), kit.PURPLE, bevel=6)
    level.mesh("Divider", mb)

    door = nm.MeshBuilder("Door")
    hw, hh = DOOR_W / 2 - 10, DOOR_H / 2
    toy_box(door, (-hw, -hh, -15), (hw, hh, 15), kit.ORANGE, bevel=6, top=kit.YELLOW, skip_bottom=False)
    for y in (-hh / 2, hh / 2):
        door.polygon(kit.PAINT, [(-hw + 20, y - 8, -15.5), (hw - 20, y - 8, -15.5), (hw - 20, y + 8, -15.5), (-hw + 20, y + 8, -15.5)],
                     normal=(0, 0, -1), colours=[kit.WOOD_DARK + (1,)] * 4)
        door.polygon(kit.PAINT, [(-hw + 20, y - 8, 15.5), (hw - 20, y - 8, 15.5), (hw - 20, y + 8, 15.5), (-hw + 20, y + 8, 15.5)],
                     normal=(0, 0, 1), colours=[kit.WOOD_DARK + (1,)] * 4)
    door.write(MESHES / "Door.nmmesh", lightmap=False)
    for k, dx in enumerate(DOORS_X):
        obstacle = ("  <Component Type=\"Nav Obstacle\" Version=\"1\">\n" + vec_xml("HalfExtents", (DOOR_W / 2, DOOR_H / 2, 30))
                    + "    <Shape Value=\"0\" />\n    <Enabled Value=\"true\" />\n  </Component>\n")
        level.add(f"Door_{k + 1}", [
            ("Render", render_xml(MESH_PATH + "Door.nmmesh")),
            ("Bounds", bounds_xml((-hw, -hh, -15), (hw, hh, 15), static=False)),
            ("Rigid Body", rigid_body_xml("Kinematic", shape=1, source=1)),
            ("Nav Obstacle", obstacle),
            ("Transform", nm.transform_xml((dx, DOOR_H / 2, DIVIDER_Z)))], mobility="Dynamic")


def make_decor(level):
    """What nothing walks on: the mud's colour, the bases' pads and flags, flowers."""
    mb = nm.MeshBuilder("Decor")
    x0, x1, z0, z1 = MUD
    mb.polygon(kit.PAINT, [(x0, 1, z0), (x1, 1, z0), (x1, 1, z1), (x0, 1, z1)], normal=(0, 1, 0), colours=[kit.MUD + (1,)] * 4)
    for k, (px, pz, r) in enumerate(((1200, -1800, 160), (1700, -2000, 220), (2050, -1700, 140), (1450, -2150, 120))):
        ring = [(px + math.sin(a) * r * (1.0 + 0.15 * math.sin(3 * a + k)), 1.5, pz + math.cos(a) * r) for a in
                [2 * math.pi * i / 14 for i in range(14)]]
        mb.polygon(kit.PAINT, list(reversed(ring)), normal=(0, 1, 0), colours=[shade(kit.MUD, 0.75) + (1,)] * 14)
    for (cx, cz), team, light in (((-2400, -2400), kit.RED, kit.RED_LIGHT), ((2400, 2400), kit.BLUE, kit.BLUE_LIGHT)):
        mb.polygon(kit.PAINT, [(cx - 450, 1, cz - 450), (cx + 450, 1, cz - 450), (cx + 450, 1, cz + 450), (cx - 450, 1, cz + 450)],
                   normal=(0, 1, 0), colours=[light + (1,)] * 4)
        mb.polygon(kit.PAINT, [(cx - 300, 2, cz - 300), (cx + 300, 2, cz - 300), (cx + 300, 2, cz + 300), (cx - 300, 2, cz + 300)],
                   normal=(0, 1, 0), colours=[team + (1,)] * 4)
        fx, fz = cx + (-380 if cx < 0 else 380), cz + (-380 if cz < 0 else 380)
        toy_cylinder(mb, (fx, fz), 8, 0, 520, kit.WHITE, segments=6)
        for side in (-1, 1):
            mb.polygon(kit.PAINT, [(fx, 380, fz), (fx + 160 * (1 if cx < 0 else -1), 420, fz), (fx, 500, fz)],
                       normal=(0, 0, side), colours=[team + (1,)] * 3)
    for k in range(40):        # flowers: little spots of colour
        a = k * 2.399963
        r = 600 + (k * 397) % 2200
        fx, fz = math.sin(a) * r, math.cos(a) * r
        if abs(fx) < PLAZA + 100 and abs(fz) < PLAZA + 100:
            continue
        if TRENCH[0] < fx < TRENCH[1] + 100 and TRENCH[2] - 100 < fz < TRENCH[3] + 100:
            continue
        colour = (kit.YELLOW, kit.WHITE, kit.RED_LIGHT, kit.PURPLE)[k % 4]
        petal = [(fx + math.sin(b) * 14, 1.2, fz + math.cos(b) * 14) for b in [2 * math.pi * i / 6 for i in range(6)]]
        mb.polygon(kit.PAINT, list(reversed(petal)), normal=(0, 1, 0), colours=[colour + (1,)] * 6)
    level.mesh("Decor", mb, collide=False, casts=False)


def make_navigation(level):
    volume = ("  <Component Type=\"Nav Mesh Volume\" Version=\"1\">\n" + vec_xml("HalfExtents", (HALF + 100, 600, HALF + 100))
              + "".join(f"    <{k} Value=\"{nm.num(v)}\" />\n" for k, v in (
                  ("CellSize", 15), ("CellHeight", 10), ("AgentHeight", 180), ("AgentRadius", 35), ("AgentMaxClimb", 45),
                  ("AgentMaxSlope", 45), ("TileSize", 48), ("RegionMinSize", 8), ("RegionMergeSize", 20),
                  ("EdgeMaxLength", 1200), ("EdgeMaxError", 1.3), ("LayerMask", -1)))
              + f"    <NavMesh Value=\"{NAVMESH}\" />\n    <BuildAtRuntime Value=\"true\" />\n"
              + "    <RebuildOnChange Value=\"true\" />\n  </Component>\n")
    level.add("NavMeshVolume", [("Nav Mesh Volume", volume), ("Transform", nm.transform_xml((0, 0, 0)))], mobility=None)
    x0, x1, z0, z1 = MUD
    modifier = ("  <Component Type=\"Nav Modifier\" Version=\"1\">\n"
                + vec_xml("HalfExtents", ((x1 - x0) / 2, 100, (z1 - z0) / 2))
                + f"    <Area Value=\"1\" />\n    <CustomArea Value=\"2\" />\n    <Cost Value=\"{nm.num(MUD_COST)}\" />\n"
                + "  </Component>\n")
    level.add("Mud", [("Nav Modifier", modifier), ("Transform", nm.transform_xml(((x0 + x1) / 2, 0, (z0 + z1) / 2)))],
              mobility=None)


def make_markers(level):
    for team, (cx, cz), yaw in (("Red", (-2400, -2400), 45.0), ("Blue", (2400, 2400), -135.0)):
        for k, (ox, oz) in enumerate(((-150, 150), (150, -150), (150, 150))):
            level.marker(f"Spawn_{team}_{k + 1}", (cx + ox, 0, cz + oz), yaw)
    patrol = [(-1800, -1900), (-600, -1500), (600, -2600), (2200, -1000), (1700, 600), (0, 0), (-2000, 600),
              (-2200, 2300), (-400, 2500), (1500, 2600)]
    for k, (x, z) in enumerate(patrol):
        y = PLAZA_H if abs(x) < PLAZA and abs(z) < PLAZA else 0.0
        level.marker(f"Patrol_{k + 1}", (x, y, z))


def make_post():
    POST.mkdir(parents=True, exist_ok=True)
    nodes = [
        ("Effect.AmbientOcclusion", {"Intensity": "0.7", "Radius": "60", "Samples": "8", "Power": "1.4", "Blur": "true"}),
        ("Effect.Bloom", {"Threshold": "0.95", "Intensity": "0.22", "Radius": "0.6", "Levels": "5", "Tint": "1 0.98 0.95"}),
        ("Effect.ToneMapping", {"Exposure": "0.15", "Tone Mapper": "ACES"}),
        ("Effect.Antialiasing", {"Method": "FXAA", "Quality": "High", "Edge Threshold": "0.125", "Sharpness": "0.3"}),
        ("Effect.Material", {"Material": kit.mat("ToonInk"), "Stage": "After Effects", "Order": "0"}),
    ]
    lines = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <Nodes>\n"]
    for k, (type_name, props) in enumerate(nodes):
        node_id = nm.guid("ToonyPost", type_name)
        lines.append(f"    <Node Id=\"{node_id}\" Type=\"{type_name}\" Version=\"1\" X=\"{-300 + 150 * k}\" Y=\"0\">\n")
        for key in sorted(props):
            lines.append(f"      <Prop Name=\"{key}\" Value=\"{props[key]}\" />\n")
        lines.append("    </Node>\n")
    lines.append("  </Nodes>\n  <Comments>\n    <Comment X=\"-350\" Y=\"-120\" Width=\"900\" Height=\"90\" "
                 "Text=\"Toony: soft occlusion, a little bloom, ACES, FXAA, then ToonInk -- ink outlines on depth and colour "
                 "edges, banded light, a touch more saturation.\" />\n  </Comments>\n</NodeGraph>\n")
    (POST / "Toony.pp.nmgraph").write_text("".join(lines), newline="\n")


def make_environment(level):
    sun = nm.component_xml("Directional Light", 3, [
        ("Color", ("colour", (255, 246, 228))), ("Intensity", 9), ("Active", True), ("CastShadows", True),
        ("ShadowTechnique", 2), ("ShadowTint", ("colour", (92, 70, 140))), ("ShadowBias", 0.0012),
        ("ShadowNormalBias", 2), ("ShadowDistance", 9000), ("CascadeCount", 4), ("CascadeSplitLambda", 0.75),
        ("SourceAngle", 1.5)])
    level.add("Sun", [("Directional Light", sun),
                      ("Transform", nm.transform_xml((0, 6000, 0), (SUN_ELEVATION, SUN_AZIMUTH + 180.0, 0)))])
    sky = nm.component_xml("Sky Light", 2, [
        ("SkyColor", ("colour", (150, 200, 255))), ("GroundColor", ("colour", (120, 170, 90))), ("Intensity", 0.6),
        ("Active", True), ("Environment", SKY), ("EnvironmentIntensity", 0.7), ("EnvironmentRotation", 0),
        ("DrawBackground", True)])
    level.add("Sky", [("Sky Light", sky), ("Transform", nm.transform_xml((0, 0, 0)))])
    post = nm.component_xml("Post Process", 2, [("Stack", STACK), ("Priority", 0), ("Enabled", True), ("BlendDistance", 0)])
    level.add("PostProcessToony", [("Post Process", post), ("Transform", nm.transform_xml((0, 0, 0)))], mobility=None)
    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 30000), ("Active", True), ("Projection", 1), ("FovY", 58)])
    level.add("OverviewCamera", [("Camera", camera), ("Transform", nm.transform_xml((0, 3400, -5200), (34, 0, 0)))],
              mobility=None)


# ==============================================================================================
# The fixture: what walks, as an OBJ, for AICore.Tests
# ==============================================================================================

def write_fixture(level):
    lines = ["# Toon Arena navigation fixture -- written by Games/WorldsLab/Tools/Toony/make_toon_arena.py.\n",
             "# The level's static, walkable triangles (world space, cm). AICore.Tests bakes them.\n",
             "#   area <cx> <cy> <cz> <hx> <hy> <hz> <yaw> <id>   a Nav Modifier's box\n",
             "#   point <name> <x> <y> <z>                         where the test asks paths from and to\n"]
    x0, x1, z0, z1 = MUD
    lines.append(f"# area {(x0 + x1) / 2:g} 0 {(z0 + z1) / 2:g} {(x1 - x0) / 2:g} 100 {(z1 - z0) / 2:g} 0 1\n")
    points = {"RedBase": (-2400, 0, -2400), "BlueBase": (2400, 0, 2400), "PlazaTop": (0, PLAZA_H, -400),
              "MudWest": (600, 0, -1900), "MudEast": (2650, 0, -1900), "SteepFoot": (0, 0, PLAZA + 300),
              "PitFloor": (-1500, PIT_Y, -1100), "TrenchSouth": (-2200, 0, -1600), "TrenchNorth": (-2200, 0, -600),
              "MazeWest": (700, 0, 1650), "MazeEast": (2400, 0, 1650)}
    for name, (x, y, z) in points.items():
        lines.append(f"# point {name} {x:g} {y:g} {z:g}\n")
    base = 1
    faces = []
    for builder, (px, py, pz) in level.collision:
        for material in builder.order:
            for tri in builder.sections[material]:
                for vertex in tri:
                    p = vertex[0]
                    lines.append(f"v {p[0] + px:.1f} {p[1] + py:.1f} {p[2] + pz:.1f}\n")
                faces.append(f"f {base} {base + 1} {base + 2}\n")
                base += 3
    lines += faces
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE.write_text("".join(lines), newline="\n")
    print(f"  {FIXTURE.relative_to(kit.REPO_DIR)}: {len(faces)} triangles")


def main():
    level = Level()
    print("Toon Arena:")
    MESHES.mkdir(parents=True, exist_ok=True)
    for stale in MESHES.glob("*.nmmesh"):
        stale.unlink()
    make_ground(level)
    make_walls(level)
    make_plaza(level)
    make_trench_bridge(level)
    make_cover(level)
    make_maze(level)
    make_divider(level)
    make_decor(level)
    make_navigation(level)
    make_markers(level)
    make_post()
    make_environment(level)
    level.write()
    write_fixture(level)
    gen = kit.generator()
    if gen.exists():
        subprocess.run([str(gen), str(POST / "Toony.pp.nmgraph")], check=True)
    else:
        print(f"  {gen} not found: open Toony.pp.nmgraph in the editor and save it")
    size = sum(p.stat().st_size for p in MESHES.glob("*.nmmesh"))
    print(f"  {WORLD.relative_to(GAME_DIR)}: {len(level.entities)} entities")
    print(f"  {len(list(MESHES.glob('*.nmmesh')))} meshes, {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
