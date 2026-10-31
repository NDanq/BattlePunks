#!/usr/bin/env python3
"""
make_steampunk_street.py -- writes the Steampunk street corner: WorldsLab's
first world, Content/Levels/Steampunk/SteampunkStreet.nmworld, as the
editor's File > Save writes a text world (the manifest, and a folder of
.nmentity and .nmprefabinst files), with everything it uses under a
Steampunk folder:

    Content/Meshes/Steampunk/       buildings, streets, clock tower, props
    Content/Prefabs/Steampunk/      GasLamp, SteamVent
    Content/Particles/Steampunk/    Steam, ChimneySmoke
    Content/PostProcess/Steampunk/  Street.pp.nmgraph (the look)

(The textures and materials come from make_steampunk_textures.py and
make_steampunk_materials.py.)

The plan, 1 unit = 1 cm, +Y up, north +Z, east +X:

                 ............ north row (backdrop) ............
          +------------------------------------------------------------+
          |    back street, cobbles                                     |
     side |  +-------------------+  cross  +-------------------+        | side
    street|  |   WEST BLOCK      |  street |   EAST BLOCK      |        |street
          |  |  24 m x 20 m      |  street |  24 m x 20 m      |        |
          |  |                   |         |  CLOCK TOWER at SW|        |
          |  +-------------------+         +-------------------+        |
          |    front street, cobbles, tram rails                        |
          +------------------------------------------------------------+
                 ............ south row (backdrop) ............

Two city blocks (1 x 2), each ringed by flagstone pavements and granite
curbs, walled by Victorian shop-houses -- brick over rusticated stone, sash
windows, shopfronts with awnings, mansard roofs with copper dormers. The
cross street meets the front and back streets in two four-way
intersections and runs on, north and south, to a closing building. The
clock tower stands on the east block's south-west corner, facing the front
intersection, where the Player starts.

It is late afternoon: a low sun down the front street (a cascaded Sun), a
hazy sky (Sky_HDR, the Sky Light's image-based lighting), gas lamps lit,
steam from the grates and the boiler, smoke from chimneys, volumetric haze
(Street.pp.nmgraph), and an airship overhead.

Entity GUIDs come from names, so running it again rewrites the same files.

    python Games/WorldsLab/Tools/Steampunk/make_steampunk_street.py
"""

import math
import os
import random
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nmformats as nm                       # noqa: E402
import steampunk_kit as kit                  # noqa: E402
from make_steampunk_textures import SUN_AZIMUTH, SUN_ELEVATION   # noqa: E402

GAME_DIR = Path(__file__).resolve().parents[2]
CONTENT = GAME_DIR / "Content"
WORLD = CONTENT / "Levels" / "Steampunk" / "SteampunkStreet.nmworld"
FOLDER = WORLD.with_suffix("")
MESHES = CONTENT / "Meshes" / "Steampunk"
PREFABS = CONTENT / "Prefabs" / "Steampunk"
PARTICLES = CONTENT / "Particles" / "Steampunk"
POST = CONTENT / "PostProcess" / "Steampunk"

MESH_PATH = "Content/Meshes/Steampunk/"
SKY = "Content/Textures/Steampunk/Sky_HDR.nmtexture"
STACK = "Content/PostProcess/Steampunk/Street.pp.nmgraph"
LAMP_PREFAB = "Content/Prefabs/Steampunk/GasLamp.nmprefab"
VENT_PREFAB = "Content/Prefabs/Steampunk/SteamVent.nmprefab"
STEAM = "Content/Particles/Steampunk/Steam.nmparticle"
SMOKE = "Content/Particles/Steampunk/ChimneySmoke.nmparticle"

SCOPE = "SteampunkStreet"
GENERATOR = GAME_DIR / "Binaries" / "Linux_Debug" / "NodeGraphGenerate"

# --- The plan ---------------------------------------------------------------------------------
# Everything is derived from the street's two widths and the blocks' size.
ROAD = 650.0                                         # a roadway, curb to curb
SIDEWALK = 220.0                                     # a pavement, curb to wall
STREET = ROAD + 2 * SIDEWALK                         # facade to facade
CURB_H = 15.0
BLOCK_X, BLOCK_Z = 2400.0, 2000.0                    # a block's buildings
OUTER_DEPTH = 1800.0                                 # the far rows' depth (north and south)
SIDE_DEPTH = 800.0                                   # the far rows' depth (west and east)

LOT_IN = ROAD / 2 + SIDEWALK                         # a block's wall on the cross street (|x|)
WEST_LOT = (-LOT_IN - BLOCK_X, -LOT_IN, -BLOCK_Z / 2, BLOCK_Z / 2)       # x0, x1, z0, z1
EAST_LOT = (LOT_IN, LOT_IN + BLOCK_X, -BLOCK_Z / 2, BLOCK_Z / 2)
CROSS_X = (-ROAD / 2, ROAD / 2)                      # the cross street's roadway
FRONT_C = -BLOCK_Z / 2 - SIDEWALK - ROAD / 2         # the front street's centre line (z)
BACK_C = -FRONT_C
FAR_Z = BLOCK_Z / 2 + STREET                         # the far rows' facades (|z|)
SIDE_X = LOT_IN + BLOCK_X + STREET                   # the side rows' facades (|x|)
SIDE_C = LOT_IN + BLOCK_X + STREET / 2               # the side streets' centre lines (|x|)
OUTER = SIDE_X + SIDE_DEPTH                          # the world's walls (|x|)
EXT_END = FAR_Z + OUTER_DEPTH                        # the cross street's extensions end here (|z|)
CLOSER_DEPTH = 800.0

