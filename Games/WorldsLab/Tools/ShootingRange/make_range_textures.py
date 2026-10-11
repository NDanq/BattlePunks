#!/usr/bin/env python3
"""
make_range_textures.py -- writes Content/Textures/ShootingRange/: the PBR sets
of the Shooting Range warehouse (Content/Levels/ShootingRange), all
procedural and tileable, plus the daylight sky its skylights let in.

Each surface is three maps, as the Steampunk sets are (that folder's
make_steampunk_textures.py lends its noise toolkit):

    BaseColor   sRGB albedo (alpha: the grating's holes)
    Normal      linear, OpenGL convention (green up the image)
    ORM         linear: R ambient occlusion, G roughness, B metallic

    Concrete      the slab: power-trowelled, cracked, oil-stained         3 m
    Block         painted concrete block, the lower walls (40 x 20 cm)    1.6 m
    Corrugated    galvanised corrugated siding, rust from every fixing     2 m
    Steel         structural steel in worn grey-green enamel, rusting      1 m
    Grating       galvanised bar grating: 3 cm bars, holes in alpha       0.5 m
    DiamondPlate  tread plate, worn bright on the diamonds                0.6 m
    Plywood       range baffles: sanded ply, scuffed, holed              1.2 m
    Rubber        the backstop's rubber crumb, pocked by rounds            1 m
    FloorPaint    worn safety-yellow line paint over concrete              1 m
    Glass         skylight glass: frosted, filmed with dirt                1 m
    SteelTarget   an AR500 plate: white paint shot to grey lead splash  0.5 m

and single textures (UV 0-1):

    TargetPaper_BaseColor   a B-27 style silhouette on buff card, clamped
    Sky_HDR                 an equirectangular midday sky, broken cloud

Deterministic (seeded). Needs numpy.

    python Games/WorldsLab/Tools/ShootingRange/make_range_textures.py [names...]
"""

import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "Steampunk"))
import nmformats as nm                                              # noqa: E402
from make_steampunk_textures import (Noise, blur, smoothstep, normal_from_height, cavity_ao,   # noqa: E402
                                     rgb)

GAME_DIR = HERE.parents[1]
OUT = GAME_DIR / "Content" / "Textures" / "ShootingRange"

# The sun: high, from the south-west. make_range_level.py aims the Sun at it.
SUN_ELEVATION = 58.0     # degrees above the horizon
SUN_AZIMUTH = -35.0      # degrees from +Z, toward +X positive


def save_set(name, albedo, height, normal_strength, roughness, metallic, ao, alpha=None):
    albedo = np.clip(albedo, 0.0, 1.0)
    if alpha is not None:
        albedo = np.concatenate([albedo, alpha[..., None]], axis=-1)
    normal = normal_from_height(height, normal_strength)
    orm = np.stack([np.clip(ao, 0, 1), np.clip(roughness, 0.02, 1), np.clip(metallic, 0, 1)], axis=-1)
    nm.write_texture(OUT / f"{name}_BaseColor.nmtexture", albedo, srgb=True)
    nm.write_texture(OUT / f"{name}_Normal.nmtexture", normal, srgb=False, normal_map=True)
    nm.write_texture(OUT / f"{name}_ORM.nmtexture", orm, srgb=False)
    print(f"  {name}: {albedo.shape[1]} x {albedo.shape[0]}")


def grid(size):
    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    return xs, ys


def cracks(n, cells, width, threshold=0.93):
    """Thin branching lines: where ridged noise peaks."""
    r = n.ridged(cells, 5)
    return smoothstep(threshold - width, threshold, r)


def stains(n, cells, lo, hi):
    return smoothstep(lo, hi, n.fbm(cells, 5))


# ==============================================================================================
# Surfaces
# ==============================================================================================

