#!/usr/bin/env python3
"""
make_range_level.py -- writes the Shooting Range world: Content/Levels/
ShootingRange.nmworld and its entities, Content/Meshes/ShootingRange/,
Content/Prefabs/ShootingRange/ (the targets and the shooter), Content/
Particles/ShootingRange/ and Content/PostProcess/ShootingRange/.

A steel-framed warehouse, 30 x 50 m and 10 m to the roof, lit by the midday
sun through two rows of skylights (shafts in the dusty air) and by
fluorescent tubes. +Z is down range.

    Staging     the south end: tables, lockers, crates and pallets, where the
                shooter starts (the Player Start, facing down range)
    Firing line six booths of plywood dividers and a bench at Z = -700
    Range       six 2 m lanes between block walls to a sloped rubber backstop;
                hinged steel plates (they swing when hit -- Jolt) at 14 and
                22 m, paper silhouettes at 18 m, baffles overhead
    Upper level stairs up the west wall to a diamond-plate mezzanine, then
                grated catwalks: along both walls and twice across the range,
                railed, 4.5 m up

Collision is JoltPhysics' (Config/DefaultEngine.ini [Modules]): the shell,
structure, range and props are static triangle meshes on layer 1; the
stairs, mezzanine, catwalks and railings walk on invisible boxes on layer 2,
which the shooter's rays stand on and its bullets pass (the grating).

Run after make_range_textures.py and make_range_materials.py, from the
repository root:

    python Games/WorldsLab/Tools/ShootingRange/make_range_level.py
"""

import math
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "Steampunk"))
import nmformats as nm                                     # noqa: E402
from make_range_textures import SUN_ELEVATION, SUN_AZIMUTH   # noqa: E402

GAME_DIR = HERE.parents[1]
SCOPE = "ShootingRange"
WORLD = GAME_DIR / "Content" / "Levels" / "ShootingRange.nmworld"
FOLDER = GAME_DIR / "Content" / "Levels" / "ShootingRange"
MESHES = GAME_DIR / "Content" / "Meshes" / "ShootingRange"
MESH_PATH = "Content/Meshes/ShootingRange/"
PREFABS = GAME_DIR / "Content" / "Prefabs" / "ShootingRange"
PREFAB_PATH = "Content/Prefabs/ShootingRange/"
PARTICLES = GAME_DIR / "Content" / "Particles" / "ShootingRange"
PARTICLE_PATH = "Content/Particles/ShootingRange/"
POST = GAME_DIR / "Content" / "PostProcess" / "ShootingRange"
STACK = "Content/PostProcess/ShootingRange/Range.pp.nmgraph"
SKY = "Content/Textures/ShootingRange/Sky_HDR.nmtexture"
GENERATOR = GAME_DIR / "Binaries" / "Linux_Debug" / "NodeGraphGenerate"
DUMMY = "Engine/Content/Characters/CrashDummy/"


def mat(name):
    return f"Content/Materials/ShootingRange/{name}.material.nmgraph"


CONCRETE, BLOCK, CORRUGATED, STEEL = mat("Concrete"), mat("Block"), mat("Corrugated"), mat("Steel")
GRATING, DIAMOND, PLYWOOD, RUBBER = mat("Grating"), mat("DiamondPlate"), mat("Plywood"), mat("Rubber")
FLOORPAINT, GLASS, FLUORESCENT = mat("FloorPaint"), mat("Glass"), mat("Fluorescent")
PAPER, STEEL_TARGET, MUZZLE = mat("TargetPaper"), mat("SteelTarget"), mat("MuzzleFlash")

# --- The building (cm) -------------------------------------------------------------------------
HW, HL = 1500.0, 2500.0          # half width (X), half length (Z), inside faces
WALL_T = 25.0
WALL_H = 1000.0
BLOCK_H = 250.0                  # block wainscot under the siding
ROOF_T = 25.0
SKY_X = (-600.0, 600.0)          # skylight rows (centres)
SKY_Z = (-1800.0, -800.0, 200.0, 1200.0, 2100.0)
SKY_W, SKY_L = 300.0, 500.0      # each opening, X by Z
CURB_H = 50.0
BAYS_Z = [-2000.0, -1000.0, 0.0, 1000.0, 2000.0]   # columns and rafters

# --- The range --------------------------------------------------------------------------------------
LANES = 6
LANE_W = 200.0
LANE_X0 = -LANE_W * LANES / 2    # -600
FIRING_Z = -700.0
STALL_D = 140.0
RANGE_WALL_X = 815.0
BACKSTOP_Z0, BACKSTOP_Z1 = 2050.0, 2460.0
BAFFLE_Z = (-150.0, 1000.0)

# --- The upper level --------------------------------------------------------------------------------
UP = 450.0                       # deck height
WALK_W = 150.0
STAIR_X = (-1480.0, -1350.0)
STAIR_Z0 = -2300.0
RISER, TREAD = 25.0, 28.0
STEPS = int(UP / RISER)          # 18
STAIR_Z1 = STAIR_Z0 + STEPS * TREAD
MEZZ = (-1480.0, -880.0, STAIR_Z1, STAIR_Z1 + 600.0)      # x0, x1, z0, z1
WEST_X = (-1480.0, -1330.0)
EAST_X = (1330.0, 1480.0)
WALK_Z1 = 2300.0
CROSS_Z = (400.0, 1600.0)        # cross catwalk centres

LAYER_WORLD, LAYER_WALK = 1, 2
PROBES = False                   # see make_environment


def guid(name):
    return nm.guid(SCOPE, name)


def rgb255(r, g, b):
    return (int(r), int(g), int(b))


# ==============================================================================================
# Grime: vertex colour (the materials multiply their base colour by it)
# ==============================================================================================

def hash01(x, y, z):
    v = math.sin(x * 0.0123 + y * 0.0371 + z * 0.0217) * 43758.5453
    return v - math.floor(v)


def grime(p, n):
    """Dirt where it gathers: the floor at the walls and under traffic, walls low down."""
    x, y, z = p
    shade = 0.86 + 0.14 * hash01(x, y, z)
    if n[1] > 0.7 and y < 5.0:                                   # the floor
        to_wall = min(HW - abs(x), HL - abs(z))
        shade *= 0.72 + 0.28 * min(1.0, to_wall / 250.0)
        if FIRING_Z - 300 < z < FIRING_Z + 100 and abs(x) < 650:   # shuffled at the line
            shade *= 0.85
    elif abs(n[1]) < 0.3:                                        # walls
        shade *= 0.62 + 0.38 * min(1.0, y / 120.0)
    return (shade, shade, shade, 1.0)


# ==============================================================================================
# Entities
# ==============================================================================================

