#!/usr/bin/env python3
"""
make_courtyard_textures.py -- writes Content/Textures/Courtyard/, the PBR
texture sets of the Spanish Colonial Revival courtyard: procedural, every one
(but Foliage) seamlessly tileable, each set three .nmtexture files.

    <Set>_BaseColor   sRGB RGBA8 albedo (A = 255; Foliage: A = coverage)
    <Set>_Normal      linear RGBA8 tangent-space normal, OpenGL convention:
                      +X red to the image's right, +Y green toward its top
    <Set>_Surface     linear RGBA8: R roughness, G ambient occlusion
                      (1 = open), B metallic, A 255 (Glass: G = grime)

    set          one repeat covers  what
    Stucco       2.0 m              hand-troweled lime plaster
    RoofTile     1.0 m              old handmade terracotta clay (surface only)
    FloorTile    1.2 m              4 x 4 Saltillo tiles, 30 cm, 8 mm grout
    Paving       2.4 m              basketweave clay pavers, 20 x 10 cm, 5 mm joints
    Wood         2.0 m              dark stained hand-hewn timber, grain along U
    GreenPaint   1.0 m              green-painted wood, chipped, grain along U
    Iron         1.0 m              hammered black wrought iron, some rust
    Stone        1.5 m              buff cantera / limestone
    Talavera     0.6 m              4 x 4 glazed Talavera tiles, 15 cm, 3 mm grout
    Ground       4.0 m              decomposed granite, pebbles, dry grass
    Glass        0.6 m              old window glass (Normal, Surface only)
    Water        1.0 m              small ripples (Normal only)
    Foliage      one leaf card      citrus leaf cluster, alpha-masked, not tiling
    Macro        (very large)       Macro_Noise: R/G/B independent fBm, 0.1-0.9

Sets are generated at 1024 (Glass, Water, Macro 512) and written at the size
in RES below -- 1024 for the big-on-screen BaseColors, 512 for the rest --
box-filtered down, so pattern scale per metre never changes.

Image row 0 is the top (v = 0). Heights are built in millimetres and turned
into normals with periodic central differences at the set's real texel size,
so relief has its true slope (times a per-set strength). All noise is
periodic (FFT-filtered white noise, wrapped Voronoi, wrapped stamps), so the
textures tile. Files are written as RenderCore's TextureAsset.cpp reads them:
NTEX holding THDR (version 3), TPIX and TMIP -- RGBA8, linear filtering, wrap
Repeat (Foliage: Clamp), a full Box mip chain to 1 x 1: colour averaged in
linear light, data averaged raw, normals renormalised, Foliage's alpha scaled
per level to keep its coverage. PNG previews go to --preview (default: none).

Deterministic: running it again writes the same bytes. Needs numpy. Usage:

    python Games/WorldsLab/Tools/Courtyard/make_courtyard_textures.py [--preview DIR] [Set ...]
"""

import argparse
import struct
import sys
import zlib
from multiprocessing import Pool
from pathlib import Path

import numpy as np

OUTPUT = Path(__file__).resolve().parents[2] / "Content" / "Textures" / "Courtyard"