# One mesh per row of buildings (one draw per material for the row) instead of
# one per building. Fewer draws for the render thread; coarser culling, and the
# editor selects a row, not a building. STEAMPUNK_MERGE_ROWS=1 turns it on.
MERGE_ROWS = os.environ.get("STEAMPUNK_MERGE_ROWS", "0") == "1"


def guid(name):
    return nm.guid(SCOPE, name)


# ==============================================================================================
# Entities
# ==============================================================================================

class Level:
    def __init__(self):
        self.entities = []       # (name, xml)
        self.instances = []      # (name, xml)
        self.names = set()
        self.meshes = {}         # key -> path
        self.chimney_tops = {}   # building mesh key -> its chimney pots, local
        self.chimneys = []       # every chimney pot in the world
        self.bounds = {}         # mesh path -> (lo, hi), its box

    def unique(self, name):
        base, k = name, 1
        while name in self.names:
            k += 1
            name = f"{base}_{k}"
        self.names.add(name)
        return name

    def add(self, name, components, mobility="Static", detail=None):
        """detail: the lowest scalability tier it renders at (DetailMode.h); None, every tier.
        Only for what nothing collides with -- a bucket hides the draw, not the Bounds."""
        name = self.unique(name)
        self.entities.append((name, nm.entity_xml(guid(name), name, components, mobility, detail=detail)))
        return name

    def mesh_entity(self, name, mesh_path, position, yaw=0.0, mobility="Static", occluder=False, casts=True,
                    scale=(1, 1, 1), rotation=None, solid=False, detail=None):
        """solid: a Bounds component boxing the mesh -- the explore mode walks round it."""
        render = render_xml(mesh_path, occluder=occluder, casts=casts)
        rot = rotation if rotation is not None else (0, yaw, 0)
        components = [("Render", render), ("Transform", nm.transform_xml(position, rot, scale))]
        if solid:
            components.append(("Bounds", bounds_xml(*self.bounds[mesh_path])))
        return self.add(name, components, mobility, detail)

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
        manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n",
                    "  <Settings GameMode=\"WorldsLabExplore\" />\n"]
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