def make_concrete(size=1024):
    n = Noise(2101, size)
    base = rgb(0.43, 0.42, 0.40) * (0.86 + 0.22 * n.fbm(6, 6))[..., None]
    trowel = n.fbm(3, 3, cells_v=40) * 0.5 + n.fbm(2, 4, cells_v=24) * 0.5    # swirled streaks
    base *= (0.94 + 0.1 * trowel)[..., None]
    pores = smoothstep(0.82, 0.9, n.white()) * smoothstep(0.4, 0.7, n.fbm(40, 3))
    crack = cracks(n, 3, 0.012)
    oil = stains(n, 4, 0.6, 0.78)
    grime = stains(n, 10, 0.45, 0.85)
    albedo = base * (1 - 0.18 * grime)[..., None]
    albedo = albedo * (1 - 0.55 * oil)[..., None] + rgb(0.05, 0.045, 0.04) * (0.25 * oil)[..., None]
    albedo *= (1 - 0.7 * crack - 0.35 * pores)[..., None]
    # Saw-cut control joints every 3 m: the tile's edges.
    xs, ys = grid(size)
    joint = smoothstep(3.0, 0.5, np.minimum(np.minimum(xs, size - xs), np.minimum(ys, size - ys)))
    albedo *= (1 - 0.6 * joint)[..., None]
    height = 0.6 + 0.05 * n.fbm(30, 4) - 0.5 * crack - 0.2 * pores - 0.4 * joint
    rough = 0.78 + 0.1 * n.fbm(20, 3) - 0.35 * oil - 0.12 * trowel * (1 - grime)
    ao = cavity_ao(height, 3, 4.0)
    save_set("Concrete", albedo, height, 2.0, rough, np.zeros_like(rough), ao)


def make_block(size=1024):
    """Concrete masonry, 40 x 20 cm blocks, 1.6 m a tile: 4 across, 8 down, running bond, painted."""
    n = Noise(2202, size)
    xs, ys = grid(size)
    bh = size / 8.0
    row = np.floor(ys / bh)
    bw = size / 4.0
    fx = (xs + (row % 2) * bw / 2) % bw
    fy = ys % bh
    mortar_px = 4.5
    d = np.minimum(np.minimum(fx, bw - fx), np.minimum(fy, bh - fy))
    face = smoothstep(mortar_px * 0.5, mortar_px * 1.4, d)
    paint = rgb(0.62, 0.62, 0.58) * (0.9 + 0.12 * n.fbm(8, 5))[..., None]
    peel = smoothstep(0.7, 0.74, n.fbm(5, 6))
    raw = rgb(0.40, 0.39, 0.37) * (0.8 + 0.3 * n.white() * n.fbm(60, 2))[..., None]
    albedo = paint * (1 - peel[..., None]) + raw * peel[..., None]
    mortar = rgb(0.36, 0.35, 0.33)
    albedo = albedo * face[..., None] + mortar * (1 - face)[..., None]
    pits = smoothstep(0.86, 0.95, n.white()) * 0.6
    albedo *= (1 - 0.25 * pits)[..., None]
    height = face * (0.6 + 0.04 * n.fbm(40, 3)) - pits * 0.05 - peel * 0.03
    rough = 0.72 + 0.12 * peel + 0.08 * n.fbm(30, 3)
    ao = cavity_ao(height, 4, 3.0)
    save_set("Block", albedo, height, 3.0, rough, np.zeros_like(rough), ao)


def make_corrugated(size=1024):
    """Vertical corrugations every 7.6 cm (2 m a tile: 26 across), a panel lap at the
    tile edge, fixings every 30 cm down each lap, rust running from each."""
    n = Noise(2303, size)
    xs, ys = grid(size)
    waves = 26
    phase = xs / size * waves * 2 * math.pi
    height = 0.5 + 0.45 * np.sin(phase)
    galv = rgb(0.50, 0.52, 0.53) * (0.82 + 0.25 * n.fbm(4, 5, cells_v=12))[..., None]
    spangle = n.value(90) * 0.08
    galv *= (0.95 + spangle)[..., None]
    # Fixings: along both tile edges (the lap) and every 4th crest, every 30 cm down.
    spacing = size / (200.0 / 30.0)
    fix = np.zeros((size, size))
    for cx in [0.0, size] + [size * k / waves + size / waves * 0.25 for k in range(0, waves, 4)]:
        dy = (ys % spacing) - spacing / 2
        d = np.sqrt((xs - cx) ** 2 + dy ** 2)
        fix = np.maximum(fix, np.clip(1 - d / 5.0, 0, 1))
    seed = blur(fix, 2.0)
    streak = np.zeros_like(seed)
    for k in range(1, 120, 4):
        streak = np.maximum(streak, np.roll(seed, k, axis=0) * (1 - k / 120.0))
    streak *= 0.6 + n.fbm(30, 3, cells_v=3)
    rust_amt = np.clip(streak * 1.4, 0, 1) + 0.6 * smoothstep(0.66, 0.8, n.fbm(5, 5)) * (0.5 + 0.5 * np.sin(phase))
    rust_amt = np.clip(rust_amt, 0, 1)
    rust = rgb(0.34, 0.15, 0.06) * (0.7 + 0.5 * n.fbm(50, 3))[..., None]
    dirt = smoothstep(0.4, 0.9, n.fbm(3, 4)) * 0.35
    albedo = galv * (1 - rust_amt[..., None]) + rust * rust_amt[..., None]
    albedo *= (1 - dirt)[..., None]
    albedo *= (1 - 0.7 * fix)[..., None]
    height = height + fix * 0.3
    metal = 0.85 * (1 - rust_amt)
    rough = 0.42 + 0.35 * rust_amt + 0.2 * dirt + 0.08 * n.fbm(40, 3)
    ao = 0.75 + 0.25 * height
    save_set("Corrugated", albedo, height, 9.0, rough, metal, ao)


