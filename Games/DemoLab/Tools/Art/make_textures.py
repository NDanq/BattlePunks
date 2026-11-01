#!/usr/bin/env python3
"""
make_textures.py -- Demo Lab's tiling texture sets, terrain layers and decal textures,
all procedural (numpy), all seeded. Writes Content/Textures/Kit/, Content/Textures/Terrain/
and PNG copies in Saved/ArtCache/Textures for the Blender previews.

Kit sets (UV0 of the kit meshes is in metres; each material scales by 1 / the metres one
repeat covers, listed here):

    Brass        brushed, tarnished brass; streaks along U                   0.5 m
    Copper       copper going to verdigris                                   1 m
    Steel        dark blued, brushed steel with rust freckles                1 m
    RivetPanel   painted iron plates, riveted, chipped to steel at edges;
                 BaseColor A is the paint mask (tinted by the material)     2 m
    Grating      expanded-metal catwalk mesh; BaseColor A is the opacity    0.5 m
    Walnut       lacquered walnut veneer, grain along U                     1 m
    Planks       weathered dock planks along U, nail heads                  2 m
    StoneBlocks  rusticated granite ashlar, 50 cm courses                   3 m
    Rock         stratified cliff rock, cracks and lichen                    4 m
    MarbleFloor  cream and green marble checker, brass inlay (metal)        2 m
    Marble       white veined marble                                        1 m
    Leather      pebble-grain leather                                        0.25 m
    Velvet       crushed velvet pile                                         0.1 m
    Canvas       airship envelope canvas: gores, seams, stitching           4 m
    Concrete     cast concrete with formwork lines                           2 m
    Glass        smudges and dust (roughness), faint waviness               1 m

Single textures (UV 0-1):

    GaugeFace_BaseColor    a pressure-gauge dial (emissive in its material)
    Books_BaseColor        8 x 2 book-spine and cover designs (see make_l2.py)
    LeafPanel_BaseColor    a pressed fern on paper; A is the leaf mask

Terrain layers (Content/Textures/Terrain, 2 m a repeat; BaseColor A is the height the
terrain material blends by; no ORM -- the terrain material may sample only 7 textures):
Grass and Dirt (BaseColor, Normal), Sand (BaseColor), and Rock (the kit's Rock set, 4 m).

    python Games/DemoLab/Tools/Art/make_textures.py [set names...]
"""

import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import texlib as T            # noqa: E402
from texlib import Noise, blur, smoothstep, lerp, rgb, colour, cavity_ao, normal_from_height  # noqa: E402

GAME_DIR = Path(__file__).resolve().parents[2]
KIT = GAME_DIR / "Content" / "Textures" / "Kit"
TERRAIN = GAME_DIR / "Content" / "Textures" / "Terrain"


# ==============================================================================================
# Metals
# ==============================================================================================

def brass(size=1024):
    n = Noise(11, size)
    # Brushing: fine streaks along U (stretched noise), a few deeper scratches.
    streak = n.fbm(4, 4, cells_v=256) * 0.6 + n.fbm(8, 3, cells_v=512) * 0.4
    scratches = smoothstep(0.86, 0.97, n.value(3, 700)) * 0.5
    # Tarnish: blotchy darkening and a brown-green film, less where hands polish (noise).
    tarnish = smoothstep(0.42, 0.75, n.fbm(5, 5))
    spots = smoothstep(0.74, 0.88, n.fbm(24, 3)) * 0.6
    film = np.clip(tarnish * 0.55 + spots * 0.5, 0, 1)
    bright = rgb(0.91, 0.75, 0.47)
    dark = rgb(0.52, 0.42, 0.27)
    base = lerp(colour(bright, 0.9 + 0.12 * streak), colour(dark, 0.85 + 0.2 * n.fbm(16, 3)), film)
    rough = 0.24 + 0.1 * streak + 0.28 * film + scratches * 0.1
    height = 0.5 + (streak - 0.5) * 0.25 - scratches * 0.3 - spots * 0.05
    ao = 1.0 - 0.1 * film
    T.save_set(KIT, "Brass", base, height, 1.5, rough, np.ones_like(rough), ao, orm_scale=2)


def copper(size=512):
    n = Noise(12, size)
    streak = n.fbm(4, 4, cells_v=128)
    patina = smoothstep(0.66, 0.86, n.fbm(4, 5) + 0.15 * n.fbm(32, 3))
    crust = smoothstep(0.6, 0.85, n.fbm(20, 3)) * patina
    metal_col = colour(rgb(0.93, 0.55, 0.38), 0.85 + 0.15 * streak)
    oxide = colour(rgb(0.45, 0.24, 0.16), 0.8 + 0.3 * n.fbm(12, 3))
    verd = colour(rgb(0.36, 0.56, 0.48), 0.8 + 0.35 * n.fbm(16, 3))
    darken = smoothstep(0.3, 0.6, n.fbm(6, 4))
    base = lerp(metal_col, oxide, darken * 0.7)
    base = lerp(base, verd, patina)
    metal = 1.0 - patina
    rough = 0.3 + 0.25 * darken + 0.45 * patina
    height = 0.5 + 0.2 * (streak - 0.5) + crust * 0.4
    T.save_set(KIT, "Copper", base, height, 3.0, rough, metal, 1.0 - 0.2 * crust)


def steel(size=512):
    n = Noise(13, size)
    streak = n.fbm(3, 4, cells_v=192)
    blue = smoothstep(0.3, 0.7, n.fbm(3, 4))
    rust = smoothstep(0.74, 0.86, n.fbm(10, 4) * 0.7 + n.fbm(48, 2) * 0.3)
    base = lerp(colour(rgb(0.36, 0.37, 0.40), 0.85 + 0.2 * streak), colour(rgb(0.24, 0.27, 0.34), 1.0), blue * 0.5)
    base = lerp(base, colour(rgb(0.32, 0.15, 0.07), 0.7 + 0.5 * n.fbm(32, 3)), rust)
    rough = 0.32 + 0.12 * streak + 0.5 * rust
    metal = 1.0 - rust * 0.9
    height = 0.5 + 0.15 * (streak - 0.5) + 0.25 * rust * n.fbm(64, 2)
    T.save_set(KIT, "Steel", base, height, 2.5, rough, metal, 1.0 - 0.25 * rust)


