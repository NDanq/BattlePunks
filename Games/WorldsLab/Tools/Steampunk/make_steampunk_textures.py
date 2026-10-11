#!/usr/bin/env python3
"""
make_steampunk_textures.py -- writes Content/Textures/Steampunk/: the PBR
texture sets of the Steampunk street corner (Content/Levels/Steampunk), all
procedural, all tileable, plus its sky.

Each surface is three maps, named <Surface>_<Map>.nmtexture:

    BaseColor   sRGB albedo
    Normal      linear, OpenGL convention (green up the image)
    ORM         linear: R ambient occlusion, G roughness, B metallic

    Cobble      granite setts in staggered rows, the roadway       1.92 m
    Flagstone   worn sandstone paving slabs, the pavements           3 m
    Curb        long granite curb blocks                              2 m
    Brick       sooty Victorian red brick, running bond             1.8 m
    Stone       rusticated limestone ashlar, ground floors and trim   2 m
    Brass       tarnished, brushed brass                             0.5 m
    Copper      copper going to verdigris                              1 m
    Iron        cast iron painted dark green, chipped to rust          1 m
    Plate       riveted iron plates, oily, rust-streaked               2 m
    Wood        weathered dark oak planks                              1 m
    Paint       chipped painted boards (tinted per entity)             1 m
    Slate       a roof of blue-grey slates                           1.5 m
    Glass       window glass grime (roughness and a little dirt)       1 m

and single textures:

    ClockFace_BaseColor   an enamel clock dial, Roman numerals (UV 0-1)
    Gauge_BaseColor       a pressure gauge dial (UV 0-1)
    WindowLit_Emissive    a lit window's curtains, glowing (UV 0-1 a pane)
    Sky_HDR               an equirectangular HDRI: a hazy late-afternoon
                          sky with a low sun (RGBE8, for the Sky Light)

Deterministic (seeded): running it again writes the same bytes. Needs numpy.

    python Games/WorldsLab/Tools/Steampunk/make_steampunk_textures.py [names...]
"""

import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nmformats as nm   # noqa: E402

GAME_DIR = Path(__file__).resolve().parents[2]
OUT = GAME_DIR / "Content" / "Textures" / "Steampunk"

# ==============================================================================================
# Tileable noise toolkit (every function wraps at the image's edges)
# ==============================================================================================


class Noise:
    def __init__(self, seed, size):
        self.rng = np.random.default_rng(seed)
        self.size = size
        ys, xs = np.mgrid[0:size, 0:size].astype(np.float64)
        self.u = (xs + 0.5) / size
        self.v = (ys + 0.5) / size

    def value(self, cells, u=None, v=None, cells_v=None):
        """Smooth value noise in 0-1 with cells x cells_v lattice cells across the tile."""
        cells_v = cells_v or cells
        u = self.u if u is None else u
        v = self.v if v is None else v
        lattice = self.rng.random((cells_v, cells))
        x, y = u * cells, v * cells_v
        xi, yi = np.floor(x).astype(int), np.floor(y).astype(int)
        fx, fy = x - xi, y - yi
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        x0, x1 = xi % cells, (xi + 1) % cells
        y0, y1 = yi % cells_v, (yi + 1) % cells_v
        top = lattice[y0, x0] * (1 - fx) + lattice[y0, x1] * fx
        bottom = lattice[y1, x0] * (1 - fx) + lattice[y1, x1] * fx
        return top * (1 - fy) + bottom * fy

    def fbm(self, cells, octaves=5, gain=0.5, cells_v=None, u=None, v=None):
        total, amp, norm = 0.0, 1.0, 0.0
        cv = cells_v or cells
        for _ in range(octaves):
            total = total + self.value(cells, u, v, cv) * amp
            norm += amp
            amp *= gain
            cells, cv = cells * 2, cv * 2
        return total / norm

    def ridged(self, cells, octaves=4):
        total, amp, norm = 0.0, 1.0, 0.0
        for _ in range(octaves):
            total = total + (1.0 - np.abs(self.value(cells) * 2 - 1)) * amp
            norm += amp
            amp *= 0.5
            cells *= 2
        return total / norm

    def white(self):
        return self.rng.random((self.size, self.size))