class Level:
    def __init__(self):
        self.entities, self.instances, self.names = [], [], set()
        self.meshes, self.bounds = {}, {}

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

    def mesh_entity(self, name, mesh_path, position=(0, 0, 0), yaw=0.0, casts=True, collide=True, tint=None,
                    occluder=False):
        components = [("Render", render_xml(mesh_path, casts=casts, tint=tint, occluder=occluder)),
                      ("Transform", nm.transform_xml(position, (0, yaw, 0)))]
        if collide:
            components.append(("Rigid Body", rigid_body_xml("Static", layer=LAYER_WORLD)))
        return self.add(name, components)

    def collider(self, name, lo, hi, rotation=(0, 0, 0), layer=LAYER_WALK):
        """An invisible static box, lo-hi about its centre, turned by rotation."""
        centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
        half = tuple((b - a) / 2 for a, b in zip(lo, hi))
        return self.collider_at(name, centre, half, rotation, layer)

    def collider_at(self, name, centre, half, rotation=(0, 0, 0), layer=LAYER_WALK):
        components = [("Bounds", bounds_xml(tuple(-h for h in half), half)),
                      ("Rigid Body", rigid_body_xml("Static", layer=layer)),
                      ("Transform", nm.transform_xml(centre, rotation))]
        return self.add(name, components)

    def instance(self, name, prefab, position, yaw=0.0):
        name = self.unique(name)
        text = (f"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                f"<Instance Guid=\"{guid(name)}\" Name=\"{name}\" Prefab=\"{prefab}\">\n"
                "  <Entity Id=\"1\">\n"
                "    <Component Type=\"Transform\" Version=\"1\">\n"
                f"      <Position X=\"{nm.num(position[0])}\" Y=\"{nm.num(position[1])}\" Z=\"{nm.num(position[2])}\" />\n"
                f"      <Rotation X=\"0\" Y=\"{nm.num(yaw)}\" Z=\"0\" />\n"
                "    </Component>\n"
                "  </Entity>\n"
                "</Instance>\n")
        self.instances.append((name, text))

    def write(self):
        FOLDER.mkdir(parents=True, exist_ok=True)
        for stale in list(FOLDER.glob("*.nmentity")) + list(FOLDER.glob("*.nmprefabinst")):
            stale.unlink()
        manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n"]
        for name, text in self.entities:
            (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
            manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
        for name, text in self.instances:
            (FOLDER / f"{guid(name)}.nmprefabinst").write_text(text, newline="\n")
            manifest.append(f"  <Instance Guid=\"{guid(name)}\" />\n")
        manifest.append("</World>\n")
        WORLD.write_text("".join(manifest), newline="\n")


def bounds_xml(lo, hi):
    return ("  <Component Type=\"Bounds\" Version=\"1\">\n"
            f"    <Local MinX=\"{nm.num(lo[0])}\" MinY=\"{nm.num(lo[1])}\" MinZ=\"{nm.num(lo[2])}\" "
            f"MaxX=\"{nm.num(hi[0])}\" MaxY=\"{nm.num(hi[1])}\" MaxZ=\"{nm.num(hi[2])}\" />\n"
            "    <Static Value=\"true\" />\n"
            "  </Component>\n")


def render_xml(mesh, casts=True, tint=None, occluder=False, materials=()):
    lines = ["  <Component Type=\"Render\" Version=\"5\">\n", "    <Visible Value=\"true\" />\n"]
    lines += [f"    <Material{slot} Value=\"{materials[slot] if slot < len(materials) else ''}\" />\n" for slot in range(8)]
    lines.append(f"    <Mesh Value=\"{mesh}\" />\n")
    if tint:
        lines += ["    <ParameterCount Value=\"1\" />\n", "    <Parameter0Name Value=\"Tint\" />\n",
                  "    <Parameter0Slot Value=\"0\" />\n",
                  f"    <Parameter0X Value=\"{nm.num(tint[0])}\" />\n", f"    <Parameter0Y Value=\"{nm.num(tint[1])}\" />\n",
                  f"    <Parameter0Z Value=\"{nm.num(tint[2])}\" />\n", "    <Parameter0W Value=\"0\" />\n"]
    else:
        lines.append("    <ParameterCount Value=\"0\" />\n")
    lines.append(f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n")
    lines.append("    <CastInsetShadow Value=\"false\" />\n")
    lines.append(f"    <Occluder Value=\"{'true' if occluder else 'false'}\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def rigid_body_xml(motion, layer=LAYER_WORLD, mass=0.0, shape=0, linear_damping=0.05, angular_damping=0.05,
                   friction=0.6, restitution=0.0, mask=-1, enabled=True, source=0, continuous=False):
    motion_value = {"Dynamic": 0, "Kinematic": 1, "Static": 2}[motion]
    return nm.component_xml("Rigid Body", 1, [
        ("Enabled", enabled), ("Motion", motion_value), ("Shape", shape), ("ShapeSource", source), ("Mass", mass),
        ("Friction", friction), ("Restitution", restitution), ("LinearDamping", linear_damping),
        ("AngularDamping", angular_damping), ("GravityFactor", 1), ("Continuous", continuous), ("Sensor", False),
        ("AllowSleeping", True), ("Layer", layer), ("Mask", mask)]).replace(
        "  </Component>\n",
        "    <InitialLinearVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n"
        "    <InitialAngularVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n  </Component>\n")


def point_light_xml(colour, intensity, radius, shadows=False, source=8):
    return nm.component_xml("Point Light", 2, [
        ("Color", ("colour", colour)), ("Intensity", intensity), ("Radius", radius), ("Active", True),
        ("CastShadows", shadows), ("ShadowTechnique", 0), ("ShadowTint", ("colour", (30, 32, 40))),
        ("ShadowBias", 0.004), ("ShadowNormalBias", 2), ("SourceRadius", source)])


def spot_light_xml(colour, intensity, radius, inner, outer, shadows=False, source=10):
    return nm.component_xml("Spot Light", 2, [
        ("Color", ("colour", colour)), ("Intensity", intensity), ("Range", radius), ("InnerConeAngle", inner),
        ("OuterConeAngle", outer), ("Active", True), ("CastShadows", shadows), ("ShadowTechnique", 1),
        ("ShadowTint", ("colour", (20, 22, 28))), ("ShadowBias", 0.002), ("ShadowNormalBias", 1.5),
        ("SourceRadius", source)])


def write_mesh(level, key, builder, lightmap=False):
    if key in level.meshes:
        return level.meshes[key]
    lo, hi = builder.write(MESHES / f"{key}.nmmesh", lightmap=lightmap)
    level.meshes[key] = MESH_PATH + f"{key}.nmmesh"
    level.bounds[level.meshes[key]] = (lo, hi)
    return level.meshes[key]


def indent(text):
    return "".join("  " + line + "\n" for line in text.rstrip("\n").split("\n"))


# ==============================================================================================
# Geometry helpers
# ==============================================================================================

def box(mb, material, lo, hi, skip=(), dirty=True):
    mb.box(material, lo, hi, colour_fn=grime if dirty else None, skip=skip)


def ibeam(mb, axis, start, length, depth=40.0, flange=20.0, web=1.2, thick=1.6, centre=(0, 0, 0), material=STEEL):
    """An I-beam along axis ('x', 'y' or 'z') from start for length; its depth along
    y (beams) or x (columns), centred on the other two coordinates in centre."""
    cx, cy, cz = centre
    a0, a1 = start, start + length
    if axis == "x":      # depth along y, flanges along z
        box(mb, material, (a0, cy + depth / 2 - thick, cz - flange / 2), (a1, cy + depth / 2, cz + flange / 2))
        box(mb, material, (a0, cy - depth / 2, cz - flange / 2), (a1, cy - depth / 2 + thick, cz + flange / 2))
        box(mb, material, (a0, cy - depth / 2 + thick, cz - web / 2), (a1, cy + depth / 2 - thick, cz + web / 2))
    elif axis == "z":
        box(mb, material, (cx - flange / 2, cy + depth / 2 - thick, a0), (cx + flange / 2, cy + depth / 2, a1))
        box(mb, material, (cx - flange / 2, cy - depth / 2, a0), (cx + flange / 2, cy - depth / 2 + thick, a1))
        box(mb, material, (cx - web / 2, cy - depth / 2 + thick, a0), (cx + web / 2, cy + depth / 2 - thick, a1))
    else:                # a column: depth along x, flanges along z
        box(mb, material, (cx + depth / 2 - thick, a0, cz - flange / 2), (cx + depth / 2, a1, cz + flange / 2))
        box(mb, material, (cx - depth / 2, a0, cz - flange / 2), (cx - depth / 2 + thick, a1, cz + flange / 2))
        box(mb, material, (cx - depth / 2 + thick, a0, cz - web / 2), (cx + depth / 2 - thick, a1, cz + web / 2))


def channel_z(mb, x, y, z0, z1, depth=20.0, width=6.0, facing=1, material=STEEL):
    """A C-channel along Z, its web at x, flanges toward +X (facing 1) or -X, from y to y+depth."""
    w = width * facing
    xa, xb = sorted((x, x + 0.8 * facing))
    box(mb, material, (xa, y, z0), (xb, y + depth, z1))
    for yy in (y, y + depth - 0.8):
        xa, xb = sorted((x, x + w))
        box(mb, material, (xa, yy, z0), (xb, yy + 0.8, z1))


def channel_x(mb, z, y, x0, x1, depth=20.0, width=6.0, facing=1, material=STEEL):
    w = width * facing
    za, zb = sorted((z, z + 0.8 * facing))
    box(mb, material, (x0, y, za), (x1, y + depth, zb))
    for yy in (y, y + depth - 0.8):
        za, zb = sorted((z, z + w))
        box(mb, material, (x0, yy, za), (x1, yy + 0.8, zb))


def frange(a, b, step):
    out, v = [], a
    while v <= b + 1e-6:
        out.append(v)
        v += step
    return out


# ==============================================================================================
# The shell: slab, walls, roof with skylights, doors
# ==============================================================================================

def make_shell(level):
    mb = nm.MeshBuilder("Shell")
    # The slab, in 2.5 m cells so the grime varies across it.
    cell = 250.0
    xs = frange(-HW, HW, cell)
    zs = frange(-HL, HL, cell)
    for i in range(len(xs) - 1):
        for j in range(len(zs) - 1):
            p = [(xs[i], 0, zs[j]), (xs[i], 0, zs[j + 1]), (xs[i + 1], 0, zs[j + 1]), (xs[i + 1], 0, zs[j])]
            mb.polygon(CONCRETE, p, normal=(0, 1, 0), colours=[grime(q, (0, 1, 0)) for q in p])
    # Walls: block up to BLOCK_H, siding above; inner faces in 2.5 m panels, outer plain.
    for side in ("w", "e", "s", "n"):
        along = zs if side in "we" else xs
        for k in range(len(along) - 1):
            a0, a1 = along[k], along[k + 1]
            for y0, y1, material in ((0, 60, BLOCK), (60, BLOCK_H, BLOCK), (BLOCK_H, 600, CORRUGATED),
                                     (600, WALL_H, CORRUGATED)):
                if side == "w":
                    p, n = [(-HW, y0, a1), (-HW, y0, a0), (-HW, y1, a0), (-HW, y1, a1)], (1, 0, 0)
                elif side == "e":
                    p, n = [(HW, y0, a0), (HW, y0, a1), (HW, y1, a1), (HW, y1, a0)], (-1, 0, 0)
                elif side == "s":
                    p, n = [(a0, y0, -HL), (a1, y0, -HL), (a1, y1, -HL), (a0, y1, -HL)], (0, 0, 1)
                else:
                    p, n = [(a1, y0, HL), (a0, y0, HL), (a0, y1, HL), (a1, y1, HL)], (0, 0, -1)
                mb.polygon(material, p, normal=n, colours=[grime(q, n) for q in p])
    # Outer faces and wall tops (closed for the sun's shadows).
    o = WALL_T
    box(mb, CORRUGATED, (-HW - o, 0, -HL - o), (-HW, WALL_H, HL + o), skip=("+x",), dirty=False)
    box(mb, CORRUGATED, (HW, 0, -HL - o), (HW + o, WALL_H, HL + o), skip=("-x",), dirty=False)
    box(mb, CORRUGATED, (-HW, 0, -HL - o), (HW, WALL_H, -HL), skip=("+z",), dirty=False)
    box(mb, CORRUGATED, (-HW, 0, HL), (HW, WALL_H, HL + o), skip=("-z",), dirty=False)
    # The roof: a grid of cells, the skylight openings left out.
    holes = [(cx - SKY_W / 2, cx + SKY_W / 2, cz - SKY_L / 2, cz + SKY_L / 2) for cx in SKY_X for cz in SKY_Z]
    rx = sorted({-HW - o, HW + o} | {h[0] for h in holes} | {h[1] for h in holes})
    rz = sorted({-HL - o, HL + o} | {h[2] for h in holes} | {h[3] for h in holes})

    def is_hole(x0, x1, z0, z1):
        return any(h[0] <= x0 and x1 <= h[1] and h[2] <= z0 and z1 <= h[3] for h in holes)

    y0, y1 = WALL_H, WALL_H + ROOF_T
    for i in range(len(rx) - 1):
        for j in range(len(rz) - 1):
            x0, x1, z0, z1 = rx[i], rx[i + 1], rz[j], rz[j + 1]
            if is_hole(x0, x1, z0, z1):
                continue
            mb.polygon(CORRUGATED, [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], normal=(0, -1, 0))
            mb.polygon(STEEL, [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)], normal=(0, 1, 0))
    # Each opening: its curb, lined inside with siding, capped.
    for (x0, x1, z0, z1) in holes:
        t = 12.0
        box(mb, CORRUGATED, (x0 - t, y0, z0 - t), (x0, y1 + CURB_H, z1 + t), skip=("-y",))
        box(mb, CORRUGATED, (x1, y0, z0 - t), (x1 + t, y1 + CURB_H, z1 + t), skip=("-y",))
        box(mb, CORRUGATED, (x0, y0, z0 - t), (x1, y1 + CURB_H, z0), skip=("-y",))
        box(mb, CORRUGATED, (x0, y0, z1), (x1, y1 + CURB_H, z1 + t), skip=("-y",))
    # The roll-up door and a personnel door, south wall, a little proud of it.
    box(mb, CORRUGATED, (-450, 0, -HL), (150, 480, -HL + 4), dirty=True)
    box(mb, STEEL, (-470, 0, -HL), (-450, 500, -HL + 12))
    box(mb, STEEL, (150, 0, -HL), (170, 500, -HL + 12))
    box(mb, STEEL, (-470, 480, -HL), (170, 520, -HL + 25))
    box(mb, STEEL, (700, 0, -HL), (800, 210, -HL + 5))
    level.mesh_entity("Shell", write_mesh(level, "Shell", mb), occluder=True)

    glass = nm.MeshBuilder("Skylights")
    for (x0, x1, z0, z1) in holes:
        yy = y1 + CURB_H
        glass.polygon(GLASS, [(x0, yy, z0), (x1, yy, z0), (x1, yy, z1), (x0, yy, z1)], normal=(0, -1, 0))
        # Wire: a few bars across, shadowing the shafts.
        for k in range(1, 4):
            zz = z0 + (z1 - z0) * k / 4
            box(glass, STEEL, (x0, yy - 4, zz - 2), (x1, yy, zz + 2), dirty=False)
    level.mesh_entity("Skylights", write_mesh(level, "Skylights", glass), casts=False, collide=False)


def make_structure(level):
    mb = nm.MeshBuilder("Structure")
    for z in BAYS_Z:
        ibeam(mb, "x", -HW, 2 * HW, depth=45, flange=22, centre=(0, WALL_H - 22.5, z))
        for x in (-HW + 12, HW - 12):
            ibeam(mb, "y", 0, WALL_H - 45, depth=24, flange=24, centre=(x, 0, z))
    # Purlins between the rafters, clear of the skylights.
    for x in (-1150.0, 0.0, 1150.0):
        for k in range(len(BAYS_Z) + 1):
            z0 = -HL if k == 0 else BAYS_Z[k - 1]
            z1 = HL if k == len(BAYS_Z) else BAYS_Z[k]
            channel_z(mb, x, WALL_H - 22, z0, z1, depth=22, width=7)
    # Diagonal bracing in the end bays of both long walls: flat straps.
    level.mesh_entity("Structure", write_mesh(level, "Structure", mb))


# ==============================================================================================
# The range: booths, bench, side walls, baffles, backstop, floor lines
# ==============================================================================================

def make_range(level):
    mb = nm.MeshBuilder("Range")
    # Booth dividers: plywood panels on steel posts, behind the line.
    for k in range(LANES + 1):
        x = LANE_X0 + k * LANE_W
        box(mb, PLYWOOD, (x - 2, 20, FIRING_Z - STALL_D), (x + 2, 210, FIRING_Z))
        box(mb, STEEL, (x - 3, 0, FIRING_Z - 6), (x + 3, 215, FIRING_Z))
        box(mb, STEEL, (x - 3, 0, FIRING_Z - STALL_D), (x + 3, 215, FIRING_Z - STALL_D + 6))
    # The bench: a plywood shelf on angle legs along the line.
    box(mb, PLYWOOD, (LANE_X0, 92, FIRING_Z - 55), (-LANE_X0, 97, FIRING_Z + 5))
    for x in frange(LANE_X0 + 100, -LANE_X0 - 100, LANE_W):
        box(mb, STEEL, (x - 2, 0, FIRING_Z - 50), (x + 2, 92, FIRING_Z - 46))
        box(mb, STEEL, (x - 2, 0, FIRING_Z - 2), (x + 2, 92, FIRING_Z + 2))
    # Side walls: block, 3 m, the length of the range.
    for sx in (-1, 1):
        x0, x1 = sorted((sx * RANGE_WALL_X, sx * (RANGE_WALL_X + 30)))
        box(mb, BLOCK, (x0, 0, FIRING_Z), (x1, 300, BACKSTOP_Z1 + 20))
    # Backstop: a rubber berm sloping up to a steel wall.
    xl, xr = -RANGE_WALL_X, RANGE_WALL_X
    z0, z1, top = BACKSTOP_Z0, BACKSTOP_Z1 - 20, 420.0
    mb.polygon(RUBBER, [(xl, 0, z0), (xr, 0, z0), (xr, top, z1), (xl, top, z1)],
               colours=[grime(p, (0, 0.7, -0.7)) for p in [(xl, 0, z0), (xr, 0, z0), (xr, top, z1), (xl, top, z1)]])
    mb.polygon(RUBBER, [(xl, 0, z0), (xl, top, z1), (xl, 0, z1)])
    mb.polygon(RUBBER, [(xr, 0, z0), (xr, 0, z1), (xr, top, z1)])
    box(mb, STEEL, (xl, 0, z1), (xr, 620, z1 + 20))
    for x in frange(xl, xr, 200):
        box(mb, STEEL, (x - 6, 0, z1 - 4), (x + 6, 620, z1))
    # Overhead baffles: plywood faced steel panels, tilted to catch strays.
    for bz in BAFFLE_Z:
        c, s = math.cos(math.radians(35)), math.sin(math.radians(35))
        mb.oriented_box(PLYWOOD, (0, 760, bz), [(1, 0, 0), (0, c, s), (0, -s, c)], (RANGE_WALL_X, 100, 2.5))
        for x in (-700.0, 0.0, 700.0):
            box(mb, STEEL, (x - 1, 840, bz - 1), (x + 1, WALL_H - 45, bz + 1))
    level.mesh_entity("Range", write_mesh(level, "Range", mb))

    # Floor lines: the firing line, the walkway behind the booths, lane numbers' ticks.
    paint = nm.MeshBuilder("FloorLines")

    def stripe(x0, x1, z0, z1):
        paint.polygon(FLOORPAINT, [(x0, 0.4, z0), (x0, 0.4, z1), (x1, 0.4, z1), (x1, 0.4, z0)], normal=(0, 1, 0))

    stripe(LANE_X0, -LANE_X0, FIRING_Z - 2, FIRING_Z + 10)
    stripe(-1000, 1000, FIRING_Z - STALL_D - 160, FIRING_Z - STALL_D - 150)
    for x in (-1000.0, 990.0):
        stripe(x, x + 10, -2400, FIRING_Z - STALL_D - 150)
    for k in range(LANES):
        x = LANE_X0 + (k + 0.5) * LANE_W
        for d in range(k + 1):
            stripe(x - 30 + d * 14, x - 22 + d * 14, FIRING_Z + 40, FIRING_Z + 80)
    level.mesh_entity("FloorLines", write_mesh(level, "FloorLines", paint), casts=False, collide=False)


# ==============================================================================================
# The upper level: stairs, mezzanine, catwalks, railings -- visual meshes, box colliders
# ==============================================================================================

def railing(mb, points, posts_every=150.0, toe=True):
    """A pipe railing along a polyline at deck height: posts, a top and a mid rail, a toe board."""
    for (a, b) in zip(points, points[1:]):
        length = math.dist((a[0], a[2]), (b[0], b[2]))
        n = max(1, int(round(length / posts_every)))
        for k in range(n + 1):
            t = k / n
            p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
            mb.tube(STEEL, [p, (p[0], p[1] + 107, p[2])], 2.4, segments=8)
        for h in (55.0, 105.0):
            mb.tube(STEEL, [(a[0], a[1] + h, a[2]), (b[0], b[1] + h, b[2])], 2.2, segments=8)
        if toe and abs(a[1] - b[1]) < 1:
            lo = (min(a[0], b[0]) - 0.5, a[1], min(a[2], b[2]) - 0.5)
            hi = (max(a[0], b[0]) + 0.5, a[1] + 10, max(a[2], b[2]) + 0.5)
            box(mb, STEEL, lo, hi, dirty=False)


def deck(mb, x0, x1, z0, z1, along_z=True):
    """A grated catwalk deck at UP: the grating, channel stringers at its edges, cross supports."""
    mb.polygon(GRATING, [(x0, UP, z0), (x0, UP, z1), (x1, UP, z1), (x1, UP, z0)], normal=(0, 1, 0))
    if along_z:
        channel_z(mb, x0, UP - 20, z0, z1, facing=1)
        channel_z(mb, x1, UP - 20, z0, z1, facing=-1)
        for z in frange(z0, z1, 120):
            box(mb, STEEL, (x0, UP - 6, z - 2), (x1, UP - 1, z + 2), dirty=False)
    else:
        channel_x(mb, z0, UP - 20, x0, x1, facing=1)
        channel_x(mb, z1, UP - 20, x0, x1, facing=-1)
        for x in frange(x0, x1, 120):
            box(mb, STEEL, (x - 2, UP - 6, z0), (x + 2, UP - 1, z1), dirty=False)


def make_upper_level(level):
    mb = nm.MeshBuilder("UpperLevel")
    rails = nm.MeshBuilder("Railings")
    x0, x1 = STAIR_X
    # Stairs: open diamond-plate treads between channel stringers, rising +Z.
    for k in range(STEPS):
        y = (k + 1) * RISER
        z = STAIR_Z0 + k * TREAD
        box(mb, DIAMOND, (x0 + 4, y - 4, z), (x1 - 4, y, z + TREAD + 2), dirty=False)
    run = STAIR_Z1 - STAIR_Z0
    ang = math.atan2(UP, run)
    for x in (x0 + 2, x1 - 2):
        c, s = math.cos(ang), math.sin(ang)
        mid = (x, UP / 2 - 12, (STAIR_Z0 + STAIR_Z1) / 2)
        mb.oriented_box(STEEL, mid, [(1, 0, 0), (0, c, -s), (0, s, c)], (2, 14, math.hypot(run, UP) / 2))
    # Under the stairs: a plywood store, so nothing walks into the stringers.
    box(mb, PLYWOOD, (x0, 0, STAIR_Z0 + 130), (x1, 4 + RISER * 5, STAIR_Z0 + 134))
    box(mb, PLYWOOD, (x1, 0, STAIR_Z0 + 130), (x1 + 3, RISER * 5, STAIR_Z1))
    railing(rails, [(x1, RISER, STAIR_Z0), (x1, UP, STAIR_Z1)], posts_every=170, toe=False)

    # The mezzanine: tread plate on a steel frame, posts to the floor.
    mx0, mx1, mz0, mz1 = MEZZ
    box(mb, DIAMOND, (mx0, UP - 1.5, mz0), (mx1, UP, mz1), skip=("-y",), dirty=False)
    box(mb, STEEL, (mx0, UP - 25, mz0), (mx1, UP - 1.5, mz1), skip=("+y",))
    for px in (mx0 + 10, mx1 - 10):
        for pz in (mz0 + 10, mz1 - 10):
            ibeam(mb, "y", 0, UP - 25, depth=15, flange=15, centre=(px, 0, pz))
    # Catwalks: along each long wall and twice across the range.
    wx0, wx1 = WEST_X
    ex0, ex1 = EAST_X
    deck(mb, wx0, wx1, mz1, WALK_Z1)
    deck(mb, ex0, ex1, mz1, WALK_Z1)
    deck(mb, mx1, ex0, mz1 - WALK_W, mz1, along_z=False)   # the south cross walk, joining the mezzanine east
    for cz in CROSS_Z:
        deck(mb, wx1, ex0, cz - WALK_W / 2, cz + WALK_W / 2, along_z=False)
    # Support: posts on the wall walks, rods from the rafters on the cross walks.
    for z in frange(mz1 + 300, WALK_Z1, 500):
        for x in (wx1 - 4, ex0 + 4):
            ibeam(mb, "y", 0, UP - 20, depth=12, flange=12, centre=(x, 0, z))
    for cz in CROSS_Z:
        for x in frange(wx1 + 300, ex0 - 300, 600):
            for zz in (cz - WALK_W / 2 - 2, cz + WALK_W / 2 + 2):
                rails.tube(STEEL, [(x, UP - 20, zz), (x, WALL_H - 45, zz)], 1.2, segments=6)
    level.mesh_entity("UpperLevel", write_mesh(level, "UpperLevel", mb), collide=False)

    # Railings: the open edges of every walk.
    railing(rails, [(mx1, UP, mz0), (mx1, UP, mz1 - WALK_W)])
    railing(rails, [(x1, UP, mz0), (mx1, UP, mz0)])
    railing(rails, [(wx1, UP, mz1), (wx1, UP, CROSS_Z[0] - WALK_W / 2)])
    railing(rails, [(wx1, UP, CROSS_Z[0] + WALK_W / 2), (wx1, UP, CROSS_Z[1] - WALK_W / 2)])
    railing(rails, [(wx1, UP, CROSS_Z[1] + WALK_W / 2), (wx1, UP, WALK_Z1), (wx0, UP, WALK_Z1)])
    railing(rails, [(ex0, UP, mz1), (ex0, UP, CROSS_Z[0] - WALK_W / 2)])
    railing(rails, [(ex0, UP, CROSS_Z[0] + WALK_W / 2), (ex0, UP, CROSS_Z[1] - WALK_W / 2)])
    railing(rails, [(ex0, UP, CROSS_Z[1] + WALK_W / 2), (ex0, UP, WALK_Z1), (ex1, UP, WALK_Z1)])
    railing(rails, [(mx1, UP, mz1), (ex0, UP, mz1)])
    railing(rails, [(mx1, UP, mz1 - WALK_W), (ex0, UP, mz1 - WALK_W)])
    for cz in CROSS_Z:
        for zz in (cz - WALK_W / 2, cz + WALK_W / 2):
            railing(rails, [(wx1, UP, zz), (ex0, UP, zz)])
    level.mesh_entity("Railings", write_mesh(level, "Railings", rails), collide=False, tint=(0.85, 0.62, 0.08))

    # Colliders (layer 2: walked on, shot through).
    level.collider_at("StairRamp", ((x0 + x1) / 2, UP / 2 - 6, (STAIR_Z0 + STAIR_Z1) / 2),
                      ((x1 - x0) / 2, 6, math.hypot(run, UP) / 2), (-math.degrees(ang), 0, 0))
    level.collider("StairStore", (x0, 0, STAIR_Z0 + 130), (x1 + 3, RISER * 5 - 2, STAIR_Z1 - 20))
    level.collider("Mezzanine", (mx0, UP - 25, mz0), (mx1, UP, mz1))
    level.collider("WalkWest", (wx0, UP - 20, mz1), (wx1, UP, WALK_Z1))
    level.collider("WalkEast", (ex0, UP - 20, mz1), (ex1, UP, WALK_Z1))
    level.collider("WalkSouth", (mx1, UP - 20, mz1 - WALK_W), (ex0, UP, mz1))
    for k, cz in enumerate(CROSS_Z):
        level.collider(f"WalkCross{k}", (wx1, UP - 20, cz - WALK_W / 2), (ex0, UP, cz + WALK_W / 2))
    # Railings as walls, a metre tall.
    rail_walls = [
        ((x1, RISER, STAIR_Z0), (x1 + 4, UP + 110, STAIR_Z1)),
        ((mx1, UP, mz0), (mx1 + 4, UP + 110, mz1 - WALK_W)),
        ((x1, UP, mz0 - 4), (mx1, UP + 110, mz0)),
        ((wx1, UP, mz1), (wx1 + 4, UP + 110, CROSS_Z[0] - WALK_W / 2)),
        ((wx1, UP, CROSS_Z[0] + WALK_W / 2), (wx1 + 4, UP + 110, CROSS_Z[1] - WALK_W / 2)),
        ((wx1, UP, CROSS_Z[1] + WALK_W / 2), (wx1 + 4, UP + 110, WALK_Z1)),
        ((wx0, UP, WALK_Z1), (wx1, UP + 110, WALK_Z1 + 4)),
        ((ex0 - 4, UP, mz1), (ex0, UP + 110, CROSS_Z[0] - WALK_W / 2)),
        ((ex0 - 4, UP, CROSS_Z[0] + WALK_W / 2), (ex0, UP + 110, CROSS_Z[1] - WALK_W / 2)),
        ((ex0 - 4, UP, CROSS_Z[1] + WALK_W / 2), (ex0, UP + 110, WALK_Z1)),
        ((ex0, UP, WALK_Z1), (ex1, UP + 110, WALK_Z1 + 4)),
        ((mx1, UP, mz1 - WALK_W - 4), (ex0, UP + 110, mz1 - WALK_W)),
    ]
    for cz in CROSS_Z:
        rail_walls.append(((wx1, UP, cz - WALK_W / 2 - 4), (ex0, UP + 110, cz - WALK_W / 2)))
        rail_walls.append(((wx1, UP, cz + WALK_W / 2), (ex0, UP + 110, cz + WALK_W / 2 + 4)))
    # The south walk's north rail, except where it meets the walls' walks.
    rail_walls.append(((wx1, UP, mz1), (ex0, UP + 110, mz1 + 4)))
    for k, (lo, hi) in enumerate(rail_walls):
        level.collider(f"Rail{k}", lo, hi)


# ==============================================================================================
# Props: tables, lockers, crates, pallets, drums, ammo cans
# ==============================================================================================

def make_props(level):
    table = nm.MeshBuilder("Table")
    box(table, PLYWOOD, (-90, 88, -40), (90, 92, 40), dirty=False)
    for sx in (-1, 1):
        for sz in (-1, 1):
            box(table, STEEL, (sx * 84 - 3, 0, sz * 34 - 3), (sx * 84 + 3, 88, sz * 34 + 3), dirty=False)
    box(table, STEEL, (-84, 20, -2), (84, 23, 2), dirty=False)
    table_path = write_mesh(level, "Table", table)

    locker = nm.MeshBuilder("Locker")
    box(locker, STEEL, (-45, 0, -25), (45, 185, 25), dirty=False)
    for x in (-15.0, 15.0):
        box(locker, STEEL, (x - 0.6, 10, 25), (x + 0.6, 180, 25.5), dirty=False)
    for x in (-30.0, 0.0, 30.0):
        for y in frange(150, 170, 5):
            box(locker, STEEL, (x - 8, y, 25), (x + 8, y + 1.5, 25.6), dirty=False)
    locker_path = write_mesh(level, "Locker", locker)

    crate = nm.MeshBuilder("Crate")
    box(crate, PLYWOOD, (-50, 0, -40), (50, 70, 40), dirty=False)
    for x in (-50.5, 50.5):
        box(crate, PLYWOOD, (min(x, x * 0.98), 0, -41), (max(x, x * 0.98), 70, 41), dirty=False)
    crate_path = write_mesh(level, "Crate", crate)

    pallet = nm.MeshBuilder("Pallet")
    for z in frange(-50, 50, 20):
        box(pallet, PLYWOOD, (-60, 12, z - 4.5), (60, 14.5, z + 4.5), dirty=False)
    for x in (-55.0, 0.0, 55.0):
        box(pallet, PLYWOOD, (x - 5, 0, -50), (x + 5, 12, 50), dirty=False)
    pallet_path = write_mesh(level, "Pallet", pallet)

    drum = nm.MeshBuilder("Drum")
    drum.lathe(STEEL, [(0, 0), (29, 0), (29, 2), (28.5, 30), (29.5, 31), (28.5, 32), (28.5, 58), (29.5, 59),
                       (28.5, 60), (29, 86), (29, 88), (0, 88)], segments=20)
    drum_path = write_mesh(level, "Drum", drum)

    can = nm.MeshBuilder("AmmoCan")
    box(can, STEEL, (-14, 0, -6), (14, 18, 6), dirty=False)
    box(can, STEEL, (-14.5, 18, -6.5), (14.5, 20, 6.5), dirty=False)
    box(can, STEEL, (-5, 20, -1), (5, 22, 1), dirty=False)
    can_path = write_mesh(level, "AmmoCan", can)

    # Staging: tables behind the booths, lockers on the east wall, crates and pallets round the edges.
    for k, x in enumerate((-500.0, 0.0, 500.0)):
        level.mesh_entity("Table", table_path, (x, 0, -1150))
        level.mesh_entity("AmmoCan", can_path, (x - 40, 92, -1150 + 10), yaw=12 * k, collide=False)
        level.mesh_entity("AmmoCan", can_path, (x + 25, 92, -1160), yaw=-80, collide=False)
    for k in range(6):
        level.mesh_entity("Locker", locker_path, (HW - 26, 0, -2300 + k * 92), yaw=-90, tint=(0.42, 0.48, 0.52))
    stacks = [(-1100, -2350, 0), (-950, -2380, 18), (900, -1500, -5), (1150, -1450, 30), (-1000, -1050, 8)]
    for k, (x, z, yaw) in enumerate(stacks):
        level.mesh_entity("Pallet", pallet_path, (x, 0, z), yaw=yaw)
        level.mesh_entity("Crate", crate_path, (x, 14.5, z), yaw=yaw + 4)
        if k % 2 == 0:
            level.mesh_entity("Crate", crate_path, (x + 5, 84.5, z - 3), yaw=yaw - 9)
    drums = [(1350, -900, 0.6, 0.12, 0.1), (1300, -820, 0.15, 0.25, 0.42), (1380, -760, 0.6, 0.12, 0.1),
             (-1300, 2380, 0.2, 0.22, 0.2), (1100, 2400, 0.6, 0.45, 0.08)]
    for x, z, r, g, b in drums:
        level.mesh_entity("Drum", drum_path, (x, 0, z), yaw=x * 0.1, tint=(r, g, b))


# ==============================================================================================
# Light fittings and lights
# ==============================================================================================

def make_lights(level):
    fixture = nm.MeshBuilder("TubeFixture")
    box(fixture, STEEL, (-62, 8, -10), (62, 12, 10), dirty=False)
    box(fixture, STEEL, (-62, 0, -10), (-60, 8, 10), dirty=False)
    box(fixture, STEEL, (60, 0, -10), (62, 8, 10), dirty=False)
    for z in (-4.0, 4.0):
        fixture.tube(FLUORESCENT, [(-58, 4, z), (58, 4, z)], 1.6, segments=8)
    for x in (-50.0, 50.0):
        fixture.tube(STEEL, [(x, 12, 0), (x, 200, 0)], 0.4, segments=4)    # chains, roughly
    fixture_path = write_mesh(level, "TubeFixture", fixture)

    cage = nm.MeshBuilder("CageLight")
    cage.lathe(STEEL, [(0, 0), (8, 0), (9, 3), (9, 12), (0, 12)], segments=12)
    cage.lathe(FLUORESCENT, [(0, -9), (5, -8), (6.5, -4), (6, 0), (0, 0)], segments=12)
    cage_path = write_mesh(level, "CageLight", cage)

    cool = rgb255(222, 236, 255)
    # Staging: four rows of hanging tubes.
    # (WorldBridge lights 16 point and spot lights a frame: every fitting glows, half of them light.)
    for x in (-1000.0, -350.0, 350.0, 1000.0):
        for z in (-2200.0, -1650.0, -1150.0):
            level.mesh_entity("Tube", fixture_path, (x, 760, z), yaw=90, collide=False)
    for x in (-675.0, 675.0):
        for z in (-2200.0, -1650.0, -1150.0):
            level.add("TubeLight", [("Point Light", point_light_xml(cool, 3.0, 950, source=40)),
                                    ("Transform", nm.transform_xml((x, 745, z)))], mobility=None)
    # Over the booths: one a lane.
    for k in range(LANES):
        x = LANE_X0 + (k + 0.5) * LANE_W
        level.mesh_entity("Tube", fixture_path, (x, 300, FIRING_Z - STALL_D / 2), yaw=90, collide=False)
    for x in (-300.0, 300.0):
        level.add("BoothLight", [("Point Light", point_light_xml(cool, 1.6, 560, source=30)),
                                 ("Transform", nm.transform_xml((x, 290, FIRING_Z - STALL_D / 2)))], mobility=None)
    # The targets: two shadowed floods from the south catwalk, down range.
    for x in (-400.0, 400.0):
        level.add("TargetFlood", [("Spot Light", spot_light_xml(rgb255(255, 240, 215), 16, 2900, 22, 38, shadows=True)),
                                  ("Transform", nm.transform_xml((x, 640, CROSS_Z[0] - 120), (21, 0, 0)))],
                  mobility=None)
    # Warm caged lights along the wall walks.
    for z in frange(-600, 2000, 650):
        for x in (WEST_X[0] + 8, EAST_X[1] - 8):
            level.mesh_entity("CageLight", cage_path, (x, 700, z), collide=False)
            if z in (50.0, 1350.0):
                level.add("WalkLight", [("Point Light", point_light_xml(rgb255(255, 196, 132), 1.1, 620, source=6)),
                                        ("Transform", nm.transform_xml((x, 690, z)))], mobility=None)


# ==============================================================================================
# Prefabs: the targets and the shooter
# ==============================================================================================

def physics_constraint_xml(kind, pivot=(0, 0, 0), axis=(1, 0, 0), limits=(0, 0)):
    return ("  <Component Type=\"Physics Constraint\" Version=\"1\">\n"
            "    <Enabled Value=\"true\" />\n"
            f"    <Type Value=\"{ {'Fixed': 0, 'Point': 1, 'Hinge': 2, 'Slider': 3, 'Distance': 4}[kind] }\" />\n"
            "    <TargetName Value=\"\" />\n"
            f"    <Pivot X=\"{nm.num(pivot[0])}\" Y=\"{nm.num(pivot[1])}\" Z=\"{nm.num(pivot[2])}\" />\n"
            f"    <Axis X=\"{nm.num(axis[0])}\" Y=\"{nm.num(axis[1])}\" Z=\"{nm.num(axis[2])}\" />\n"
            f"    <LimitMin Value=\"{nm.num(limits[0])}\" />\n"
            f"    <LimitMax Value=\"{nm.num(limits[1])}\" />\n"
            "    <Motor Value=\"false\" />\n"
            "    <MotorSpeed Value=\"0\" />\n"
            "  </Component>\n")


def entity_block(entity_id, name, components, parent=None, mobility=None):
    attrs = f" Id=\"{entity_id}\" Name=\"{name}\""
    if parent:
        attrs += f" Parent=\"{parent}\""
    if mobility:
        attrs += f" Mobility=\"{mobility}\""
    body = "".join(indent(text) for _, text in sorted(components, key=lambda c: c[0]))
    return f"  <Entity{attrs}>\n{body}  </Entity>\n"


def prefab_text(name, entities, script=None, tick="Off"):
    head = f"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Prefab Format=\"1\" Name=\"{name}\" NextId=\"{len(entities) + 1}\">\n"
    if script:
        head += f"  <Script Path=\"{script}\" />\n"
    head += f"  <Tick Mode=\"{tick}\" />\n"
    return head + "".join(entities) + "</Prefab>\n"


def make_prefabs(level):
    PREFABS.mkdir(parents=True, exist_ok=True)
    # Steel plate: an A-frame stand, the plate hung from a hinge on its crossbar.
    stand = nm.MeshBuilder("SteelStand")
    for x in (-32.0, 32.0):
        box(stand, STEEL, (x - 2.5, 0, -2.5), (x + 2.5, 168, 2.5), dirty=False)
        box(stand, STEEL, (x - 3, 0, -35), (x + 3, 4, 35), dirty=False)
    box(stand, STEEL, (-35, 160, -3), (35, 168, 3), dirty=False)
    stand_path = write_mesh(level, "SteelStand", stand)
    plate = nm.MeshBuilder("SteelPlate")
    plate.polygon(STEEL_TARGET, [(-16, -50, -0.7), (16, -50, -0.7), (16, -6, -0.7), (-16, -6, -0.7)], normal=(0, 0, -1))
    box(plate, STEEL_TARGET, (-16, -50, -0.7), (16, -6, 0.7), skip=("-z",), dirty=False)
    for x in (-10.0, 10.0):
        box(plate, STEEL, (x - 1.5, -8, -1), (x + 1.5, 0, 1), dirty=False)
    plate_path = write_mesh(level, "SteelPlate", plate)
    steel = prefab_text("SteelPlate", [
        entity_block(1, "SteelStand", [("Render", render_xml(stand_path)), ("Rigid Body", rigid_body_xml("Static")),
                                       ("Transform", nm.transform_xml((0, 0, 0)))], mobility="Static"),
        entity_block(2, "SteelPlate", [("Render", render_xml(plate_path)),
                                       ("Rigid Body", rigid_body_xml("Dynamic", mass=14, shape=1, linear_damping=0.1,
                                                                     angular_damping=0.25, restitution=0.2)),
                                       ("Physics Constraint", physics_constraint_xml("Hinge", limits=(-110, 110))),
                                       ("Transform", nm.transform_xml((0, 156, 0)))], parent=1),
    ])
    (PREFABS / "SteelPlate.nmprefab").write_text(steel, newline="\n")

    # Paper silhouette: two 2x4 uprights in a steel base, a card backer, the target stapled on.
    pstand = nm.MeshBuilder("PaperStand")
    for x in (-30.0, 30.0):
        box(pstand, PLYWOOD, (x - 2, 0, -4), (x + 2, 175, 4), dirty=False)
        box(pstand, STEEL, (x - 4, 0, -30), (x + 4, 6, 30), dirty=False)
    box(pstand, PLYWOOD, (-28, 75, 0.5), (28, 172, 1.5), dirty=False)
    pstand_path = write_mesh(level, "PaperStand", pstand)
    paper = nm.MeshBuilder("Paper")
    pts = [(-23, 78, -0.1), (23, 78, -0.1), (23, 170, -0.1), (-23, 170, -0.1)]
    paper.polygon(PAPER, pts, normal=(0, 0, -1), uvs=[(0, 1), (1, 1), (1, 0), (0, 0)])
    paper_path = write_mesh(level, "Paper", paper)
    paper_prefab = prefab_text("PaperTarget", [
        entity_block(1, "PaperStand", [("Render", render_xml(pstand_path)), ("Rigid Body", rigid_body_xml("Static")),
                                       ("Transform", nm.transform_xml((0, 0, 0)))], mobility="Static"),
        entity_block(2, "Paper", [("Render", render_xml(paper_path, casts=False)),
                                  ("Transform", nm.transform_xml((0, 0, 0)))], parent=1, mobility="Static"),
    ])
    (PREFABS / "PaperTarget.nmprefab").write_text(paper_prefab, newline="\n")

    # The shooter: the engine's crash dummy with its pistol, a camera on a boom (RangeMove swings it).
    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 20000), ("Active", False), ("Projection", 1),
                                            ("FovY", 62)])
    harness = nm.component_xml("AvatarHarness", 1, [("InputMap", "Range"), ("Simulation", "RangeMove"),
                                                    ("AutoPossess", 0), ("Enabled", True), ("ViewTarget", True),
                                                    ("Requestable", False)])
    animation = nm.component_xml("Animation", 1, [("Animation", DUMMY + "CrashDummy.nmanimstate"), ("PlayRate", 1),
                                                  ("Paused", False), ("ParameterCount", 0)])
    socket = ("  <Component Type=\"Anim Socket\" Version=\"1\">\n"
              "    <Bone Value=\"DEF-spine\" />\n"
              "    <PositionOffset X=\"-19.719\" Y=\"0.128\" Z=\"-4.773\" />\n"
              "    <RotationOffset X=\"-75.407\" Y=\"7.744\" Z=\"2.009\" />\n"
              "  </Component>\n")
    shooter = prefab_text("RangeShooter", [
        entity_block(1, "Shooter", [("AvatarHarness", harness), ("Animation", animation),
                                    ("Render", render_xml(DUMMY + "CrashDummy.nmmesh", materials=[mat("DummyPaint")] * 6)),
                                    ("Transform", nm.transform_xml((0, 0, 0)))]),
        entity_block(2, "Pistol", [("Anim Socket", socket),
                                   ("Render", render_xml(DUMMY + "Weapon_Pistol.nmmesh", materials=[mat("GunMetal")] * 2)),
                                   ("Transform", nm.transform_xml((0, 0, 0)))], parent=1),
        # The barrel's end, in the pistol's space (its barrel runs along -Z).
        entity_block(3, "Muzzle", [("Transform", nm.transform_xml((0, 5.5, -18)))], parent=2),
        entity_block(4, "CameraBoom", [("Transform", nm.transform_xml((0, 150, 0)))], parent=1),
        entity_block(5, "Camera", [("Camera", camera), ("Transform", nm.transform_xml((42, 12, -230)))], parent=4),
    ], script="Scripts/ShootingRange/RangeShooter.lua", tick="Tick")
    (PREFABS / "RangeShooter.nmprefab").write_text(shooter, newline="\n")

    # A target dummy: the crash dummy standing armed, its pistol in hand. Shot dead it ragdolls
    # (the engine's Ragdoll component: hit boxes while alive, Jolt's ragdoll once killed) and lets
    # the pistol go -- a disabled Rigid Body the ragdoll enables as it drops it.
    ragdoll = nm.component_xml("Ragdoll", 1, [
        ("Enabled", True), ("HitBoxes", True), ("StartActive", False), ("DropProps", True), ("Mass", 70),
        ("Thickness", 1), ("JointFriction", 1), ("Friction", 0.8), ("LinearDamping", 0.05), ("AngularDamping", 0.5),
        ("Layer", LAYER_WORLD), ("Mask", -1)])
    hand = ("  <Component Type=\"Anim Socket\" Version=\"1\">\n"
            "    <Bone Value=\"DEF-hand.R\" />\n"
            "    <PositionOffset X=\"-2.11\" Y=\"7.087\" Z=\"-0.769\" />\n"
            "    <RotationOffset X=\"-85.592\" Y=\"13.264\" Z=\"175.197\" />\n"
            "  </Component>\n")
    dummy = prefab_text("RangeDummy", [
        entity_block(1, "Dummy", [("Animation", animation), ("Ragdoll", ragdoll),
                                  ("Render", render_xml(DUMMY + "CrashDummy.nmmesh", materials=[mat("DummyPaint")] * 6)),
                                  ("Transform", nm.transform_xml((0, 0, 0)))]),
        entity_block(2, "Pistol", [("Anim Socket", hand),
                                   ("Render", render_xml(DUMMY + "Weapon_Pistol.nmmesh", materials=[mat("GunMetal")] * 2)),
                                   ("Rigid Body", rigid_body_xml("Dynamic", mass=1.1, shape=5, source=2, enabled=False,
                                                                 continuous=True, angular_damping=0.2)),
                                   ("Transform", nm.transform_xml((0, 0, 0)))], parent=1),
    ], script="Scripts/ShootingRange/RangeDummy.script.nmgraph", tick="Tick")
    (PREFABS / "RangeDummy.nmprefab").write_text(dummy, newline="\n")

    # The targets down range: steel at 14 m and 22 m, alternating lanes; paper at 18 m.
    for k in range(LANES):
        x = LANE_X0 + (k + 0.5) * LANE_W
        level.instance(f"Steel{k}", PREFAB_PATH + "SteelPlate.nmprefab", (x, 0, FIRING_Z + (1400 if k % 2 == 0 else 2200)))
        level.instance(f"Paper{k}", PREFAB_PATH + "PaperTarget.nmprefab", (x, 0, FIRING_Z + 1800))
    # Dummies at 10-12 m in every other lane, beside the lane's centre (away from the camera's shoulder) so the plate and paper
    # behind stay in sight; facing the line (imported characters face -Z).
    for k in (0, 2, 4):
        x = LANE_X0 + (k + 0.5) * LANE_W - 65
        level.instance(f"Dummy{k}", PREFAB_PATH + "RangeDummy.nmprefab", (x, 0, FIRING_Z + (1000 if k != 2 else 1200)))