def make_steel(size=512):
    n = Noise(2404, size)
    chips = smoothstep(0.68, 0.72, n.fbm(10, 6))
    wear = smoothstep(0.58, 0.72, n.fbm(6, 6))
    paint = rgb(0.17, 0.21, 0.19) * (0.85 + 0.2 * n.fbm(20, 4))[..., None]
    rust = rgb(0.30, 0.13, 0.05) * (0.7 + 0.6 * n.fbm(60, 4))[..., None]
    mill = rgb(0.22, 0.22, 0.23)
    albedo = paint * (1 - 0.5 * wear[..., None]) + rust * (0.5 * wear)[..., None]
    albedo = albedo * (1 - chips[..., None]) + (rust * 0.6 + mill * 0.4) * chips[..., None]
    dust = smoothstep(0.5, 0.9, n.fbm(4, 3)) * 0.25
    albedo = albedo * (1 - dust[..., None]) + rgb(0.45, 0.42, 0.38) * dust[..., None]
    height = (1 - chips) * 0.5 + 0.04 * n.fbm(80, 3) - 0.05 * wear
    metal = chips * 0.5
    rough = 0.5 + 0.25 * wear + 0.3 * chips + 0.2 * dust
    ao = 1 - 0.3 * chips
    save_set("Steel", albedo, height, 2.5, rough, metal, ao)


def make_grating(size=512):
    """Bar grating, 0.5 m a tile: bearing bars along U every 3.125 cm (16 a tile),
    cross rods along V every 10 cm (5 a tile). Holes are alpha 0 (Masked)."""
    n = Noise(2505, size)
    xs, ys = grid(size)
    pitch_b = size / 16.0
    pitch_c = size / 5.0
    db = np.abs((ys % pitch_b) - pitch_b / 2)          # bearing bars: horizontal lines (along U)
    dc = np.abs((xs % pitch_c) - pitch_c / 2)          # cross rods: vertical lines
    bar = smoothstep(pitch_b / 2 - 2.6, pitch_b / 2 - 1.6, db)       # 1 on the bar
    rod = smoothstep(pitch_c / 2 - 3.2, pitch_c / 2 - 2.2, dc)
    solid = np.maximum(bar, rod)
    alpha = (solid > 0.5).astype(np.float64)
    galv = rgb(0.46, 0.47, 0.47) * (0.8 + 0.3 * n.fbm(6, 5))[..., None]
    grime = smoothstep(0.45, 0.85, n.fbm(5, 4)) * 0.5
    rust = smoothstep(0.7, 0.8, n.fbm(9, 5))
    albedo = galv * (1 - grime[..., None]) + rgb(0.16, 0.13, 0.10) * grime[..., None]
    albedo = albedo * (1 - rust[..., None]) + rgb(0.32, 0.14, 0.06) * rust[..., None]
    # Serrated tops on the bearing bars: shiny worn notches.
    serr = (0.5 + 0.5 * np.sin(xs / size * 2 * math.pi * 64)) * bar
    height = 0.4 * bar + 0.5 * rod + 0.1 * serr
    metal = (0.8 - 0.5 * grime - 0.6 * rust) * solid
    rough = 0.45 + 0.35 * grime + 0.3 * rust - 0.1 * serr
    ao = 0.6 + 0.4 * solid
    save_set("Grating", albedo, height, 3.0, rough, metal, ao, alpha=alpha)