def render_xml(mesh, occluder=False, casts=True):
    lines = ["  <Component Type=\"Render\" Version=\"5\">\n", "    <Visible Value=\"true\" />\n"]
    lines += [f"    <Material{slot} Value=\"\" />\n" for slot in range(8)]
    lines.append(f"    <Mesh Value=\"{mesh}\" />\n")
    lines.append("    <ParameterCount Value=\"0\" />\n")
    lines.append(f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n")
    lines.append("    <CastInsetShadow Value=\"false\" />\n")
    lines.append(f"    <Occluder Value=\"{'true' if occluder else 'false'}\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def point_light_xml(colour, intensity, radius, shadows=False):
    return nm.component_xml("Point Light", 2, [
        ("Color", ("colour", colour)), ("Intensity", intensity), ("Radius", radius), ("Active", True),
        ("CastShadows", shadows), ("ShadowTechnique", 0), ("ShadowTint", ("colour", (40, 45, 60))),
        ("ShadowBias", 0.004), ("ShadowNormalBias", 2), ("SourceRadius", 6)])


def particle_xml(system, rate=1.0):
    return ("  <Component Type=\"Particle System\" Version=\"3\">\n"
            f"    <System Value=\"{system}\" />\n"
            "    <Emitting Value=\"true\" />\n"
            "    <Tint R=\"255\" G=\"255\" B=\"255\" A=\"255\" />\n"
            f"    <SpawnRateScale Value=\"{nm.num(rate)}\" />\n"
            "    <TimeScale Value=\"1\" />\n"
            "    <Seed Value=\"0\" />\n"
            "    <UseBeamTarget Value=\"false\" />\n"
            "    <BeamTarget X=\"0\" Y=\"200\" Z=\"0\" />\n"
            "  </Component>\n")


def write_mesh(level, key, builder):
    if key in level.meshes:
        return level.meshes[key]
    path = MESHES / f"{key}.nmmesh"
    lo, hi = builder.write(path, lightmap=False)
    level.meshes[key] = MESH_PATH + f"{key}.nmmesh"
    level.bounds[level.meshes[key]] = (lo, hi)
    return level.meshes[key]


# ==============================================================================================
# The ground: roads, pavements, curbs, tram rails
# ==============================================================================================

def lots():
    """Every building footprint (x0, x1, z0, z1)."""
    out = [WEST_LOT, EAST_LOT]
    out += [(-OUTER, -LOT_IN, -EXT_END, -FAR_Z), (LOT_IN, OUTER, -EXT_END, -FAR_Z)]       # south rows
    out += [(-OUTER, -LOT_IN, FAR_Z, EXT_END), (LOT_IN, OUTER, FAR_Z, EXT_END)]           # north rows
    out += [(-OUTER, -SIDE_X, -FAR_Z, FAR_Z), (SIDE_X, OUTER, -FAR_Z, FAR_Z)]             # west, east rows
    out += [(-LOT_IN, LOT_IN, -EXT_END - CLOSER_DEPTH, -EXT_END),
            (-LOT_IN, LOT_IN, EXT_END, EXT_END + CLOSER_DEPTH)]                           # closers
    return out


def clear_meshes():
    """Meshes are named by row and building: an old layout's would linger."""
    MESHES.mkdir(parents=True, exist_ok=True)
    for stale in MESHES.glob("*.nmmesh"):
        stale.unlink()


def make_ground(level):
    footprints = lots()
    pavements = [(a - SIDEWALK, b + SIDEWALK, c - SIDEWALK, d + SIDEWALK) for a, b, c, d in footprints]
    xs = sorted({v for r in footprints + pavements for v in r[:2]} | {-OUTER, OUTER})
    zs = sorted({v for r in footprints + pavements for v in r[2:]} | {-EXT_END - CLOSER_DEPTH, EXT_END + CLOSER_DEPTH})
    xs = [x for x in xs if -OUTER <= x <= OUTER]
    zs = [z for z in zs if -EXT_END - CLOSER_DEPTH <= z <= EXT_END + CLOSER_DEPTH]

    def within(rects, x, z):
        return any(r[0] <= x <= r[1] and r[2] <= z <= r[3] for r in rects)

    kind = {}
    for i in range(len(xs) - 1):
        for j in range(len(zs) - 1):
            cx, cz = (xs[i] + xs[i + 1]) / 2, (zs[j] + zs[j + 1]) / 2
            kind[(i, j)] = "lot" if within(footprints, cx, cz) else ("walk" if within(pavements, cx, cz) else "road")

    road = nm.MeshBuilder("Streets")
    walk = nm.MeshBuilder("Pavements")
    # Big cells are cut into 4 m tiles so vertex grime can vary and the depth is well spread.
    tile = 200.0
    rng = random.Random(9)
    for (i, j), k in kind.items():
        if k == "lot":
            continue
        x0, x1, z0, z1 = xs[i], xs[i + 1], zs[j], zs[j + 1]
        y = 0.0 if k == "road" else CURB_H
        mat = kit.COBBLE if k == "road" else kit.FLAG
        target = road if k == "road" else walk
        nx = max(1, int(math.ceil((x1 - x0) / tile)))
        nz = max(1, int(math.ceil((z1 - z0) / tile)))
        for a in range(nx):
            for b in range(nz):
                ax0, ax1 = x0 + (x1 - x0) * a / nx, x0 + (x1 - x0) * (a + 1) / nx
                bz0, bz1 = z0 + (z1 - z0) * b / nz, z0 + (z1 - z0) * (b + 1) / nz
                pts = [(ax0, y, bz0), (ax0, y, bz1), (ax1, y, bz1), (ax1, y, bz0)]
                cols = [ground_grime(p, footprints, rng, road=(k == "road")) for p in pts]
                target.polygon(mat, pts, normal=(0, 1, 0), colours=cols)
        if k == "walk":
            # A curb stone wherever the pavement meets the road.
            for (di, dj, edge) in ((-1, 0, "x0"), (1, 0, "x1"), (0, -1, "z0"), (0, 1, "z1")):
                if kind.get((i + di, j + dj)) != "road":
                    continue
                w = 30.0
                if edge == "x0":
                    lo, hi = (x0, 0, z0), (x0 + w, CURB_H + 1, z1)
                elif edge == "x1":
                    lo, hi = (x1 - w, 0, z0), (x1, CURB_H + 1, z1)
                elif edge == "z0":
                    lo, hi = (x0, 0, z0), (x1, CURB_H + 1, z0 + w)
                else:
                    lo, hi = (x0, 0, z1 - w), (x1, CURB_H + 1, z1)
                walk.box(kit.CURB, lo, hi, skip=("-y",))
    # Tram rails down the front street, through both intersections.
    for z in (FRONT_C - 72, FRONT_C + 72):
        road.box(kit.PLATE, (-OUTER, -2, z - 4), (OUTER, 1.2, z + 4), colour_fn=lambda p, n: (0.85, 0.85, 0.85, 1))
    path = write_mesh(level, "Streets", road)
    level.mesh_entity("Streets", path, (0, 0, 0))
    path = write_mesh(level, "Pavements", walk)
    level.mesh_entity("Pavements", path, (0, 0, 0))


def ground_grime(p, footprints, rng, road=False):
    """Darker near the walls, a little mottling; on the road, wet along the
    gutters and in shallow puddles (alpha: the roughness scale)."""
    x, _, z = p
    d = min(min(abs(x - r[0]), abs(x - r[1])) if r[2] <= z <= r[3] else 1e9 for r in footprints)
    d2 = min(min(abs(z - r[2]), abs(z - r[3])) if r[0] <= x <= r[1] else 1e9 for r in footprints)
    near = min(d, d2)
    k = 1.0 - 0.3 * math.exp(-near / 120.0)
    k *= rng.uniform(0.84, 1.05)
    wet = 1.0
    if road:
        gutter = math.exp(-max(near - SIDEWALK, 0.0) / 60.0)          # the road's edge, by the curb
        puddle = max(0.0, math.sin(x * 0.0037 + 1.3) * math.sin(z * 0.0051 + 0.4) * math.sin((x + z) * 0.0023))
        puddle = min(1.0, puddle * 3.0) if rng.random() < 0.8 else 0.0
        wetness = max(gutter * 0.75, puddle)
        wet = 1.0 - 0.8 * wetness
        k *= 1.0 - 0.3 * wetness
    return (k, k, k, wet)


# ==============================================================================================
# Buildings
# ==============================================================================================

def row(level, name, length, depth, place, widths, seeds, styles, sides_first=(), sides_last=(), first_style=None):
    """A row of buildings along a street: an entity per building, or (MERGE_ROWS)
    one mesh for the row. place(offset, width) -> (position, yaw) for a
    building whose local x runs from offset to offset + width.

    Beside them, per building, a Bounds-only entity (its footprint, which the
    explore mode walks round), and for the row one Occluder: a box hidden
    inside each building. Only that cheap mesh fills the occlusion pre-pass,
    and the detailed buildings are tested and culled like everything else."""
    merged = nm.MeshBuilder(name)
    occluder = nm.MeshBuilder(name + "Occluder")
    offset = 0.0
    for k, w in enumerate(widths):
        sides = []
        if k == 0:
            sides += list(sides_first)
        if k == len(widths) - 1:
            sides += list(sides_last)
        seed = seeds[k % len(seeds)]
        floors, roof = styles[k % len(styles)]
        style = first_style if (k == 0 and first_style) else kit.Style(seed, floors, roof=roof)
        mb = kit.building(style, w, depth, sides=tuple(sides), seed=seed)
        position, yaw = place(offset, w)
        if MERGE_ROWS:
            transform_into(merged, mb, position, yaw)
        else:
            path = write_mesh(level, f"{name}{k}", mb)
            level.mesh_entity(f"{name}{k}", path, position, yaw)
        for local in style.chimney_tops:
            level.chimneys.append(to_world(local, position, yaw))
        # Collision: the walls' footprint (not the awnings, balconies or cornices over the pavement).
        level.add(f"{name}{k}Wall", [("Bounds", bounds_xml((-5, 0, -5), (w + 5, style.top, depth + 5))),
                                     ("Transform", nm.transform_xml(position, (0, yaw, 0)))])
        # The occluder: 80 in from every wall, past the window and shop recesses; up to the roof.
        box = nm.MeshBuilder("Box")
        box.box("", (80, 0, 80), (w - 80, style.top, depth - 80))
        transform_into(occluder, box, position, yaw)
        offset += w
    assert abs(offset - length) < 1e-3, (name, offset, length)
    if MERGE_ROWS:
        path = write_mesh(level, name, merged)
        level.mesh_entity(name, path, (0, 0, 0))
    path = write_mesh(level, name + "Occluder", occluder)
    level.mesh_entity(name + "Occluder", path, (0, 0, 0), occluder=True, casts=False)


def transform_into(target, source, position, yaw):
    """source's triangles, turned by yaw and placed at position, added to target (UVs kept)."""
    c, s_ = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    for material in source.order:
        for tri in source.sections[material]:
            verts = []
            for (p, n, uv, col, chart) in tri:
                verts.append((to_world(p, position, yaw), (n[0] * c + n[2] * s_, n[1], -n[0] * s_ + n[2] * c), uv, col))
            target.triangle_raw(material, verts, tri[0][4])


def to_world(local, position, yaw):
    """A point in an entity's frame to the world: turned by yaw about Y (+Z to (sin, 0, cos))."""
    c, s_ = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    x, y, z = local
    return (position[0] + x * c + z * s_, position[1] + y, position[2] - x * s_ + z * c)


def make_buildings(level):
    rng = random.Random(77)
    s_mixed = [(3, "mansard"), (3, "flat"), (4, "flat"), (2, "mansard"), (3, "flat"), (4, "mansard")]

    # --- The west block: south row faces the front street, north row the back street.
    x0, x1, z0, z1 = WEST_LOT
    row(level, "WestSouth", BLOCK_X, BLOCK_Z / 2, lambda o, w: ((x0 + o, 0, z0), 0.0),
        [650, 550, 600, 600], [11, 12, 13, 14], s_mixed, sides_first=("left",), sides_last=("right",))
    row(level, "WestNorth", BLOCK_X, BLOCK_Z / 2, lambda o, w: ((x1 - o, 0, z1), 180.0),
        [600, 650, 500, 650], [15, 16, 17, 18], s_mixed[2:] + s_mixed[:2], sides_first=("left",),
        sides_last=("right",))

    # --- The east block: its south-west corner building carries the clock tower.
    x0, x1, z0, z1 = EAST_LOT
    hero = kit.Style(21, 4, roof="flat")
    row(level, "EastSouth", BLOCK_X, BLOCK_Z / 2, lambda o, w: ((x0 + o, 0, z0), 0.0),
        [700, 550, 600, 550], [21, 22, 23, 24], [(4, "flat")] + s_mixed[1:], sides_first=("left",),
        sides_last=("right",), first_style=hero)
    tower = kit.clock_tower(hero.top + 10.0)
    path = write_mesh(level, "ClockTower", tower)
    level.mesh_entity("ClockTower", path, (x0 + 230, 0, z0 + 230))
    row(level, "EastNorth", BLOCK_X, BLOCK_Z / 2, lambda o, w: ((x1 - o, 0, z1), 180.0),
        [650, 600, 550, 600], [25, 26, 27, 28], s_mixed[3:] + s_mixed[:3], sides_first=("left",),
        sides_last=("right",))

    # --- The backdrop: the far sides of the streets.
    pool = [31, 32, 33, 34, 35, 36]
    far = OUTER - LOT_IN
    widths_a = fit([700, 600, 800, 600, 600, 700, 800], far)
    # South rows face north (+Z); their local x runs west from the place given.
    row(level, "SouthW", far, OUTER_DEPTH, lambda o, w: ((-LOT_IN - o, 0, -FAR_Z), 180.0),
        widths_a, pool, s_mixed, sides_first=("left",))
    row(level, "SouthE", far, OUTER_DEPTH, lambda o, w: ((OUTER - o, 0, -FAR_Z), 180.0),
        widths_a[::-1], pool[::-1], s_mixed, sides_last=("right",))
    row(level, "NorthW", far, OUTER_DEPTH, lambda o, w: ((-OUTER + o, 0, FAR_Z), 0.0),
        widths_a, pool[2:] + pool[:2], s_mixed, sides_last=("right",))
    row(level, "NorthE", far, OUTER_DEPTH, lambda o, w: ((LOT_IN + o, 0, FAR_Z), 0.0),
        widths_a[::-1], pool[4:] + pool[:4], s_mixed, sides_first=("left",))
    widths_b = fit([800, 700, 600, 700, 600, 800, 1000], 2 * FAR_Z)
    # West row faces east (+X): yaw -90, local x runs +Z. East row faces west: yaw 90, local x runs -Z.
    row(level, "WestRow", 2 * FAR_Z, SIDE_DEPTH, lambda o, w: ((-SIDE_X, 0, -FAR_Z + o), -90.0),
        widths_b, pool[1:] + pool[:1], s_mixed)
    row(level, "EastRow", 2 * FAR_Z, SIDE_DEPTH, lambda o, w: ((SIDE_X, 0, FAR_Z - o), 90.0),
        widths_b[::-1], pool[3:] + pool[:3], s_mixed)
    # The cross street's closing buildings.
    row(level, "CloseSouth", 2 * LOT_IN, CLOSER_DEPTH, lambda o, w: ((LOT_IN - o, 0, -EXT_END), 180.0),
        [2 * LOT_IN], [41], [(4, "mansard")])
    row(level, "CloseNorth", 2 * LOT_IN, CLOSER_DEPTH, lambda o, w: ((-LOT_IN + o, 0, EXT_END), 0.0),
        [2 * LOT_IN], [42], [(5, "flat")])


def fit(widths, length):
    """widths scaled to sum to length, each rounded to 10 units, the last taking the remainder."""
    total = sum(widths)
    out = [round(w * length / total / 10.0) * 10.0 for w in widths[:-1]]
    return out + [length - sum(out)]


# ==============================================================================================
# Props, lamps, steam
# ==============================================================================================

def make_props(level):
    paths = {
        "GasLamp": write_mesh(level, "GasLamp", kit.gas_lamp()),
        "Bollard": write_mesh(level, "Bollard", kit.bollard()),
        "Barrel": write_mesh(level, "Barrel", kit.barrel()),
        "Crate": write_mesh(level, "Crate", kit.crate()),
        "AdvertColumn": write_mesh(level, "AdvertColumn", kit.advert_column()),
        "Boiler": write_mesh(level, "Boiler", kit.boiler()),
        "SteamGrate": write_mesh(level, "SteamGrate", kit.manhole()),
        "StreetSign": write_mesh(level, "StreetSign", kit.street_sign()),
        "Airship": write_mesh(level, "Airship", kit.airship()),
    }
    write_prefabs(paths)

    # --- Gas lamps: along every pavement's curb side, about 8 m apart.
    lamp_spots = []
    edge = 55.0       # in from the curb
    for (x0, x1, z0, z1), tag in ((WEST_LOT, "W"), (EAST_LOT, "E")):
        for x in frange(x0 + 250, x1 - 250, 4):
            lamp_spots.append((x, z0 - SIDEWALK + edge, f"{tag}S"))
            lamp_spots.append((x, z1 + SIDEWALK - edge, f"{tag}N"))
    # The far pavements, opposite, staggered.
    for x in (-3500, -2300, -1100, 1100, 2300, 3500):
        lamp_spots.append((x + 250, -FAR_Z + SIDEWALK - edge, "FarS"))
        lamp_spots.append((x - 250, FAR_Z - SIDEWALK + edge, "FarN"))
    for z in (-1300, 0, 1300):
        lamp_spots.append((-SIDE_X + SIDEWALK - edge, z + 300, "FarW"))
        lamp_spots.append((SIDE_X - SIDEWALK + edge, z - 300, "FarE"))
    # The cross street, its west pavement.
    for z in (-400, 400):
        lamp_spots.append((CROSS_X[0] - edge, z, "CrossW"))
    for k, (x, z, t) in enumerate(lamp_spots):
        level.instance(f"GasLamp_{t}{k}", LAMP_PREFAB, (x, CURB_H, z), yaw=(k * 37) % 90)

    # --- The front intersection's corners: bollards, signs, advertising columns.
    zc = -BLOCK_Z / 2
    for (x, z) in ((LOT_IN - 50, zc - SIDEWALK + 40), (LOT_IN - SIDEWALK + 40, zc - 50),
                   (LOT_IN - SIDEWALK + 45, zc - SIDEWALK + 45)):
        level.mesh_entity("Bollard", paths["Bollard"], (x, CURB_H, z), solid=True)
    level.mesh_entity("StreetSign", paths["StreetSign"], (-LOT_IN - SIDEWALK + 50, CURB_H, zc - SIDEWALK + 50), 0.0,
                      solid=True)
    level.mesh_entity("StreetSign", paths["StreetSign"], (LOT_IN + 50, CURB_H, -zc + SIDEWALK - 50), 180.0, solid=True)
    level.mesh_entity("AdvertColumn", paths["AdvertColumn"], (-1150, CURB_H, zc - SIDEWALK + 110), solid=True)
    level.mesh_entity("AdvertColumn", paths["AdvertColumn"], (-LOT_IN - 160, CURB_H, -FAR_Z + 110), solid=True)

    # --- The boiler on the cross street's east pavement, piped into the clock building; steam from its valve.
    bx = (CROSS_X[1] + LOT_IN) / 2
    level.mesh_entity("Boiler", paths["Boiler"], (bx, CURB_H, 250), 90.0, solid=True)
    level.add("BoilerSteam", [("Particle System", particle_xml(STEAM, 0.6)),
                              ("Transform", nm.transform_xml((bx, CURB_H + 275, 190)))], mobility=None, detail="Medium")
    rng = random.Random(5)
    wx = -(CROSS_X[1] + LOT_IN) / 2
    for k, (x, z) in enumerate(((bx, -350), (bx + 50, -470), (bx, -560), (wx, 600), (wx - 40, 470), (wx + 10, 520))):
        if k % 3 == 2:
            level.mesh_entity("Crate", paths["Crate"], (x, CURB_H, z), rng.uniform(0, 90), solid=True)
        else:
            level.mesh_entity("Barrel", paths["Barrel"], (x, CURB_H, z), rng.uniform(0, 360), solid=True)
    level.mesh_entity("Crate", paths["Crate"], (wx - 30, CURB_H + 70, 520), 25.0, solid=True)
    for k, (x, y, z) in enumerate(((-2400, 0, BACK_C + 150), (2000, CURB_H, -FAR_Z + 100), (-SIDE_C + 150, 0, -400),
                                   (SIDE_C - 150, 0, 900))):
        level.mesh_entity("Barrel", paths["Barrel"], (x, y, z), k * 50.0, solid=True)

    # --- Steam vents: grates in the road with steam rising (prefabs).
    for (x, z) in ((200, FRONT_C + 100), (-2100, FRONT_C - 100), (2450, BACK_C), (-SIDE_C, 900),
                   (0, (FAR_Z + EXT_END) / 2)):
        level.instance("SteamVent", VENT_PREFAB, (x, 0, z))

    # --- Overhead pipes across the cross street, a leaking flange.
    pipes = nm.MeshBuilder("OverheadPipes")
    a, b = LOT_IN, ROAD / 2
    for (z, y, r) in ((-250, 720, 9.0), (-205, 735, 6.0)):
        pipes.tube(kit.COPPER, [(-a, y, z), (-b, y, z), (-b + 70, y - 40, z), (b - 70, y - 40, z), (b, y, z),
                                (a, y, z)], r, segments=12)
        for x in (-b + 30, -100, 100, b - 30):
            pipes.lathe(kit.BRASS, [(r + 4, -5), (r + 4, 5)], centre=(x, y - 40, z), segments=12, axis="x")
    for x in (-a, a):
        pipes.box(kit.IRON, (x - (0 if x < 0 else 40), 690, -280), (x + (40 if x < 0 else 0), 700, -180))
    path = write_mesh(level, "OverheadPipes", pipes)
    # Scalability buckets (RenderCore's SCALABILITY.md): dressing nothing collides with.
    level.mesh_entity("OverheadPipes", path, (0, 0, 0), detail="High")
    level.add("PipeLeak", [("Particle System", particle_xml(STEAM, 0.35)),
                           ("Transform", nm.transform_xml((100, 690, -250)))], mobility=None, detail="High")

    # --- Chimney smoke from every fifth chimney.
    for k, (x, y, z) in enumerate(level.chimneys):
        if k % 5 == 2:
            level.add("ChimneySmoke", [("Particle System", particle_xml(SMOKE, 1.0)),
                                       ("Transform", nm.transform_xml((x, y, z)))], mobility=None, detail="Medium")

    # --- The airship, high over the back street, heading east-north-east.
    level.mesh_entity("Airship", paths["Airship"], (-1200, 5200, 3400), -18.0, mobility=None, casts=True, detail="Ultra")


def frange(a, b, n):
    return [a + (b - a) * k / (n - 1) for k in range(n)]


def write_prefabs(paths):
    """GasLamp: the lamp mesh, its warm point light at the mantle. SteamVent:
    an iron grate and a steam emitter."""
    PREFABS.mkdir(parents=True, exist_ok=True)
    lamp = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Prefab Format=\"1\" Name=\"GasLamp\" NextId=\"3\">\n"
            "  <Tick Mode=\"Off\" />\n"
            "  <Entity Id=\"1\" Name=\"GasLamp\" Mobility=\"Static\">\n"
            + indent(bounds_xml((-26, 0, -26), (26, 480, 26)))
            + indent(render_xml(paths["GasLamp"]))
            + indent(nm.transform_xml((0, 0, 0)))
            + "  </Entity>\n"
            "  <Entity Id=\"2\" Name=\"Flame\" Parent=\"1\" Mobility=\"Static\">\n"
            + indent(point_light_xml((255, 176, 104), 3.5, 900))
            + indent(nm.transform_xml((0, 400, 0)))
            + "  </Entity>\n"
            "</Prefab>\n")
    (PREFABS / "GasLamp.nmprefab").write_text(lamp, newline="\n")
    vent = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Prefab Format=\"1\" Name=\"SteamVent\" NextId=\"3\">\n"
            "  <Tick Mode=\"Off\" />\n"
            "  <Entity Id=\"1\" Name=\"SteamVent\" Mobility=\"Static\">\n"
            + indent(render_xml(paths["SteamGrate"], casts=False))
            + indent(nm.transform_xml((0, 0, 0)))
            + "  </Entity>\n"
            "  <Entity Id=\"2\" Name=\"Steam\" Parent=\"1\">\n"
            + indent(particle_xml(STEAM, 1.0))
            + indent(nm.transform_xml((0, 8, 0)))
            + "  </Entity>\n"
            "</Prefab>\n")
    (PREFABS / "SteamVent.nmprefab").write_text(vent, newline="\n")


