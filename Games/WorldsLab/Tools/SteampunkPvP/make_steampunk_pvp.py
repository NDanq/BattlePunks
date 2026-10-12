#!/usr/bin/env python3
"""
make_steampunk_pvp.py -- writes Steampunk PvP's level,
Content/Levels/SteampunkPvP.nmworld: the Steampunk street
(Tools/Steampunk/make_steampunk_street.py, run that first) made something
to fight in. NetCore's sample mode plays it (WorldsLabPvP.cpp).

From the street, as it is on disk:

  - every entity and prefab instance, but the flyover's cameras, its
    Sequence Player and its director, and the explore mode's "Player" camera
  - collision for the walkers and the rounds (JoltPhysics, layer 1): a static
    Rigid Body on every entity with a Bounds (the buildings' footprints, the
    props) and on the ground meshes (Streets, Pavements), whose triangles
    it takes

and then:

  - invisible walls round the playable streets, so nobody walks off the end
    of one
  - eight Player Starts round the loop and on the cross street
  - an overview camera, for the moment before a fighter spawns

The plan (make_steampunk_street.py): two blocks, the front and back streets
along z = -1545 and +1545, side streets along x = -3490 and +3490, and the
cross street along x = 0, running on north and south to z = +-3890. One
loop round the blocks, crossed in the middle.

Entity GUIDs are the street's (copied entities) or from names (new ones), so
running it again rewrites the same files.

    python Games/WorldsLab/Tools/SteampunkPvP/make_steampunk_pvp.py
"""

import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "Steampunk"))
import nmformats as nm                                     # noqa: E402

GAME_DIR = HERE.parents[1]
LEVELS = GAME_DIR / "Content" / "Levels"
SOURCE_WORLD = LEVELS / "Steampunk" / "SteampunkStreet.nmworld"
SOURCE = SOURCE_WORLD.with_suffix("")
WORLD = LEVELS / "SteampunkPvP.nmworld"
FOLDER = WORLD.with_suffix("")
SCOPE = "SteampunkPvP"
PREFAB = "Content/Prefabs/SteampunkPvP/PvpFighter.nmprefab"

LAYER_WORLD = 1

# What the street has that a fight does not: the flyover and the explore camera.
DROP_NAMES = re.compile(r'^(CineCam_.*|Cinematic|Player)$')
DROP_PREFABS = ("Content/Prefabs/CinematicDirector.nmprefab",)
GROUND = ("Streets", "Pavements")

# The plan's numbers (make_steampunk_street.py).
ROAD, SIDEWALK = 650.0, 220.0
STREET = ROAD + 2 * SIDEWALK
BLOCK_X, BLOCK_Z = 2400.0, 2000.0
LOT_IN = ROAD / 2 + SIDEWALK
FAR_Z = BLOCK_Z / 2 + STREET
SIDE_X = LOT_IN + BLOCK_X + STREET
EXT_END = FAR_Z + 1800.0
FRONT_C = -BLOCK_Z / 2 - SIDEWALK - ROAD / 2
SIDE_C = LOT_IN + BLOCK_X + STREET / 2


def guid(name):
    return nm.guid(SCOPE, name)


def rigid_body_xml():
    """Static, Auto: a mesh's triangles, or the Bounds box where there is no mesh."""
    return nm.component_xml("Rigid Body", 1, [
        ("Enabled", True), ("Motion", 2), ("Shape", 0), ("ShapeSource", 0), ("Mass", 0.0),
        ("Friction", 0.6), ("Restitution", 0.0), ("LinearDamping", 0.05), ("AngularDamping", 0.05),
        ("GravityFactor", 1), ("Continuous", False), ("Sensor", False), ("AllowSleeping", True),
        ("Layer", LAYER_WORLD), ("Mask", -1)]).replace(
        "  </Component>\n",
        "    <InitialLinearVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n"
        "    <InitialAngularVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n  </Component>\n")


def bounds_xml(lo, hi):
    return ("  <Component Type=\"Bounds\" Version=\"1\">\n"
            f"    <Local MinX=\"{nm.num(lo[0])}\" MinY=\"{nm.num(lo[1])}\" MinZ=\"{nm.num(lo[2])}\" "
            f"MaxX=\"{nm.num(hi[0])}\" MaxY=\"{nm.num(hi[1])}\" MaxZ=\"{nm.num(hi[2])}\" />\n"
            "    <Static Value=\"true\" />\n"
            "  </Component>\n")


def with_component(text, type_name, xml):
    """Inserts a component into an entity file's text, keeping the components sorted by type."""
    lines = text.splitlines(keepends=True)
    out, inserted = [], False
    for line in lines:
        match = re.match(r'\s*<Component Type="([^"]+)"', line)
        if not inserted and ((match and match.group(1) > type_name) or line.startswith("</Entity>")):
            out.append(xml)
            inserted = True
        out.append(line)
    return "".join(out)


class Level:
    def __init__(self):
        self.entities, self.instances = [], []   # (guid, text)

    def add(self, name, components, mobility=None):
        g = guid(name)
        self.entities.append((g, nm.entity_xml(g, name, components, mobility=mobility)))

    def write(self):
        FOLDER.mkdir(parents=True, exist_ok=True)
        for stale in list(FOLDER.glob("*.nmentity")) + list(FOLDER.glob("*.nmprefabinst")):
            stale.unlink()
        manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n"]
        for g, text in self.entities:
            (FOLDER / f"{g}.nmentity").write_text(text, newline="\n")
            manifest.append(f"  <Entity Guid=\"{g}\" />\n")
        for g, text in self.instances:
            (FOLDER / f"{g}.nmprefabinst").write_text(text, newline="\n")
            manifest.append(f"  <Instance Guid=\"{g}\" />\n")
        manifest.append("</World>\n")
        WORLD.write_text("".join(manifest), newline="\n")