# ==============================================================================================
# Particles and the post process look
# ==============================================================================================

EMITTER = """Emitter {name}
    Enabled true
    Type Sprite
    Modules Shape Velocity Forces
    Mesh Engine/Content/Meshes/SpriteOctagon.nmmesh
    Material {material}
    Offset 0,0,0
    Billboard {billboard}
    VelocityStretch {stretch}
    MeshOrientation RandomSpin
    RandomRotation true
    RotationRate 0.5
    SpawnMode Burst
    MaxParticles {max}
    Shape {shape}
    ShapeSurface false
    BoxSize 10,10,10
    ConeAngle {cone}
    RingWidth 0
    ShapeSpeed {speed}
    SpawnRate 0
    BurstCount {count}
    BurstInterval 0
    BurstCycles 1
    Lifetime {life}
    LifetimeVariance 0.4
    EmitterRadius {radius}
    Seed 0
    CullDistance 6000
    PauseOffscreen true
    Velocity {velocity}
    VelocitySpread {spread}
    Acceleration {accel}
    Drag {drag}
    Turbulence {turb}
    CollisionResponse None
    CollisionHeight -100000
    Bounce 0.3
    Friction 0.2
    FlipbookColumns 1
    FlipbookRows 1
    FlipbookFrames 0
    FlipbookRate 0
    FlipbookCycles 1
    FlipbookRandomStart false
    BeamEnd 0,200,0
    BeamSegments 16
    BeamNoise 0
    BeamNoiseSpeed 4
    SizeMode StartEnd
    StartSize {start}
    EndSize {end}
    SizeKeys
    ColorMode StartEnd
    StartColor {c0}
    EndColor {c1}
    ColorCurve 0.6
    FadeIn 0.0
    FadeOut 0.5
    ColorKeys
"""


