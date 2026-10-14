#!/usr/bin/env python3
"""
make_meadow_level.py -- the Meadow level, a Procedural Spawner showcase on
the Hills terrain (Tools/Terrain/make_hills_level.py's heights and paint):

    Content/Levels/Meadow.nmworld (+ Meadow/*.nmentity)
    Content/Levels/Meadow/Terrain/Ground.nmterrain

Nothing in it is placed by hand except a boulder; everything else comes from
spawners (Engine/Modules/RenderCore/PROCEDURAL.md):

    Road     a Spline (Width 600) winding across the map, its strip painted
             earth on the terrain; gas lamps either side of it (Spline Path,
             Path Offset, Both Sides, Align to Path)
    Meadow   a closed Spline around the middle, filled with grass and a few
             bushes (Spline Area) on the grass layer, in patches (noise), not
             on the road (Exclusions)
    Woods    pines in a box on the east hills, gentle slopes only
    Rocks    rocks over the whole terrain, tilted to the ground and sunk
    Boulder  a big rock (a Render mesh) with grass on its top (Mesh Surface:
             Max Slope keeps it off the sides, Min Height off the buried base)

Registers [World_Meadow] (WorldsLabExplore) in Config/DefaultEngine.ini if it
is not there.

    python Games/WorldsLab/Tools/Procedural/make_meadow_level.py
"""

import math
import sys
from pathlib import Path

import numpy as np

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Steampunk"))
sys.path.insert(0, str(GAME_DIR / "Tools" / "Terrain"))
import nmformats as nm  # noqa: E402
import make_hills_level as hills  # noqa: E402

LEVELS  = GAME_DIR / "Content" / "Levels"
WORLD   = LEVELS / "Meadow.nmworld"
FOLDER  = LEVELS / "Meadow"
TERRAIN = FOLDER / "Terrain" / "Ground.nmterrain"
TERRAIN_RUNTIME = "Content/Levels/Meadow/Terrain/Ground.nmterrain"
SKY     = "Content/Textures/Steampunk/Sky_HDR.nmtexture"

FOLIAGE = "Content/Meshes/Foliage/"
LAMP    = "Content/Meshes/Steampunk/GasLamp.nmmesh"

ROAD = [(-12000, 0, -4200), (-8200, 0, -2600), (-4600, 0, -2900), (-1800, 0, -700),
        (1200, 0, 600), (4200, 0, 200), (7400, 0, 2200), (10200, 0, 2000), (12400, 0, 3600)]
ROAD_WIDTH = 600.0

MEADOW = [(-9500, 0, -7500), (-2500, 0, -9000), (3500, 0, -6500), (5000, 0, -1500),
          (3500, 0, 5000), (-1500, 0, 7500), (-7500, 0, 6000), (-10000, 0, 500)]


def guid(name):
    return nm.guid("Meadow", name)


def vector_xml(name, v):
    return f"    <{name} X=\"{nm.num(v[0])}\" Y=\"{nm.num(v[1])}\" Z=\"{nm.num(v[2])}\" />\n"


def spline_xml(points, closed, width, curve=True):
    lines = ["  <Component Type=\"Spline\" Version=\"1\">\n",
             f"    <Closed Value=\"{'true' if closed else 'false'}\" />\n",
             f"    <Type Value=\"{0 if curve else 1}\" />\n",
             f"    <Width Value=\"{nm.num(width)}\" />\n",
             f"    <PointCount Value=\"{len(points)}\" />\n"]
    lines += [vector_xml(f"Point{i}", p) for i, p in enumerate(points)]
    lines.append("  </Component>\n")
    return "".join(lines)


