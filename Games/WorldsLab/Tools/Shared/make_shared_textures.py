#!/usr/bin/env python3
"""
make_shared_textures.py -- writes Content/Textures/Shared/, the PBR texture
sets WorldsLab's shared materials use (shared_materials.py: Wood, Stone):
procedural, seamlessly tileable, each set three .nmtexture files.

    <Set>_BaseColor   sRGB RGBA8 albedo (A = 255)
    <Set>_Normal      linear RGBA8 tangent-space normal, OpenGL convention:
                      +X red to the image's right, +Y green toward its top
    <Set>_Surface     linear RGBA8: R roughness, G ambient occlusion
                      (1 = open), B metallic, A 255

    set          one repeat covers  what
    Wood         2.0 m              dark stained hand-hewn timber, grain along U
    Stone        1.5 m              buff cantera / limestone
    Macro        (very large)       Macro_Noise: R/G/B independent fBm, 0.1-0.9

Sets are generated at 1024 (Macro 512) and written at the size in RES below
-- 1024 for Wood's BaseColor, 512 for the rest -- box-filtered down, so
pattern scale per metre never changes. (They began as two of the Courtyard
world's sets; the world is gone, its generator in git history at 61af1eec.)

Image row 0 is the top (v = 0). Heights are built in millimetres and turned
into normals with periodic central differences at the set's real texel size,
so relief has its true slope (times a per-set strength). All noise is
periodic (FFT-filtered white noise, wrapped Voronoi, wrapped stamps), so the
textures tile. Files are written as RenderCore's TextureAsset.cpp reads them:
NTEX holding THDR (version 3), TPIX and TMIP -- RGBA8, linear filtering, wrap
Repeat, a full Box mip chain to 1 x 1: colour averaged in linear light, data
averaged raw, normals renormalised. PNG previews go to --preview (default:
none).

Deterministic: running it again writes the same bytes. Needs numpy. Usage:

    python Games/WorldsLab/Tools/Shared/make_shared_textures.py [--preview DIR] [Set ...]
"""

import argparse
import struct
import sys
import zlib
from multiprocessing import Pool
from pathlib import Path

import numpy as np

OUTPUT = Path(__file__).resolve().parents[2] / "Content" / "Textures" / "Shared"

# Written resolution of each map (they are committed, so most stay small). Sets are
# generated at their own internal size (1024; Macro 512) and box-filtered
# down to this -- pattern scale per world metre is unaffected. Unlisted maps: DEFAULT_RES.
DEFAULT_RES = 512
RES = {
    "Wood_BaseColor": 1024,
}


# --- helpers ------------------------------------------------------------------------------------

def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    return a + (b - a) * t


def to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0.0), 1.0 / 2.4) - 0.055)


def lin(r, g, b):
    """An sRGB colour as a linear RGB vector."""
    return to_linear(np.array([r, g, b]))


def col(field, colour):
    """A scalar field times a colour: H x W x 3."""
    return field[..., None] * np.asarray(colour)[None, None, :]


def mix(a, b, t):
    """Blend colour images / vectors by a scalar field."""
    t = np.asarray(t)
    if t.ndim == 2:
        t = t[..., None]
    return a + (np.asarray(b) - a) * t


def _freqs(h, w):
    fy = np.fft.fftfreq(h)[:, None] * h          # cycles per texture
    fx = np.fft.rfftfreq(w)[None, :] * w
    return fy, fx


def spectral(rng, shape, lo=1.0, hi=None, beta=1.0, sx=1.0, sy=1.0):
    """Periodic Gaussian noise, zero mean, unit std: amplitude ~ f^-beta between
    lo and hi cycles per texture. sx > 1 stretches features along x (columns),
    sy > 1 along y (rows)."""
    h, w = shape
    white = rng.standard_normal(shape)
    spec = np.fft.rfft2(white)
    fy, fx = _freqs(h, w)
    f = np.sqrt((fx * sx) ** 2 + (fy * sy) ** 2)
    f[0, 0] = 1e-9
    filt = f ** -beta
    filt *= (f / lo) ** 4 / (1.0 + (f / lo) ** 4)
    if hi is not None:
        filt *= np.exp(-(f / hi) ** 2)
    filt[0, 0] = 0.0
    out = np.fft.irfft2(spec * filt, s=shape)
    out -= out.mean()
    return out / (out.std() + 1e-12)