def indent(text):
    return "".join("  " + line + "\n" for line in text.rstrip("\n").split("\n"))


# ==============================================================================================
# Particles and the post process look
# ==============================================================================================

PARTICLE_TEMPLATE = """NimbleParticleSystem 1

Emitter {name}
    Enabled true
    Type Sprite
    Modules Shape Velocity Forces
    Mesh Engine/Content/Meshes/SpriteOctagon.nmmesh
    Material Content/Materials/Steampunk/Steam.material.nmgraph
    Offset 0,0,0
    Billboard FaceCamera
    VelocityStretch 0
    MeshOrientation RandomSpin
    RandomRotation true
    RotationRate 0.3
    SpawnMode Continuous
    MaxParticles {max}
    Shape Sphere
    ShapeSurface false
    BoxSize 100,100,100
    ConeAngle 25
    RingWidth 0
    ShapeSpeed 0
    SpawnRate {rate}
    BurstCount 32
    BurstInterval 0
    BurstCycles 1
    Lifetime {life}
    LifetimeVariance 0.35
    EmitterRadius {radius}
    Seed 0
    CullDistance 9000
    PauseOffscreen true
    Velocity {velocity}
    VelocitySpread {spread}
    Acceleration {accel}
    Drag 0.6
    Turbulence {turb}
    CollisionResponse Bounce
    CollisionHeight -100000
    Bounce 0.4
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
    FadeIn 0.12
    FadeOut 0.6
    ColorKeys
"""


