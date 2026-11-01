"""
texlib.py -- the tileable noise toolkit and texture-set writer behind make_textures.py.

Every function wraps at the image's edges (so every set tiles) and is seeded, so a run
writes the same bytes. A set is written twice: as .nmtexture files the engine reads
(Content/Textures/...) and as PNGs in Saved/ArtCache/Textures, which the Blender scripts
use to preview the same surfaces in their contact sheets.

Maps (as WorldsLab's Steampunk sets):
    BaseColor   sRGB albedo; A is the set's height (0-1), or a mask a set documents
    Normal      linear, OpenGL convention (green up the image)
    ORM         linear: R ambient occlusion, G roughness, B metallic
"""

import math
from pathlib import Path

import numpy as np

import nmformats as nm

ART_DIR = Path(__file__).resolve().parents[1]
GAME_DIR = ART_DIR.parents[1]
CACHE = GAME_DIR / "Saved" / "ArtCache" / "Textures"


class Noise:
    def __init__(self, seed, size, size_v=None):
        self.rng = np.random.default_rng(seed)
        self.w = size
        self.h = size_v or size
        ys, xs = np.mgrid[0:self.h, 0:self.w].astype(np.float64)
        self.u = (xs + 0.5) / self.w
        self.v = (ys + 0.5) / self.h

    def value(self, cells, cells_v=None, u=None, v=None):
        """Smooth value noise in 0-1, cells x cells_v lattice cells across the tile."""
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
            total = total + self.value(cells, cv, u, v) * amp
            norm += amp
            amp *= gain
            cells, cv = cells * 2, cv * 2
        return total / norm

    def ridged(self, cells, octaves=4, cells_v=None):
        total, amp, norm = 0.0, 1.0, 0.0
        cv = cells_v or cells
        for _ in range(octaves):
            total = total + (1.0 - np.abs(self.value(cells, cv) * 2 - 1)) * amp
            norm += amp
            amp *= 0.5
            cells, cv = cells * 2, cv * 2
        return total / norm

    def white(self):
        return self.rng.random((self.h, self.w))

    def warp(self, cells, amount, octaves=3):
        """u, v pushed about by fbm: for marble, wood and rock."""
        du = (self.fbm(cells, octaves) - 0.5) * amount
        dv = (self.fbm(cells, octaves) - 0.5) * amount
        return (self.u + du) % 1.0, (self.v + dv) % 1.0

    def voronoi(self, cells, cells_v=None, jitter=1.0, u=None, v=None):
        """Cellular noise: F1, F2 (in cell units) and each texel's cell id (0-1)."""
        cells_v = cells_v or cells
        u = self.u if u is None else u
        v = self.v if v is None else v
        pts = self.rng.random((cells_v, cells, 2)) * jitter + (1 - jitter) * 0.5
        ids = self.rng.random((cells_v, cells))
        x, y = u * cells, v * cells_v
        xi, yi = np.floor(x).astype(int), np.floor(y).astype(int)
        f1 = np.full(x.shape, 9.0)
        f2 = np.full(x.shape, 9.0)
        cid = np.zeros(x.shape)
        for oy in (-1, 0, 1):
            for ox in (-1, 0, 1):
                cx, cy = xi + ox, yi + oy
                p = pts[cy % cells_v, cx % cells]
                d = np.hypot(cx + p[..., 0] - x, cy + p[..., 1] - y)
                closer = d < f1
                f2 = np.where(closer, f1, np.minimum(f2, d))
                cid = np.where(closer, ids[cy % cells_v, cx % cells], cid)
                f1 = np.where(closer, d, f1)
        return f1, f2, cid


def blur(img, sigma_px, sigma_v=None):
    """Periodic Gaussian blur (FFT), sigma in pixels (sigma_v: a different vertical one)."""
    sigma_v = sigma_px if sigma_v is None else sigma_v
    if sigma_px <= 0 and sigma_v <= 0:
        return img
    h, w = img.shape[:2]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    kernel = np.exp(-2 * (math.pi ** 2) * ((sigma_px ** 2) * fx ** 2 + (sigma_v ** 2) * fy ** 2))
    if img.ndim == 2:
        return np.real(np.fft.ifft2(np.fft.fft2(img) * kernel))
    return np.stack([np.real(np.fft.ifft2(np.fft.fft2(img[..., c]) * kernel)) for c in range(img.shape[2])], axis=-1)


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def lerp(a, b, t):
    if np.ndim(t) == 2 and (np.ndim(a) == 3 or np.ndim(b) == 3):
        t = t[..., None]
    return a + (b - a) * t


def rgb(r, g, b):
    return np.array([r, g, b], dtype=np.float64)