def blur(img, sigma):
    """Periodic Gaussian blur, sigma in pixels (2-D or H x W x C)."""
    if sigma <= 0:
        return img
    if img.ndim == 3:
        return np.stack([blur(img[..., c], sigma) for c in range(img.shape[2])], axis=-1)
    h, w = img.shape
    fy, fx = _freqs(h, w)
    g = np.exp(-2.0 * np.pi ** 2 * sigma ** 2 * ((fy / h) ** 2 + (fx / w) ** 2))
    return np.fft.irfft2(np.fft.rfft2(img) * g, s=img.shape)


def rescale(x, lo=0.0, hi=1.0):
    a, b = x.min(), x.max()
    return lo + (x - a) / (b - a + 1e-12) * (hi - lo)


def sample(img, y, x):
    """Bilinear, wrapped sampling of img at float pixel coordinates."""
    h, w = img.shape[:2]
    y0 = np.floor(y).astype(np.int64)
    x0 = np.floor(x).astype(np.int64)
    fy, fx = y - y0, x - x0
    y0 %= h
    x0 %= w
    y1, x1 = (y0 + 1) % h, (x0 + 1) % w
    if img.ndim == 3:
        fy, fx = fy[..., None], fx[..., None]
    return ((img[y0, x0] * (1 - fx) + img[y0, x1] * fx) * (1 - fy)
            + (img[y1, x0] * (1 - fx) + img[y1, x1] * fx) * fy)


def warp(img, dy, dx):
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    return sample(img, yy + dy, xx + dx)


def voronoi(rng, shape, nx, ny, jitter=1.0):
    """Periodic jittered-grid Voronoi, nx x ny cells over the texture.
    Returns F1, F2 (pixels), the nearest cell's id and the offset (dy, dx)
    from that cell's seed to the pixel (pixels)."""
    h, w = shape
    cw, ch = w / nx, h / ny
    ox = 0.5 + (rng.random((ny, nx)) - 0.5) * jitter
    oy = 0.5 + (rng.random((ny, nx)) - 0.5) * jitter
    X = (np.arange(w)[None, :] + 0.5) / cw
    Y = (np.arange(h)[:, None] + 0.5) / ch
    X = np.broadcast_to(X, shape)
    Y = np.broadcast_to(Y, shape)
    ci, cj = np.floor(X).astype(int), np.floor(Y).astype(int)
    f1 = np.full(shape, 1e9)
    f2 = np.full(shape, 1e9)
    cid = np.zeros(shape, dtype=np.int64)
    rdx = np.zeros(shape)
    rdy = np.zeros(shape)
    for dj in (-1, 0, 1):
        for di in (-1, 0, 1):
            ni, nj = ci + di, cj + dj
            mi, mj = ni % nx, nj % ny
            px = ni + ox[mj, mi]
            py = nj + oy[mj, mi]
            dx = (X - px) * cw
            dy = (Y - py) * ch
            d = np.sqrt(dx * dx + dy * dy)
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d))
            f1 = np.where(closer, d, f1)
            cid = np.where(closer, mj * nx + mi, cid)
            rdx = np.where(closer, dx, rdx)
            rdy = np.where(closer, dy, rdy)
    return f1, f2, cid, rdy, rdx


