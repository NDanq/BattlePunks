#!/usr/bin/env python3
"""
make_grasslab_level.py -- GrassLab, the custom render pass validation world
(Engine/Modules/RenderCore/RENDERPIPELINE.md, "Custom render passes";
WorldsLabGrassLab.cpp):

    Content/Levels/GrassLab.nmworld (+ GrassLab/*.nmentity)
    Content/Levels/GrassLab/Terrain/Ground.nmterrain
    Content/RenderTargets/GrassTrail.nmrt2d

    Ground   a small terrain, 64 m, gently rolling inside the field and rising
             to a rim past it, so the ball stays in; a static heightfield body
    Field    a Procedural Spawner: GrassClump (make_grasslab_meshes.py; its
             _Far past 9 m of the ball -- WorldsLabGrassLab centres the LOD
             rings on it) over the field (make_grasslab_materials.py's
             FIELD), dense, drawn with GrassBlade -- the grass the ball's
             trail bends
    Ball     a 1 m sphere, a dynamic rigid body; in custom pass mesh group 0
             (CustomPassGroups 1), which the x-ray pass draws
    Sun, Sky, Player (the camera)

GrassTrail.nmrt2d is the render target the trail pass draws into and the
grass samples: 512 x 512 over the 48 m field, about 9 cm a texel.

Registers [World_GrassLab] (WorldsLabGrassLab) in Config/DefaultEngine.ini if
it is not there. Run make_grasslab_meshes.py and make_grasslab_materials.py
(and NodeGraphGenerate) too.

    python Games/WorldsLab/Tools/GrassLab/make_grasslab_level.py
"""

import struct
import sys
from pathlib import Path

import numpy as np

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Steampunk"))
sys.path.insert(0, str(GAME_DIR / "Tools" / "Terrain"))
sys.path.insert(0, str(GAME_DIR / "Tools" / "Procedural"))
sys.path.insert(0, str(GAME_DIR / "Tools" / "GrassLab"))
import nmformats as nm  # noqa: E402
import make_hills_level as hills  # noqa: E402
import make_meadow_level as meadow  # noqa: E402
import make_grasslab_materials as mats  # noqa: E402

LEVELS  = GAME_DIR / "Content" / "Levels"
WORLD   = LEVELS / "GrassLab.nmworld"
FOLDER  = LEVELS / "GrassLab"
TERRAIN = FOLDER / "Terrain" / "Ground.nmterrain"
TERRAIN_RUNTIME = "Content/Levels/GrassLab/Terrain/Ground.nmterrain"
TRAIL   = GAME_DIR / "Content" / "RenderTargets" / "GrassTrail.nmrt2d"
SKY     = "Content/Textures/Steampunk/Sky_HDR.nmtexture"

GRASS   = "Content/Meshes/GrassLab/GrassClump.nmmesh"
GRASS_FAR = "Content/Meshes/GrassLab/GrassClump_Far.nmmesh"
SPHERE  = "Engine/Content/Meshes/Sphere.nmmesh"
GRASS_MATERIAL = "Content/Materials/GrassLab/GrassBlade.material.nmgraph"
BALL_MATERIAL  = "Content/Materials/GrassLab/Ball.material.nmgraph"

RES, SPACING = 129, 50.0                 # 64 m of terrain, a sample every 50 cm
SIZE = (RES - 1) * SPACING
TRAIL_SIZE = 512
BALL_DIAMETER = 100.0
LAYER_WORLD = 1


def guid(name):
    return nm.guid("GrassLab", name)


def make_heights():
    idx = np.arange(RES, dtype=np.float64)
    X, Z = np.meshgrid(idx, idx)
    wx, wz = X * SPACING - SIZE / 2, Z * SPACING - SIZE / 2
    # Low swells in the field (a ball rolls down them and the grass shows it),
    # then a rim rising past the field's edge to turn the ball back.
    swell = (np.sin(wx / 900.0) * np.cos(wz / 1100.0) + 0.5 * np.sin((wx + wz) / 650.0)) * 22.0
    edge = np.maximum(np.abs(wx), np.abs(wz))
    rim = np.clip((edge - mats.FIELD[2] / 2 + 100.0) / 700.0, 0.0, 1.0)
    return swell * (1.0 - rim) + rim * rim * 260.0


