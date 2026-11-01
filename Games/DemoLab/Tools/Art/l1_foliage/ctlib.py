"""ctlib.py -- texture helpers copied from WorldsLab Tools/Courtyard/make_courtyard_textures.py
(noise, normals, coverage-kept alpha mips, the .nmtexture and PNG writers) for the L1 foliage
generators beside it."""

import struct
import zlib
from pathlib import Path

import numpy as np

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