def blur(img, sigma_px):
    """Periodic Gaussian blur (FFT), sigma in pixels."""
    if sigma_px <= 0:
        return img
    h, w = img.shape[:2]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    kernel = np.exp(-2 * (math.pi ** 2) * (sigma_px ** 2) * (fx ** 2 + fy ** 2))
    if img.ndim == 2:
        return np.real(np.fft.ifft2(np.fft.fft2(img) * kernel))
    return np.stack([np.real(np.fft.ifft2(np.fft.fft2(img[..., c]) * kernel)) for c in range(img.shape[2])], axis=-1)


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def normal_from_height(height, strength_px):
    """height in 0-1 across the tile; strength: how many pixels of rise one unit
    of height is (larger is bumpier). OpenGL convention: +G is up the image."""
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5   # down the image
    nx = -dx * strength_px
    ny = dy * strength_px
    nz = np.ones_like(height)
    n = np.stack([nx, ny, nz], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def cavity_ao(height, radius_px, strength):
    """Occlusion from how far a texel sits below its surroundings."""
    around = blur(height, radius_px)
    ao = 1.0 - np.clip((around - height) * strength, 0.0, 1.0)
    return ao


def lerp(a, b, t):
    t = t[..., None] if np.ndim(t) == 2 and np.ndim(a) == 3 or (np.ndim(t) == 2 and np.ndim(b) == 3) else t
    return a + (b - a) * t


def rgb(r, g, b):
    return np.array([r, g, b], dtype=np.float64)


def colourise(base, variation):
    """base (3,) times per-texel brightness variation (H x W) -> H x W x 3."""
    return base[None, None, :] * variation[..., None]


def save_set(name, albedo, height, normal_strength, roughness, metallic, ao, extra_normal=None):
    albedo = np.clip(albedo, 0.0, 1.0)
    normal = normal_from_height(height, normal_strength)
    if extra_normal is not None:
        normal = extra_normal
    orm = np.stack([np.clip(ao, 0, 1), np.clip(roughness, 0.02, 1), np.clip(metallic, 0, 1)], axis=-1)
    nm.write_texture(OUT / f"{name}_BaseColor.nmtexture", albedo, srgb=True)
    nm.write_texture(OUT / f"{name}_Normal.nmtexture", normal, srgb=False, normal_map=True)
    nm.write_texture(OUT / f"{name}_ORM.nmtexture", orm, srgb=False)
    print(f"  {name}: {albedo.shape[1]} x {albedo.shape[0]}")


# ==============================================================================================
# Shared: rows of rounded blocks (setts, bricks, ashlar, flagstones, curbs)
# ==============================================================================================

def block_rows(size, rows, widths_fn, seed, gap_px, round_px, jitter_px=0.0, row_offsets=None):
    """Lays rows of rectangular blocks over the tile. Returns:
        inside   distance (px) from each texel into its block (0 in the gap)
        block_id a random number per block (0-1)
        local_u, local_v  0-1 across the block
    widths_fn(row, rng) -> list of widths (fractions summing to 1)."""
    rng = np.random.default_rng(seed)
    inside = np.zeros((size, size))
    block_id = np.zeros((size, size))
    local_u = np.zeros((size, size))
    local_v = np.zeros((size, size))
    row_h = size / rows
    xs = np.arange(size) + 0.5
    for r in range(rows):
        y0 = int(round(r * row_h))
        y1 = int(round((r + 1) * row_h))
        widths = widths_fn(r, rng)
        offset = (row_offsets[r] if row_offsets is not None else rng.random()) * size
        edges = np.concatenate([[0.0], np.cumsum(widths)]) * size + offset
        ys = np.arange(y0, y1) + 0.5
        dy = np.minimum(ys - y0, y1 - ys)[:, None]                       # px to row edge
        for b in range(len(widths)):
            a, e = edges[b], edges[b + 1]
            # wrap: test x and x + size
            xw = (xs - a) % size
            within = xw < (e - a)
            dx = np.minimum(xw, (e - a) - xw)
            jit = (rng.random() - 0.5) * jitter_px
            d = np.minimum(dx[None, :], dy) - gap_px * 0.5 + jit
            # rounded corners: inside both corner bands, the distance is to the arc
            cx = round_px - dx[None, :]
            cy = round_px - dy
            corner = np.where((cx > 0) & (cy > 0), round_px - np.sqrt(np.maximum(cx, 0) ** 2 + np.maximum(cy, 0) ** 2),
                              np.inf)
            d = np.minimum(d, corner - gap_px * 0.5 + jit)
            mask = np.broadcast_to(within[None, :], d.shape)
            region = inside[y0:y1]
            region[mask] = np.maximum(d[mask], 0.0)
            block_id[y0:y1][mask] = rng.random()
            lu = np.broadcast_to((xw / max(e - a, 1e-6))[None, :], d.shape)
            local_u[y0:y1][mask] = lu[mask]
            local_v[y0:y1][mask] = np.broadcast_to(((ys - y0) / (y1 - y0))[:, None], d.shape)[mask]
    return inside, block_id, local_u, local_v


def per_block(block_id, palette):
    """block_id 0-1 -> a colour from the palette (N x 3), chosen and mixed."""
    idx = (block_id * len(palette) * 7.31).astype(int) % len(palette)
    return palette[idx]


# ==============================================================================================
# Surfaces
# ==============================================================================================

def make_cobble(size=1024):
    n = Noise(101, size)
    rows = 20

    def widths(row, rng):
        ws = []
        total = 0.0
        while total < 0.999:
            w = rng.uniform(0.06, 0.115)
            if total + w > 1.0 - 0.05:
                w = 1.0 - total
            ws.append(w)
            total += w
        return ws

    inside, bid, lu, lv = block_rows(size, rows, widths, 11, gap_px=8, round_px=15, jitter_px=7)
    # Pillowed, worn-flat tops.
    dome = 1.0 - np.exp(-inside / 9.0)
    wobble = n.fbm(24, 4)
    height = dome * (0.75 + 0.25 * (bid * 0.6 + 0.4)) + (wobble - 0.5) * 0.12 * dome
    grit = n.fbm(64, 3)
    height = np.where(inside > 0, height, 0.05 * grit)
    pits = smoothstep(0.72, 0.85, n.fbm(96, 3)) * 0.08
    height = height - pits * dome
    stone = inside > 0

    palette = np.array([[0.42, 0.41, 0.40], [0.36, 0.35, 0.36], [0.47, 0.44, 0.41],
                        [0.33, 0.33, 0.35], [0.40, 0.37, 0.34], [0.29, 0.29, 0.30]])
    base = per_block(bid, palette)
    speck = n.white()
    speck_b = blur(speck, 0.7)
    flecks = (smoothstep(0.62, 0.75, speck_b) * 0.35 - smoothstep(0.3, 0.2, speck_b) * 0.25)
    tone = 0.85 + 0.3 * n.fbm(32, 4) + flecks
    albedo = base * tone[..., None]
    # Wheel-worn tops are a touch lighter and smoother; edges darker with grime.
    edge_grime = smoothstep(14.0, 2.0, inside)
    albedo *= (1.0 - 0.35 * edge_grime)[..., None]
    soil = rgb(0.10, 0.085, 0.07) * (0.7 + 0.6 * grit)[..., None]
    albedo = np.where(stone[..., None], albedo, soil)
    # Damp patches: darker, glossier, gathering in the low spots.
    damp = smoothstep(0.55, 0.7, n.fbm(5, 4)) * (1.0 - dome * 0.6)
    albedo *= (1.0 - 0.45 * damp)[..., None]
    rough = np.where(stone, 0.62 + 0.18 * n.fbm(48, 3) - 0.12 * dome, 0.95)
    rough = rough * (1.0 - 0.7 * damp)
    ao = cavity_ao(height, 10, 3.0) * np.where(stone, 1.0, 0.55)
    save_set("Cobble", albedo, height, 9.0, rough, np.zeros_like(rough), ao)


def make_flagstone(size=1024):
    n = Noise(202, size)
    rows = 5   # 60 cm

    def widths(row, rng):
        ws, total = [], 0.0
        while total < 0.999:
            w = rng.uniform(0.22, 0.34)
            if total + w > 1.0 - 0.14:
                w = 1.0 - total
            ws.append(w)
            total += w
        return ws

    inside, bid, lu, lv = block_rows(size, rows, widths, 22, gap_px=5, round_px=5, jitter_px=2)
    slab = inside > 0
    edge = 1.0 - np.exp(-inside / 6.0)
    undulate = n.fbm(6, 4)
    height = edge * (0.7 + 0.3 * undulate) + (bid - 0.5) * 0.15 * edge
    # Cracks: ridged noise thresholded.
    crack = smoothstep(0.93, 0.975, n.ridged(5, 4)) * slab
    height -= crack * 0.5
    palette = np.array([[0.55, 0.50, 0.43], [0.50, 0.47, 0.42], [0.58, 0.54, 0.47], [0.46, 0.43, 0.39]])
    base = per_block(bid, palette)
    tone = 0.8 + 0.35 * n.fbm(16, 5)
    stains = smoothstep(0.55, 0.75, n.fbm(4, 4))
    albedo = base * tone[..., None]
    albedo *= (1.0 - 0.35 * stains)[..., None]
    albedo *= (1.0 - 0.35 * smoothstep(8.0, 1.0, inside))[..., None]
    albedo *= (1.0 - 0.6 * crack)[..., None]
    grout = rgb(0.12, 0.11, 0.09) * (0.7 + 0.6 * n.fbm(64, 2))[..., None]
    albedo = np.where(slab[..., None], albedo, grout)
    rough = np.where(slab, 0.78 + 0.12 * n.fbm(40, 3) - 0.18 * stains, 0.95)
    ao = cavity_ao(height, 8, 3.0) * np.where(slab, 1.0, 0.6)
    save_set("Flagstone", albedo, height, 6.0, rough, np.zeros_like(rough), ao)


def make_curb(size=512):
    n = Noise(303, size)

    def widths(row, rng):
        return [0.5, 0.5]

    inside, bid, lu, lv = block_rows(size, 1, widths, 33, gap_px=4, round_px=3, row_offsets=[0.13])
    block = inside > 0
    edge = 1.0 - np.exp(-inside / 5.0)
    height = edge * (0.8 + 0.2 * n.fbm(8, 5))
    tone = 0.8 + 0.3 * n.fbm(24, 5)
    speck = blur(n.white(), 0.8)
    tone += smoothstep(0.6, 0.72, speck) * 0.25 - smoothstep(0.35, 0.25, speck) * 0.2
    albedo = rgb(0.43, 0.42, 0.42)[None, None, :] * tone[..., None]
    scuffs = smoothstep(0.6, 0.8, n.fbm(10, 3))
    albedo *= (1.0 - 0.3 * scuffs)[..., None]
    albedo = np.where(block[..., None], albedo, rgb(0.1, 0.09, 0.08))
    rough = np.where(block, 0.7 + 0.15 * n.fbm(30, 3), 0.95)
    ao = cavity_ao(height, 6, 3.0)
    save_set("Curb", albedo, height, 5.0, rough, np.zeros_like(rough), ao)


def make_brick(size=1024):
    n = Noise(404, size)
    courses = 24          # 7.5 cm each over 1.8 m
    per_row = 8           # 22.5 cm each

    def widths(row, rng):
        return [1.0 / per_row] * per_row

    offsets = [((r % 2) * 0.5 / per_row) + 0.013 for r in range(courses)]
    inside, bid, lu, lv = block_rows(size, courses, widths, 44, gap_px=6, round_px=3, jitter_px=1.5,
                                     row_offsets=offsets)
    brick = inside > 0
    face = 1.0 - np.exp(-inside / 3.0)
    chips = smoothstep(0.7, 0.82, n.fbm(40, 4))
    height = face * (0.85 + 0.15 * n.fbm(64, 3)) - chips * 0.35 * face
    palette = np.array([[0.42, 0.17, 0.11], [0.48, 0.21, 0.13], [0.36, 0.14, 0.10], [0.52, 0.26, 0.16],
                        [0.30, 0.12, 0.09], [0.45, 0.24, 0.17], [0.25, 0.11, 0.09]])
    base = per_block(bid, palette)
    tone = 0.82 + 0.3 * n.fbm(48, 4)
    albedo = base * tone[..., None]
    # Fired surface blotches and dark burnt headers.
    blot = smoothstep(0.55, 0.8, n.fbm(20, 3))
    albedo = albedo * (1.0 - 0.25 * blot)[..., None]
    burnt = (bid > 0.86)
    albedo = np.where(burnt[..., None], albedo * 0.55, albedo)
    # Soot and efflorescence, low frequency, streaking down (stretched in v).
    soot = smoothstep(0.45, 0.75, n.fbm(3, 5, cells_v=1))
    albedo *= (1.0 - 0.45 * soot)[..., None]
    salts = smoothstep(0.7, 0.85, n.fbm(6, 4)) * 0.25
    albedo = albedo * (1 - salts[..., None]) + rgb(0.55, 0.53, 0.5) * salts[..., None]
    mortar = rgb(0.38, 0.36, 0.33) * (0.75 + 0.4 * n.fbm(96, 3))[..., None] * (1.0 - 0.4 * soot)[..., None]
    albedo = np.where(brick[..., None], albedo, mortar)
    rough = np.where(brick, 0.82 + 0.1 * n.fbm(64, 2), 0.95)
    ao = cavity_ao(height, 6, 2.5) * np.where(brick, 1.0, 0.7)
    save_set("Brick", albedo, height, 6.0, rough, np.zeros_like(rough), ao)


def make_stone(size=1024):
    n = Noise(505, size)
    rows = 5   # 40 cm courses over 2 m

    def widths(row, rng):
        return [0.25, 0.25, 0.25, 0.25]

    offsets = [(r % 2) * 0.125 for r in range(rows)]
    inside, bid, lu, lv = block_rows(size, rows, widths, 55, gap_px=10, round_px=4, row_offsets=offsets)
    block = inside > 0
    # Rusticated: a chamfered margin, a rough-tooled face.
    chamfer = np.clip(inside / 14.0, 0, 1)
    tooled = n.fbm(80, 4)
    height = chamfer * 0.75 + (tooled - 0.5) * 0.12 * chamfer
    palette = np.array([[0.62, 0.58, 0.50], [0.58, 0.55, 0.48], [0.66, 0.61, 0.52], [0.55, 0.51, 0.45]])
    base = per_block(bid, palette)
    tone = 0.85 + 0.25 * n.fbm(12, 5)
    albedo = base * tone[..., None]
    streaks = smoothstep(0.5, 0.85, n.fbm(16, 4, cells_v=1))
    albedo *= (1.0 - 0.4 * streaks)[..., None]
    albedo *= (1.0 - 0.3 * smoothstep(10.0, 1.0, inside))[..., None]
    albedo = np.where(block[..., None], albedo, rgb(0.3, 0.28, 0.25))
    rough = np.where(block, 0.85 + 0.08 * tooled, 0.95)
    ao = cavity_ao(height, 10, 2.5) * np.where(block, 1.0, 0.65)
    save_set("Stone", albedo, height, 7.0, rough, np.zeros_like(rough), ao)


def make_brass(size=512):
    n = Noise(606, size)
    brushed = n.fbm(2, 6, cells_v=64)                       # fine lines along u
    tarnish = smoothstep(0.35, 0.8, n.fbm(6, 6)) * 0.7
    grime = smoothstep(0.66, 0.8, n.fbm(16, 5)) * 0.35
    scratches = smoothstep(0.75, 0.9, n.fbm(1, 3, cells_v=200))
    polished = rgb(0.90, 0.72, 0.40)
    dull = rgb(0.62, 0.47, 0.25)
    albedo = lerp(np.broadcast_to(polished, (size, size, 3)), np.broadcast_to(dull, (size, size, 3)), tarnish)
    albedo = albedo * (0.94 + 0.08 * brushed + 0.05 * scratches)[..., None]
    dirt = rgb(0.10, 0.08, 0.05)
    albedo = albedo * (1 - grime[..., None]) + dirt * grime[..., None]
    metal = 1.0 - grime
    rough = 0.25 + 0.22 * tarnish + 0.08 * brushed + 0.4 * grime - 0.08 * scratches
    height = brushed * 0.3 + scratches * 0.1
    ao = 1.0 - 0.2 * grime
    save_set("Brass", albedo, height, 1.5, rough, metal, ao)


def make_copper(size=512):
    n = Noise(707, size)
    patina = smoothstep(0.55, 0.72, n.fbm(4, 7))
    drips = smoothstep(0.62, 0.8, n.fbm(8, 5, cells_v=1))
    patina = np.clip(patina + drips * 0.5, 0, 1) * (0.75 + 0.25 * n.fbm(64, 3))
    oxide = smoothstep(0.35, 0.75, n.fbm(8, 6))
    copper = rgb(0.84, 0.48, 0.32) * (0.88 + 0.16 * n.fbm(48, 4))[..., None]
    brown = rgb(0.42, 0.22, 0.14)
    metal_col = copper * (1 - oxide[..., None] * 0.7) + brown * oxide[..., None] * 0.7
    verd = rgb(0.33, 0.58, 0.48) * (0.8 + 0.3 * n.fbm(40, 5))[..., None]
    albedo = metal_col * (1 - patina[..., None]) + verd * patina[..., None]
    metal = (1.0 - patina) * (1.0 - oxide * 0.4)
    rough = 0.3 + 0.3 * oxide + 0.5 * patina
    height = patina * 0.4 + n.fbm(60, 3) * 0.2
    ao = 1.0 - 0.15 * patina
    save_set("Copper", albedo, height, 3.0, rough, metal, ao)


def make_iron(size=512):
    n = Noise(808, size)
    chips = smoothstep(0.66, 0.7, n.fbm(8, 6))
    rust_spread = smoothstep(0.55, 0.7, n.fbm(8, 6)) - chips
    paint = rgb(0.06, 0.085, 0.07) * (0.85 + 0.25 * n.fbm(30, 4))[..., None]
    rust = rgb(0.32, 0.14, 0.06) * (0.7 + 0.6 * n.fbm(50, 4))[..., None]
    bare = rgb(0.35, 0.34, 0.33)
    albedo = paint * (1 - rust_spread[..., None] * 0.6) + rust * rust_spread[..., None] * 0.6
    albedo = albedo * (1 - chips[..., None]) + (rust * 0.7 + bare * 0.3) * chips[..., None]
    metal = chips * 0.35
    rough = 0.42 + 0.15 * n.fbm(40, 3) + 0.4 * chips + 0.25 * rust_spread
    height = (1.0 - chips) * 0.5 + n.fbm(90, 3) * 0.1
    ao = 1.0 - chips * 0.3
    save_set("Iron", albedo, height, 2.5, rough, metal, ao)


def make_plate(size=1024):
    n = Noise(909, size)
    # Plates 1 m x 0.5 m: 2 across, 4 down; rivets every 10 cm round each.
    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    pw, ph = size / 2, size / 4
    fx = (xs % pw)
    fy = (ys % ph)
    seam_d = np.minimum(np.minimum(fx, pw - fx), np.minimum(fy, ph - fy))
    plate_id = (np.floor(xs / pw) + 2 * np.floor(ys / ph))
    lap = smoothstep(0.0, 4.0, seam_d)
    height = lap * (0.55 + 0.04 * (plate_id % 3))
    rivet = np.zeros((size, size))
    spacing = size / 20.0
    inset = 14.0
    rad = 6.5
    # Rivets along each plate's border, inset from the seam.
    for along_x in (True, False):
        pos_along = (xs if along_x else ys) % spacing - spacing / 2
        if along_x:
            dist_edge = np.minimum(np.abs(fy - inset), np.abs(fy - (ph - inset)))
        else:
            dist_edge = np.minimum(np.abs(fx - inset), np.abs(fx - (pw - inset)))
        d = np.sqrt(pos_along ** 2 + dist_edge ** 2)
        rivet = np.maximum(rivet, np.sqrt(np.clip(1 - (d / rad) ** 2, 0, 1)))
    height = height + rivet * 0.35
    base = rgb(0.17, 0.165, 0.16) * (0.8 + 0.3 * n.fbm(10, 5))[..., None]
    base = base * (0.9 + 0.12 * ((plate_id * 0.37) % 1.0))[..., None]
    rust_seed = blur(rivet, 3.0) + smoothstep(6.0, 0.0, seam_d) * 0.6
    # Rust running down from rivets and seams: smear downward (shift and accumulate).
    streak = np.zeros_like(rust_seed)
    acc = rust_seed.copy()
    for k in range(1, 60, 3):
        streak = np.maximum(streak, np.roll(acc, k, axis=0) * (1 - k / 60.0))
    streak = streak * (0.5 + n.fbm(40, 3, cells_v=4))
    rust_amt = np.clip(streak * 0.8, 0, 1) * smoothstep(0.3, 0.6, n.fbm(6, 4))
    rust = rgb(0.30, 0.13, 0.06) * (0.8 + 0.4 * n.fbm(60, 3))[..., None]
    oily = smoothstep(0.55, 0.75, n.fbm(5, 4))
    albedo = base * (1 - rust_amt[..., None]) + rust * rust_amt[..., None]
    albedo *= (1.0 - 0.3 * oily)[..., None]
    metal = 0.75 * (1 - rust_amt)
    rough = 0.5 + 0.3 * rust_amt - 0.25 * oily + 0.1 * n.fbm(80, 3) - 0.15 * rivet
    ao = cavity_ao(height, 5, 3.0)
    save_set("Plate", albedo, height, 5.0, rough, metal, ao)


def wood_fields(n, size, planks):
    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    pw = size / planks
    fx = xs % pw
    plank = np.floor(xs / pw)
    gap = np.minimum(fx, pw - fx)
    rng = np.random.default_rng(1234)
    offsets = rng.random(planks)
    plank_off = offsets[plank.astype(int) % planks]
    # Grain: noise stretched down the plank, warped into rings.
    warp = n.fbm(4, 4, cells_v=1)
    grain_coord = (xs / size * planks * 6.0 + warp * 3.0 + plank_off * 10.0)
    rings = 0.5 + 0.5 * np.sin(grain_coord * 2.0 * math.pi)
    fine = n.fbm(64, 3, cells_v=4)
    # Butt joints: planks end at random heights.
    joint_y = (plank_off * size)
    dj = np.abs(((ys - joint_y) + size / 2) % size - size / 2)
    return gap, rings, fine, plank_off, dj


def make_wood(size=512):
    n = Noise(1010, size)
    gap, rings, fine, poff, dj = wood_fields(n, size, 8)
    board = (gap > 2.5) & (dj > 1.5)
    edge = smoothstep(2.5, 6.0, np.minimum(gap, dj + 2))
    height = edge * (0.6 + 0.15 * rings + 0.1 * fine)
    base = rgb(0.22, 0.14, 0.08) * (0.75 + 0.3 * poff)[..., None]
    albedo = base * (0.75 + 0.35 * rings * fine + 0.15 * fine)[..., None]
    weather = smoothstep(0.5, 0.8, n.fbm(6, 4, cells_v=2))
    albedo = albedo * (1 - weather[..., None] * 0.5) + rgb(0.3, 0.28, 0.25) * weather[..., None] * 0.5
    albedo = np.where(board[..., None], albedo, rgb(0.03, 0.025, 0.02))
    rough = 0.7 + 0.15 * fine + 0.1 * weather
    ao = cavity_ao(height, 4, 3.0) * np.where(board, 1.0, 0.4)
    save_set("Wood", albedo, height, 4.0, rough, np.zeros_like(rough), ao)


def make_paint(size=512):
    """Painted panelling, near white so a Tint parameter colours it: chips show
    the dark wood under it."""
    n = Noise(1111, size)
    gap, rings, fine, poff, dj = wood_fields(n, size, 6)
    board = gap > 2.0
    edge = smoothstep(2.0, 5.0, gap)
    chips = smoothstep(0.7, 0.74, n.fbm(10, 6)) * board
    height = edge * 0.6 + rings * 0.05 * fine - chips * 0.15
    paint = rgb(0.80, 0.80, 0.78) * (0.88 + 0.15 * n.fbm(20, 4))[..., None]
    dirt = smoothstep(0.5, 0.85, n.fbm(5, 4, cells_v=2))
    paint *= (1.0 - 0.35 * dirt)[..., None]
    wood = rgb(0.16, 0.10, 0.06) * (0.7 + 0.4 * rings)[..., None]
    albedo = paint * (1 - chips[..., None]) + wood * chips[..., None]
    albedo = np.where(board[..., None], albedo, rgb(0.05, 0.05, 0.05))
    rough = 0.45 + 0.15 * dirt + 0.3 * chips
    ao = cavity_ao(height, 4, 3.0) * np.where(board, 1.0, 0.5)
    save_set("Paint", albedo, height, 4.0, rough, np.zeros_like(rough), ao)


def make_slate(size=512):
    n = Noise(1212, size)
    rows = 6

    def widths(row, rng):
        return [1 / 6.0] * 6

    offsets = [(r % 2) * (0.5 / 6) + 0.01 for r in range(rows)]
    inside, bid, lu, lv = block_rows(size, rows, widths, 66, gap_px=3, round_px=2, jitter_px=2, row_offsets=offsets)
    slate = inside > 0
    # Each slate thickens toward its bottom edge (overlap).
    height = (0.3 + 0.6 * lv) * smoothstep(0.0, 3.0, inside) + n.fbm(48, 4) * 0.08
    palette = np.array([[0.20, 0.22, 0.25], [0.17, 0.18, 0.21], [0.23, 0.24, 0.26], [0.19, 0.19, 0.20]])
    albedo = per_block(bid, palette) * (0.85 + 0.3 * n.fbm(24, 4))[..., None]
    lichen = smoothstep(0.72, 0.85, n.fbm(8, 5))
    albedo = albedo * (1 - lichen[..., None] * 0.5) + rgb(0.35, 0.36, 0.25) * lichen[..., None] * 0.5
    albedo = np.where(slate[..., None], albedo, rgb(0.04, 0.04, 0.045))
    rough = 0.55 + 0.2 * n.fbm(40, 3) + 0.2 * lichen
    ao = cavity_ao(height, 5, 3.0) * np.where(slate, 1.0, 0.4)
    save_set("Slate", albedo, height, 5.0, rough, np.zeros_like(rough), ao)


def make_glass(size=512):
    n = Noise(1313, size)
    grime = smoothstep(0.45, 0.9, n.fbm(4, 5))
    streak = smoothstep(0.55, 0.85, n.fbm(24, 4, cells_v=2))
    dirt = np.clip(grime * 0.7 + streak * 0.3, 0, 1)
    albedo = rgb(0.03, 0.035, 0.035) * (1 - dirt[..., None]) + rgb(0.25, 0.23, 0.2) * dirt[..., None] * 0.6
    rough = 0.04 + 0.5 * dirt
    wavy = n.fbm(3, 3) * 0.6    # old crown glass ripples
    ao = np.ones((size, size))
    save_set("Glass", albedo, wavy, 2.0, rough, np.zeros_like(rough), ao)


# ==============================================================================================
# Dials, windows, sky
# ==============================================================================================

def stroke_mask(size, segments, width_px):
    """Anti-aliased union of line segments (in pixel coords)."""
    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    mask = np.zeros((size, size))
    for (x0, y0), (x1, y1) in segments:
        dx, dy = x1 - x0, y1 - y0
        length2 = dx * dx + dy * dy
        lo_x, hi_x = int(max(min(x0, x1) - width_px - 2, 0)), int(min(max(x0, x1) + width_px + 2, size))
        lo_y, hi_y = int(max(min(y0, y1) - width_px - 2, 0)), int(min(max(y0, y1) + width_px + 2, size))
        sx, sy = xs[lo_y:hi_y, lo_x:hi_x], ys[lo_y:hi_y, lo_x:hi_x]
        t = np.clip(((sx - x0) * dx + (sy - y0) * dy) / max(length2, 1e-9), 0, 1)
        d = np.hypot(sx - (x0 + t * dx), sy - (y0 + t * dy))
        mask[lo_y:hi_y, lo_x:hi_x] = np.maximum(mask[lo_y:hi_y, lo_x:hi_x], np.clip(width_px * 0.5 - d + 0.5, 0, 1))
    return mask


ROMAN = ["XII", "I", "II", "III", "IIII", "V", "VI", "VII", "VIII", "IX", "X", "XI"]


def glyph_segments(letter, x, y, w, h):
    """A serif-less Roman letter in the box (x, y) top-left, w x h."""
    if letter == "I":
        return [((x + w / 2, y), (x + w / 2, y + h)), ((x + w * 0.1, y), (x + w * 0.9, y)),
                ((x + w * 0.1, y + h), (x + w * 0.9, y + h))]
    if letter == "V":
        return [((x, y), (x + w / 2, y + h)), ((x + w, y), (x + w / 2, y + h))]
    if letter == "X":
        return [((x, y), (x + w, y + h)), ((x + w, y), (x, y + h))]
    return []


def make_dial(name, size, numerals, ticks, face, ink, rim, needle=None, numeral_radius=0.31, red_zone=None):
    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    cx = cy = size / 2
    r = np.hypot(xs - cx, ys - cy) / (size / 2)
    n = Noise(1414 + len(name), size)
    albedo = np.broadcast_to(face, (size, size, 3)) * (0.9 + 0.12 * n.fbm(8, 4))[..., None]
    # Age: yellowed toward the rim, crazing.
    albedo = albedo * (1 - 0.18 * smoothstep(0.5, 0.95, r))[..., None]
    crazing = smoothstep(0.94, 0.985, n.ridged(10, 3)) * 0.15
    albedo = albedo * (1 - crazing[..., None])
    segments = []
    for k in range(ticks):
        a = 2 * math.pi * k / ticks
        major = k % (ticks // 12) == 0 if ticks >= 12 else True
        r0 = 0.80 if major else 0.85
        r1 = 0.90
        segments.append(((cx + math.sin(a) * r0 * size / 2, cy - math.cos(a) * r0 * size / 2),
                         (cx + math.sin(a) * r1 * size / 2, cy - math.cos(a) * r1 * size / 2)))
    ink_mask = stroke_mask(size, segments, size * 0.006)
    # Thick hour ticks.
    thick = []
    for k in range(12):
        a = 2 * math.pi * k / 12
        thick.append(((cx + math.sin(a) * 0.78 * size / 2, cy - math.cos(a) * 0.78 * size / 2),
                      (cx + math.sin(a) * 0.90 * size / 2, cy - math.cos(a) * 0.90 * size / 2)))
    ink_mask = np.maximum(ink_mask, stroke_mask(size, thick, size * 0.014))
    if numerals:
        glyphs = []
        gh = size * 0.075
        gw = gh * 0.55
        for k, text in enumerate(numerals):
            a = 2 * math.pi * k / len(numerals)
            px = cx + math.sin(a) * numeral_radius * 2 * size / 2
            py = cy - math.cos(a) * numeral_radius * 2 * size / 2
            total_w = len(text) * gw + (len(text) - 1) * gw * 0.25
            x = px - total_w / 2
            for letter in text:
                glyphs += glyph_segments(letter, x, py - gh / 2, gw, gh)
                x += gw * 1.25
        ink_mask = np.maximum(ink_mask, stroke_mask(size, glyphs, size * 0.011))
    if red_zone:
        a = (np.arctan2(xs - cx, -(ys - cy)) % (2 * math.pi)) / (2 * math.pi)
        zone = (a > red_zone[0]) & (a < red_zone[1]) & (r > 0.80) & (r < 0.9)
        albedo = np.where(zone[..., None], rgb(0.55, 0.06, 0.04), albedo)
    albedo = albedo * (1 - ink_mask[..., None]) + ink * ink_mask[..., None]
    ring = (r > 0.93)
    albedo = np.where(ring[..., None], rim * (0.8 + 0.3 * n.fbm(30, 3))[..., None], albedo)
    albedo = np.where((r > 1.0)[..., None], rim * 0.5, albedo)
    if needle is not None:
        a = needle
        seg = [((cx - math.sin(a) * 0.12 * size / 2, cy + math.cos(a) * 0.12 * size / 2),
                (cx + math.sin(a) * 0.72 * size / 2, cy - math.cos(a) * 0.72 * size / 2))]
        nmask = stroke_mask(size, seg, size * 0.018)
        hub = smoothstep(0.07, 0.05, r)
        nmask = np.maximum(nmask, hub)
        albedo = albedo * (1 - nmask[..., None]) + rgb(0.05, 0.04, 0.03) * nmask[..., None]
    nm.write_texture(OUT / f"{name}_BaseColor.nmtexture", np.clip(albedo, 0, 1), srgb=True, wrap="clamp")
    print(f"  {name}: {size} x {size}")


def make_window_lit(size=256):
    """A lit pane from the street: a warm room behind glass -- lamplight pooled
    high, the room's dark furniture low, a soft net curtain over it all."""
    n = Noise(1515, size)
    ys, xs = np.mgrid[0:size, 0:size] + 0.5
    u, v = xs / size, ys / size
    folds = 0.82 + 0.18 * np.sin(u * 2 * math.pi * 22 + n.fbm(3, 3) * 5.0) ** 2
    lamp = np.exp(-((u - 0.62) ** 2 * 5 + (v - 0.35) ** 2 * 4))
    room = 0.25 + 0.75 * lamp
    furniture = smoothstep(0.55, 0.75, v) * smoothstep(0.35, 0.65, n.fbm(3, 4)) * 0.7
    glow = room * (1 - furniture) * folds * (0.85 + 0.25 * n.fbm(6, 4))
    warm = rgb(1.0, 0.66, 0.34)
    deep = rgb(0.55, 0.28, 0.12)
    img = (deep * (1 - lamp[..., None]) + warm * lamp[..., None]) * glow[..., None]
    nm.write_texture(OUT / "WindowLit_Emissive.nmtexture", np.clip(img, 0, 1), srgb=True, wrap="clamp")
    print(f"  WindowLit: {size} x {size}")


def make_sky(width=2048, height=1024):
    """Equirectangular, row 0 straight up; azimuth 0 at the image's centre column
    looking +Z. A hazy late-afternoon sky: warm near the sun and the horizon,
    blue-grey overhead, scattered cumulus, a smoky band low on the horizon."""
    n = Noise(1616, 1)
    ys, xs = np.mgrid[0:height, 0:width] + 0.5
    lon = (xs / width - 0.5) * 2 * math.pi            # -pi..pi
    lat = (0.5 - ys / height) * math.pi                # +pi/2 up
    dx = np.cos(lat) * np.sin(lon)
    dy = np.sin(lat)
    dz = np.cos(lat) * np.cos(lon)
    sun_el, sun_az = math.radians(SUN_ELEVATION), math.radians(SUN_AZIMUTH)
    sx, sy, sz = math.cos(sun_el) * math.sin(sun_az), math.sin(sun_el), math.cos(sun_el) * math.cos(sun_az)
    cos_sun = np.clip(dx * sx + dy * sy + dz * sz, -1, 1)
    up = np.clip(dy, 0, 1)
    zenith = np.array([0.20, 0.32, 0.55])
    horizon = np.array([0.95, 0.80, 0.62])
    t = np.power(1 - up, 3.0)[..., None]
    sky = zenith * (1 - t) + horizon * t
    # Mie glow round the sun, and the sun itself.
    glow = np.power(np.clip((cos_sun + 1) / 2, 0, 1), 12)[..., None] * np.array([1.6, 1.0, 0.45])
    halo = np.power(np.clip(cos_sun, 0, 1), 200)[..., None] * np.array([6.0, 4.2, 2.2])
    sky = sky * 1.0 + glow * 1.2 + halo
    # Clouds: fbm on the sky dome (projected onto a plane), lit warm on the sun's side.
    noise_size = 512
    cn = Noise(1717, noise_size)
    plane_u = (dx / np.maximum(dy, 0.06)) * 0.18
    plane_v = (dz / np.maximum(dy, 0.06)) * 0.18
    cu, cv = plane_u % 1.0, plane_v % 1.0
    cloud = cn.fbm(6, 5, u=cu, v=cv)
    cover = smoothstep(0.52, 0.72, cloud) * smoothstep(0.0, 0.25, dy)
    cloud_lit = np.array([1.25, 1.0, 0.82]) * (0.6 + 0.9 * np.power(np.clip((cos_sun + 1) / 2, 0, 1), 3))[..., None]
    sky = sky * (1 - cover[..., None] * 0.85) + cloud_lit * cover[..., None] * 0.85
    # Industrial haze: a brown band low on the horizon.
    haze = np.exp(-np.abs(dy) * 9.0)[..., None]
    sky = sky * (1 - haze * 0.35) + np.array([0.62, 0.50, 0.40]) * haze * 0.35
    # The sun disc, hot.
    disc = (cos_sun > math.cos(math.radians(0.6)))[..., None]
    sky = np.where(disc, np.array([60.0, 48.0, 32.0]), sky)
    # Below the horizon: dim warm ground.
    ground = np.array([0.18, 0.15, 0.12]) * (0.6 + 0.4 * np.exp(dy * 6))[..., None]
    below = (dy < 0)[..., None]
    sky = np.where(below, ground, sky)
    sky *= SKY_SCALE
    nm.write_hdri(OUT / "Sky_HDR.nmtexture", sky.astype(np.float64))
    print(f"  Sky: {width} x {height} HDR")


# The sun: low in the west-south-west, warm. The level's Sun entity matches it
# (make_steampunk_street.py imports these).
SUN_ELEVATION = 24.0     # degrees above the horizon
SUN_AZIMUTH = -118.0     # degrees from +Z, toward +X positive
SKY_SCALE = 1.0

MAKERS = {
    "Cobble": make_cobble, "Flagstone": make_flagstone, "Curb": make_curb, "Brick": make_brick,
    "Stone": make_stone, "Brass": make_brass, "Copper": make_copper, "Iron": make_iron, "Plate": make_plate,
    "Wood": make_wood, "Paint": make_paint, "Slate": make_slate, "Glass": make_glass,
    "ClockFace": lambda: make_dial("ClockFace", 1024, ROMAN, 60, rgb(0.86, 0.82, 0.70), rgb(0.04, 0.035, 0.03),
                                   rgb(0.72, 0.55, 0.25)),
    "Gauge": lambda: make_dial("Gauge", 256, None, 40, rgb(0.88, 0.85, 0.76), rgb(0.05, 0.04, 0.03),
                               rgb(0.70, 0.52, 0.24), needle=math.radians(130), red_zone=(0.30, 0.42)),
    "WindowLit": make_window_lit,
    "Sky": make_sky,
}


def main():
    names = sys.argv[1:] or list(MAKERS)
    print(f"Writing {OUT}")
    for name in names:
        MAKERS[name]()


if __name__ == "__main__":
    main()