def make_particles():
    PARTICLES.mkdir(parents=True, exist_ok=True)
    (PARTICLES / "Steam.nmparticle").write_text(PARTICLE_TEMPLATE.format(
        name="Steam", max=400, rate=40, life=4.5, radius=18, velocity="0,170,0", spread=30, accel="25,10,10",
        turb=60, start=45, end=340, c0="165,163,160,55", c1="150,150,150,0"), newline="\n")
    (PARTICLES / "ChimneySmoke.nmparticle").write_text(PARTICLE_TEMPLATE.format(
        name="Smoke", max=300, rate=10, life=14, radius=25, velocity="0,90,0", spread=15, accel="40,4,20",
        turb=40, start=140, end=1100, c0="62,58,55,50", c1="95,92,90,0"), newline="\n")


def make_post():
    POST.mkdir(parents=True, exist_ok=True)
    nodes = [
        ("Effect.AmbientOcclusion", {"Intensity": "1.1", "Radius": "70", "Samples": "12", "Power": "1.6",
                                     "Blur": "true"}),
        ("Effect.ScreenSpaceReflections", {"Intensity": "1", "Max Roughness": "0.5", "Max Distance": "2500",
                                           "Thickness": "25", "Steps": "32"}),
        ("Effect.VolumetricFog", {"Density": "0.0001", "Height Falloff": "0.00045", "Base Height": "0",
                                  "Max Distance": "12000", "Anisotropy": "0.65", "Sun Scattering": "1.2",
                                  "Sky Scattering": "0.8", "Light Scattering": "1.4", "Albedo": "1 0.92 0.8",
                                  "Steps": "32", "Shadows": "true"}),
        ("Effect.Bloom", {"Threshold": "0.95", "Intensity": "0.45", "Radius": "0.75", "Levels": "6",
                          "Tint": "1 0.92 0.82"}),
        ("Effect.AutoExposure", {"Key": "0.16", "Min EV": "-3", "Max EV": "4", "Speed Up": "2",
                                 "Speed Down": "1", "Centre Weight": "0.5"}),
        ("Effect.ToneMapping", {"Exposure": "0", "Tone Mapper": "ACES"}),
        ("Effect.Antialiasing", {"Method": "TAA", "Quality": "High", "Edge Threshold": "0.125",
                                 "Sharpness": "0.35"}),
    ]
    lines = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n",
             "<NodeGraph Version=\"1\" Schema=\"PostProcess\">\n  <Nodes>\n"]
    for k, (type_name, props) in enumerate(nodes):
        node_id = nm.guid("SteampunkPost", type_name)
        lines.append(f"    <Node Id=\"{node_id}\" Type=\"{type_name}\" Version=\"1\" X=\"{-300 + 150 * k}\" Y=\"0\">\n")
        for key in sorted(props):
            lines.append(f"      <Prop Name=\"{key}\" Value=\"{props[key]}\" />\n")
        lines.append("    </Node>\n")
    lines.append("  </Nodes>\n  <Comments>\n    <Comment X=\"-350\" Y=\"-120\" Width=\"1100\" Height=\"90\" "
                 "Text=\"The Steampunk street: corners darkened (AO), wet cobbles and glass reflecting (SSR), a "
                 "warm industrial haze the low sun shafts through (fog), lamps blooming, eye adaptation, ACES, "
                 "TAA. No Bounds: everywhere.\" />\n  </Comments>\n</NodeGraph>\n")
    (POST / "Street.pp.nmgraph").write_text("".join(lines), newline="\n")