def copy_street(level):
    """The street's entities, in its manifest's order, with collision added."""
    if not SOURCE_WORLD.exists():
        sys.exit(f"{SOURCE_WORLD} is missing: run Tools/Steampunk/make_steampunk_street.py first")
    order = re.findall(r'<(Entity|Instance) Guid="([0-9a-f]+)"', SOURCE_WORLD.read_text())
    kept = dropped = bodies = 0
    for kind, g in order:
        if kind == "Instance":
            text = (SOURCE / f"{g}.nmprefabinst").read_text()
            if any(f'Prefab="{p}"' in text for p in DROP_PREFABS):
                dropped += 1
                continue
            level.instances.append((g, text))
            kept += 1
            continue
        text = (SOURCE / f"{g}.nmentity").read_text()
        name = re.search(r'<Entity [^>]*Name="([^"]*)"', text).group(1)
        if DROP_NAMES.match(name):
            dropped += 1
            continue
        if 'Type="Rigid Body"' not in text and ('Type="Bounds"' in text or name in GROUND):
            text = with_component(text, "Rigid Body", rigid_body_xml())
            bodies += 1
        level.entities.append((g, text))
        kept += 1
    print(f"  the street: {kept} kept, {dropped} dropped (the flyover, the explore camera), {bodies} given a static body")


def add_walls(level):
    """Invisible boxes round the playable streets, 4 m high over the eaves' reach of a jump."""
    height = 600.0
    thick = 100.0
    walls = {
        # Across the far ends of the front and back streets, beyond the side streets.
        "BoundaryWest":  ((-SIDE_X - thick, 0, -FAR_Z), (-SIDE_X, height, FAR_Z)),
        "BoundaryEast":  ((SIDE_X, 0, -FAR_Z), (SIDE_X + thick, height, FAR_Z)),
        # Along the far rows' facades, either side of the cross street.
        "BoundarySouthW": ((-SIDE_X, 0, -FAR_Z - thick), (-LOT_IN, height, -FAR_Z)),
        "BoundarySouthE": ((LOT_IN, 0, -FAR_Z - thick), (SIDE_X, height, -FAR_Z)),
        "BoundaryNorthW": ((-SIDE_X, 0, FAR_Z), (-LOT_IN, height, FAR_Z + thick)),
        "BoundaryNorthE": ((LOT_IN, 0, FAR_Z), (SIDE_X, height, FAR_Z + thick)),
        # The cross street's extensions: their sides, and their closing ends.
        "BoundaryCrossSW": ((-LOT_IN - thick, 0, -EXT_END), (-LOT_IN, height, -FAR_Z)),
        "BoundaryCrossSE": ((LOT_IN, 0, -EXT_END), (LOT_IN + thick, height, -FAR_Z)),
        "BoundaryCrossNW": ((-LOT_IN - thick, 0, FAR_Z), (-LOT_IN, height, EXT_END)),
        "BoundaryCrossNE": ((LOT_IN, 0, FAR_Z), (LOT_IN + thick, height, EXT_END)),
        "BoundaryCrossS":  ((-LOT_IN, 0, -EXT_END - thick), (LOT_IN, height, -EXT_END)),
        "BoundaryCrossN":  ((-LOT_IN, 0, EXT_END), (LOT_IN, height, EXT_END + thick)),
    }
    for name, (lo, hi) in walls.items():
        centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
        half = tuple((b - a) / 2 for a, b in zip(lo, hi))
        level.add(name, [("Bounds", bounds_xml(tuple(-h for h in half), half)), ("Rigid Body", rigid_body_xml()),
                         ("Transform", nm.transform_xml(centre))], mobility="Static")
    print(f"  {len(walls)} boundary walls")


def yaw_toward(frm, to):
    return math.degrees(math.atan2(to[0] - frm[0], to[1] - frm[1]))


def add_starts(level):
    """Eight starts, each facing along its street toward the middle of the map."""
    spots = [
        (0.0, -EXT_END + 900.0), (0.0, EXT_END - 900.0),
        (-SIDE_C, 0.0), (SIDE_C, 0.0),
        (-SIDE_C / 2, FRONT_C), (SIDE_C / 2, FRONT_C),
        (-SIDE_C / 2, -FRONT_C), (SIDE_C / 2, -FRONT_C),
    ]
    for index, (x, z) in enumerate(spots):
        # Along the street it stands in: toward the cross street, or toward the loop.
        target = (0.0, z) if abs(z) < FAR_Z and abs(x) > 1.0 else (x, 0.0)
        start = nm.component_xml("PlayerStart", 1, [("AvatarPrefab", PREFAB), ("PlayerSlot", 0), ("Enabled", True)])
        level.add(f"PlayerStart{index + 1}", [("PlayerStart", start),
                                              ("Transform", nm.transform_xml((x, 0, z), (0, yaw_toward((x, z), target), 0)))])
    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 40000), ("Active", True), ("Projection", 1), ("FovY", 62)])
    # High over the front street's west end, looking east along it: what a fighter
    # sees while it waits to respawn (its own camera is off then), and before it spawns.
    level.add("OverviewCamera", [("Camera", camera),
                                 ("Transform", nm.transform_xml((-SIDE_C, 650, FRONT_C), (14, 90, 0)))])
    print(f"  {len(spots)} player starts and an overview camera")


def main():
    print("Steampunk PvP:")
    level = Level()
    copy_street(level)
    add_walls(level)
    add_starts(level)
    level.write()
    print(f"  wrote {WORLD.relative_to(GAME_DIR)} ({len(level.entities)} entities, {len(level.instances)} instances)")


if __name__ == "__main__":
    main()