def make_weights(h):
    """Grass everywhere, a little earth only where the rim is steepest."""
    gz, gx = np.gradient(h, SPACING)
    slope = np.sqrt(gx * gx + gz * gz)
    earth = np.clip((slope - 0.55) / 0.3, 0, 1) * 0.6
    grass = 1.0 - earth
    w = np.stack([grass, earth, np.zeros_like(h), np.zeros_like(h)], axis=-1)
    q = np.floor(w * 255.0 + 0.5).astype(np.int32)
    q[..., 0] += 255 - q.sum(axis=-1)
    return np.clip(q, 0, 255).astype(np.uint8)


def write_trail_target():
    """RenderTargetAsset.h: NRTA v1 { RTHD v1: kind, width, height, filter, wrap, clear colour }."""
    # Kind Tex2D (0), Filter Linear (1), Wrap Clamp (1) -- ETextureFilter/ETextureWrap's
    # values; clear all zeros: the trail at rest.
    header = struct.pack("<BIIBB4B", 0, TRAIL_SIZE, TRAIL_SIZE, 1, 1, 0, 0, 0, 0)
    TRAIL.parent.mkdir(parents=True, exist_ok=True)
    TRAIL.write_bytes(nm.chunk("NRTA", nm.chunk("RTHD", header)))


def rigid_body_xml(motion, shape, mass=0.0, friction=0.6, restitution=0.0, linear=0.05, angular=0.05,
                   continuous=False, sleep=True):
    return nm.component_xml("Rigid Body", 1, [
        ("Enabled", True), ("Motion", motion), ("Shape", shape), ("ShapeSource", 0), ("Mass", mass),
        ("Friction", friction), ("Restitution", restitution), ("LinearDamping", linear), ("AngularDamping", angular),
        ("GravityFactor", 1), ("Continuous", continuous), ("Sensor", False), ("AllowSleeping", sleep),
        ("Layer", LAYER_WORLD), ("Mask", -1)]).replace(
        "  </Component>\n",
        "    <InitialLinearVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n"
        "    <InitialAngularVelocity X=\"0\" Y=\"0\" Z=\"0\" />\n  </Component>\n")


def render_xml(mesh, materials=(), groups=0, cast_shadows=True):
    slots = list(materials) + [""] * (8 - len(materials))
    return ("  <Component Type=\"Render\" Version=\"6\">\n    <Visible Value=\"true\" />\n"
            + "".join(f"    <Material{slot} Value=\"{slots[slot]}\" />\n" for slot in range(8))
            + f"    <Mesh Value=\"{mesh}\" />\n    <ParameterCount Value=\"0\" />\n"
            f"    <CastShadows Value=\"{'true' if cast_shadows else 'false'}\" />\n"
            "    <CastInsetShadow Value=\"true\" />\n    <Occluder Value=\"false\" />\n"
            f"    <CustomPassGroups Value=\"{groups}\" />\n"
            "  </Component>\n")