# Written resolution of each map (they are committed, so most stay small). Sets are
# generated at their own internal size (1024; Glass and Water 512) and box-filtered
# down to this -- pattern scale per world metre is unaffected. Unlisted maps: DEFAULT_RES.
DEFAULT_RES = 512
RES = {
    "Stucco_BaseColor": 1024,
    "FloorTile_BaseColor": 1024,
    "Paving_BaseColor": 1024,
    "RoofTile_BaseColor": 1024,
    "Wood_BaseColor": 1024,
    "Talavera_BaseColor": 1024,
    "Foliage_BaseColor": 1024,
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

def set_stucco():
    S, world = 1024, 2000.0
    t = world / S
    rng = np.random.default_rng(101)
    shape = (S, S)

    # Trowel pads: overlapping curved sweeps, each pressed flat over what was there,
    # leaving a soft lip of pushed plaster along one edge.
    pad_h = spectral(rng, shape, lo=1, hi=8, beta=1.8) * 0.8
    pad_tone = np.zeros(shape)
    ridge = np.zeros(shape)
    edge_n = spectral(rng, shape, lo=10, hi=80, beta=1.0)
    for _ in range(340):
        cy, cx = rng.random() * S, rng.random() * S
        L = rng.uniform(220, 520) / t / 2         # half length, px
        Wd = rng.uniform(90, 200) / t / 2         # half width
        ang = rng.uniform(0, np.pi)
        curv = rng.normal(0, 0.7) / L
        half = int(L + Wd + 4)
        dy, dx = window(half, cy, cx)
        u = dx * np.cos(ang) + dy * np.sin(ang)
        v = -dx * np.sin(ang) + dy * np.cos(ang)
        v = v - curv * u * u
        rows = (np.arange(-half, half + 1) + int(cy)) % S
        cols = (np.arange(-half, half + 1) + int(cx)) % S
        ix = np.ix_(rows, cols)
        e = (np.abs(u / L) ** 2.5 + np.abs(v / Wd) ** 2.5) ** (1 / 2.5) + edge_n[ix] * 0.06
        m = smoothstep(1.0, 1.0 - 10.0 / Wd, e)
        level = pad_h[ix].mean() * 0.7 + rng.normal(0, 0.3) + u / L * rng.normal(0, 0.15)
        pad_h[ix] = pad_h[ix] * (1 - m) + level * m
        lip = m * (1 - m) * 4 * rng.uniform(0.05, 0.16) * smoothstep(-0.3, 0.6, v / Wd)
        ridge[ix] = ridge[ix] * (1 - m) + lip
        pad_tone[ix] = pad_tone[ix] * (1 - m) + rng.normal(0, 1) * m
    pad_h = blur(pad_h, 2.0)
    ridge = blur(ridge, 1.5)
    pad_tone = blur(pad_tone, 4.0)

    grain = spectral(rng, shape, lo=150, hi=700, beta=0.2)
    grain2 = spectral(rng, shape, lo=40, hi=200, beta=0.6)
    sand, sand_v, _ = scatter(rng, shape, 220, 0.4, 0.4, 1.2, soft=1.0)
    cracks = crack_lines(rng, shape, 4, 220, drift=0.02, jag=0.35, branch=1.0, width=0.55)
    height = pad_h + ridge + grain * 0.03 + grain2 * 0.03 + sand * (sand_v - 0.2) * 0.12 - cracks * 0.25

    # Colour: warm lime white, softly clouded, with grime that settles low and streaks down.
    base = lin(0.86, 0.82, 0.74)
    mottle = spectral(rng, shape, lo=3, hi=60, beta=1.1)
    cloud = spectral(rng, shape, lo=1, hi=8, beta=1.5)
    warm = spectral(rng, shape, lo=1, hi=10, beta=1.3)
    tone = 1 + 0.025 * mottle + 0.03 * cloud + 0.02 * pad_tone + 0.03 * grain + 0.02 * grain2
    c = col(tone, base)
    c *= np.stack([1 + 0.012 * warm, 1 + 0.004 * warm, 1 - 0.02 * warm], -1)
    c = mix(c, c * (lin(0.74, 0.69, 0.60) / base), sand * np.clip(sand_v - 0.3, 0, 1))
    low = smoothstep(0.0, 0.25, blur(height, 5) - height)
    streak = spectral(rng, shape, lo=3, hi=80, beta=0.9, sy=12)
    grime = np.clip(0.3 * smoothstep(0.8, 2.6, spectral(rng, shape, lo=1, hi=6, beta=1.4))
                    + 0.15 * smoothstep(0.8, 2.8, streak) + 0.35 * low, 0, 1)
    c = mix(c, c * (lin(0.70, 0.65, 0.57) / base), grime * 0.4)
    c = mix(c, c * 0.62, cracks * 0.6)

    ao = cavity_ao(height, [(2, 1.0), (8, 0.6)], 0.5) * (1 - 0.3 * cracks)
    rough = 0.89 + 0.02 * grain + 0.01 * mottle - 0.5 * ridge - 0.03 * smoothstep(0.1, 0.6, pad_h - blur(pad_h, 30))
    rough = np.clip(rough, 0.8, 0.95)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(height, t, 1.5) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_rooftile():
    S, world = 1024, 1000.0
    t = world / S
    rng = np.random.default_rng(202)
    shape = (S, S)
    n1 = spectral(rng, shape, lo=1, hi=20, beta=1.4)
    n2 = spectral(rng, shape, lo=8, hi=140, beta=1.0)
    n3 = spectral(rng, shape, lo=100, hi=600, beta=0.3)
    dy = spectral(rng, shape, lo=2, hi=20, beta=1.2) * 14
    dx = spectral(rng, shape, lo=2, hi=20, beta=1.2) * 14
    n1w = warp(n1, dy, dx)
    m = np.clip(0.55 + 0.13 * n1w + 0.09 * warp(n2, dy * 0.5, dx * 0.5), 0, 1)
    ramp_t = np.array([0.0, 0.3, 0.55, 0.8, 1.0])
    ramp_c = np.array([lin(0.46, 0.26, 0.17), lin(0.56, 0.29, 0.18), lin(0.65, 0.36, 0.22),
                       lin(0.71, 0.43, 0.28), lin(0.75, 0.50, 0.35)])
    c = np.stack([np.interp(m, ramp_t, ramp_c[:, k]) for k in range(3)], -1)
    c *= (1 + 0.06 * n3 + 0.03 * spectral(rng, shape, lo=40, hi=200, beta=0.7))[..., None]

    # firing flashes: broad darker, greyer clouds
    flash = smoothstep(0.9, 2.4, warp(spectral(rng, shape, lo=1, hi=8, beta=1.6), dy * 2, dx * 2))
    c = mix(c, c * (lin(0.40, 0.33, 0.30) / lin(0.62, 0.40, 0.30)), flash * 0.6)

    pits, pit_v, pit_r = scatter(rng, shape, 70, 0.4, 0.4, 1.8, soft=0.9)
    grit, grit_v, _ = scatter(rng, shape, 150, 0.35, 0.4, 1.2, soft=0.8)
    c = mix(c, c * 0.62, pits * 0.7)
    c = mix(c, c * np.where(grit_v[..., None] > 0.5, 1.18, 0.85), grit * 0.5)

    # faint efflorescence: a whitish bloom, thicker in the pores
    eff_mask = smoothstep(1.3, 2.8, spectral(rng, shape, lo=2, hi=14, beta=1.3))
    eff = eff_mask * np.clip(0.5 + 0.5 * blur(spectral(rng, shape, lo=30, hi=300, beta=0.6), 1.0), 0, 1)
    c = mix(c, lin(0.78, 0.76, 0.70), eff * 0.3)

    # sparse dark crustose lichen: irregular spots in a few colonies
    zone = smoothstep(1.3, 2.3, spectral(rng, shape, lo=2, hi=10, beta=1.2))
    lich, lich_v, lich_r = scatter(rng, shape, 45, 0.55, 3.0, 9.0, soft=2.0)
    edge_n = spectral(rng, shape, lo=60, hi=300, beta=0.5)
    lich = smoothstep(0.15, 0.7, lich - 0.3 * edge_n - 0.25 * lich_r ** 2) * smoothstep(0.2, 0.8, zone + 0.3 * lich_v - 0.3)
    lc = mix(np.broadcast_to(lin(0.09, 0.09, 0.075), shape + (3,)), lin(0.25, 0.25, 0.20), smoothstep(-1, 1.5, edge_n) * 0.6)
    c = mix(c, lc, lich * 0.9)

    h = (spectral(rng, shape, lo=1, hi=8, beta=1.8) * 0.5
         + spectral(rng, shape, lo=8, hi=80, beta=1.0, sx=4) * 0.1
         + n3 * 0.025 - pits * np.clip(1 - pit_r ** 2, 0, 1) * 0.4 + grit * 0.1 + lich * (0.1 + 0.05 * edge_n))
    ao = cavity_ao(h, [(1.5, 1.0), (6, 0.5)], 0.8)
    rough = np.clip(0.8 + 0.04 * n2 + 0.03 * n3 - 0.04 * flash + 0.08 * lich + 0.05 * eff, 0.7, 0.9)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(h, t, 1.5) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def _round_box(px, py, hx, hy, r):
    qx = np.abs(px) - (hx - r)
    qy = np.abs(py) - (hy - r)
    return (np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2)
            + np.minimum(np.maximum(qx, qy), 0) - r)