def make_diamond_plate(size=512):
    n = Noise(2606, size)
    xs, ys = grid(size)
    cell = size / 12.0
    # Diamonds in a staggered herringbone: each a lozenge rotated +-45 by row.
    row = np.floor(ys / cell)
    cx = (xs % cell) - cell / 2
    cy = (ys % cell) - cell / 2
    sign = np.where(row % 2 == 0, 1.0, -1.0)
    u = (cx + sign * cy) / math.sqrt(2)
    v = (cx - sign * cy) / math.sqrt(2)
    lozenge = np.clip(1 - (np.abs(u) / (cell * 0.36)) ** 2 - (np.abs(v) / (cell * 0.09)) ** 2, 0, 1)
    raised = np.sqrt(lozenge)
    steel = rgb(0.42, 0.42, 0.42) * (0.75 + 0.3 * n.fbm(5, 5))[..., None]
    dirt = smoothstep(0.35, 0.8, n.fbm(6, 4)) * (1 - raised * 0.8)
    albedo = steel * (1 - 0.7 * dirt[..., None]) + rgb(0.12, 0.10, 0.08) * (0.7 * dirt)[..., None]
    albedo = albedo * (1 + 0.35 * raised[..., None])
    height = raised * 0.6 + 0.03 * n.fbm(60, 3)
    metal = 0.9 - 0.6 * dirt
    rough = 0.55 - 0.3 * raised + 0.35 * dirt
    ao = cavity_ao(height, 4, 2.0)
    save_set("DiamondPlate", albedo, height, 4.0, rough, metal, ao)


def make_plywood(size=512):
    """Rotary-cut fir veneer: long irregular streaks of figure along U, fine grain, knots' patches."""
    n = Noise(2707, size)
    xs, ys = grid(size)
    figure = n.fbm(2, 5, cells_v=20)                      # broad streaks, long along U
    grain = n.fbm(3, 4, cells_v=96)                       # fine lines
    patch = smoothstep(0.7, 0.85, n.fbm(4, 4, cells_v=6))
    wood = rgb(0.64, 0.50, 0.34) * (0.86 + 0.18 * n.fbm(3, 3))[..., None]
    albedo = wood * (0.84 + 0.2 * figure + 0.08 * grain)[..., None]
    albedo = albedo * (1 - 0.25 * patch)[..., None] + rgb(0.42, 0.28, 0.16) * (0.25 * patch)[..., None]
    scuff = smoothstep(0.62, 0.8, n.fbm(8, 5))
    grime = smoothstep(0.4, 0.9, n.fbm(3, 3)) * 0.4
    albedo = albedo * (1 - 0.35 * scuff - grime)[..., None]
    # Bullet holes: dark round pocks with splintered rims.
    holes = np.zeros((size, size))
    rng = np.random.default_rng(27)
    for _ in range(14):
        hx, hy = rng.random(2) * size
        d = np.sqrt(np.minimum(np.abs(xs - hx), size - np.abs(xs - hx)) ** 2
                    + np.minimum(np.abs(ys - hy), size - np.abs(ys - hy)) ** 2)
        holes = np.maximum(holes, np.clip(1 - d / 4.5, 0, 1))
        holes = np.maximum(holes, 0.35 * np.clip(1 - d / 11.0, 0, 1) * n.white())
    albedo *= (1 - 0.85 * holes)[..., None]
    height = 0.5 + 0.03 * figure + 0.03 * grain - 0.6 * holes - 0.05 * scuff
    rough = 0.7 + 0.15 * scuff + 0.1 * grime
    ao = cavity_ao(height, 3, 3.0)
    save_set("Plywood", albedo, height, 2.5, rough, np.zeros_like(rough), ao)


def make_rubber(size=512):
    n = Noise(2808, size)
    crumb = blur(n.white(), 1.2)
    crumb = (crumb - crumb.min()) / (crumb.max() - crumb.min())
    xs, ys = grid(size)
    pocks = np.zeros((size, size))
    rng = np.random.default_rng(28)
    for _ in range(60):
        hx, hy = rng.random(2) * size
        d = np.sqrt(np.minimum(np.abs(xs - hx), size - np.abs(xs - hx)) ** 2
                    + np.minimum(np.abs(ys - hy), size - np.abs(ys - hy)) ** 2)
        pocks = np.maximum(pocks, np.clip(1 - d / (3 + 5 * rng.random()), 0, 1))
    lead = smoothstep(0.75, 0.85, n.fbm(20, 4)) * 0.5
    albedo = rgb(0.05, 0.05, 0.05) * (0.7 + 0.6 * crumb)[..., None]
    albedo = albedo * (1 - lead[..., None]) + rgb(0.22, 0.22, 0.23) * lead[..., None]
    albedo *= (1 - 0.5 * pocks)[..., None]
    height = 0.5 + 0.15 * crumb - 0.5 * pocks
    rough = 0.9 - 0.25 * lead
    ao = cavity_ao(height, 3, 3.0)
    save_set("Rubber", albedo, height, 3.0, rough, lead * 0.4, ao)


