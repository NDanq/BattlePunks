#!/usr/bin/env python3
"""
make_hills_level.py -- the Hills level, a terrain test bed:

    Content/Levels/Hills.nmworld (+ Hills/*.nmentity)
    Content/Levels/Hills/Terrain/Ground.nmterrain

257 x 257 samples a metre apart (256 m a side), centred on the origin:
rolling fBm hills up to ~25 m, a ridge, a flat clearing at the centre where
the player starts. Paint: grass, earth in the low valleys, rock on steep
slopes, snow above 18 m. Lit by a low sun with cascaded shadows and the
Steampunk sky. Registers [World_Hills] (WorldsLabExplore) in
Config/DefaultEngine.ini if it is not there.

    python Games/WorldsLab/Tools/Terrain/make_hills_level.py
"""

import math
import struct
import sys
from pathlib import Path

import numpy as np

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Steampunk"))
import nmformats as nm  # noqa: E402

LEVELS  = GAME_DIR / "Content" / "Levels"
WORLD   = LEVELS / "Hills.nmworld"
FOLDER  = LEVELS / "Hills"
TERRAIN = FOLDER / "Terrain" / "Ground.nmterrain"
TERRAIN_RUNTIME = "Content/Levels/Hills/Terrain/Ground.nmterrain"
SKY     = "Content/Textures/Steampunk/Sky_HDR.nmtexture"

RES     = 257
SPACING = 100.0
SIZE    = (RES - 1) * SPACING


def fbm(x, z, octaves=5, seed=7):
    rng = np.random.default_rng(seed)
    total = np.zeros_like(x)
    amp, freq, norm = 1.0, 1.0, 0.0
    for _ in range(octaves):
        grid = rng.random((70, 70))
        u, v = x * freq, z * freq
        x0, z0 = np.floor(u).astype(int) % 64, np.floor(v).astype(int) % 64
        fx, fz = u - np.floor(u), v - np.floor(v)
        fx, fz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
        a, b = grid[z0, x0], grid[z0, x0 + 1]
        c, d = grid[z0 + 1, x0], grid[z0 + 1, x0 + 1]
        total += ((a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz) * amp
        norm += amp
        amp *= 0.5
        freq *= 2.0
    return total / norm


def make_heights():
    idx = np.arange(RES, dtype=np.float64)
    X, Z = np.meshgrid(idx, idx)            # rows are Z, columns X
    wx, wz = X * SPACING - SIZE / 2, Z * SPACING - SIZE / 2
    h = (fbm(X / 40.0, Z / 40.0) - 0.45) * 3200.0
    ridge = np.exp(-((wx * 0.6 + wz * 0.8 - 6000.0) / 2500.0) ** 2) * 1600.0
    h = h + ridge
    r = np.sqrt(wx * wx + wz * wz)
    clearing = np.clip((r - 1500.0) / 2500.0, 0.0, 1.0)
    clearing = clearing * clearing * (3 - 2 * clearing)
    return h * clearing                       # 0 at the centre, the hills fading in past 15 m


def make_weights(h):
    gz, gx = np.gradient(h, SPACING)
    slope = np.sqrt(gx * gx + gz * gz)
    rock = np.clip((slope - 0.45) / 0.35, 0, 1)
    snow = np.clip((h - 1800.0) / 400.0, 0, 1) * (1 - rock * 0.6)
    earth = np.clip((-h - 200.0) / 400.0, 0, 1) * (1 - rock)
    grass = np.clip(1.0 - rock - snow - earth, 0, 1)
    w = np.stack([grass, earth, rock, snow], axis=-1)
    w = w / np.maximum(w.sum(axis=-1, keepdims=True), 1e-6)
    q = np.floor(w * 255.0 + 0.5).astype(np.int32)
    q[..., 0] += 255 - q.sum(axis=-1)          # sums exactly 255, as the brushes keep them
    return np.clip(q, 0, 255).astype(np.uint8)


def write_terrain(h, w):
    lo, hi = float(h.min()), float(h.max())
    span = hi - lo if hi > lo else 1.0
    samples = np.clip(np.rint((h - lo) * 65535.0 / span), 0, 65535).astype("<u2")
    header = struct.pack("<Ifff", RES, SPACING, lo, hi)
    names = b"".join(nm.string(s) for s in ("Grass", "Earth", "Rock", "Snow"))
    body = (nm.chunk("THDR", header) + nm.chunk("HGHT", samples.tobytes())
            + nm.chunk("SPLT", w.astype(np.uint8).tobytes()) + nm.chunk("LAYR", names))
    TERRAIN.parent.mkdir(parents=True, exist_ok=True)
    TERRAIN.write_bytes(nm.chunk("NTRN", body))
    return lo, hi


def guid(name):
    return nm.guid("Hills", name)


def main():
    h = make_heights()
    w = make_weights(h)
    lo, hi = write_terrain(h, w)

    entities = []

    def add(name, components, mobility="Static"):
        entities.append((name, nm.entity_xml(guid(name), name, components, mobility)))

    terrain = nm.component_xml("Terrain", 1, [
        ("Terrain", TERRAIN_RUNTIME), ("Material", ""), ("Visible", True), ("CastShadows", True),
        ("Occluder", True), ("Walkable", True), ("UVTileSize", 400)])
    body = nm.component_xml("Rigid Body", 1, [("Motion", 2), ("Shape", 7), ("Enabled", True)])
    add("Ground", [("Terrain", terrain), ("Transform", nm.transform_xml((-SIZE / 2, 0, -SIZE / 2)))])

    # A pillar in the clearing: what a shadow on the terrain looks like next to the terrain's own.
    render = ("  <Component Type=\"Render\" Version=\"5\">\n    <Visible Value=\"true\" />\n"
              + "".join(f"    <Material{slot} Value=\"\" />\n" for slot in range(8))
              + "    <Mesh Value=\"Engine/Content/Meshes/Box.nmmesh\" />\n    <ParameterCount Value=\"0\" />\n"
              "    <CastShadows Value=\"true\" />\n    <CastInsetShadow Value=\"false\" />\n    <Occluder Value=\"false\" />\n"
              "  </Component>\n")
    add("Pillar", [("Render", render), ("Transform", nm.transform_xml((300, 400, 300), (0, 0, 0), (100, 800, 100)))])

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
                   ("Transform", nm.transform_xml((-4200, 170, 2600), (-6, 140, 0)))], mobility=None)

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
    if "[World_Hills]" not in text:
        anchor = "[World_SteampunkStreet]"
        block = "[World_Hills]\nFile=Content/Levels/Hills.nmworld\nGameMode=WorldsLabExplore\nCook=true\n\n"
        text = text.replace(anchor, block + anchor) if anchor in text else text + "\n" + block
        ini.write_text(text)
    print(f"  {WORLD.relative_to(GAME_DIR)}: {len(entities)} entities")
    print(f"  {TERRAIN.relative_to(GAME_DIR)}: {RES}x{RES}, heights {lo:.0f}..{hi:.0f} cm, "
          f"{TERRAIN.stat().st_size / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