def rivet_panel(size=1024):
    """2 m: plates 1 m wide and 50 cm tall in running bond, rivets 6.25 cm apart along
    their edges. Paint is a light neutral (the material tints it); chips show steel."""
    n = Noise(14, size)
    px = size / 2.0                   # pixels a metre
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    row = np.floor(ys / (0.5 * px)).astype(int)
    xo = (xs + (row % 2) * 0.5 * px) % size
    lx = xo % px                      # px across the plate
    ly = ys % (0.5 * px)
    edge = np.minimum(np.minimum(lx, px - lx), np.minimum(ly, 0.5 * px - ly))   # px to the plate edge
    seam = smoothstep(1.5, 4.0, edge)
    # Plates overlap: a slight bevel down to each seam, rivets 3 cm in from the edge.
    bevel = smoothstep(0.0, 14.0, edge)
    spacing = 0.0625 * px
    near_x = np.minimum(lx, px - lx)
    near_y = np.minimum(ly, 0.5 * px - ly)
    rv_r = 0.011 * px
    inset = 0.03 * px
    dx_line = np.abs(near_x - inset)
    dy_line = np.abs(near_y - inset)
    along_y = ((ly / spacing) % 1.0 - 0.5) * spacing
    along_x = ((lx / spacing) % 1.0 - 0.5) * spacing
    d1 = np.hypot(dx_line, along_y)
    d2 = np.hypot(dy_line, along_x)
    d = np.minimum(d1, d2)
    rivet = np.sqrt(np.clip(1.0 - (d / rv_r) ** 2, 0, 1))
    # Dents and waviness per plate.
    plate_id = (np.floor(xo / px) * 7 + row * 13) % 17 / 17.0
    wav = n.fbm(6, 4) * 0.5 + plate_id * 0.2
    height = 0.35 + 0.35 * bevel + 0.1 * (wav - 0.5) + 0.35 * rivet
    height = height * seam + 0.05 * (1 - seam)
    # Paint chips: near edges and rivets, noisy.
    chipn = n.fbm(24, 4)
    wear = np.clip(smoothstep(20.0, 2.0, edge) * 0.55 + smoothstep(rv_r * 1.8, rv_r * 0.6, d) * 0.7, 0, 1)
    chips = smoothstep(0.62, 0.66, chipn * 0.6 + wear * 0.55 + n.fbm(64, 2) * 0.15)
    paint_mask = 1.0 - chips
    paint = colour(rgb(0.72, 0.72, 0.70), 0.88 + 0.12 * n.fbm(8, 4) - 0.1 * (1 - bevel))
    grime = smoothstep(0.4, 0.8, n.fbm(3, 4)) * 0.25 + (1 - seam) * 0.5
    streaks = smoothstep(0.5, 0.9, n.fbm(40, 3, cells_v=3)) * 0.18
    paint *= (1.0 - grime - streaks)[..., None]
    metal_col = colour(rgb(0.42, 0.40, 0.38), 0.8 + 0.3 * n.fbm(32, 3))
    base = lerp(metal_col, paint, paint_mask)
    rough = lerp(0.42 + 0.15 * n.fbm(32, 3), 0.48 + 0.2 * n.fbm(16, 3) + grime * 0.3, paint_mask)
    metal = 1.0 - paint_mask
    height = height - chips * 0.04
    ao = cavity_ao(height, 6, 2.5) * (0.6 + 0.4 * seam)
    T.save_set(KIT, "RivetPanel", base, height, 6.0, rough, metal, ao, alpha=paint_mask, orm_scale=2)


def grating(size=512):
    """0.5 m: expanded-metal diamonds, 5 x 10 across. A: opacity."""
    n = Noise(15, size)
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    cw, ch = size / 5.0, size / 10.0
    u = xs / cw
    v = ys / ch
    v2 = v + 0.5 * (np.floor(u) % 2)       # offset columns
    fu = (u % 1.0) - 0.5
    fv = (v2 % 1.0) - 0.5
    diamond = np.abs(fu) * 1.0 + np.abs(fv) * 1.0       # 0.5 at the diamond's edge
    strand = smoothstep(0.36, 0.42, diamond)             # 1 on the metal strands
    opacity = strand
    height = smoothstep(0.36, 0.5, diamond)
    base = colour(rgb(0.30, 0.30, 0.31), 0.8 + 0.3 * n.fbm(16, 3))
    rust = smoothstep(0.65, 0.8, n.fbm(8, 4))
    base = lerp(base, colour(rgb(0.30, 0.16, 0.08), 1.0), rust * 0.7)
    rough = 0.45 + 0.35 * rust
    T.save_set(KIT, "Grating", base, height, 4.0, rough, 1.0 - rust * 0.8, 0.7 + 0.3 * height, alpha=opacity)


# ==============================================================================================
# Wood
# ==============================================================================================