def make_floor_paint(size=512):
    """Safety-yellow line paint, worn through to concrete in the traffic (1 m a tile)."""
    n = Noise(2909, size)
    wear = smoothstep(0.5, 0.72, n.fbm(5, 6) * 0.7 + 0.3 * n.fbm(40, 3))
    paint = rgb(0.78, 0.58, 0.06) * (0.85 + 0.2 * n.fbm(10, 4))[..., None]
    conc = rgb(0.42, 0.41, 0.39) * (0.85 + 0.2 * n.fbm(30, 4))[..., None]
    grime = smoothstep(0.4, 0.9, n.fbm(4, 4)) * 0.35
    albedo = paint * (1 - wear[..., None]) + conc * wear[..., None]
    albedo *= (1 - grime)[..., None]
    height = 0.55 - 0.08 * wear + 0.03 * n.fbm(60, 3)
    rough = 0.55 + 0.25 * wear + 0.1 * grime
    ao = np.ones_like(rough)
    save_set("FloorPaint", albedo, height, 2.0, rough, np.zeros_like(rough), ao)


def make_glass(size=512):
    n = Noise(3010, size)
    film = smoothstep(0.3, 0.9, n.fbm(4, 5))
    albedo = rgb(0.80, 0.82, 0.80) * (1 - 0.35 * film)[..., None] + rgb(0.30, 0.27, 0.22) * (0.35 * film)[..., None]
    height = 0.5 + 0.1 * n.fbm(40, 4)
    rough = 0.35 + 0.45 * film
    save_set("Glass", albedo, height, 1.5, rough, np.zeros_like(rough), np.ones_like(rough))


def make_steel_target(size=512):
    """AR500 plate paint: white, shot to grey where lead splashed, rust at the edge."""
    n = Noise(3111, size)
    xs, ys = grid(size)
    paint = rgb(0.86, 0.85, 0.80) * (0.92 + 0.1 * n.fbm(8, 4))[..., None]
    splash = np.zeros((size, size))
    rng = np.random.default_rng(31)
    for _ in range(40):
        hx, hy = rng.random(2) * size
        r = 6 + 22 * rng.random()
        ang = np.arctan2(ys - hy, xs - hx)
        d = np.sqrt((xs - hx) ** 2 + (ys - hy) ** 2)
        star = r * (0.6 + 0.6 * np.abs(np.sin(ang * (5 + rng.integers(0, 5)) + rng.random() * 6)))
        splash = np.maximum(splash, smoothstep(star, star * 0.5, d))
    lead = rgb(0.30, 0.31, 0.32) * (0.8 + 0.4 * n.fbm(40, 3))[..., None]
    rust = smoothstep(0.72, 0.82, n.fbm(6, 5)) * 0.6
    albedo = paint * (1 - splash[..., None]) + lead * splash[..., None]
    albedo = albedo * (1 - rust[..., None]) + rgb(0.35, 0.16, 0.07) * rust[..., None]
    height = 0.5 + 0.08 * splash + 0.02 * n.fbm(60, 3)
    rough = 0.55 + 0.25 * splash + 0.2 * rust
    metal = 0.6 * splash
    save_set("SteelTarget", albedo, height, 2.0, rough, metal, np.ones_like(rough))