def emitter(**kw):
    defaults = dict(billboard="FaceCamera", stretch=0, shape="Sphere", cone=25, speed=0, radius=1, velocity="0,0,0",
                    spread=0, accel="0,0,0", drag=0.5, turb=0, max=64)
    defaults.update(kw)
    return EMITTER.format(**defaults)


def make_particles():
    PARTICLES.mkdir(parents=True, exist_ok=True)
    steam = "Content/Materials/Steampunk/Steam.material.nmgraph"   # a soft lit puff
    muzzle = ("NimbleParticleSystem 1\n\n"
              + emitter(name="Flash", material=MUZZLE, count=3, life=0.05, radius=1, start=14, end=22,
                        c0="255,214,140,255", c1="255,140,50,0")
              + "\n" + emitter(name="Smoke", material=steam, count=6, life=0.9, radius=2, velocity="0,25,0",
                               spread=18, accel="0,20,0", turb=20, start=6, end=40, c0="150,148,145,70",
                               c1="140,140,140,0"))
    (PARTICLES / "MuzzleFlash.nmparticle").write_text(muzzle, newline="\n")
    sparks = ("NimbleParticleSystem 1\n\n"
              + emitter(name="Sparks", material=MUZZLE, billboard="VelocityAligned", stretch=0.03, shape="Cone",
                        cone=70, speed=420, count=24, life=0.35, radius=1, spread=120, accel="0,-900,0", drag=0.8,
                        start=2.2, end=0.6, c0="255,236,170,255", c1="255,110,30,0")
              + "\n" + emitter(name="Dust", material=steam, count=5, life=0.8, radius=3, velocity="0,30,0", spread=30,
                               start=8, end=36, c0="170,165,160,60", c1="160,160,160,0"))
    (PARTICLES / "Sparks.nmparticle").write_text(sparks, newline="\n")
    dust = ("NimbleParticleSystem 1\n\n"
            + emitter(name="Chips", material=steam, shape="Cone", cone=40, speed=220, count=10, life=0.5, radius=1,
                      spread=60, accel="0,-700,0", start=2, end=2, c0="120,110,95,255", c1="110,100,90,0")
            + "\n" + emitter(name="Dust", material=steam, count=8, life=1.2, radius=4, velocity="0,20,0", spread=30,
                             turb=15, start=10, end=55, c0="165,158,148,80", c1="160,155,150,0"))
    (PARTICLES / "Dust.nmparticle").write_text(dust, newline="\n")