def set_floortile():
    S, world, N = 1024, 1200.0, 4
    t = world / S
    rng = np.random.default_rng(303)
    shape = (S, S)
    cell = world / N
    Y, X = (np.mgrid[0:S, 0:S] + 0.5) * t
    ex = spectral(rng, shape, lo=10, hi=60, beta=1.0) * 1.0
    ey = spectral(rng, shape, lo=10, hi=60, beta=1.0) * 1.0
    ci = np.floor(X / cell).astype(int) % N
    cj = np.floor(Y / cell).astype(int) % N
    tid = cj * N + ci
    T = N * N
    off_x, off_y = rng.normal(0, 0.8, T), rng.normal(0, 0.8, T)
    rot = rng.normal(0, 0.003, T)
    size_j = rng.normal(0, 0.8, T)
    lx = X - (ci + 0.5) * cell - off_x[tid] + ex
    ly = Y - (cj + 0.5) * cell - off_y[tid] + ey
    lx, ly = lx * np.cos(rot[tid]) - ly * np.sin(rot[tid]), lx * np.sin(rot[tid]) + ly * np.cos(rot[tid])
    half = cell / 2 - 4.0 + size_j[tid]
    d = _round_box(lx, ly, half, half, 9.0)
    inside = smoothstep(0.7, -0.7, d)

    pillow = rng.uniform(0.3, 1.0, T)
    tilt_x, tilt_y = rng.normal(0, 0.004, T), rng.normal(0, 0.004, T)
    lump = spectral(rng, shape, lo=4, hi=30, beta=1.4)
    fine = spectral(rng, shape, lo=100, hi=600, beta=0.3)
    th = (3.5 + 1.2 * smoothstep(0, 9, -d) + pillow[tid] * 0.9 * (1 - (lx ** 2 + ly ** 2) / (2 * half ** 2))
          + tilt_x[tid] * lx + tilt_y[tid] * ly + lump * 0.25 + fine * 0.015)
    gh = 1.0 + spectral(rng, shape, lo=150, hi=700, beta=0.2) * 0.12
    height = th * inside + gh * (1 - inside)
    height = lerp(height, blur(height, 0.8), 1 - inside)

    # Tile colour.
    hue = rng.random(T) ** 1.3
    bright = rng.uniform(0.88, 1.06, T)
    orange = lin(0.70, 0.37, 0.21)
    buff = lin(0.78, 0.53, 0.40)
    base = mix(np.broadcast_to(orange, shape + (3,)), buff, hue[tid]) * bright[tid][..., None]
    mot1 = spectral(rng, shape, lo=3, hi=40, beta=1.2)
    mot2 = spectral(rng, shape, lo=30, hi=250, beta=0.7)
    c = base * (1 + 0.07 * mot1 + 0.04 * mot2 + 0.04 * fine)[..., None]
    rim = smoothstep(40, 0, -d + 10 * mot1) * rng.uniform(0.4, 1.0, T)[tid]
    c = mix(c, c * lin(0.52, 0.27, 0.17) / lin(0.70, 0.40, 0.26), rim * 0.7)
    burn = smoothstep(0.6, 2.2, spectral(rng, shape, lo=3, hi=25, beta=1.3)) * (rng.random(T) < 0.5)[tid]
    c = mix(c, c * lin(0.50, 0.33, 0.25) / lin(0.70, 0.42, 0.28), burn * 0.6)
    gdir = rng.uniform(0, 2 * np.pi, T)
    side = (np.cos(gdir[tid]) * lx + np.sin(gdir[tid]) * ly) / half
    c *= (1 - 0.07 * rng.random(T)[tid] * (side + 1))[..., None]
    # dark firing spots (occasional) and pale caliche flecks
    spots = np.zeros(shape)
    for k in range(T):
        for _ in range(rng.poisson(1.4)):
            sy_ = ((k // N) + rng.uniform(0.1, 0.9)) * cell / t
            sx_ = ((k % N) + rng.uniform(0.1, 0.9)) * cell / t
            r = rng.uniform(5, 30) / t
            hw = int(r * 2.2) + 2
            wy, wx = window(hw, sy_, sx_)
            rr = np.sqrt(wx ** 2 + wy ** 2) / r
            rows = (np.arange(-hw, hw + 1) + int(sy_)) % S
            cols = (np.arange(-hw, hw + 1) + int(sx_)) % S
            nn = mot2[np.ix_(rows, cols)]
            stamp(spots, sy_, sx_, hw, smoothstep(1.1, 0.0, rr + 0.12 * nn) * rng.uniform(0.3, 0.7), "max")
    c = mix(c, c * (lin(0.36, 0.21, 0.14) / lin(0.70, 0.40, 0.26)), spots * inside)
    fleck, fleck_v, _ = scatter(rng, shape, 160, 0.07, 0.4, 1.3, soft=0.9)
    c = mix(c, lin(0.82, 0.74, 0.62), fleck * 0.4 * inside)

    grout_c = lin(0.50, 0.45, 0.37) * (1 + 0.08 * mot2 + 0.08 * spectral(rng, shape, lo=150, hi=700, beta=0.1))[..., None]
    grout_dirt = smoothstep(4, 0, d) * 0.3 + 0.2 * smoothstep(0.5, 2, mot1)
    grout_c = mix(grout_c, grout_c * 0.6, grout_dirt)
    c = mix(grout_c, c, inside)

    ao = cavity_ao(height, [(2, 1.0), (6, 0.8)], 0.22)
    base_r = rng.uniform(0.26, 0.36, T)
    smudge = smoothstep(0.2, 2.0, spectral(rng, shape, lo=3, hi=40, beta=1.0))
    tr = np.clip(base_r[tid] + 0.08 * smudge + 0.02 * mot2, 0.25, 0.45)
    rough = lerp(0.9, tr, inside)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(height, t, 1.0) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_paving():
    S, world, N = 1024, 2400.0, 12
    t = world / S
    rng = np.random.default_rng(404)
    shape = (S, S)
    cell = world / N
    Y, X = (np.mgrid[0:S, 0:S] + 0.5) * t
    ex = spectral(rng, shape, lo=10, hi=90, beta=1.3) * 1.0
    ey = spectral(rng, shape, lo=10, hi=90, beta=1.3) * 1.0
    Xw, Yw = X + ex, Y + ey
    ci = np.floor(Xw / cell).astype(int) % N
    cj = np.floor(Yw / cell).astype(int) % N
    lx = Xw - np.floor(Xw / cell) * cell
    ly = Yw - np.floor(Yw / cell) * cell
    horiz = (ci + cj) % 2 == 0
    b = np.where(horiz, (ly >= cell / 2), (lx >= cell / 2)).astype(int)
    bx = np.where(horiz, lx - cell / 2, ly - cell / 2)                   # along brick
    by = np.where(horiz, ly - cell / 4 - b * cell / 2, lx - cell / 4 - b * cell / 2)
    bid = (cj * N + ci) * 2 + b
    B = N * N * 2
    hx, hy = cell / 2 - 2.5, cell / 4 - 2.5
    sz = rng.normal(0, 0.8, B)
    d = _round_box(bx, by, hx + sz[bid], hy + sz[bid] * 0.5, 5.0)
    # chipped corners / edges: shallow broken scoops, irregular
    chip_n = spectral(rng, shape, lo=40, hi=250, beta=0.8) * 1.6
    dchip = np.full(shape, 99.0)
    for _ in range(2):
        has = rng.random(B) < 0.4
        sx_ = rng.choice([-1, 1], B)
        sy_ = rng.choice([-1, 1], B)
        along = np.where(rng.random(B) < 0.6, 1.0, rng.uniform(-0.8, 0.8, B))
        ccx = sx_ * hx * along
        ccy = sy_ * hy
        cr = rng.uniform(4, 11, B)
        dc = np.sqrt((bx - ccx[bid]) ** 2 + (by - ccy[bid]) ** 2) - cr[bid] + chip_n
        dchip = np.where(has[bid], np.minimum(dchip, dc), dchip)
    chipm = smoothstep(0.8, -0.8, dchip)
    inside = smoothstep(0.9, -0.9, d)

    wear = spectral(rng, shape, lo=8, hi=80, beta=1.2)
    fine = spectral(rng, shape, lo=150, hi=600, beta=0.3)
    bh = rng.normal(0, 1.0, B)
    tlx, tly = rng.normal(0, 0.008, B), rng.normal(0, 0.008, B)
    top = 6.0 + bh[bid] + tlx[bid] * bx + tly[bid] * by
    bevel = smoothstep(0, 10, -d)
    hb = top - 3.0 * (1 - bevel) ** 1.5 + wear * 0.35 + fine * 0.05
    pits, _, pr = scatter(rng, shape, 140, 0.35, 0.5, 1.5, soft=0.8)
    hb -= pits * 0.6
    hb = lerp(hb, top - 3.5 + chip_n * 0.6 + smoothstep(0, 6, -dchip) * 1.5 + fine * 0.15, chipm)
    sand_n = spectral(rng, shape, lo=200, hi=700, beta=0.1)
    hj = 1.0 + sand_n * 0.25 + spectral(rng, shape, lo=20, hi=200, beta=0.8) * 0.3
    height = hb * inside + hj * (1 - inside)

    # brick colours
    pal = np.array([lin(0.49, 0.29, 0.22), lin(0.43, 0.25, 0.19), lin(0.53, 0.34, 0.25),
                    lin(0.47, 0.34, 0.27), lin(0.34, 0.22, 0.18), lin(0.56, 0.41, 0.32)])
    pick = rng.choice(len(pal), B, p=[0.28, 0.2, 0.18, 0.16, 0.08, 0.10])
    pick2 = rng.choice(len(pal), B)
    blend = rng.uniform(0, 0.4, B)
    bc = pal[pick] * (1 - blend[:, None]) + pal[pick2] * blend[:, None]
    bc *= rng.uniform(0.9, 1.08, B)[:, None]
    c = bc[bid]
    mot = spectral(rng, shape, lo=10, hi=120, beta=1.0)
    c = c * (1 + 0.08 * mot + 0.06 * fine)[..., None]
    grad_dir = rng.choice([-1, 1], B) * rng.uniform(0, 1, B)
    endflash = smoothstep(0.2, 1.0, grad_dir[bid] * bx / hx)
    c = mix(c, c * 0.6, endflash * 0.5)
    c = mix(c, c * 0.5, pits * 0.6)
    worn = smoothstep(0.4, 1.8, wear) * bevel
    c = mix(c, c * lin(0.62, 0.50, 0.44) / lin(0.53, 0.30, 0.22), worn * 0.15)
    c = mix(c, c * (lin(0.40, 0.22, 0.16) / lin(0.49, 0.29, 0.22)), chipm * 0.8)
    edge_dirt = smoothstep(14, 0, -d) * (0.5 + 0.5 * smoothstep(-1, 1, mot))
    c = mix(c, c * lin(0.35, 0.30, 0.26) / lin(0.55, 0.45, 0.40), edge_dirt * 0.5)
    grime = smoothstep(0.3, 2.0, spectral(rng, shape, lo=1, hi=8, beta=1.4))
    c = mix(c, c * 0.75, grime * 0.5)
    dust = np.clip(0.12 + 0.1 * spectral(rng, shape, lo=2, hi=30, beta=1.2), 0, 0.4)
    c = mix(c, lin(0.56, 0.49, 0.41), dust)

    joint = lin(0.40, 0.36, 0.30) * (1 + 0.18 * sand_n)[..., None]
    joint = mix(joint, joint * 0.45, 0.5 * smoothstep(-0.5, 1.5, spectral(rng, shape, lo=8, hi=100, beta=0.8)))
    moss_zone = smoothstep(0.2, 1.8, spectral(rng, shape, lo=2, hi=20, beta=1.2) + 0.4 * mot)
    moss_edge = smoothstep(-4, 1.5, d + 2.5 * fine)
    moss = moss_zone * np.clip(moss_edge * (0.6 + 0.6 * sand_n), 0, 1)
    c = mix(joint, c, inside)
    c = mix(c, lin(0.17, 0.21, 0.09) * (1 + 0.3 * fine)[..., None], moss * 0.75)

    ao = cavity_ao(height, [(2, 1.0), (6, 0.8)], 0.16)
    rough = np.clip(0.79 + 0.04 * mot + 0.03 * fine - 0.05 * worn + 0.04 * pits + 0.05 * chipm, 0.75, 0.9)
    rough = lerp(0.92, rough, inside)
    rough = lerp(rough, 0.9, moss)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(height, t, 1.0) * 0.5 + 0.5),
                Surface=surface(rough, ao))


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