def spawner_xml(fields, meshes, exclusions=(), box=(1000, 500, 1000)):
    """fields: overrides of the defaults below; meshes: [(mesh, weight, scale, far mesh)]."""
    values = {
        "Visible": True, "Source": 2, "Project": 3, "TerrainLayer": 0, "Seed": 1, "Spacing": 200, "Jitter": 1,
        "ProjectRange": 5000, "PathOffset": 0, "BothSides": False, "Density": 1, "NoiseScale": 0, "NoiseThreshold": 0.5,
        "MaxSlope": 90, "MinHeight": -1000000, "MaxHeight": 1000000, "LayerThreshold": 0.5, "ScaleMin": 0.8,
        "ScaleMax": 1.2, "AlignToNormal": 0, "Sink": 0, "ColorVariation": 0.1, "RandomYaw": True,
        "AlignToPath": False, "CastShadows": True, "CellSize": 4000, "LODDistance": 0, "LOD1Density": 0.5,
        "LOD2Density": 0.25, "CullDistance": 0, "MaxInstances": 200000,
    }
    values.update(fields)
    lines = ["  <Component Type=\"Procedural Spawner\" Version=\"1\">\n"]
    for name, value in values.items():
        if name == "Seed":
            lines.append(f"    <Seed Value=\"{int(value)}\" />\n")
            lines.append(vector_xml("BoxExtent", box))
            continue
        if isinstance(value, bool):
            lines.append(f"    <{name} Value=\"{'true' if value else 'false'}\" />\n")
        else:
            lines.append(f"    <{name} Value=\"{nm.num(value)}\" />\n")
    lines.append(f"    <MeshCount Value=\"{len(meshes)}\" />\n")
    for i, (mesh, weight, scale, far) in enumerate(meshes):
        lines += [f"    <Mesh{i}Path Value=\"{mesh}\" />\n", f"    <Mesh{i}Far Value=\"{far}\" />\n",
                  f"    <Mesh{i}Material Value=\"\" />\n", f"    <Mesh{i}Weight Value=\"{nm.num(weight)}\" />\n",
                  f"    <Mesh{i}Scale Value=\"{nm.num(scale)}\" />\n"]
    lines.append(f"    <ExclusionCount Value=\"{len(exclusions)}\" />\n")
    lines += [f"    <Exclusion{i} Value=\"{name}\" />\n" for i, name in enumerate(exclusions)]
    lines.append("  </Component>\n")
    return "".join(lines)


def catmull_rom(points, steps=24):
    """The centripetal curve GameCore's SplineCurve draws (open, mirrored ends), as a dense polyline (X, Z)."""
    p = [np.array([x, z], dtype=np.float64) for x, _, z in points]
    ext = [2 * p[0] - p[1]] + p + [2 * p[-1] - p[-2]]
    out = []
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        t0 = 0.0
        t1 = t0 + max(math.sqrt(np.linalg.norm(p1 - p0)), 1e-4)
        t2 = t1 + max(math.sqrt(np.linalg.norm(p2 - p1)), 1e-4)
        t3 = t2 + max(math.sqrt(np.linalg.norm(p3 - p2)), 1e-4)
        for s in range(steps):
            u = t1 + (t2 - t1) * s / steps
            a1 = p0 + (p1 - p0) * (u - t0) / (t1 - t0)
            a2 = p1 + (p2 - p1) * (u - t1) / (t2 - t1)
            a3 = p2 + (p3 - p2) * (u - t2) / (t3 - t2)
            b1 = a1 + (a2 - a1) * (u - t0) / (t2 - t0)
            b2 = a2 + (a3 - a2) * (u - t1) / (t3 - t1)
            out.append(b1 + (b2 - b1) * (u - t1) / (t2 - t1))
    out.append(p[-1])
    return np.array(out)


def distance_to_polyline(wx, wz, poly):
    best = np.full(wx.shape, 1e30)
    for a, b in zip(poly[:-1], poly[1:]):
        d = b - a
        length2 = max(float(d @ d), 1e-9)
        t = np.clip(((wx - a[0]) * d[0] + (wz - a[1]) * d[1]) / length2, 0.0, 1.0)
        px, pz = a[0] + d[0] * t - wx, a[1] + d[1] * t - wz
        best = np.minimum(best, px * px + pz * pz)
    return np.sqrt(best)


def paint_road(h, w):
    """Earth along the road, fading out past its edge; the ground under it smoothed a little."""
    idx = np.arange(hills.RES, dtype=np.float64)
    X, Z = np.meshgrid(idx, idx)
    wx, wz = X * hills.SPACING - hills.SIZE / 2, Z * hills.SPACING - hills.SIZE / 2
    dist = distance_to_polyline(wx, wz, catmull_rom(ROAD))
    earth = np.clip(1.0 - (dist - ROAD_WIDTH * 0.35) / (ROAD_WIDTH * 0.4), 0.0, 1.0)
    weights = w.astype(np.float64) / 255.0
    weights = weights * (1.0 - earth[..., None])
    weights[..., 1] += earth
    q = np.floor(weights * 255.0 + 0.5).astype(np.int32)
    q[..., 1] += 255 - q.sum(axis=-1)
    return np.clip(q, 0, 255).astype(np.uint8)