def colour(base, variation):
    """base (3,) times per-texel brightness (H x W) -> H x W x 3."""
    variation = np.asarray(variation, dtype=np.float64)
    if variation.ndim == 0:
        return np.asarray(base, dtype=np.float64)[None, None, :] * variation
    return np.asarray(base, dtype=np.float64)[None, None, :] * variation[..., None]


def normal_from_height(height, strength_px):
    """height 0-1 across the tile; strength_px: the rise in pixels of one unit of height.
    OpenGL convention: +G up the image."""
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5   # down the image
    n = np.stack([-dx * strength_px, dy * strength_px, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def cavity_ao(height, radius_px, strength):
    around = blur(height, radius_px)
    return 1.0 - np.clip((around - height) * strength, 0.0, 1.0)


def rivets(size, spacing_px, radius_px, offset=(0.5, 0.5), rows=None, cols=None, size_v=None):
    """A grid of domed rivet heads: height 0-1 (a dome per rivet). rows/cols: restrict to
    bands, as boolean functions of the row/column index."""
    h = size_v or size
    ys, xs = np.mgrid[0:h, 0:size].astype(np.float64) + 0.5
    gx = (xs / spacing_px - offset[0]) % 1.0 - 0.5
    gy = (ys / spacing_px - offset[1]) % 1.0 - 0.5
    d = np.hypot(gx, gy) * spacing_px
    dome = np.sqrt(np.clip(1.0 - (d / radius_px) ** 2, 0.0, 1.0))
    return dome


def edges_of_rects(size, rects, size_v=None):
    """Distance (px) from each texel to the nearest edge of the rectangle it is in
    (rects: [(x0, y0, x1, y1)] in pixels; texels in none get 0)."""
    h = size_v or size
    ys, xs = np.mgrid[0:h, 0:size].astype(np.float64) + 0.5
    out = np.zeros((h, size))
    for x0, y0, x1, y1 in rects:
        inside = (xs >= x0) & (xs < x1) & (ys >= y0) & (ys < y1)
        d = np.minimum(np.minimum(xs - x0, x1 - xs), np.minimum(ys - y0, y1 - ys))
        out = np.where(inside, d, out)
    return out


def downsample(img, factor):
    if factor <= 1:
        return img
    h, w = img.shape[:2]
    shape = (h // factor, factor, w // factor, factor) + img.shape[2:]
    return img.reshape(shape).mean(axis=(1, 3))


# ==============================================================================================
# Writing
# ==============================================================================================

WRITTEN = []    # (path, bytes) of every texture this run, for the size report


def _png(path, img, srgb):
    from PIL import Image
    path.parent.mkdir(parents=True, exist_ok=True)
    data = np.clip(img, 0.0, 1.0)
    if data.ndim == 2:
        data = np.stack([data] * 3, axis=-1)
    Image.fromarray((data * 255.0 + 0.5).astype(np.uint8)).save(path)


def write(out_dir, name, img, srgb, normal_map=False, wrap="repeat", alpha=None, png=True):
    """One texture: img H x W x 3 (or 4) floats; alpha (H x W) optional."""
    if alpha is not None:
        img = np.concatenate([img[..., :3], alpha[..., None]], axis=-1)
    path = Path(out_dir) / f"{name}.nmtexture"
    nm.write_texture(path, img, srgb=srgb, wrap=wrap, normal_map=normal_map)
    WRITTEN.append((path, path.stat().st_size))
    if png:
        _png(CACHE / f"{name}.png", img[..., :3], srgb)
        if img.shape[-1] == 4:
            _png(CACHE / f"{name}_A.png", img[..., 3], False)
    return path


def save_set(out_dir, name, albedo, height, normal_strength, roughness, metallic, ao, normal=None, orm_scale=1,
             alpha=None, orm=True, normal_map=True):
    """A PBR set: <name>_BaseColor (A: alpha, else height), _Normal, _ORM (orm_scale: 2 writes
    it at half the resolution)."""
    albedo = np.clip(albedo, 0.0, 1.0)
    if normal is None:
        normal = normal_from_height(height, normal_strength)
    orm_img = np.stack([np.clip(ao, 0, 1), np.clip(roughness, 0.03, 1), np.clip(metallic, 0, 1)], axis=-1)
    orm_img = downsample(orm_img, orm_scale)
    write(out_dir, f"{name}_BaseColor", albedo, True, alpha=np.clip(height if alpha is None else alpha, 0, 1))
    if normal_map:
        write(out_dir, f"{name}_Normal", normal, False, normal_map=True)
    if orm is not False:
        write(out_dir, f"{name}_ORM", orm_img, False)
    print(f"  {name}: {albedo.shape[1]} x {albedo.shape[0]}")