def set_greenpaint():
    S, world = 1024, 1000.0
    t = world / S
    rng = np.random.default_rng(606)
    shape = (S, S)
    phase = _grain_phase(rng, shape, 110, 6.0, sx=18)
    ring = phase - np.floor(phase)
    figure = smoothstep(-1.2, 1.2, spectral(rng, shape, lo=2, hi=20, beta=1.2, sx=10))
    late = smoothstep(0.5, 0.9, ring) * (1 - smoothstep(0.92, 1.0, ring)) * (0.3 + 0.7 * figure)
    fiber = spectral(rng, shape, lo=30, hi=500, beta=0.6, sx=40)

    # brush strokes laid along U, each with its own bristle offset
    bristle = spectral(rng, shape, lo=40, hi=350, beta=0.6, sx=60)
    strokes = np.zeros(shape)
    load = np.zeros(shape)
    for _ in range(260):
        cy, cx = rng.random() * S, rng.random() * S
        L = rng.uniform(150, 450) / t / 2
        Wd = rng.uniform(35, 70) / t / 2
        half = int(L + 4)
        dy, dx = window(half, cy, cx)
        e = np.maximum(np.abs(dx) / L, np.abs(dy) / Wd)
        m = smoothstep(1.0, 0.85, e) * (np.abs(dy) < Wd + 2)
        rows = (np.arange(-half, half + 1) + int(cy)) % S
        cols = (np.arange(-half, half + 1) + int(cx)) % S
        ix = np.ix_(rows, cols)
        off = rng.integers(0, S)
        br = bristle[(rows[:, None] + off) % S, cols[None, :]]
        thin = np.clip((dx / L + 1) / 2, 0, 1) if rng.random() < 0.5 else np.clip((1 - dx / L) / 2, 0, 1)
        strokes[ix] = strokes[ix] * (1 - m) + br * (0.6 + 0.6 * thin) * m
        load[ix] = load[ix] * (1 - m) + (1 - thin) * m
    strokes = blur(strokes, 0.6)

    chip_f = (spectral(rng, shape, lo=4, hi=40, beta=1.2, sx=3.5)
              + 0.35 * spectral(rng, shape, lo=40, hi=300, beta=0.4, sx=3.0))
    th = np.quantile(chip_f, 0.97)
    chip = smoothstep(th - 0.02, th + 0.02, chip_f)
    halo = np.clip(blur(chip, 10) * 3.0, 0, 1) * (1 - chip)

    wood_h = late * 0.06 + fiber * 0.025
    paint_h = 0.18 + wood_h * 0.8 + strokes * 0.018 + load * 0.01
    height = lerp(paint_h, wood_h - 0.05 + fiber * 0.03, chip)

    paint = lin(0.12, 0.30, 0.21)
    pv = spectral(rng, shape, lo=1, hi=20, beta=1.2)
    c = np.broadcast_to(paint, shape + (3,)) * (1 + 0.035 * pv + 0.018 * strokes + 0.03 * load)[..., None]
    fade = np.clip(halo * 0.8 + 0.25 * smoothstep(0.5, 2.0, spectral(rng, shape, lo=2, hi=12, beta=1.3)), 0, 1)
    c = mix(c, lin(0.24, 0.38, 0.31), fade * 0.6)
    grime = smoothstep(0.0, 0.03, blur(height, 3) - height)
    c = mix(c, c * 0.8, grime * 0.3)
    woodc = mix(np.broadcast_to(lin(0.50, 0.48, 0.44), shape + (3,)), lin(0.35, 0.34, 0.32), late * 0.6)
    woodc = woodc * (1 + 0.15 * fiber)[..., None]
    woodc = mix(woodc, lin(0.36, 0.40, 0.36), 0.25 * smoothstep(-1, 1, pv))   # old green stain in the wood
    c = mix(c, woodc, chip)
    inner = np.clip(chip - blur(chip, 1.2), 0, 1)        # shadowed lip just inside the chip
    outer = np.clip(blur(chip, 1.0) - chip, 0, 1)        # worn, lighter paint edge
    c = mix(c, c * 0.55, np.clip(inner * 4, 0, 1) * 0.6)

    ao = cavity_ao(height, [(1.5, 1.0), (5, 0.6)], 1.5)
    rough = np.clip(0.4 + 0.04 * pv + 0.06 * fade - 0.02 * strokes, 0.35, 0.5)
    rough = lerp(rough, 0.85, chip)
    return dict(BaseColor=rgba(finish_albedo(c, lo=0.01)),
                Normal=rgba(normal_from_height(height, t, 2.0) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_iron():
    S, world = 1024, 1000.0
    t = world / S
    rng = np.random.default_rng(707)
    shape = (S, S)
    f1, f2, cid, dy, dx = voronoi(rng, shape, 80, 80, jitter=0.9)
    big = np.clip((f1 / (S / 80 * 0.75)) ** 2, 0, 1.3) * rng.uniform(0.6, 1.4, 80 * 80)[cid]
    f1b, _, cidb, _, _ = voronoi(rng, shape, 170, 170, jitter=0.9)
    small = np.clip((f1b / (S / 170 * 0.75)) ** 2, 0, 1.3) * rng.uniform(0.3, 1.0, 170 * 170)[cidb]
    peen = blur(big * 0.55 + small * 0.14, 0.8)
    forge = spectral(rng, shape, lo=1, hi=10, beta=1.8) * 0.25
    scale = spectral(rng, shape, lo=100, hi=600, beta=0.3)
    ridge = smoothstep(0.05, 0.2, peen - blur(peen, 4))

    rust_f = (spectral(rng, shape, lo=2, hi=24, beta=1.5) + 0.22 * spectral(rng, shape, lo=24, hi=200, beta=0.8)
              - 0.8 * (peen - blur(peen, 6)))
    th = np.quantile(rust_f, 0.92)
    rust = smoothstep(th - 0.05, th + 0.35, rust_f)
    rust_grain = spectral(rng, shape, lo=80, hi=500, beta=0.4)
    rust_spots, _, _ = scatter(rng, shape, 90, 0.2, 0.8, 3.0, soft=2.0)
    rust = np.clip(rust + rust_spots * 0.7 * smoothstep(th - 0.6, th, rust_f), 0, 1)
    rust = blur(rust, 1.0)

    height = -peen + forge + scale * 0.012 + rust * (0.08 + 0.04 * rust_grain)
    iron_v = spectral(rng, shape, lo=2, hi=40, beta=1.0)
    gray = to_linear(np.clip(0.075 + 0.008 * iron_v + 0.01 * ridge + 0.004 * scale, 0.055, 0.095))
    c = np.stack([gray * 0.98, gray, gray * 1.03], -1)
    rv = spectral(rng, shape, lo=10, hi=120, beta=0.8)
    rc = mix(np.broadcast_to(lin(0.14, 0.085, 0.05), shape + (3,)), lin(0.29, 0.155, 0.075),
             smoothstep(-0.5, 1.5, rv + 0.5 * rust_grain + 1.5 * (rust - 0.5)))
    rc = mix(rc, lin(0.42, 0.22, 0.09), smoothstep(1.8, 3.0, rv + rust_grain + rust) * 0.6)
    rc *= (1 + 0.12 * rust_grain)[..., None]
    rmix = smoothstep(0.0, 1.0, rust)
    stain = np.clip(blur(rust, 6) * 1.5 - rust, 0, 1)
    c = mix(c, lin(0.10, 0.075, 0.06), stain * 0.6)
    c = mix(c, rc, rmix)

    ao = cavity_ao(height, [(2, 1.0), (6, 0.6)], 1.5)
    rough_i = np.clip(0.54 + 0.06 * iron_v - 0.08 * ridge + 0.04 * scale, 0.45, 0.65)
    rough = lerp(rough_i, 0.9, rmix)
    metal = lerp(0.75, 0.0, rmix)
    return dict(BaseColor=rgba(finish_albedo(c, lo=0.004)),
                Normal=rgba(normal_from_height(height, t, 1.4) * 0.5 + 0.5),
                Surface=surface(rough, ao, metal))


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


def _lens(u, v, u0, u1, w):
    """Approximate distance to a pointed leaf lying along u from u0 to u1, half width w."""
    t = (u - u0) / (u1 - u0)
    half = w * np.clip(1 - (2 * t - 1) ** 2, 0, 1)
    inside_len = (t > 0) & (t < 1)
    return np.where(inside_len, np.abs(v) - half, np.minimum(np.abs(u - u0), np.abs(u - u1)) + np.abs(v))


def _flower(r, th, r0, r1, petals, power=0.7):
    return r - (r0 + (r1 - r0) * np.abs(np.cos(th * petals / 2)) ** power)


def _talavera_motif(px, py):
    """One tile's design in tile-local coords (-1..1, D4 symmetric), as distances
    (< 0 inside): blue, ochre, and a blue outline. Corners and edge midpoints
    carry quarter / half motifs that complete across the joints."""
    ax, ay = np.abs(px), np.abs(py)
    r = np.sqrt(px * px + py * py)
    th = np.arctan2(py, px)
    # centre: blue medallion holding an ochre eight-petal flower and a blue eye
    med = r - 0.36
    flw = _flower(r, th, 0.09, 0.28, 8)
    eye = r - 0.055
    dots = np.full(px.shape, 9.0)
    for i in range(16):
        a = i * np.pi / 8
        dots = np.minimum(dots, np.sqrt((px - 0.45 * np.cos(a)) ** 2 + (py - 0.45 * np.sin(a)) ** 2) - 0.028)
    # diagonals: blue leaves reaching toward the corner rosettes
    u = (ax + ay) / np.sqrt(2)
    v = (ax - ay) / np.sqrt(2)
    leaf = _lens(u, v, 0.50, 0.86, 0.075)
    leaf_vein = np.where((u > 0.52) & (u < 0.8), np.abs(v) - 0.008, 9.0)
    # axes: ochre teardrops with a blue outline, toward the edge motifs
    ua, va = np.maximum(ax, ay), np.minimum(ax, ay)
    drop = _lens(ua, va, 0.52, 0.76, 0.06)
    # corners: quarter of a blue rosette with a cream flower and ochre heart
    cx, cy = 1 - ax, 1 - ay
    cr = np.sqrt(cx * cx + cy * cy)
    cth = np.arctan2(cy, cx)
    cdisc = cr - 0.32
    cflw = _flower(cr, cth, 0.07, 0.24, 8)
    cheart = cr - 0.07
    # edge midpoints: half of a blue ring with an ochre centre
    ex = np.minimum(np.sqrt((ax - 1) ** 2 + py ** 2), np.sqrt(px ** 2 + (ay - 1) ** 2))
    ering = np.abs(ex - 0.15) - 0.045
    ecentre = ex - 0.07

    blue = np.minimum.reduce([np.maximum(med, -flw), eye, dots, leaf,
                              np.maximum(cdisc, -cflw), ering])
    blue = np.maximum(blue, -np.minimum(leaf_vein, 9.0) * (leaf < 0))
    ochre = np.minimum.reduce([np.maximum(flw, -eye), drop, cheart, ecentre])
    outline = np.abs(drop) - 0.009
    return blue, ochre, outline


def set_talavera():
    S, world, N = 1024, 600.0, 4
    t = world / S
    rng = np.random.default_rng(909)
    shape = (S, S)
    cell = world / N
    Y, X = (np.mgrid[0:S, 0:S] + 0.5) * t
    ci = np.floor(X / cell).astype(int) % N
    cj = np.floor(Y / cell).astype(int) % N
    tid = cj * N + ci
    T = N * N
    lx = X - (ci + 0.5) * cell
    ly = Y - (cj + 0.5) * cell
    jx, jy = rng.normal(0, 0.25, T), rng.normal(0, 0.25, T)
    d = _round_box(lx - jx[tid], ly - jy[tid], cell / 2 - 1.5, cell / 2 - 1.5, 2.0)
    inside = smoothstep(0.35, -0.35, d)

    # hand painting: wobbly coords, per-tile pressure
    wob = 0.012
    wx = spectral(rng, shape, lo=8, hi=60, beta=1.0) * wob
    wy = spectral(rng, shape, lo=8, hi=60, beta=1.0) * wob
    px = (lx / (cell / 2)) + wx
    py = (ly / (cell / 2)) + wy
    blue_d, ochre_d, outline_d = _talavera_motif(px, py)
    soft = 0.006
    blue = smoothstep(soft, -soft, blue_d)
    ochre = smoothstep(soft, -soft, ochre_d)
    outline = smoothstep(soft, -soft, outline_d)
    bleed = 0.8
    blue, ochre, outline = blur(blue, bleed), blur(ochre, bleed), blur(outline, 0.6)
    pool = np.clip(blue - blur(blue, 3), 0, 1)
    dens_n = spectral(rng, shape, lo=10, hi=200, beta=0.8)
    pressure = rng.uniform(0.85, 1.0, T)[tid]
    blue_a = np.clip(blue * (0.86 + 0.12 * dens_n) * pressure + 0.5 * pool, 0, 1)
    blue_a = np.maximum(blue_a, outline * 0.95)
    ochre_a = np.clip(ochre * (0.7 + 0.2 * dens_n), 0, 1) * (1 - blue_a)

    ground = lin(0.90, 0.87, 0.78) * (1 + 0.025 * spectral(rng, shape, lo=2, hi=40, beta=1.0)
                                      + 0.012 * spectral(rng, shape, lo=100, hi=500, beta=0.3))[..., None]
    ground *= rng.uniform(0.97, 1.02, T)[tid][..., None]
    light_blue = lin(0.32, 0.45, 0.74)
    deep_blue = lin(0.10, 0.20, 0.55)
    bc = mix(np.broadcast_to(light_blue, shape + (3,)), deep_blue, smoothstep(0.35, 0.85, blue_a))
    c = mix(ground, bc, smoothstep(0.0, 0.35, blue_a))
    oc = mix(np.broadcast_to(lin(0.90, 0.72, 0.30), shape + (3,)), lin(0.76, 0.46, 0.12), ochre_a)
    c = mix(c, oc, smoothstep(0.0, 0.4, ochre_a) * 0.95)

    f1, f2, _, _, _ = voronoi(rng, shape, 110, 110, jitter=1.0)
    craze = smoothstep(0.9, 0.0, f2 - f1) * np.clip(0.4 + 0.6 * spectral(rng, shape, lo=3, hi=30, beta=1.0), 0, 1)
    c = mix(c, c * 0.88, craze * 0.6 * inside)
    pin, _, _ = scatter(rng, shape, 90, 0.12, 0.3, 0.8, soft=0.6)
    c = mix(c, c * 0.7, pin * 0.6)

    grout = lin(0.62, 0.60, 0.55) * (1 + 0.1 * spectral(rng, shape, lo=150, hi=700, beta=0.1))[..., None]
    grout = mix(grout, grout * 0.7, smoothstep(2, 0, d))
    c = mix(grout, c, inside)

    peel = spectral(rng, shape, lo=20, hi=200, beta=1.0)
    glaze_h = (2.0 + 0.8 * smoothstep(0, 4.0, -d) + 0.08 * (1 - (lx ** 2 + ly ** 2) / (2 * (cell / 2) ** 2))
               + peel * 0.006 + blue_a * 0.008 - craze * 0.004 - pin * 0.03)
    gh = 0.6 + spectral(rng, shape, lo=150, hi=700, beta=0.1) * 0.05
    height = glaze_h * inside + gh * (1 - inside)
    ao = cavity_ao(height, [(2, 1.0), (6, 0.7)], 0.2)
    rough = np.clip(0.13 + 0.025 * spectral(rng, shape, lo=2, hi=40, beta=1.0) + 0.03 * craze + 0.02 * blue_a,
                    0.1, 0.2)
    rough = lerp(0.85, rough, inside)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(height, t, 1.0) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_ground():
    S, world = 1024, 4000.0
    t = world / S
    rng = np.random.default_rng(1212)
    shape = (S, S)
    macro = spectral(rng, shape, lo=1, hi=10, beta=1.5)
    mid = spectral(rng, shape, lo=10, hi=120, beta=1.0)
    fine = spectral(rng, shape, lo=200, hi=700, beta=0.1)
    base = lin(0.66, 0.57, 0.45)
    c = np.broadcast_to(base, shape + (3,)) * (1 + 0.05 * macro + 0.05 * mid + 0.08 * fine)[..., None]
    # mineral grains: quartz white, feldspar pink, biotite dark
    g1, g1v, _ = scatter(rng, shape, 300, 0.6, 0.35, 0.9, soft=0.7)
    c = mix(c, np.where((g1v < 0.4)[..., None], lin(0.85, 0.82, 0.76),
                        np.where((g1v < 0.7)[..., None], lin(0.72, 0.55, 0.46), lin(0.22, 0.20, 0.18))), g1 * 0.7)

    # pebbles
    peb_h = np.zeros(shape)
    peb_c = np.zeros(shape + (3,))
    peb_m = np.zeros(shape)
    pal = np.array([lin(0.58, 0.54, 0.48), lin(0.70, 0.64, 0.55), lin(0.45, 0.40, 0.34),
                    lin(0.62, 0.48, 0.38), lin(0.35, 0.33, 0.31), lin(0.78, 0.74, 0.66)])
    for n, prob, rmin, rmax in ((30, 0.5, 2.0, 5.0), (70, 0.55, 1.0, 2.6), (140, 0.5, 0.7, 1.5)):
        f1, f2, cid, dy, dx = voronoi(rng, shape, n, n, jitter=0.8)
        cells = n * n
        present = rng.random(cells) < prob * np.clip(1, 0, 1)
        r = (rmin + (rmax - rmin) * rng.random(cells) ** 1.5)[cid]
        ang = rng.uniform(0, np.pi, cells)[cid]
        el = rng.uniform(1.0, 1.7, cells)[cid]
        u = dx * np.cos(ang) + dy * np.sin(ang)
        v = -dx * np.sin(ang) + dy * np.cos(ang)
        rr = np.sqrt((u / el) ** 2 + v ** 2) / r
        m = smoothstep(1.05, 0.85, rr) * present[cid]
        dome = np.sqrt(np.clip(1 - rr ** 2, 0, 1)) * r * t * 0.45
        pc = pal[rng.integers(0, len(pal), cells)][cid] * (1 + 0.1 * fine)[..., None]
        sel = m > peb_m
        peb_h = np.where(sel, dome * m, peb_h)
        peb_c = np.where(sel[..., None], pc, peb_c)
        peb_m = np.maximum(peb_m, m)
    density = smoothstep(-1.2, 1.0, mid + 0.6 * macro)
    peb_m *= density
    dust_on = 0.1 + 0.25 * smoothstep(0, 1.5, macro)
    peb_c = mix(peb_c, c, dust_on)
    c = mix(c, peb_c, peb_m)
    ring = np.clip(blur(peb_m, 1.5) - peb_m, 0, 1)
    c = mix(c, c * 0.7, ring * 1.5)

    damp = smoothstep(1.0, 2.6, warp(spectral(rng, shape, lo=2, hi=14, beta=1.4), mid * 6, macro * 6) + 0.25 * mid)
    c = mix(c, c * lin(0.50, 0.43, 0.35) / lin(0.66, 0.57, 0.45), damp * 0.5)

    # sparse dry grass: irregular clumps of thin curved blades, plus loose straw
    grass = np.zeros(shape)
    gtone = np.zeros(shape)

    def blade(y, x, a, L, curl, tone):
        n = max(int(L * 2), 4)
        k = np.linspace(0, 1, n)
        ang = a + curl * k
        ys = y + np.cumsum(np.sin(ang)) * L / n
        xs = x + np.cumsum(np.cos(ang)) * L / n
        idx = (ys.astype(int) % S, xs.astype(int) % S)
        wgt = 0.5 * (1 - 0.6 * k)
        np.add.at(grass, idx, wgt)
        np.add.at(gtone, idx, wgt * tone)

    for _ in range(30):
        cy, cx = rng.random() * S, rng.random() * S
        lean = rng.uniform(0, 2 * np.pi)
        spread = rng.uniform(0.4, 1.3)
        size = rng.uniform(0.7, 1.8)
        for _ in range(int(rng.integers(40, 110) * size)):
            r0 = abs(rng.normal(0, 2.5 * size))
            a0 = rng.uniform(0, 2 * np.pi)
            blade(cy + np.sin(a0) * r0, cx + np.cos(a0) * r0, lean + rng.normal(0, spread),
                  rng.uniform(4, 22) * size, rng.normal(0, 0.8), rng.random())
    for _ in range(500):
        blade(rng.random() * S, rng.random() * S, rng.uniform(0, 2 * np.pi), rng.uniform(2, 8), rng.normal(0, 0.5),
              rng.random())
    tone = np.clip(gtone / np.maximum(grass, 1e-6), 0, 1)
    grass = np.clip(blur(grass, 0.9) * 3.0, 0, 1)
    tone = blur(tone, 0.6)
    gc = mix(np.broadcast_to(lin(0.76, 0.68, 0.51), shape + (3,)), lin(0.58, 0.53, 0.41), tone)
    c = mix(c, c * 0.8, np.clip(blur(grass, 3) * 1.5, 0, 0.4))
    c = mix(c, gc, grass * 0.85)

    height = (macro * 1.5 + mid * 0.4 + fine * 0.15 + g1 * 0.3 + peb_h * peb_m + grass * 0.4)
    ao = cavity_ao(height, [(1.5, 1.0), (5, 0.6)], 0.25)
    c = mix(c, c * 0.8, (1 - ao) * 0.8)
    rough = np.clip(0.9 + 0.02 * fine + 0.02 * mid - 0.05 * damp - 0.03 * peb_m, 0.85, 0.95)
    return dict(BaseColor=rgba(finish_albedo(c)), Normal=rgba(normal_from_height(height, t, 1.2) * 0.5 + 0.5),
                Surface=surface(rough, ao))


def set_glass():
    S, world = 512, 600.0
    t = world / S
    rng = np.random.default_rng(1010)
    shape = (S, S)
    # cylinder glass: long waves drawn along U, a finer ripple, sparse seed bubbles
    # drawn out along U. Slopes are set directly (rms), kept small.
    wave = spectral(rng, shape, lo=2, hi=24, beta=1.5, sx=12)
    wave *= 0.012 / slope_rms(wave, t)
    wave2 = spectral(rng, shape, lo=8, hi=70, beta=1.0, sx=5)
    wave2 *= 0.004 / slope_rms(wave2, t)
    ripple = spectral(rng, shape, lo=12, hi=60, beta=1.0)
    ripple *= 0.0025 / slope_rms(ripple, t)
    bub, _, br = scatter(rng, shape, 24, 0.3, 0.5, 1.4, soft=0.6, aspect=2.4)
    height = wave + wave2 + ripple + bub * np.sqrt(np.clip(1 - br ** 2, 0, 1)) * 0.05
    n = normal_from_height(height, t, 1.0)
    smudge = smoothstep(0.7, 2.4, warp(spectral(rng, shape, lo=2, hi=25, beta=1.3),
                                       spectral(rng, shape, lo=4, hi=30) * 6, spectral(rng, shape, lo=4, hi=30) * 6))
    dust = (np.clip(0.5 + 0.5 * spectral(rng, shape, lo=60, hi=250, beta=0.4), 0, 1)
            * smoothstep(-0.5, 2.0, spectral(rng, shape, lo=1, hi=6, beta=1.5)))
    grime = np.clip(smudge * 0.75 + dust * 0.3, 0, 1)
    rough = np.clip(0.055 + 0.012 * spectral(rng, shape, lo=4, hi=60, beta=1.0), 0.03, 0.08)
    rough = np.clip(rough + smudge * 0.22 + dust * 0.03, 0, 0.3)
    s = np.stack([rough, grime, np.zeros(shape), np.ones(shape)], -1)
    return dict(Normal=rgba(n * 0.5 + 0.5), Surface=s)


def set_water():
    S, world = 512, 1000.0
    t = world / S
    rng = np.random.default_rng(1111)
    shape = (S, S)
    white = rng.standard_normal(shape)
    fy, fx = _freqs(S, S)
    f = np.sqrt(fx ** 2 + fy ** 2) + 1e-9
    peak = 28.0          # ~3.5 cm wavelength
    filt = np.exp(-((np.log(f / peak)) ** 2) / (2 * 0.45 ** 2)) / f
    filt[0, 0] = 0
    h = np.fft.irfft2(np.fft.rfft2(white) * filt, s=shape)
    h /= h.std()
    for _ in range(4):
        kx, ky = rng.integers(-24, 25), rng.integers(-24, 25)
        h += 0.3 * np.sin(2 * np.pi * (kx * np.arange(S)[None, :] + ky * np.arange(S)[:, None]) / S + rng.uniform(0, 6.28))
    h *= 0.07 / slope_rms(h, t)   # gentle: ~4 degrees rms
    n = normal_from_height(h, t, 1.0)
    return dict(Normal=rgba(n * 0.5 + 0.5))


def set_foliage():
    S = 1024
    rng = np.random.default_rng(1313)
    shape = (S, S)
    cov = np.zeros(shape)
    colr = np.zeros(shape + (3,))
    nrm = np.zeros(shape + (3,))
    nrm[..., 2] = 1
    rough = np.full(shape, 0.45)
    ao = np.ones(shape)

    margin = 30.0

    def grow(p, a, length, wander, step=8.0):
        """A twig from p heading a, stopping short of the card's edge."""
        pts = [np.array(p, dtype=float)]
        q = pts[0].copy()
        for _ in range(int(length / step)):
            a += rng.normal(0, wander)
            q = q + np.array([np.cos(a), np.sin(a)]) * step
            if not (margin < q[0] < S - margin and margin < q[1] < S - margin):
                break
            pts.append(q.copy())
        return np.array(pts)

    # a central stem rising from the bottom, alternate branches off it, a few sub-branches
    stem = grow((S * 0.5, S - margin - 1), -np.pi / 2 + rng.normal(0, 0.05), S * 0.78, 0.035)
    twigs = [stem]
    side = 1
    for frac in np.linspace(0.12, 0.85, 10):
        i0 = int(frac * (len(stem) - 1))
        d = stem[min(i0 + 1, len(stem) - 1)] - stem[i0 - 1]
        up = np.arctan2(d[1], d[0])
        twigs.append(grow(stem[i0], up + side * rng.uniform(0.7, 1.25), rng.uniform(300, 460) * (1.1 - 0.45 * frac), 0.06))
        side = -side
    for _ in range(5):
        parent = twigs[1 + rng.integers(0, 10)]
        if len(parent) < 8:
            continue
        i0 = rng.integers(3, len(parent) - 3)
        d = parent[i0 + 1] - parent[i0 - 1]
        twigs.append(grow(parent[i0], np.arctan2(d[1], d[0]) + rng.choice([-1, 1]) * rng.uniform(0.5, 0.9),
                          rng.uniform(80, 180), 0.08))

    yy, xx = np.mgrid[0:S, 0:S] + 0.5
    twig_m = np.zeros(shape)
    for tw in twigs:
        seg = np.sqrt((np.diff(tw, axis=0) ** 2).sum(1))
        arc = np.concatenate([[0], np.cumsum(seg)])
        k = np.arange(0, arc[-1], 1.0)
        dense = np.stack([np.interp(k, arc, tw[:, 0]), np.interp(k, arc, tw[:, 1])], -1)
        for i, (x0, y0) in enumerate(dense):
            w = 4.5 * (1 - i / len(dense)) + 1.8
            hw = int(w) + 3
            if not (hw <= x0 < S - hw and hw <= y0 < S - hw):
                continue
            wy, wx = window(hw, y0, x0)
            stamp(twig_m, y0, x0, hw, smoothstep(w, w - 1.2, np.sqrt(wx ** 2 + wy ** 2)), "max")
    twig_c = lin(0.30, 0.24, 0.17)
    colr = mix(colr, twig_c, twig_m)
    cov = np.maximum(cov, twig_m)
    tw_n = normal_from_height(blur(twig_m, 1.5) * 4, 1.0, 1.0)
    nrm = mix(nrm, tw_n, twig_m)
    rough = lerp(rough, 0.7, twig_m)

    # leaves along the twigs, alternating sides; drop any that would leave the card
    cand = []
    for tw in twigs[1:] + [twigs[0][len(twigs[0]) // 3:]]:
        flip = 1
        for i in range(2, len(tw) - 1, int(rng.integers(4, 6))):
            d = tw[min(i + 1, len(tw) - 1)] - tw[i - 1]
            cand.append((tw[i], np.arctan2(d[1], d[0]) + flip * rng.uniform(0.5, 1.0)))
            flip = -flip
        d = tw[-1] - tw[max(len(tw) - 3, 0)]
        cand.append((tw[-1], np.arctan2(d[1], d[0]) + rng.normal(0, 0.2)))
    leaves = []
    for (p, a) in cand:
        L = rng.uniform(190, 280)
        for _ in range(3):
            pet = 14.0
            dvec = np.array([np.cos(a), np.sin(a)])
            perp = np.array([-dvec[1], dvec[0]])
            W = L * 0.45
            probes = [p + dvec * (pet + L), p + dvec * (pet + 0.4 * L) + perp * W * 0.55,
                      p + dvec * (pet + 0.4 * L) - perp * W * 0.55]
            if all(6 < q[0] < S - 6 and 6 < q[1] < S - 6 for q in probes):
                leaves.append((p, a, L))
                break
            L *= 0.8
    # keep 40, spread out: greedy farthest-first on the leaf centres
    centres = np.array([p + np.array([np.cos(a), np.sin(a)]) * (14 + L / 2) for (p, a, L) in leaves])
    chosen = [int(rng.integers(0, len(leaves)))]
    dist = np.linalg.norm(centres - centres[chosen[0]], axis=1)
    while len(chosen) < min(40, len(leaves)):
        k = int(np.argmax(dist + rng.random(len(dist)) * 20))
        chosen.append(k)
        dist = np.minimum(dist, np.linalg.norm(centres - centres[k], axis=1))
    leaves = [leaves[i] for i in rng.permutation(chosen)]
    greens = [lin(0.13, 0.24, 0.07), lin(0.18, 0.31, 0.09), lin(0.24, 0.37, 0.11),
              lin(0.10, 0.19, 0.06), lin(0.36, 0.43, 0.13)]
    vein_n = spectral(rng, shape, lo=6, hi=60, beta=1.0)
    for (p, a, L) in leaves:
        W = L * rng.uniform(0.40, 0.50)
        bend = rng.normal(0, 0.12)
        pet = 14.0
        cx, cy = p
        hw = int(L + pet + 10)
        x0, x1 = max(0, int(cx) - hw), min(S, int(cx) + hw + 1)
        y0, y1 = max(0, int(cy) - hw), min(S, int(cy) + hw + 1)
        X = xx[y0:y1, x0:x1] - cx
        Yl = yy[y0:y1, x0:x1] - cy
        u = X * np.cos(a) + Yl * np.sin(a) - pet
        v = -X * np.sin(a) + Yl * np.cos(a)
        tt = u / L
        v = v - bend * L * tt * tt
        tc = np.clip(tt, 0, 1)
        half_w = W * 0.5 * (tc ** 0.55) * (1 - tc) ** 0.9 / (0.38 ** 0.55 * 0.62 ** 0.9)
        half_w *= np.where(tt > 0.85, 1 - 0.25 * (tt - 0.85) / 0.15, 1)
        e = np.abs(v) - half_w
        leaf = smoothstep(0.8, -0.8, e) * (tt > 0) * (tt < 1)
        petiole = smoothstep(2.6, 1.6, np.abs(v)) * (u > -pet) * (u < 4)
        m = np.clip(np.maximum(leaf, petiole), 0, 1)
        if m.sum() == 0:
            continue
        rel = np.clip(np.abs(v) / np.maximum(half_w, 1e-3), 0, 1)
        midrib = smoothstep(2.2, 0.6, np.abs(v)) * (tt > 0) * (tt < 0.95)
        veins = smoothstep(0.82, 1.0, np.cos((u - np.abs(v) * 0.9) / (L * 0.07) * np.pi * 2 * 0.5) ** 2) * (1 - rel) * (1 - midrib) * 0.5
        g = greens[rng.choice(len(greens), p=[0.25, 0.3, 0.2, 0.15, 0.1])]
        g = g * rng.uniform(0.85, 1.12)
        vn = vein_n[y0:y1, x0:x1]
        lc = np.broadcast_to(g, m.shape + (3,)) * (1 + 0.08 * vn + 0.12 * (1 - rel) - 0.1 * tc)[..., None]
        lc = mix(lc, lc * 1.6 * np.array([1.1, 1.05, 0.7]), midrib * 0.8 + veins * 0.4)
        lc = mix(lc, lc * 1.25, smoothstep(0.85, 1.0, rel) * 0.4)
        # leaf height: cupped across, arched along, midrib groove
        lh = (np.sin(np.pi * tc) * 4.0 + rel ** 2 * 3.0 * rng.uniform(0.6, 1.2)
              - midrib * 0.8 + veins * 0.15)
        gy, gx = np.gradient(lh)
        n = np.stack([-gx * 0.6, gy * 0.6, np.ones_like(lh)], -1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        sl = (slice(y0, y1), slice(x0, x1))
        # shadow the leaves underneath along this leaf's edge
        sh = blur(np.pad(m, 8), 5)[8:-8, 8:-8] if min(m.shape) > 20 else m
        ao[sl] = np.where(m > 0.5, ao[sl], np.minimum(ao[sl], 1 - 0.45 * sh * cov[sl]))
        colr[sl] = mix(colr[sl], lc, m)
        nrm[sl] = mix(nrm[sl], n, m)
        rough[sl] = lerp(rough[sl], np.clip(0.38 + 0.05 * vn + 0.05 * midrib, 0.35, 0.5), m)
        ao[sl] = lerp(ao[sl], 1.0, m * 0.8)
        cov[sl] = np.maximum(cov[sl], m)

    colr = colr * ao[..., None]
    # colour bleed into gaps (push-pull dilation)
    filled = colr.copy()
    w = cov.copy()
    for sigma in (2, 6, 18, 60, 200):
        num = blur(colr * cov[..., None], sigma)
        den = blur(cov, sigma)[..., None]
        est = num / np.maximum(den, 1e-6)
        need = (w < 0.99)[..., None] & (den > 1e-4)
        filled = np.where(need, filled * w[..., None] + est * (1 - w[..., None]), filled)
        w = np.maximum(w, np.clip(den[..., 0] * 4, 0, 1))
    filled = np.where(cov[..., None] > 0.99, colr, filled)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    nrm = mix(np.broadcast_to(np.array([0, 0, 1.0]), nrm.shape), nrm, smoothstep(0, 0.5, cov))
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    return dict(BaseColor=rgba(finish_albedo(filled, lo=0.008), cov),
                Normal=rgba(nrm * 0.5 + 0.5), Surface=surface(rough, ao))


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
    "Stucco": (set_stucco, 2.0),
    "RoofTile": (set_rooftile, 1.0),
    "FloorTile": (set_floortile, 1.2),
    "Paving": (set_paving, 2.4),
    "Wood": (set_wood, 2.0),
    "GreenPaint": (set_greenpaint, 1.0),
    "Iron": (set_iron, 1.0),
    "Stone": (set_stone, 1.5),
    "Talavera": (set_talavera, 0.6),
    "Ground": (set_ground, 4.0),
    "Glass": (set_glass, 0.6),
    "Water": (set_water, 1.0),
    "Foliage": (set_foliage, None),
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