def scatter(rng, shape, n, prob, rmin, rmax, soft=1.0, aspect=1.0):
    """Periodic scattered round spots (one possible per Voronoi cell of an
    n x n grid): returns coverage 0-1 and a per-spot random value."""
    f1, f2, cid, dy, dx = voronoi(rng, shape, n, n, jitter=0.8)
    cells = n * n
    present = rng.random(cells) < prob
    radius = rmin + (rmax - rmin) * rng.random(cells) ** 2
    value = rng.random(cells)
    d = np.sqrt((dx / aspect) ** 2 + dy ** 2)
    r = radius[cid]
    cov = smoothstep(r + soft * 0.5, r - soft * 0.5, d) * present[cid]
    return cov, value[cid], d / np.maximum(r, 1e-6)


def stamp(target, cy, cx, half, values, op="add"):
    """Write a (2h+1) x (2h+1) window of values into target around (cy, cx), wrapped."""
    h, w = target.shape[:2]
    rows = (np.arange(-half, half + 1) + int(cy)) % h
    cols = (np.arange(-half, half + 1) + int(cx)) % w
    ix = np.ix_(rows, cols)
    if op == "add":
        target[ix] += values
    elif op == "max":
        target[ix] = np.maximum(target[ix], values)
    elif op == "min":
        target[ix] = np.minimum(target[ix], values)
    elif op == "set":
        target[ix] = values


def window(half, cy, cx):
    """Local pixel offsets (dy, dx) of a stamp window, including the sub-pixel part."""
    o = np.arange(-half, half + 1, dtype=np.float64)
    dy = o[:, None] + (np.floor(cy) - cy) + 0.5
    dx = o[None, :] + (np.floor(cx) - cx) + 0.5
    return np.broadcast_to(dy, (o.size, o.size)), np.broadcast_to(dx, (o.size, o.size))