def main():
    h = make_heights()
    w = make_weights(h)
    hills.RES, hills.SPACING, hills.TERRAIN = RES, SPACING, TERRAIN
    lo, hi = hills.write_terrain(h, w)
    write_trail_target()

    entities = []

    def add(name, components, mobility="Static"):
        entities.append((name, nm.entity_xml(guid(name), name, components, mobility)))

    terrain = nm.component_xml("Terrain", 3, [
        ("Terrain", TERRAIN_RUNTIME), ("Material", ""), ("Visible", True), ("CastShadows", True),
        ("Occluder", True), ("Walkable", True), ("UVTileSize", 300), ("LODDistance", 0), ("Foliage", ""),
        ("FoliageVisible", True)])
    add("Ground", [("Terrain", terrain), ("Rigid Body", rigid_body_xml(2, 7, friction=0.8)),
                   ("Transform", nm.transform_xml((-SIZE / 2, 0, -SIZE / 2)))])

    # The field: dense tufts over the trail's square, a little inside its edge.
    half = mats.FIELD[2] / 2 - 60.0
    field = meadow.spawner_xml({"Source": 2, "Project": 1, "Spacing": 19, "Jitter": 1, "ProjectRange": 3000,
                                "ScaleMin": 0.75, "ScaleMax": 1.2, "ColorVariation": 0.18, "AlignToNormal": 0.4,
                                "CastShadows": False, "CellSize": 400, "LODDistance": 900, "LOD1Density": 0.8,
                                "LOD2Density": 0.55, "Seed": 17},
                               [(GRASS, 1, 1, GRASS_FAR)], box=(half, 600, half))
    field = field.replace("<Mesh0Material Value=\"\" />", f"<Mesh0Material Value=\"{GRASS_MATERIAL}\" />")
    add("Field", [("Procedural Spawner", field), ("Transform", nm.transform_xml((0, 0, 0)))])

    # The ball: in custom pass mesh group 0, which the x-ray pass draws.
    add("Ball", [("Render", render_xml(SPHERE, [BALL_MATERIAL], groups=1)),
                 ("Rigid Body", rigid_body_xml(0, 2, mass=25.0, friction=0.9, restitution=0.15, linear=0.15,
                                               angular=0.6, continuous=True, sleep=False)),
                 ("Transform", nm.transform_xml((0, 160, 0), scale=(BALL_DIAMETER,) * 3))], mobility="Dynamic")

    sun = nm.component_xml("Directional Light", 3, [
        ("Color", ("colour", (255, 236, 210))), ("Intensity", 10), ("Active", True), ("CastShadows", True),
        ("ShadowTechnique", 2), ("ShadowTint", ("colour", (40, 50, 80))), ("ShadowBias", 0.0012),
        ("ShadowNormalBias", 2), ("ShadowDistance", 8000), ("CascadeCount", 3), ("CascadeSplitLambda", 0.8),
        ("SourceAngle", 0.6)])
    add("Sun", [("Directional Light", sun), ("Transform", nm.transform_xml((0, 8000, 0), (38, 220, 0)))])
    sky = nm.component_xml("Sky Light", 2, [
        ("SkyColor", ("colour", (150, 170, 200))), ("GroundColor", ("colour", (70, 65, 55))), ("Intensity", 0.7),
        ("Active", True), ("Environment", SKY), ("EnvironmentIntensity", 1.0), ("EnvironmentRotation", 0),
        ("DrawBackground", True)])
    add("Sky", [("Sky Light", sky), ("Transform", nm.transform_xml((0, 0, 0)))])

    camera = nm.component_xml("Camera", 2, [("ZNear", 5), ("ZFar", 30000), ("Active", True), ("Projection", 1),
                                            ("FovY", 60)])
    player_input = nm.component_xml("Input", 1, [("Map", "Default"), ("PlayerIndex", 0), ("Enabled", True)])
    add("Player", [("Camera", camera), ("Input", player_input),
                   ("Transform", nm.transform_xml((0, 900, -2200), (22, 0, 0)))], mobility=None)

    FOLDER.mkdir(parents=True, exist_ok=True)
    for stale in FOLDER.glob("*.nmentity"):
        stale.unlink()
    manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n",
                "  <Settings GameMode=\"WorldsLabGrassLab\" />\n"]
    for name, text in entities:
        (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
        manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
    manifest.append("</World>\n")
    WORLD.write_text("".join(manifest), newline="\n")

    ini = GAME_DIR / "Config" / "DefaultEngine.ini"
    text = ini.read_text()
    if "[World_GrassLab]" not in text:
        anchor = "[World_ToonArena]"
        block = ("; GrassLab: a ball rolls through procedural grass, bending it through a custom render\n"
                 "; pass's trail (WorldsLabGrassLab.cpp); [WorldsLab] GrassLabAutoPlay=0 to steer it.\n"
                 "[World_GrassLab]\nFile=Content/Levels/GrassLab.nmworld\nCook=true\n\n")
        text = text.replace(anchor, block + anchor) if anchor in text else text + "\n" + block
        ini.write_text(text)
    print(f"  {WORLD.relative_to(GAME_DIR)}: {len(entities)} entities")
    print(f"  {TERRAIN.relative_to(GAME_DIR)}: {RES}x{RES}, heights {lo:.0f}..{hi:.0f} cm")
    print(f"  {TRAIL.relative_to(GAME_DIR)}: {TRAIL_SIZE}x{TRAIL_SIZE}")


if __name__ == "__main__":
    main()