def make_post():
    POST.mkdir(parents=True, exist_ok=True)
    nodes = [
        ("Effect.AmbientOcclusion", {"Intensity": "1.3", "Radius": "90", "Samples": "12", "Power": "1.8", "Blur": "true"}),
        ("Effect.ScreenSpaceReflections", {"Intensity": "0.8", "Max Roughness": "0.55", "Max Distance": "2500",
                                           "Thickness": "25", "Steps": "32"}),
        ("Effect.VolumetricFog", {"Density": "0.00016", "Height Falloff": "0.00008", "Base Height": "0",
                                  "Max Distance": "6000", "Anisotropy": "0.72", "Sun Scattering": "3.2",
                                  "Sky Scattering": "0.25", "Light Scattering": "1.2", "Albedo": "0.95 0.9 0.82",
                                  "Steps": "48", "Shadows": "true"}),
        ("Effect.Bloom", {"Threshold": "0.92", "Intensity": "0.35", "Radius": "0.7", "Levels": "6", "Tint": "1 0.97 0.92"}),
        ("Effect.AutoExposure", {"Key": "0.13", "Min EV": "-4", "Max EV": "3", "Speed Up": "2", "Speed Down": "1",
                                 "Centre Weight": "0.6"}),
        ("Effect.ToneMapping", {"Exposure": "0", "Tone Mapper": "ACES"}),
        ("Effect.Antialiasing", {"Method": "TAA", "Quality": "High", "Edge Threshold": "0.125", "Sharpness": "0.3"}),
        ("Effect.Material", {"Material": mat("Grit"), "Stage": "After Effects", "Order": "0"}),
    ]
    lines = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <Nodes>\n"]
    for k, (type_name, props) in enumerate(nodes):
        node_id = nm.guid("RangePost", type_name)
        lines.append(f"    <Node Id=\"{node_id}\" Type=\"{type_name}\" Version=\"1\" X=\"{-300 + 150 * k}\" Y=\"0\">\n")
        for key in sorted(props):
            lines.append(f"      <Prop Name=\"{key}\" Value=\"{props[key]}\" />\n")
        lines.append("    </Node>\n")
    lines.append("  </Nodes>\n  <Comments>\n    <Comment X=\"-350\" Y=\"-120\" Width=\"1300\" Height=\"90\" "
                 "Text=\"The Shooting Range: corners darkened (AO), the sealed slab reflecting (SSR), dusty air the "
                 "sun shafts through from the skylights (fog), tubes blooming, eye adaptation for the dim hall, "
                 "ACES, TAA, then the Grit grade (desaturated, cooled, vignette, grain).\" />\n  </Comments>\n</NodeGraph>\n")
    (POST / "Range.pp.nmgraph").write_text("".join(lines), newline="\n")