def make_target_paper(width=512, height=1024):
    """A B-27 style silhouette: black figure on buff card, scoring rings, a 7-10 X ring."""
    ys, xs = np.mgrid[0:height, 0:width] + 0.5
    u = xs / width
    v = ys / height
    n = Noise(3212, max(width, height))
    card = rgb(0.80, 0.74, 0.60) * (0.94 + 0.08 * n.fbm(8, 4)[:height, :width])[..., None]
    # Head: an ellipse; body: a rounded trapezoid widening to the shoulders.
    head = ((u - 0.5) / 0.11) ** 2 + ((v - 0.17) / 0.085) ** 2 < 1
    neck = (np.abs(u - 0.5) < 0.06) & (v > 0.22) & (v < 0.3)
    half_w = np.where(v < 0.36, 0.16 + (v - 0.28) * 2.5, 0.36)
    body = (v > 0.28) & (v < 0.97) & (np.abs(u - 0.5) < np.clip(half_w, 0, 0.36))
    figure = head | neck | body
    black = rgb(0.035, 0.035, 0.04)
    img = np.where(figure[..., None], black, card)
    # Rings (white on black) about the centre of mass.
    rx, ry = (u - 0.5) * width, (v - 0.55) * height
    d = np.sqrt((rx / 1.0) ** 2 + (ry / 1.35) ** 2)
    ring = np.zeros_like(d, dtype=bool)
    for radius in (26, 52, 82, 112, 142):
        ring |= np.abs(d - radius) < 1.3
    img = np.where((ring & figure)[..., None], rgb(0.85, 0.84, 0.80), img)
    img = np.where(((d < 9) & figure)[..., None], rgb(0.85, 0.84, 0.80), img)
    # Printed border and a little grime.
    border = (u < 0.02) | (u > 0.98) | (v < 0.015) | (v > 0.985)
    img = np.where(border[..., None], img * 0.75, img)
    grime = smoothstep(0.5, 0.95, n.fbm(3, 4)[:height, :width]) * 0.25
    img = img * (1 - grime[..., None])
    nm.write_texture(OUT / "TargetPaper_BaseColor.nmtexture", np.clip(img, 0, 1), srgb=True, wrap="clamp")
    print(f"  TargetPaper: {width} x {height}")


def make_sky(width=2048, height=1024):
    """Equirectangular, row 0 straight up; azimuth 0 at the centre column looking
    +Z. Midday: a deep blue zenith, a pale horizon, broken cumulus, the sun high."""
    ys, xs = np.mgrid[0:height, 0:width] + 0.5
    lon = (xs / width - 0.5) * 2 * math.pi
    lat = (0.5 - ys / height) * math.pi
    dx, dy, dz = np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)
    el, az = math.radians(SUN_ELEVATION), math.radians(SUN_AZIMUTH)
    sx, sy, sz = math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)
    cos_sun = np.clip(dx * sx + dy * sy + dz * sz, -1, 1)
    up = np.clip(dy, 0, 1)
    zenith, horizon = np.array([0.16, 0.30, 0.62]), np.array([0.78, 0.84, 0.90])
    t = np.power(1 - up, 4.0)[..., None]
    sky = zenith * (1 - t) + horizon * t
    sky += np.power(np.clip((cos_sun + 1) / 2, 0, 1), 16)[..., None] * np.array([1.0, 0.9, 0.7]) * 0.8
    sky += np.power(np.clip(cos_sun, 0, 1), 400)[..., None] * np.array([8.0, 7.0, 5.5])
    cn = Noise(3313, 512)
    cu = ((dx / np.maximum(dy, 0.05)) * 0.2) % 1.0
    cv = ((dz / np.maximum(dy, 0.05)) * 0.2) % 1.0
    cover = smoothstep(0.5, 0.68, cn.fbm(5, 5, u=cu, v=cv)) * smoothstep(0.0, 0.2, dy)
    lit = np.array([1.5, 1.45, 1.4]) * (0.7 + 0.6 * np.power(np.clip((cos_sun + 1) / 2, 0, 1), 4))[..., None]
    sky = sky * (1 - cover[..., None] * 0.8) + lit * cover[..., None] * 0.8
    disc = (cos_sun > math.cos(math.radians(0.55)))[..., None]
    sky = np.where(disc, np.array([90.0, 85.0, 75.0]), sky)
    ground = np.array([0.22, 0.21, 0.19]) * (0.6 + 0.4 * np.exp(dy * 6))[..., None]
    sky = np.where((dy < 0)[..., None], ground, sky)
    nm.write_hdri(OUT / "Sky_HDR.nmtexture", sky.astype(np.float64))
    print(f"  Sky: {width} x {height} HDR")


MAKERS = {
    "Concrete": make_concrete, "Block": make_block, "Corrugated": make_corrugated, "Steel": make_steel,
    "Grating": make_grating, "DiamondPlate": make_diamond_plate, "Plywood": make_plywood, "Rubber": make_rubber,
    "FloorPaint": make_floor_paint, "Glass": make_glass, "SteelTarget": make_steel_target,
    "TargetPaper": make_target_paper, "Sky": make_sky,
}


def main():
    names = sys.argv[1:] or list(MAKERS)
    print(f"Writing {OUT}")
    for name in names:
        MAKERS[name]()


if __name__ == "__main__":
    main()