def wood_grain(n, size, rings, warp, size_v=None):
    """Flat-sawn grain along U: rings in v, pushed about into cathedrals."""
    u, v = n.u, n.v
    wv = (n.fbm(2, 4, cells_v=6) - 0.5) * warp + (n.fbm(1, 3, cells_v=3) - 0.5) * warp * 2
    cathedral = np.abs(np.sin((u * 2 * math.pi) + n.fbm(2, 2) * 3)) * 0.15
    g = (v + wv + cathedral) * rings
    ring = np.abs((g % 1.0) - 0.5) * 2.0
    late = smoothstep(0.35, 1.0, ring) ** 1.5
    pores = smoothstep(0.75, 0.95, n.value(size // 3, size // 24)) * 0.5
    return late, pores


def walnut(size=1024):
    """1 m: quarter-sawn walnut boards along U -- tight straight grain, open pores drawn
    out along the grain, a ribbon figure, medullary flecks, 12.5 cm boards each of its own
    tone and grain, under the material's clear coat."""
    n = Noise(21, size)
    u, v = n.u, n.v
    board = np.floor(v * 8).astype(int) % 8
    rng = np.random.default_rng(21)
    btone = rng.uniform(0.82, 1.12, 8)[board]
    bshift = rng.uniform(0, 1, 8)[board]
    wv = (n.fbm(2, 4, cells_v=8) - 0.5) * 0.035 + (n.fbm(6, 3, cells_v=16) - 0.5) * 0.01
    g = (v + wv + bshift) * 90
    ring = np.abs((g % 1.0) - 0.5) * 2.0
    late = smoothstep(0.45, 1.0, ring) ** 2
    pores = smoothstep(0.62, 0.9, n.value(size // 8, size // 2)) * smoothstep(0.3, 0.7, n.fbm(32, 2))
    ribbon = 0.5 + 0.5 * np.sin(u * 2 * math.pi * 6 + n.fbm(3, 3) * 4)
    fleck = smoothstep(0.8, 0.95, n.value(size // 4, size // 64)) * 0.6
    light = colour(rgb(0.34, 0.19, 0.10), 0.92 + 0.12 * n.fbm(4, 4, cells_v=24))
    dark = colour(rgb(0.13, 0.065, 0.035), 1.0)
    base = lerp(light, dark, late * 0.7 + 0.15 * (1 - ribbon) * (0.5 + 0.5 * late))
    base *= (1.0 - pores * 0.45)[..., None]
    base = lerp(base, colour(rgb(0.42, 0.26, 0.15), 1.0), fleck * 0.35)
    base *= btone[..., None]
    fv = (v * 8) % 1.0
    joint = smoothstep(0.0, 0.004, fv) * smoothstep(0.0, 0.004, 1.0 - fv)
    base *= (0.55 + 0.45 * joint)[..., None]
    height = 0.55 - late * 0.06 - pores * 0.3 - (1 - joint) * 0.4 + 0.02 * ribbon
    rough = 0.38 + 0.12 * late + 0.3 * pores + 0.2 * (1 - joint)
    T.save_set(KIT, "Walnut", base, height, 2.5, rough, np.zeros_like(rough), 1.0 - 0.2 * pores - 0.4 * (1 - joint),
               orm_scale=2)


def planks(size=1024):
    """2 m: eight 25 cm boards along U, butt joints, nail heads; grey weathered pine."""
    n = Noise(22, size)
    px = size / 2.0
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    board = np.floor(ys / (0.25 * px)).astype(int)
    rng = np.random.default_rng(5)
    offs = rng.random(8) * size
    lens = rng.uniform(1.2, 2.0, 8) * px
    tones = rng.uniform(0.75, 1.15, 8)
    bx = (xs + offs[board]) % size
    seg = np.floor(bx / lens[board])
    lx = bx - seg * lens[board]
    ly = ys % (0.25 * px)
    gap_v = np.minimum(ly, 0.25 * px - ly)
    gap_u = np.minimum(lx, lens[board] - lx)
    groove = smoothstep(1.0, 5.0, np.minimum(gap_v, gap_u * 2.0))
    late, pores = wood_grain(n, size, 30, 0.6)
    weather = n.fbm(8, 5, cells_v=48)
    cracks = smoothstep(0.93, 0.985, n.value(4, 200)) * groove
    nails_d = np.hypot(np.minimum(np.abs(lx - 0.06 * px), np.abs(lx - lens[board] + 0.06 * px)),
                       np.minimum(np.abs(ly - 0.06 * px), np.abs(ly - 0.19 * px)))
    nail = smoothstep(0.009 * px, 0.004 * px, nails_d)
    grey = colour(rgb(0.50, 0.46, 0.40), 0.8 + 0.35 * weather)
    brown = colour(rgb(0.36, 0.26, 0.17), 0.85 + 0.2 * weather)
    t = smoothstep(0.35, 0.65, n.fbm(3, 4)) * 0.6
    base = lerp(grey, brown, t) * tones[board][..., None]
    base *= (1.0 - 0.3 * late - 0.25 * pores)[..., None]
    base = lerp(base, colour(rgb(0.16, 0.11, 0.08), 1.0), nail * 0.8)
    base *= (0.35 + 0.65 * groove)[..., None]
    height = 0.6 * groove + 0.1 * (weather - 0.5) - 0.08 * late - cracks * 0.3 + 0.05 * nail
    rough = 0.72 + 0.2 * weather - 0.3 * nail
    metal = nail * 0.6
    ao = cavity_ao(height, 5, 3.0)
    T.save_set(KIT, "Planks", base, height, 5.0, rough, metal, ao, orm_scale=2)


# ==============================================================================================
# Stone and ground
# ==============================================================================================

def stone_blocks(size=1024):
    """3 m: six 50 cm courses of rusticated granite ashlar, 0.5-1.2 m blocks."""
    n = Noise(31, size)
    px = size / 3.0
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    course = np.floor(ys / (0.5 * px)).astype(int)
    rng = np.random.default_rng(31)
    inside = np.zeros((size, size))
    bid = np.zeros((size, size))
    for c in range(6):
        y0, y1 = c * 0.5 * px, (c + 1) * 0.5 * px
        rows = (ys[:, 0] >= y0) & (ys[:, 0] < y1)
        edges = [0.0]
        while edges[-1] < 3.0 - 0.45:
            edges.append(edges[-1] + rng.uniform(0.55, 1.2))
        edges[-1] = 3.0
        off = rng.random() * size
        xw = (xs[rows] + off) % size
        for a, e in zip(edges[:-1], edges[1:]):
            m = (xw >= a * px) & (xw < e * px)
            dx = np.minimum(xw - a * px, e * px - xw)
            dy = np.minimum(ys[rows] - y0, y1 - ys[rows])
            d = np.minimum(dx, dy) - 3.0
            sub = inside[rows]
            sub[m] = np.maximum(d[m], 0)
            inside[rows] = sub
            b = bid[rows]
            b[m] = rng.random()
            bid[rows] = b
    face = 1.0 - np.exp(-inside / 10.0)
    rustic = n.fbm(12, 5)
    height = face * (0.65 + 0.35 * rustic) + (bid - 0.5) * 0.1 * face
    chisel = smoothstep(0.6, 0.9, n.ridged(24, 3)) * 0.15
    height -= chisel * face
    pal = np.array([[0.52, 0.50, 0.47], [0.46, 0.45, 0.44], [0.56, 0.53, 0.49], [0.43, 0.41, 0.40]])
    base = pal[(bid * 4 * 7.3).astype(int) % 4]
    speck = blur(n.white(), 0.8)
    tone = 0.85 + 0.25 * n.fbm(20, 4) + (smoothstep(0.62, 0.72, speck) * 0.3 - smoothstep(0.38, 0.28, speck) * 0.25)
    base = base * tone[..., None]
    mortar = colour(rgb(0.38, 0.36, 0.33), 0.8 + 0.3 * n.fbm(64, 2))
    base = lerp(mortar, base, smoothstep(0.0, 1.5, inside))
    streak = smoothstep(0.55, 0.85, n.fbm(30, 3, cells_v=2)) * 0.25
    moss = smoothstep(0.62, 0.8, n.fbm(5, 5)) * smoothstep(8.0, 0.0, inside + 6 * n.fbm(32, 2))
    base *= (1.0 - streak)[..., None]
    base = lerp(base, colour(rgb(0.20, 0.25, 0.10), 1.0), moss * 0.7)
    rough = 0.78 + 0.15 * rustic
    ao = cavity_ao(height, 8, 2.5)
    T.save_set(KIT, "StoneBlocks", base, height, 7.0, rough, np.zeros_like(rough), ao, orm_scale=2)


def rock(size=1024, out=KIT, name="Rock"):
    """4 m: stratified grey rock: rounded lumps, sharp ridges, ledges along U, a few
    cracks, lichen on the tops."""
    n = Noise(32, size)
    wu, wv = n.warp(2, 0.3)
    lumps = n.fbm(4, 6, gain=0.55)
    ridge = n.ridged(3, 5)
    band = ((wv * 6 + n.fbm(3, 3) * 1.2) % 1.0)
    ledge = smoothstep(0.0, 0.12, band) * smoothstep(1.0, 0.7, band)       # 0 at each ledge's lip
    f1, f2, cid = n.voronoi(5, 3, u=wu, v=wv)
    crack = smoothstep(0.05, 0.0, f2 - f1) * smoothstep(0.4, 0.6, n.fbm(6, 3))
    height = 0.45 * lumps + 0.3 * ridge + 0.2 * ledge * band - 0.35 * crack
    height = (height - height.min()) / (height.max() - height.min())
    grey = colour(rgb(0.46, 0.45, 0.43), 0.82 + 0.3 * n.fbm(12, 4))
    warm = colour(rgb(0.50, 0.43, 0.35), 0.9 + 0.2 * lumps)
    base = lerp(grey, warm, smoothstep(0.4, 0.75, n.fbm(3, 4)) * 0.7)
    base *= (0.8 + 0.35 * (height - 0.5))[..., None]
    up = smoothstep(0.55, 0.8, height)
    lichen = smoothstep(0.62, 0.72, n.fbm(10, 4) * 0.8 + up * 0.3) * (1 - crack)
    base = lerp(base, colour(rgb(0.56, 0.56, 0.40), 0.85 + 0.3 * n.fbm(64, 2)), lichen * 0.45)
    base *= (1.0 - 0.3 * crack - 0.2 * (1 - ledge))[..., None]
    rough = 0.8 + 0.12 * lumps - 0.08 * lichen
    ao = cavity_ao(height, 12, 3.0) * (1 - 0.4 * crack)
    T.save_set(out, name, base, height, 9.0, rough, np.zeros_like(rough), ao, orm_scale=2)


def concrete(size=512):
    n = Noise(33, size)
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    board = np.minimum((ys % (size / 8)), (size / 8) - (ys % (size / 8)))
    line = smoothstep(2.0, 0.0, board) * 0.3
    pits = smoothstep(0.78, 0.88, blur(n.white(), 1.0)) * 0.4
    tone = n.fbm(6, 5)
    base = colour(rgb(0.52, 0.51, 0.49), 0.82 + 0.3 * tone)
    stain = smoothstep(0.55, 0.8, n.fbm(3, 4, cells_v=1)) * 0.25
    base *= (1.0 - stain - line * 0.5 - pits * 0.3)[..., None]
    height = 0.5 + 0.1 * (tone - 0.5) - pits * 0.4 - line * 0.2
    rough = 0.85 + 0.1 * tone
    T.save_set(KIT, "Concrete", base, height, 4.0, rough, np.zeros_like(rough), cavity_ao(height, 4, 3))


def marble_pattern(n, a, b, veins_scale=3, vein_col=rgb(0.25, 0.25, 0.22), seed_shift=0.0):
    wu, wv = n.warp(3, 0.4)
    t = (wu + wv * 0.6) * veins_scale + n.fbm(4, 5) * 2.5 + seed_shift
    vein = smoothstep(0.93, 0.997, 1.0 - np.abs(np.sin(t * math.pi)))
    fine = smoothstep(0.93, 0.995, 1.0 - np.abs(np.sin((t * 3.1 + n.fbm(8, 3) * 2) * math.pi))) * 0.5
    cloud = n.fbm(5, 5)
    col = lerp(colour(a, 1.0), colour(b, 1.0), cloud)
    col = lerp(col, colour(vein_col, 1.0), blur(np.clip(vein + fine, 0, 1), 0.8) * 0.65)
    return col, vein + fine


def marble(size=512):
    n = Noise(34, size)
    col, vein = marble_pattern(n, rgb(0.92, 0.91, 0.88), rgb(0.84, 0.82, 0.79), 2.0, rgb(0.55, 0.55, 0.53))
    height = 0.5 - vein * 0.05
    rough = 0.12 + 0.08 * vein
    T.save_set(KIT, "Marble", col, height, 1.0, rough, np.zeros_like(rough), np.ones_like(rough))


def marble_floor(size=1024):
    """2 m: 50 cm squares, cream and dark green marble, a 1 cm brass strip between."""
    n = Noise(35, size)
    px = size / 2.0
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    cx = np.floor(xs / (0.5 * px)).astype(int)
    cy = np.floor(ys / (0.5 * px)).astype(int)
    dark = (cx + cy) % 2 == 1
    cream, _ = marble_pattern(n, rgb(0.86, 0.83, 0.76), rgb(0.76, 0.72, 0.64), 2.5, rgb(0.55, 0.50, 0.42))
    green, _ = marble_pattern(n, rgb(0.08, 0.16, 0.12), rgb(0.03, 0.07, 0.05), 3.5, rgb(0.62, 0.68, 0.62), 1.3)
    base = np.where(dark[..., None], green, cream)
    lx = xs % (0.5 * px)
    ly = ys % (0.5 * px)
    edge = np.minimum(np.minimum(lx, 0.5 * px - lx), np.minimum(ly, 0.5 * px - ly))
    inlay = edge < 0.006 * px
    base = np.where(inlay[..., None], colour(rgb(0.85, 0.66, 0.36), 0.85 + 0.15 * n.fbm(64, 2)), base)
    scuff = smoothstep(0.55, 0.85, n.fbm(6, 5))
    rough = np.where(inlay, 0.3, 0.08 + 0.18 * scuff + 0.04 * n.fbm(32, 3))
    height = np.where(inlay, 0.45, 0.5) - smoothstep(3.0, 0.0, np.abs(edge - 0.006 * px)) * 0.1
    T.save_set(KIT, "MarbleFloor", base, height, 2.0, rough, inlay.astype(float), np.where(inlay, 0.85, 1.0),
               orm_scale=2)


# ==============================================================================================
# Fabric, leather, glass
# ==============================================================================================

def leather(size=512):
    n = Noise(41, size)
    f1, f2, _ = n.voronoi(48)
    pebble = smoothstep(0.0, 0.5, f2 - f1)
    wrinkle = n.ridged(6, 4)
    height = 0.6 * pebble + 0.3 * wrinkle
    base = colour(rgb(0.30, 0.15, 0.07), 0.75 + 0.35 * n.fbm(5, 5))
    base *= (0.75 + 0.25 * pebble)[..., None]
    rough = 0.55 + 0.25 * (1 - pebble) + 0.1 * n.fbm(8, 3)
    T.save_set(KIT, "Leather", base, height, 3.0, rough, np.zeros_like(rough), 0.75 + 0.25 * pebble)


def velvet(size=512):
    n = Noise(42, size)
    crush = n.fbm(4, 5)
    pile = blur(n.white(), 0.6)
    height = 0.5 + 0.3 * (crush - 0.5) + 0.1 * (pile - 0.5)
    base = colour(rgb(0.40, 0.04, 0.10), 0.75 + 0.45 * crush)
    rough = 0.8 + 0.15 * pile
    T.save_set(KIT, "Velvet", base, height, 2.0, rough, np.zeros_like(rough), 0.85 + 0.15 * crush)


def canvas(size=512):
    """4 m: envelope canvas gores 1 m wide (seams along V), doped and weathered."""
    n = Noise(43, size)
    px = size / 4.0
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    lx = xs % px
    seam_d = np.minimum(lx, px - lx)
    seam = smoothstep(4.0, 0.5, seam_d)
    stitch = seam * (np.sin(ys * 1.6) > 0.3) * smoothstep(1.5, 2.5, np.abs(seam_d - 2.5) + 2)
    weave = (np.sin(xs * 2.2) * np.sin(ys * 2.2)) * 0.5 + 0.5
    tone = n.fbm(4, 5)
    base = colour(rgb(0.78, 0.72, 0.60), 0.85 + 0.2 * tone)
    stains = smoothstep(0.6, 0.85, n.fbm(3, 4, cells_v=1)) * 0.3
    base *= (1 - stains - 0.12 * seam)[..., None]
    height = 0.5 + 0.08 * weave + 0.3 * seam - 0.1 * stitch
    rough = 0.7 + 0.15 * tone
    T.save_set(KIT, "Canvas", base, height, 2.0, rough, np.zeros_like(rough), 1 - 0.25 * seam)


def glass(size=512):
    n = Noise(44, size)
    smudge = smoothstep(0.5, 0.85, n.fbm(4, 5))
    dust = smoothstep(0.6, 0.75, blur(n.white(), 1.2)) * 0.5
    wav = n.fbm(2, 3, cells_v=6)
    base = colour(rgb(0.8, 0.82, 0.8), 1.0 - 0.2 * dust)
    rough = 0.04 + 0.25 * smudge + 0.3 * dust
    height = 0.5 + 0.3 * (wav - 0.5)
    T.save_set(KIT, "Glass", base, height, 6.0, rough, np.zeros_like(rough), np.ones_like(rough), alpha=smudge * 0.5 + dust)


# ==============================================================================================
# Decals (UV 0-1)
# ==============================================================================================

def gauge_face(size=512):
    """An enamel pressure dial: ticks 0-10 over 270 degrees, a red zone past 8; numerals as
    bars (no font needed). A: 1 the dial, 0 outside its brass bezel."""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) + 0.5
    cx = cy = size / 2
    r = np.hypot(xs - cx, ys - cy) / (size / 2)
    ang = np.degrees(np.arctan2(xs - cx, cy - ys))            # 0 up, clockwise
    sweep = (ang + 135.0) % 360.0                              # 0 at the start (lower left)
    base = np.ones((size, size, 3)) * rgb(0.93, 0.90, 0.80)
    major = (np.abs(((sweep / 27.0) + 0.5) % 1.0 - 0.5) * 27.0 < 0.9) & (r > 0.70) & (r < 0.84) & (sweep < 271)
    minor = (np.abs(((sweep / 5.4) + 0.5) % 1.0 - 0.5) * 5.4 < 0.45) & (r > 0.76) & (r < 0.84) & (sweep < 271)
    red = (sweep > 216) & (sweep < 271) & (r > 0.78) & (r < 0.86)
    arc = (np.abs(r - 0.845) < 0.008) & (sweep < 271)
    ink = major | minor | arc
    base[red] = rgb(0.75, 0.08, 0.05)
    base[ink] = rgb(0.06, 0.05, 0.05)
    # Numerals as short bars at each major tick.
    for k in range(11):
        a = math.radians(k * 27.0 - 135.0)
        px_, py_ = cx + math.sin(a) * size * 0.29, cy - math.cos(a) * size * 0.29
        d = np.hypot(xs - px_, ys - py_)
        base[d < size * 0.022] = rgb(0.1, 0.08, 0.07)
    # Maker's plate band.
    band = (np.abs(ys - size * 0.70) < size * 0.035) & (np.abs(xs - cx) < size * 0.16)
    base[band] = rgb(0.10, 0.12, 0.22)
    bezel = (r > 0.9) & (r <= 1.0)
    base[bezel] = rgb(0.85, 0.65, 0.35)
    base[r > 1.0] = rgb(0.2, 0.15, 0.1)
    vign = 1.0 - 0.18 * smoothstep(0.4, 0.9, r)
    base *= vign[..., None]
    T.write(KIT, "GaugeFace_BaseColor", base, True, wrap="clamp", alpha=(r < 0.9).astype(float))


def books(size=1024):
    """8 columns x 2 rows: row 0 (top) spines, row 1 covers. Each a leather colour with gilt
    bands and a title block."""
    n = Noise(51, size)
    img = np.zeros((size, size, 3))
    rng = np.random.default_rng(51)
    cols = [rgb(0.35, 0.06, 0.05), rgb(0.08, 0.18, 0.10), rgb(0.07, 0.10, 0.25), rgb(0.40, 0.25, 0.10),
            rgb(0.20, 0.10, 0.18), rgb(0.12, 0.12, 0.12), rgb(0.45, 0.35, 0.22), rgb(0.25, 0.05, 0.03)]
    cw, ch = size // 8, size // 2
    ys, xs = np.mgrid[0:ch, 0:cw].astype(np.float64) + 0.5
    gilt = rgb(0.85, 0.66, 0.30)
    for i, c in enumerate(cols):
        tone = 0.8 + 0.35 * n.fbm(8, 4)[0:ch, i * cw:(i + 1) * cw]
        spine = colour(c, 1.0) * tone[..., None]
        for b in rng.choice([0.08, 0.12, 0.2, 0.8, 0.88, 0.92], size=4, replace=False):
            spine[np.abs(ys / ch - b) < 0.006] = gilt
        block = (np.abs(ys / ch - 0.32) < 0.06) & (np.abs(xs / cw - 0.5) < 0.32)
        spine[block] = spine[block] * 0.5 + gilt * 0.15
        title = block & (np.abs(ys / ch - 0.32) < 0.012) & (np.abs(xs / cw - 0.5) < 0.22)
        spine[title] = gilt
        img[0:ch, i * cw:(i + 1) * cw] = spine
        cover = colour(c, 1.0) * tone[..., None] * 0.95
        frame = (np.abs(np.minimum(np.minimum(xs, cw - xs), np.minimum(ys, ch - ys)) - 6) < 1.2)
        cover[frame] = gilt
        img[ch:, i * cw:(i + 1) * cw] = cover
    T.write(KIT, "Books_BaseColor", img, True, wrap="clamp")


def leaf_panel(size=512):
    """A pressed fern on cream paper: a rachis up the middle and alternating pinnae. A: leaf."""
    n = Noise(52, size)
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float64) / size
    leaf = np.zeros((size, size))
    vein = np.zeros((size, size))
    # Rachis: a gentle S from bottom to top.
    def rx(y):
        return 0.5 + 0.04 * np.sin(y * 5.0)
    rach = np.abs(xs - rx(ys)) < (0.006 + 0.004 * (1 - ys))
    vein[rach & (ys > 0.08) & (ys < 0.94)] = 1
    for k in range(18):
        y0 = 0.12 + k * 0.045
        side = 1 if k % 2 == 0 else -1
        length = 0.32 * math.sin(math.pi * (0.15 + 0.75 * (1 - k / 18)))
        for t in np.linspace(0, 1, 30):
            px_ = rx(y0) + side * t * length
            py_ = y0 - t * 0.08 * length * 3
            w = 0.022 * (1 - t) + 0.006
            d = np.hypot(xs - px_, (ys - py_) * 1.3)
            leaf = np.maximum(leaf, smoothstep(w, w * 0.7, d))
            if t < 0.95:
                vein = np.maximum(vein, smoothstep(0.004, 0.002, d))
    leaf = np.maximum(leaf, vein * (ys > 0.08) * (ys < 0.94))
    paper = colour(rgb(0.86, 0.82, 0.70), 0.92 + 0.1 * n.fbm(8, 4))
    green = colour(rgb(0.20, 0.42, 0.10), 0.8 + 0.4 * n.fbm(16, 3))
    img = lerp(paper, green, leaf)
    img = lerp(img, colour(rgb(0.45, 0.55, 0.2), 1.0), vein * leaf * 0.5)
    T.write(KIT, "LeafPanel_BaseColor", img, True, wrap="clamp", alpha=leaf)


# ==============================================================================================
# Terrain layers
# ==============================================================================================

def terrain_grass(size=1024):
    n = Noise(61, size)
    blades = blur(n.white(), 0.7, 2.0)
    clumps = n.fbm(16, 4)
    dry = smoothstep(0.45, 0.75, n.fbm(3, 5))
    soil = smoothstep(0.62, 0.45, clumps) * 0.6
    green = colour(rgb(0.20, 0.30, 0.08), 0.7 + 0.6 * blades)
    straw = colour(rgb(0.45, 0.40, 0.20), 0.75 + 0.5 * blades)
    base = lerp(green, straw, dry * 0.7)
    base = lerp(base, colour(rgb(0.20, 0.15, 0.09), 1.0), soil * (1 - blades))
    flowers = (blur(n.white(), 0.6) > 0.72) & (n.fbm(6, 3) > 0.62)
    base[flowers] = rgb(0.85, 0.80, 0.45)
    height = 0.35 + 0.4 * clumps + 0.25 * blades
    rough = 0.85 + 0.1 * blades
    T.save_set(TERRAIN, "Grass", base, height, 6.0, rough, np.zeros_like(rough), 0.6 + 0.4 * blades, orm=False)


def terrain_dirt(size=1024):
    """Packed earth and gravel (the road)."""
    n = Noise(62, size)
    f1, f2, cid = n.voronoi(40)
    pebble = smoothstep(0.42, 0.18, f1 * (0.8 + 0.6 * cid)) * (cid > 0.72) * smoothstep(0.35, 0.6, n.fbm(5, 3))
    ruts = n.fbm(2, 4, cells_v=8)
    tone = n.fbm(8, 5)
    base = colour(rgb(0.36, 0.28, 0.19), 0.8 + 0.35 * tone)
    stones = colour(rgb(0.44, 0.41, 0.37), 0.6 + 0.5 * cid)
    base = lerp(base, stones, pebble)
    puddle = smoothstep(0.68, 0.8, ruts) * 0.6
    base *= (1 - 0.35 * puddle)[..., None]
    height = 0.4 + 0.25 * tone + 0.35 * pebble - 0.2 * puddle
    rough = 0.88 - 0.25 * pebble * 0.5 - 0.5 * puddle
    T.save_set(TERRAIN, "Dirt", base, height, 7.0, rough, np.zeros_like(rough), cavity_ao(height, 5, 3), orm=False)


def terrain_sand(size=512):
    n = Noise(63, size)
    wu, wv = n.warp(2, 0.2)
    ripple = 0.5 + 0.5 * np.sin((wv * 14 + n.fbm(3, 3)) * 2 * math.pi)
    grain = blur(n.white(), 0.5)
    shells = (grain > 0.75) & (n.fbm(8, 3) > 0.6)
    base = colour(rgb(0.74, 0.66, 0.50), 0.85 + 0.12 * ripple + 0.1 * grain)
    base[shells] = rgb(0.9, 0.86, 0.8)
    wet = smoothstep(0.55, 0.75, n.fbm(2, 4))
    base *= (1 - 0.3 * wet)[..., None]
    height = 0.3 + 0.5 * ripple + 0.2 * grain
    rough = 0.9 - 0.45 * wet
    T.save_set(TERRAIN, "Sand", base, height, 3.0, rough, np.zeros_like(rough), 0.85 + 0.15 * ripple, orm=False,
               normal_map=False)


# ==============================================================================================
# Second pass: weathering masks, detail normals, cliff strata, the sea, particle puffs
# ==============================================================================================

def grime(size=1024):
    """Grime_Mask (linear, 3 m a repeat on the kit's metre UVs), one mask a channel:
    R grime -- soot and dust collecting in broad clouds; G streaks -- rust and water stains
    running down V (down the walls: the kit's side faces map height to V); B chips --
    blotches that edge wear (vertex colour A) eats into; A fine variation for roughness."""
    n = Noise(71, size)
    clouds = n.fbm(3, 6, gain=0.55)
    clouds = smoothstep(0.35, 0.75, clouds) * (0.75 + 0.25 * n.fbm(24, 3))
    st = n.fbm(48, 3, cells_v=2) * 0.7 + n.fbm(160, 2, cells_v=4) * 0.3
    runs = smoothstep(0.45, 0.75, st) * smoothstep(0.15, 0.55, n.fbm(4, 3, cells_v=2))
    # Drips fade down from their sources (a sawtooth in V per column band).
    src = (n.v * 3 + n.value(40, 1) * 3) % 1.0
    runs *= 0.35 + 0.65 * (1.0 - src) ** 1.5
    chips = smoothstep(0.42, 0.62, n.fbm(20, 5, gain=0.6)) * (0.6 + 0.4 * blur(n.white(), 0.7))
    fine = n.fbm(64, 3)
    img = np.stack([clouds, runs, chips, fine], axis=-1)
    T.write(KIT, "Grime_Mask", img, False)
    print(f"  Grime_Mask: {size} x {size}")


def detail_normal(size=512):
    """Detail_Normal (linear normal, 0.5 m a repeat): fine pitting, hammer marks and hairline
    scratches, blended over each set's normal for close-up surface."""
    n = Noise(72, size)
    pits = smoothstep(0.75, 0.95, blur(n.white(), 0.9)) * 0.5
    f1, f2, _ = n.voronoi(18)
    hammer = smoothstep(0.0, 0.6, f1) * 0.25
    scratch = smoothstep(0.86, 0.98, n.value(size // 2, 3)) * 0.15 + smoothstep(0.88, 0.99, n.value(4, size // 2)) * 0.1
    height = 0.6 + 0.2 * n.fbm(32, 4) - pits + hammer - scratch
    T.write(KIT, "Detail_Normal", normal_from_height(height, 2.5), False, normal_map=True)
    print(f"  Detail_Normal: {size} x {size}")


def cliff_strata(size=1024):
    """6 m: layered sedimentary rock for the cliffs -- beds 15-80 cm thick in V, sandstone,
    grey shale and a rusty ironstone band, each weathering differently; vertical joints,
    spalled patches, water stains down from the joints, lichen on the hard beds."""
    n = Noise(73, size)
    rng = np.random.default_rng(73)
    wu, wv = n.warp(3, 0.05)
    bounds = [0.0]
    while bounds[-1] < 1.0:
        bounds.append(bounds[-1] + rng.uniform(0.025, 0.13))
    bounds[-1] = 1.0
    bounds = np.array(bounds)
    vv = (wv + 0.02 * np.sin(n.u * 2 * math.pi) + 0.035 * (n.fbm(3, 4) - 0.5)) % 1.0
    bed = np.clip(np.searchsorted(bounds, vv, side="right") - 1, 0, len(bounds) - 2)
    f = (vv - bounds[bed]) / (bounds[bed + 1] - bounds[bed])
    kinds = rng.choice(3, len(bounds), p=[0.55, 0.37, 0.08])
    kind = kinds[bed]
    pal = np.array([[0.53, 0.47, 0.39], [0.38, 0.365, 0.345], [0.44, 0.36, 0.29]])
    tone = rng.uniform(0.85, 1.12, len(bounds))[bed]
    base = pal[kind] * tone[..., None]
    lam = 0.5 + 0.5 * np.sin(vv * 2 * math.pi * 140 + n.fbm(8, 3) * 3)
    base *= (0.95 + 0.05 * lam)[..., None]
    base *= (0.85 + 0.3 * n.fbm(16, 4))[..., None]
    hard = (kind != 1).astype(float)
    # Soft shale weathers back from its top; hard beds keep a rounded nose.
    profile = np.where(hard > 0, 0.75 + 0.2 * np.sin(np.pi * f), 0.35 + 0.15 * f)
    # Joints: vertical cracks, offset bed by bed (each bed breaks on its own).
    shift = rng.uniform(0, 1, len(bounds))[bed]
    jointed = (rng.random(len(bounds)) > 0.35)[bed]
    f1, f2, cid = n.voronoi(4, 1, u=(wu + shift) % 1.0, v=wv)
    joint = smoothstep(0.05, 0.0, f2 - f1) * smoothstep(0.3, 0.6, n.fbm(5, 3)) * smoothstep(0.0, 0.08, f) * jointed
    base *= (0.8 + 0.4 * n.fbm(2, 3))[..., None]
    spall = smoothstep(0.62, 0.7, n.fbm(6, 4)) * hard
    height = profile - 0.4 * joint - 0.15 * spall + 0.08 * (n.fbm(24, 4) - 0.5)
    lip = (smoothstep(0.03, 0.0, f) + smoothstep(0.97, 1.0, f)) * (0.4 + 0.6 * hard)
    height -= 0.2 * lip
    stain = smoothstep(0.55, 0.8, n.fbm(40, 3, cells_v=2)) * smoothstep(0.2, 0.6, n.fbm(3, 3))
    base *= (1.0 - 0.35 * stain - 0.4 * joint - 0.3 * lip)[..., None]
    lichen = smoothstep(0.6, 0.72, n.fbm(12, 4)) * hard * (1 - joint)
    base = lerp(base, colour(rgb(0.55, 0.56, 0.42), 0.85 + 0.3 * n.fbm(64, 2)), lichen * 0.5)
    base = lerp(base, base * np.array([0.9, 0.85, 0.8]), spall[..., None] * 0.5)
    rough = 0.82 + 0.1 * (1 - hard) - 0.15 * stain
    ao = cavity_ao(height, 10, 3.0) * (1 - 0.4 * joint)
    T.save_set(KIT, "CliffStrata", base, height, 10.0, rough, np.zeros_like(rough), ao, orm_scale=2)


def sea_normal(size=1024):
    """Sea_Normal (linear normal): a tiling wave field -- a few dozen wave trains with whole
    numbers of crests across the tile (so it tiles), steeper toward the wind (+U), plus
    capillary ripples. The Sea material pans two copies of it."""
    rng = np.random.default_rng(74)
    n = Noise(74, size)
    h = np.zeros((size, size))
    for _ in range(48):
        k = rng.uniform(2, 34)
        ang = rng.normal(0.0, 0.7)
        kx, ky = int(round(k * math.cos(ang))), int(round(k * math.sin(ang)))
        if kx == 0 and ky == 0:
            continue
        mag = math.hypot(kx, ky)
        amp = mag ** -1.4
        ph = rng.uniform(0, 2 * math.pi)
        x = 2 * math.pi * (kx * n.u + ky * n.v) + ph
        h += amp * (np.sin(x) - 0.25 * np.cos(2 * x))
    h = (h - h.min()) / (h.max() - h.min())
    h += 0.06 * n.fbm(96, 3)
    T.write(KIT, "Sea_Normal", normal_from_height(h, 18.0), False, normal_map=True)
    print(f"  Sea_Normal: {size} x {size}")


def sea_foam(size=512):
    """Sea_Foam (sRGB, 4 m a repeat): bubbly foam -- the cell walls of two Voronoi fields
    and denser patches; A is the foam's coverage (the materials threshold it)."""
    n = Noise(75, size)
    f1, f2, _ = n.voronoi(24)
    walls = smoothstep(0.12, 0.0, f2 - f1)
    g1, g2, _ = n.voronoi(60)
    small = smoothstep(0.1, 0.0, g2 - g1)
    patches = n.fbm(4, 5)
    cover = np.clip(0.55 * walls + 0.35 * small + 0.9 * (patches - 0.35), 0, 1)
    base = colour(rgb(0.86, 0.9, 0.9), 0.9 + 0.1 * n.fbm(32, 2))
    T.write(KIT, "Sea_Foam", base, True, alpha=cover)
    print(f"  Sea_Foam: {size} x {size}")


def puff(size=256):
    """Puff (sRGB, UV 0-1, clamped): one soft billow for steam and dust sprites -- a round
    falloff broken up by fbm; A is its density."""
    n = Noise(76, size)
    d = np.hypot(n.u - 0.5, n.v - 0.5) * 2.0
    body = np.clip(1.0 - d, 0, 1) ** 1.3
    billow = n.fbm(4, 5, gain=0.55)
    dens = np.clip(body * (0.55 + 0.9 * (billow - 0.3)), 0, 1) * smoothstep(1.0, 0.75, d)
    base = colour(rgb(1, 1, 1), 0.85 + 0.15 * billow)
    T.write(KIT, "Puff", base, True, alpha=dens, wrap="clamp")
    print(f"  Puff: {size} x {size}")


SETS = {
    "Brass": brass, "Copper": copper, "Steel": steel, "RivetPanel": rivet_panel, "Grating": grating,
    "Walnut": walnut, "Planks": planks, "StoneBlocks": stone_blocks, "Rock": rock, "Concrete": concrete,
    "Marble": marble, "MarbleFloor": marble_floor, "Leather": leather, "Velvet": velvet, "Canvas": canvas,
    "Glass": glass, "GaugeFace": gauge_face, "Books": books, "LeafPanel": leaf_panel,
    "Grass": terrain_grass, "Dirt": terrain_dirt, "Sand": terrain_sand,
    "Grime": grime, "Detail": detail_normal, "CliffStrata": cliff_strata, "SeaNormal": sea_normal,
    "SeaFoam": sea_foam, "Puff": puff,
}


def main(names):
    for name in names or SETS:
        SETS[name]()
    total = sum(s for _, s in T.WRITTEN)
    print(f"textures: {len(T.WRITTEN)} files, {total / 1e6:.1f} MB")


if __name__ == "__main__":
    main(sys.argv[1:])