def make_environment(level):
    sun = nm.component_xml("Directional Light", 3, [
        ("Color", ("colour", (255, 244, 226))), ("Intensity", 11), ("Active", True), ("CastShadows", True),
        ("ShadowTechnique", 2), ("ShadowTint", ("colour", (25, 28, 36))), ("ShadowBias", 0.0012),
        ("ShadowNormalBias", 2), ("ShadowDistance", 7000), ("CascadeCount", 4), ("CascadeSplitLambda", 0.75),
        ("SourceAngle", 0.5)])
    level.add("Sun", [("Directional Light", sun),
                      ("Transform", nm.transform_xml((0, 6000, 0), (SUN_ELEVATION, SUN_AZIMUTH + 180.0, 0)))])
    sky = nm.component_xml("Sky Light", 2, [
        ("SkyColor", ("colour", (150, 165, 190))), ("GroundColor", ("colour", (60, 56, 50))), ("Intensity", 0.12),
        ("Active", True), ("Environment", SKY), ("EnvironmentIntensity", 0.3), ("EnvironmentRotation", 0),
        ("DrawBackground", True)])
    level.add("Sky", [("Sky Light", sky), ("Transform", nm.transform_xml((0, 0, 0)))])
    post = nm.component_xml("Post Process", 2, [("Stack", STACK), ("Priority", 0), ("Enabled", True), ("BlendDistance", 0)])
    level.add("PostProcessRange", [("Post Process", post), ("Transform", nm.transform_xml((0, 0, 0)))], mobility=None)
    # No reflection probes for now: on this branch they light a curved skinned body (the shooter) as if
    # its probe irradiance were unblurred -- a mirror finish whatever its roughness. The sky light
    # stands in. Put them back (PROBES = True) once that is fixed.
    probes = (("ProbeStaging", (0, 500, (-HL + FIRING_Z) / 2), (HW, 500, (FIRING_Z + HL) / 2)),
                               ("ProbeRange", (0, 500, (FIRING_Z + HL) / 2), (HW, 500, (HL - FIRING_Z) / 2)))
    for name, centre, half in (probes if PROBES else ()):
        probe = ("  <Component Type=\"Reflection Probe\" Version=\"1\">\n"
                 "    <Radius Value=\"3000\" />\n"
                 f"    <BoxExtent X=\"{nm.num(half[0])}\" Y=\"{nm.num(half[1])}\" Z=\"{nm.num(half[2])}\" />\n"
                 "    <Intensity Value=\"1\" />\n"
                 "    <Resolution Value=\"256\" />\n"
                 "    <Frequency Value=\"2\" />\n"
                 "    <Interval Value=\"1\" />\n"
                 "    <NearClip Value=\"5\" />\n"
                 "    <FarClip Value=\"8000\" />\n"
                 "    <CaptureShadows Value=\"true\" />\n"
                 "    <Enabled Value=\"true\" />\n"
                 "  </Component>\n")
        level.add(name, [("Reflection Probe", probe), ("Transform", nm.transform_xml(centre))])
    start = nm.component_xml("PlayerStart", 1, [("AvatarPrefab", PREFAB_PATH + "RangeShooter.nmprefab"),
                                                ("PlayerSlot", 0), ("Enabled", True)])
    level.add("PlayerStart", [("PlayerStart", start), ("Transform", nm.transform_xml((LANE_X0 + 2.5 * LANE_W, 0, -1750), (0, 0, 0)))],
              mobility=None)
    # The editor's view and a fallback camera before the shooter spawns: from the mezzanine, down range.
    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 20000), ("Active", True), ("Projection", 1),
                                            ("FovY", 62)])
    level.add("OverviewCamera", [("Camera", camera),
                                 ("Transform", nm.transform_xml((-1100, 640, -1500), (16, 28, 0)))], mobility=None)


def main():
    level = Level()
    print("Shooting Range:")
    MESHES.mkdir(parents=True, exist_ok=True)
    for stale in MESHES.glob("*.nmmesh"):
        stale.unlink()
    make_shell(level)
    make_structure(level)
    make_range(level)
    make_upper_level(level)
    make_props(level)
    make_lights(level)
    make_prefabs(level)
    make_particles()
    make_post()
    make_environment(level)
    level.write()
    if GENERATOR.exists():
        subprocess.run([str(GENERATOR), str(POST / "Range.pp.nmgraph")], check=True)
    else:
        print(f"  {GENERATOR} not found: open Range.pp.nmgraph in the editor and save it")
    size = sum(p.stat().st_size for p in MESHES.glob("*.nmmesh"))
    print(f"  {WORLD.relative_to(GAME_DIR)}: {len(level.entities)} entities, {len(level.instances)} instances")
    print(f"  {len(level.meshes)} meshes, {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