def crack_lines(rng, shape, count, length_px, angle=None, drift=0.01, jag=0.3, branch=0.0, width=0.7):
    """Periodic hairlines: wandering walks (heading = start + slow drift + a
    jagged, non-accumulating wiggle) rasterised and blurred. Returns 0-1."""
    h, w = shape
    acc = np.zeros(shape)
    walks = []
    for _ in range(count):
        a0 = rng.random() * 2 * np.pi if angle is None else angle + rng.normal(0, 0.04) + np.pi * rng.integers(0, 2)
        walks.append((rng.random() * h, rng.random() * w, a0, length_px * (0.5 + rng.random()), 1.0))
    while walks:
        y, x, a, L, strength = walks.pop()
        steps = max(int(L / 0.5), 8)
        slow = np.cumsum(rng.normal(0, drift, steps))
        wig = np.convolve(rng.normal(0, 1, steps + 40), np.ones(12) / np.sqrt(12), mode="same")[20:20 + steps]
        kinks = np.repeat(rng.normal(0, 1, steps // 16 + 1), 16)[:steps]
        ang = a + slow + jag * (0.5 * wig + 0.6 * kinks)
        ys = y + np.cumsum(np.sin(ang) * 0.5)
        xs = x + np.cumsum(np.cos(ang) * 0.5)
        i = np.arange(steps)
        fade = np.clip(np.minimum(i, steps - i) / (steps * 0.2 + 1), 0, 1)
        wgt = strength * (0.3 + 0.7 * fade)
        yi, xi = np.floor(ys).astype(int), np.floor(xs).astype(int)
        fy, fx = ys - yi, xs - xi
        for oy, ox, ww in ((0, 0, (1 - fy) * (1 - fx)), (0, 1, (1 - fy) * fx),
                           (1, 0, fy * (1 - fx)), (1, 1, fy * fx)):
            np.add.at(acc, ((yi + oy) % h, (xi + ox) % w), ww * wgt)
        if branch > 0 and strength > 0.4:
            for _ in range(rng.poisson(branch)):
                k = int(rng.integers(steps // 5, max(steps // 5 + 1, steps - 1)))
                walks.append((ys[k], xs[k], ang[k] + rng.choice([-1, 1]) * rng.uniform(0.4, 1.1),
                              L * rng.uniform(0.2, 0.5), strength * 0.6))
    acc = blur(acc, width)
    return np.clip(acc / (2.0 / (np.sqrt(2 * np.pi) * width)), 0, 1)


def slope_rms(h_mm, texel_mm):
    gx = (np.roll(h_mm, -1, 1) - np.roll(h_mm, 1, 1)) / (2 * texel_mm)
    gy = (np.roll(h_mm, -1, 0) - np.roll(h_mm, 1, 0)) / (2 * texel_mm)
    return float(np.sqrt(np.mean(gx * gx + gy * gy)))


def normal_from_height(h_mm, texel_mm, strength=1.0):
    """Tangent-space normal (OpenGL: +Y toward row 0) from a height field in mm, periodic."""
    dhdc = (np.roll(h_mm, -1, axis=1) - np.roll(h_mm, 1, axis=1)) / (2 * texel_mm)
    dhdr = (np.roll(h_mm, -1, axis=0) - np.roll(h_mm, 1, axis=0)) / (2 * texel_mm)
    n = np.stack([-dhdc * strength, dhdr * strength, np.ones_like(h_mm)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def cavity_ao(h_mm, scales, gain):
    """Ambient occlusion from how far each texel sits below its surroundings at
    several blur scales: [(sigma_px, weight), ...]; gain per mm."""
    occ = np.zeros_like(h_mm)
    for sigma, weight in scales:
        occ += weight * np.clip(blur(h_mm, sigma) - h_mm, 0, None)
    return np.clip(1.0 - occ * gain, 0.0, 1.0)


def finish_albedo(linear_rgb, lo=0.012, hi=0.88):
    return to_srgb(np.clip(linear_rgb, lo, hi))


def surface(rough, ao, metal=None):
    metal = np.zeros_like(rough) if metal is None else metal
    return np.stack([np.clip(rough, 0, 1), np.clip(ao, 0, 1), np.clip(metal, 0, 1),
                     np.ones_like(rough)], axis=-1)


def rgba(rgb, alpha=None):
    alpha = np.ones(rgb.shape[:2]) if alpha is None else alpha
    return np.concatenate([np.clip(rgb, 0, 1), alpha[..., None]], axis=-1)


# --- mips / .nmtexture ----------------------------------------------------------------------------

def _down(img):
    h, w = img.shape[0] // 2, img.shape[1] // 2
    return img.reshape(h, 2, w, 2, img.shape[2]).mean(axis=(1, 3))


def _coverage(alpha, scale):
    return float(np.mean(np.clip(alpha * scale, 0, 1) > 0.5))


def mips_for(level0, kind):
    """Every level after level0 down to 1 x 1. kind: 'color' (sRGB RGB, linear A),
    'data' (raw), 'normal' (renormalised), 'foliage' (colour + coverage-kept alpha)."""
    levels = []
    if kind in ("color", "foliage"):
        cur = np.concatenate([to_linear(level0[..., :3]), level0[..., 3:]], axis=-1)
        target = _coverage(level0[..., 3], 1.0)
        while cur.shape[0] > 1:
            cur = _down(cur)
            out = np.concatenate([to_srgb(cur[..., :3]), cur[..., 3:]], axis=-1)
            if kind == "foliage":
                lo, hi = 0.25, 8.0
                for _ in range(40):
                    mid = 0.5 * (lo + hi)
                    if _coverage(cur[..., 3], mid) < target:
                        lo = mid
                    else:
                        hi = mid
                out[..., 3] = np.clip(cur[..., 3] * hi, 0, 1)
            levels.append(out)
    elif kind == "data":
        cur = level0
        while cur.shape[0] > 1:
            cur = _down(cur)
            levels.append(cur)
    elif kind == "normal":
        vec = level0[..., :3] * 2.0 - 1.0
        alpha = level0[..., 3:]
        while vec.shape[0] > 1:
            vec = _down(vec)
            alpha = _down(alpha)
            n = np.linalg.norm(vec, axis=-1, keepdims=True)
            vec = np.where(n > 1e-6, vec / np.maximum(n, 1e-6), np.array([0.0, 0.0, 1.0]))
            levels.append(np.concatenate([vec * 0.5 + 0.5, alpha], axis=-1))
    return levels


def downsample_to(img, kind, size):
    """Box-filter img down (by halves) to size x size, as its mips are made."""
    while img.shape[0] > size:
        img = _half(img, kind)
    return img


def _half(img, kind):
    if kind in ("color", "foliage"):
        cur = _down(np.concatenate([to_linear(img[..., :3]), img[..., 3:]], axis=-1))
        return np.concatenate([to_srgb(cur[..., :3]), cur[..., 3:]], axis=-1)
    if kind == "normal":
        vec = _down(img[..., :3] * 2.0 - 1.0)
        vec /= np.maximum(np.linalg.norm(vec, axis=-1, keepdims=True), 1e-6)
        return np.concatenate([vec * 0.5 + 0.5, _down(img[..., 3:])], axis=-1)
    return _down(img)


def to_bytes(image):
    return (np.clip(image, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).tobytes()


def tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


def chunk(name, payload, version=1):
    return struct.pack("<IHI", tag(name), version, len(payload)) + payload


def serialize(level0, kind, wrap=0):
    h, w = level0.shape[:2]
    mips = mips_for(level0, kind)
    srgb = 1 if kind in ("color", "foliage") else 0
    # Width, Height, Format RGBA8, sRGB, Filter Linear, Wrap (0 Repeat / 1 Clamp),
    # MipGen Box, Compression None, MipCount, no blocks.
    header = struct.pack("<iiBBBBBBBB", w, h, 0, srgb, 1, wrap, 1, 0, 1 + len(mips), 0)
    return chunk("NTEX", chunk("THDR", header, version=3) + chunk("TPIX", to_bytes(level0))
                 + chunk("TMIP", b"".join(to_bytes(m) for m in mips)))


# --- PNG previews ---------------------------------------------------------------------------------

def write_png(path, image):
    """image: H x W x 3 or 4 floats 0-1."""
    data = (np.clip(image, 0, 1) * 255.0 + 0.5).astype(np.uint8)
    h, w, c = data.shape
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))

    def pchunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + pchunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if c == 4 else 2, 0, 0, 0))
           + pchunk(b"IDAT", zlib.compress(raw, 6)) + pchunk(b"IEND", b""))
    Path(path).write_bytes(png)


def lit_preview(base_srgb, normal_enc, surf):
    """Rough diffuse + spec shading under a raking light from the upper left."""
    albedo = to_linear(base_srgb[..., :3])
    n = normal_enc[..., :3] * 2 - 1
    L = np.array([-0.55, 0.55, 0.63])
    L /= np.linalg.norm(L)
    ndl = np.clip((n * L).sum(-1), 0, 1)
    ao = surf[..., 1]
    rough = surf[..., 0]
    metal = surf[..., 2]
    H = L + np.array([0, 0, 1.0])
    H /= np.linalg.norm(H)
    ndh = np.clip((n * H).sum(-1), 0, 1)
    a2 = np.maximum(rough ** 4, 1e-4)
    d = a2 / (np.pi * (ndh * ndh * (a2 - 1) + 1) ** 2)
    f0 = mix(np.full(albedo.shape, 0.04), albedo, metal)
    spec = f0 * (d * ndl * 0.25)[..., None]
    diffuse = albedo * (1 - metal[..., None]) * (0.25 * ao + 1.6 * ndl)[..., None]
    return to_srgb(np.clip((diffuse + spec * 3.0) * 0.8, 0, 1))


def tiled(img, half=True):
    t = np.tile(img, (2, 2, 1))
    if half:
        t = _down(t)
    return t


# --- sets -----------------------------------------------------------------------------------------
# Each returns {"BaseColor": rgba, "Normal": rgba, "Surface": rgba} (floats 0-1), any subset.

def _grain_phase(rng, shape, rings, warp_rings, sx=14, knots=()):
    """Periodic ring phase for grain running along x: rings per texture height,
    warped by noise stretched along x, bumped around knots."""
    S = shape[0]
    rows = (np.arange(S)[:, None] + 0.5) / S
    phase = np.broadcast_to(rows * rings, shape).copy()
    phase += spectral(rng, shape, lo=1, hi=10, beta=1.6, sx=sx) * warp_rings
    phase += spectral(rng, shape, lo=6, hi=60, beta=1.2, sx=sx * 0.6) * warp_rings * 0.08
    Y, X = np.mgrid[0:S, 0:shape[1]] + 0.5
    for (ky, kx, R, amp) in knots:
        dy = (Y - ky + S / 2) % S - S / 2
        dx = (X - kx + shape[1] / 2) % shape[1] - shape[1] / 2
        phase += amp * np.exp(-((dx / (3.2 * R)) ** 2 + (dy / (1.4 * R)) ** 2))
    return phase


def set_wood():
    S, world = 1024, 2000.0
    t = world / S
    rng = np.random.default_rng(505)
    shape = (S, S)
    knots = []
    for _ in range(4):
        knots.append((rng.random() * S, rng.random() * S, rng.uniform(7, 15), rng.uniform(3, 6) * rng.choice([-1, 1])))
    phase = _grain_phase(rng, shape, 120, 7.0, sx=18, knots=knots)
    ring = phase - np.floor(phase)
    figure = smoothstep(-1.2, 1.2, spectral(rng, shape, lo=2, hi=20, beta=1.2, sx=10))
    late = smoothstep(0.5, 0.88, ring) * (1 - smoothstep(0.9, 1.0, ring)) * (0.35 + 0.65 * figure)
    fiber = spectral(rng, shape, lo=30, hi=500, beta=0.5, sx=50)
    fiber2 = spectral(rng, shape, lo=6, hi=120, beta=1.0, sx=30)

    # adze scallops: shallow dishes, longer along the grain than across
    nxa, nya = 18, 26
    f1, f2, cid, ady, adx = voronoi(rng, (S, S), nxa, nya, jitter=0.85)
    cw, ch = S / nxa, S / nya
    dish = (adx / (cw * 0.7)) ** 2 + (ady / (ch * 1.2)) ** 2
    depth = rng.uniform(0.8, 1.8, nxa * nya)[cid]
    adze = blur(np.clip(dish, 0, 1.5) * depth, 2.0)
    adze_ridge = smoothstep(0.15, 0.7, adze - blur(adze, 8))

    checks = crack_lines(rng, shape, 5, 420, angle=0.0, drift=0.0015, jag=0.05, width=0.8)
    checks = np.clip(checks * 1.3, 0, 1)
    knot_m = np.zeros(shape)
    knot_r = np.zeros(shape)
    for (ky, kx, R, amp) in knots:
        hw = int(R * 2) + 3
        wy, wx = window(hw, ky, kx)
        rr = np.sqrt((wx / 1.25) ** 2 + wy ** 2) / R
        stamp(knot_m, ky, kx, hw, smoothstep(1.0, 0.65, rr), "max")
        stamp(knot_r, ky, kx, hw, smoothstep(1.0, 0.7, rr) * (0.5 + 0.5 * np.cos(rr * 14)), "max")

    height = (adze * 2.2 + fiber * 0.05 + fiber2 * 0.12 + late * 0.15 - checks * 2.5
              + knot_m * 0.3 + spectral(rng, shape, lo=1, hi=6, beta=2.0, sx=3) * 0.8)

    early = lin(0.265, 0.165, 0.10)
    latec = lin(0.15, 0.088, 0.053)
    c = mix(np.broadcast_to(early, shape + (3,)), latec, late * 0.8)
    c *= (1 + 0.10 * fiber + 0.09 * fiber2)[..., None]
    stain = spectral(rng, shape, lo=1, hi=12, beta=1.3, sx=4)
    c *= (1 + 0.10 * stain)[..., None]
    c = mix(c, c * 1.3, adze_ridge * 0.35)
    low = smoothstep(0, 0.8, blur(height, 5) - height)
    c = mix(c, c * 0.65, low * 0.5)
    c = mix(c, lin(0.10, 0.06, 0.04), knot_m * (0.6 + 0.3 * knot_r))
    c = mix(c, lin(0.05, 0.035, 0.025), checks * 0.9)
    dust = smoothstep(0.8, 2.5, spectral(rng, shape, lo=2, hi=20, beta=1.2)) * 0.15
    c = mix(c, lin(0.30, 0.26, 0.21), dust)

    ao = cavity_ao(height, [(2, 1.0), (8, 0.5)], 0.25) * (1 - 0.6 * checks)
    rough = np.clip(0.66 - 0.08 * adze_ridge + 0.05 * low + 0.03 * fiber + 0.06 * dust, 0.55, 0.75)
    return dict(BaseColor=rgba(finish_albedo(c, lo=0.006)),
                Normal=rgba(normal_from_height(height, t, 1.2) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_stone():
    S, world = 1024, 1500.0
    t = world / S
    rng = np.random.default_rng(808)
    shape = (S, S)
    dwy = spectral(rng, shape, lo=1, hi=6, beta=1.6) * 10
    bed = warp(spectral(rng, shape, lo=2, hi=60, beta=1.1, sx=22), dwy, 0.0)
    bed2 = warp(spectral(rng, shape, lo=10, hi=250, beta=0.8, sx=30), dwy, 0.0)
    macro = spectral(rng, shape, lo=1, hi=10, beta=1.4)
    grain = spectral(rng, shape, lo=120, hi=600, beta=0.2)
    base = lin(0.76, 0.66, 0.52)
    c = np.broadcast_to(base, shape + (3,)) * (1 + 0.025 * bed + 0.015 * bed2 + 0.04 * macro + 0.04 * grain
                                                + 0.03 * spectral(rng, shape, lo=20, hi=150, beta=0.8))[..., None]
    pink = spectral(rng, shape, lo=1, hi=8, beta=1.4)
    c *= np.stack([1 + 0.015 * bed + 0.025 * pink, np.ones(shape), 1 - 0.03 * bed + 0.01 * pink], -1)
    lith, lith_v, _ = scatter(rng, shape, 180, 0.18, 0.4, 1.5, soft=1.0)
    darkl = lith * (lith_v < 0.45)
    lightl = lith * (lith_v >= 0.45)
    c = mix(c, c * (lin(0.56, 0.44, 0.34) / base), darkl * 0.6)
    c = mix(c, lin(0.84, 0.78, 0.68), lightl * 0.5)

    vug, vug_v, vug_r = scatter(rng, shape, 40, 0.12, 0.8, 3.4, soft=1.2, aspect=1.6)
    vug2, _, vug2_r = scatter(rng, shape, 110, 0.06, 0.4, 1.0, soft=0.9)
    vugs = np.clip(vug + vug2 * 0.6, 0, 1)
    c = mix(c, c * (lin(0.40, 0.33, 0.26) / base), vugs * 0.9)
    weather = smoothstep(0.0, 2.0, macro + 0.6 * spectral(rng, shape, lo=4, hi=30, beta=1.0))
    c = mix(c, c * lin(0.60, 0.52, 0.42) / lin(0.76, 0.66, 0.52), weather * 0.35)

    height = (spectral(rng, shape, lo=1, hi=8, beta=1.8) * 0.35 + spectral(rng, shape, lo=8, hi=80, beta=1.0) * 0.06
              + grain * 0.03 + bed2 * 0.02 - vug * np.clip(1 - vug_r ** 2, 0, 1) * 1.2
              - vug2 * np.clip(1 - vug2_r ** 2, 0, 1) * 0.5)
    low = smoothstep(0, 0.4, blur(height, 4) - height)
    c = mix(c, c * 0.8, low * 0.4 * (1 - vugs))
    ao = cavity_ao(height, [(1.5, 1.0), (5, 0.6)], 0.9)
    rough = np.clip(0.78 + 0.03 * grain + 0.02 * bed - 0.02 * weather + 0.06 * vugs, 0.7, 0.85)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(height, t, 1.3) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_macro():
    """Three independent smooth fBm fields for breaking up repetition at a very large scale."""
    S = 512
    rng = np.random.default_rng(1414)
    shape = (S, S)

    def remap(n):
        return 0.5 + 0.4 * np.tanh(n * 0.55)

    r = remap(spectral(rng, shape, lo=1.6, hi=4.5, beta=1.2))
    g = remap(spectral(rng, shape, lo=4, hi=9, beta=1.2))
    b = remap(spectral(rng, shape, lo=2.5, hi=14, beta=1.3, sy=4))
    return dict(Noise=np.stack([r, g, b, np.ones(shape)], -1))


SETS = {
    "Wood": (set_wood, 2.0),
    "Stone": (set_stone, 1.5),
    "Macro": (set_macro, None),
}


def build(args):
    name, preview = args
    maps = SETS[name][0]()
    written = []
    wrap = 1 if name == "Foliage" else 0
    for kind, img in maps.items():
        mk = {"BaseColor": "foliage" if name == "Foliage" else "color", "Normal": "normal", "Surface": "data", "Noise": "data"}[kind]
        path = OUTPUT / f"{name}_{kind}.nmtexture"
        img = downsample_to(img, mk, RES.get(f"{name}_{kind}", DEFAULT_RES))
        maps[kind] = img
        path.write_bytes(serialize(img, mk, wrap))
        written.append((str(path), path.stat().st_size))
        if preview:
            p = Path(preview)
            if name == "Foliage" and kind == "BaseColor":
                a = img[..., 3:]
                grey = to_srgb(np.full(img.shape[:2] + (3,), 0.18))
                write_png(p / f"{name}_{kind}.png", img[..., :3] * a + grey * (1 - a))
                write_png(p / f"{name}_{kind}_alpha.png", np.repeat(a, 3, -1))
            else:
                write_png(p / f"{name}_{kind}.png", img[..., :3])
            if kind == "BaseColor" and name != "Foliage":
                write_png(p / f"{name}_{kind}_tiled.png", tiled(img[..., :3]))
            if kind in ("Normal", "Noise") and "BaseColor" not in maps:
                write_png(p / f"{name}_{kind}_tiled.png", tiled(img[..., :3]))
    if preview and "BaseColor" in maps:
        size = min(m.shape[0] for m in maps.values())
        lm = {k: downsample_to(v, "color" if k == "BaseColor" else ("normal" if k == "Normal" else "data"), size)
              for k, v in maps.items()}
        lit = lit_preview(lm["BaseColor"], lm["Normal"], lm["Surface"])
        if name == "Foliage":
            a = lm["BaseColor"][..., 3:]
            lit = lit * a + to_srgb(np.full(lit.shape, 0.18)) * (1 - a)
        write_png(Path(preview) / f"{name}_Lit.png", lit)
    return name, written


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--preview", help="directory for PNG previews")
    parser.add_argument("sets", nargs="*", help="sets to build (default: all)")
    args = parser.parse_args()
    names = args.sets or list(SETS)
    for n in names:
        if n not in SETS:
            sys.exit(f"unknown set {n}; choose from {', '.join(SETS)}")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if args.preview:
        Path(args.preview).mkdir(parents=True, exist_ok=True)
    with Pool(min(len(names), 16)) as pool:
        for name, written in pool.imap_unordered(build, [(n, args.preview) for n in names]):
            for path, size in written:
                print(f"wrote {path} ({size} bytes)")


if __name__ == "__main__":
    main()