# ==============================================================================================
# Lights, sky, camera
# ==============================================================================================

def make_environment(level):
    sun = nm.component_xml("Directional Light", 3, [
        ("Color", ("colour", (255, 208, 160))), ("Intensity", 14), ("Active", True), ("CastShadows", True),
        ("ShadowTechnique", 2), ("ShadowTint", ("colour", (40, 50, 80))), ("ShadowBias", 0.0012),
        ("ShadowNormalBias", 2), ("ShadowDistance", 6000), ("CascadeCount", 4), ("CascadeSplitLambda", 0.8),
        ("SourceAngle", 0.6)])
    # Shines along its forward: pitch down by the elevation, yaw opposite the sun's azimuth.
    level.add("Sun", [("Directional Light", sun),
                      ("Transform", nm.transform_xml((0, 6000, 0), (SUN_ELEVATION, SUN_AZIMUTH + 180.0, 0)))])
    sky = nm.component_xml("Sky Light", 2, [
        ("SkyColor", ("colour", (150, 160, 180))), ("GroundColor", ("colour", (70, 60, 50))), ("Intensity", 0.6),
        ("Active", True), ("Environment", SKY), ("EnvironmentIntensity", 1.0), ("EnvironmentRotation", 0),
        ("DrawBackground", True)])
    level.add("Sky", [("Sky Light", sky), ("Transform", nm.transform_xml((0, 0, 0)))])

    post = nm.component_xml("Post Process", 2, [("Stack", STACK), ("Priority", 0), ("Enabled", True),
                                                ("BlendDistance", 0)])
    level.add("PostProcessStreet", [("Post Process", post), ("Transform", nm.transform_xml((0, 0, 0)))],
              mobility=None)

    # Reflection probes: boxes fitted to the streets, captured once.
    for name, centre, extent in (("ProbeFront", (0, 600, FRONT_C), (2 * OUTER, 700, STREET)),
                                 ("ProbeBack", (0, 600, BACK_C), (2 * OUTER, 700, STREET)),
                                 ("ProbeCross", (0, 600, 0), (STREET, 700, 2 * FAR_Z)),
                                 ("ProbeWest", (-SIDE_C, 600, 0), (STREET, 700, 2 * FAR_Z)),
                                 ("ProbeEast", (SIDE_C, 600, 0), (STREET, 700, 2 * FAR_Z))):
        probe = ("  <Component Type=\"Reflection Probe\" Version=\"1\">\n"
                 "    <Radius Value=\"3000\" />\n"
                 f"    <BoxExtent X=\"{nm.num(extent[0] / 2)}\" Y=\"{nm.num(extent[1])}\" Z=\"{nm.num(extent[2] / 2)}\" />\n"
                 "    <Intensity Value=\"1\" />\n"
                 "    <Resolution Value=\"256\" />\n"
                 "    <Frequency Value=\"2\" />\n"
                 "    <Interval Value=\"1\" />\n"
                 "    <NearClip Value=\"5\" />\n"
                 "    <FarClip Value=\"20000\" />\n"
                 "    <CaptureShadows Value=\"true\" />\n"
                 "    <Enabled Value=\"true\" />\n"
                 "  </Component>\n")
        level.add(name, [("Reflection Probe", probe), ("Transform", nm.transform_xml(centre))])

    # The player: on the front street's far pavement, west of the intersection, looking at the clock tower.
    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 40000), ("Active", True), ("Projection", 1),
                                            ("FovY", 65)])
    player_input = nm.component_xml("Input", 1, [("Map", "Default"), ("PlayerIndex", 0), ("Enabled", True)])
    level.add("Player", [("Camera", camera), ("Input", player_input),
                         ("Transform", nm.transform_xml((-1700, 170, -FAR_Z + 110), (-17, 62, 0)))], mobility=None)


def main():
    level = Level()
    print(f"Steampunk street ({'rows merged' if MERGE_ROWS else 'a mesh per building'}):")
    clear_meshes()
    make_ground(level)
    make_buildings(level)
    make_props(level)
    make_particles()
    make_post()
    make_environment(level)
    level.write()
    # The stack's <gen_code>, compiled by the engine's own generator (as the editor's Save does).
    if GENERATOR.exists():
        subprocess.run([str(GENERATOR), str(POST / "Street.pp.nmgraph")], check=True)
    else:
        print(f"  {GENERATOR} not found: open Street.pp.nmgraph in the editor and save it")
    size = sum(p.stat().st_size for p in MESHES.glob("*.nmmesh"))
    print(f"  {WORLD.relative_to(GAME_DIR)}: {len(level.entities)} entities, {len(level.instances)} instances")
    print(f"  {len(level.meshes)} meshes, {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