def main():
    h = hills.make_heights()
    w = paint_road(h, hills.make_weights(h))
    hills.TERRAIN = TERRAIN
    lo, hi = hills.write_terrain(h, w)

    entities = []

    def add(name, components, mobility="Static"):
        entities.append((name, nm.entity_xml(guid(name), name, components, mobility)))

    terrain = nm.component_xml("Terrain", 3, [
        ("Terrain", TERRAIN_RUNTIME), ("Material", ""), ("Visible", True), ("CastShadows", True),
        ("Occluder", True), ("Walkable", True), ("UVTileSize", 400), ("LODDistance", 0), ("Foliage", ""),
        ("FoliageVisible", True)])
    add("Ground", [("Terrain", terrain), ("Transform", nm.transform_xml((-hills.SIZE / 2, 0, -hills.SIZE / 2)))])

    # The road: a spline, and lamps either side of it.
    lamps = spawner_xml({"Source": 0, "Project": 1, "Spacing": 1800, "Jitter": 0, "ProjectRange": 6000,
                         "PathOffset": 480, "BothSides": True, "AlignToPath": True, "RandomYaw": False,
                         "ScaleMin": 1, "ScaleMax": 1, "ColorVariation": 0, "Seed": 3},
                        [(LAMP, 1, 1, "")])
    add("Road", [("Spline", spline_xml(ROAD, False, ROAD_WIDTH)), ("Procedural Spawner", lamps),
                 ("Transform", nm.transform_xml((0, 0, 0)))])

    # The meadow: grass and a few bushes, in patches, on the grass layer, off the road.
    meadow = spawner_xml({"Source": 1, "Project": 1, "Spacing": 55, "ProjectRange": 6000, "TerrainLayer": 1,
                          "LayerThreshold": 0.55, "NoiseScale": 2600, "NoiseThreshold": 0.3, "AlignToNormal": 0.7,
                          "ScaleMin": 0.45, "ScaleMax": 0.85, "ColorVariation": 0.25, "CastShadows": False,
                          "CellSize": 2500, "LODDistance": 3000, "LOD1Density": 0.45, "LOD2Density": 0.15,
                          "CullDistance": 11000, "Seed": 11},
                         [(FOLIAGE + "GrassTuft.nmmesh", 1, 1, ""), (FOLIAGE + "Bush.nmmesh", 0.012, 1, "")],
                         exclusions=["Road"])
    add("Meadow", [("Spline", spline_xml(MEADOW, True, 0)), ("Procedural Spawner", meadow),
                   ("Transform", nm.transform_xml((0, 0, 0)))])

    # The woods: pines on the eastern hills, on gentle slopes, in clumps.
    woods = spawner_xml({"Source": 2, "Project": 1, "Spacing": 480, "NoiseScale": 4200, "NoiseThreshold": 0.3,
                         "MaxSlope": 30, "ScaleMin": 0.75, "ScaleMax": 1.45, "Sink": 0.05, "ColorVariation": 0.2,
                         "CellSize": 4000, "LOD1Density": 1, "LOD2Density": 1, "Seed": 5},
                        [(FOLIAGE + "Pine.nmmesh", 1, 1, FOLIAGE + "Pine_LOD1.nmmesh"),
                         (FOLIAGE + "Bush.nmmesh", 0.35, 1.3, "")],
                        exclusions=["Road"], box=(4600, 5000, 6200))
    add("Woods", [("Procedural Spawner", woods), ("Transform", nm.transform_xml((7400, 0, -5200), (0, 12, 0)))])

    # Rocks everywhere: tilted to the ground, sunk a little, on terrain and the boulder alike.
    rocks = spawner_xml({"Source": 2, "Project": 3, "Spacing": 1150, "Density": 0.6, "AlignToNormal": 1,
                         "Sink": 0.18, "ScaleMin": 0.6, "ScaleMax": 2.6, "MaxSlope": 65, "ColorVariation": 0.2,
                         "Seed": 9},
                        [(FOLIAGE + "Rock.nmmesh", 1, 1, "")],
                        exclusions=["Road"], box=(12600, 5000, 12600))
    add("Rocks", [("Procedural Spawner", rocks), ("Transform", nm.transform_xml((0, 0, 0)))])

    # A boulder in the clearing, grass on its top only.
    boulder_render = ("  <Component Type=\"Render\" Version=\"5\">\n    <Visible Value=\"true\" />\n"
                      + "".join(f"    <Material{slot} Value=\"\" />\n" for slot in range(8))
                      + f"    <Mesh Value=\"{FOLIAGE}Rock.nmmesh\" />\n    <ParameterCount Value=\"0\" />\n"
                      "    <CastShadows Value=\"true\" />\n    <CastInsetShadow Value=\"false\" />\n    <Occluder Value=\"false\" />\n"
                      "  </Component>\n")
    moss = spawner_xml({"Source": 3, "Project": 0, "Spacing": 20, "MaxSlope": 60, "MinHeight": 10, "AlignToNormal": 0.8,
                        "ScaleMin": 0.35, "ScaleMax": 0.6, "ColorVariation": 0.3, "CastShadows": False, "Seed": 2},
                       [(FOLIAGE + "GrassTuft.nmmesh", 1, 1, "")])
    add("Boulder", [("Render", boulder_render), ("Procedural Spawner", moss),
                    ("Transform", nm.transform_xml((900, -40, -1100), (0, 25, 0), (7, 5, 7)))])

    sun = nm.component_xml("Directional Light", 3, [
        ("Color", ("colour", (255, 230, 200))), ("Intensity", 12), ("Active", True), ("CastShadows", True),
        ("ShadowTechnique", 2), ("ShadowTint", ("colour", (40, 50, 80))), ("ShadowBias", 0.0012),
        ("ShadowNormalBias", 2), ("ShadowDistance", 12000), ("CascadeCount", 4), ("CascadeSplitLambda", 0.8),
        ("SourceAngle", 0.6)])
    add("Sun", [("Directional Light", sun), ("Transform", nm.transform_xml((0, 8000, 0), (32, 215, 0)))])
    sky = nm.component_xml("Sky Light", 2, [
        ("SkyColor", ("colour", (150, 170, 200))), ("GroundColor", ("colour", (70, 65, 55))), ("Intensity", 0.7),
        ("Active", True), ("Environment", SKY), ("EnvironmentIntensity", 1.0), ("EnvironmentRotation", 0),
        ("DrawBackground", True)])
    add("Sky", [("Sky Light", sky), ("Transform", nm.transform_xml((0, 0, 0)))])

    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 60000), ("Active", True), ("Projection", 1),
                                            ("FovY", 65)])
    player_input = nm.component_xml("Input", 1, [("Map", "Default"), ("PlayerIndex", 0), ("Enabled", True)])
    add("Player", [("Camera", camera), ("Input", player_input),
                   ("Transform", nm.transform_xml((-2600, 170, -1500), (-4, 80, 0)))], mobility=None)

    FOLDER.mkdir(parents=True, exist_ok=True)
    for stale in FOLDER.glob("*.nmentity"):
        stale.unlink()
    manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n"]
    for name, text in entities:
        (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
        manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
    manifest.append("</World>\n")
    WORLD.write_text("".join(manifest), newline="\n")

    ini = GAME_DIR / "Config" / "DefaultEngine.ini"
    text = ini.read_text()
    if "[World_Meadow]" not in text:
        anchor = "[World_SteampunkStreet]"
        block = "[World_Meadow]\nFile=Content/Levels/Meadow.nmworld\nGameMode=WorldsLabExplore\nCook=true\n\n"
        text = text.replace(anchor, block + anchor) if anchor in text else text + "\n" + block
        ini.write_text(text)
    print(f"  {WORLD.relative_to(GAME_DIR)}: {len(entities)} entities")
    print(f"  {TERRAIN.relative_to(GAME_DIR)}: {hills.RES}x{hills.RES}, heights {lo:.0f}..{hi:.0f} cm")


if __name__ == "__main__":
    main()
