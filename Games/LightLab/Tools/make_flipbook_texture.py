#!/usr/bin/env python3
"""
make_flipbook_texture.py -- writes Content/Textures/ExplosionFlipbook.nmtexture,
the Explosion particle system's sprite sheet (the Particle Editor's Flipbook
module): a fireball's life in 16 frames, 4 x 4 cells of 128 x 128, read left
to right then top to bottom.

    frames 0-3    a white-hot core, swelling fast
    frames 4-9    yellow to orange to red, billowing out
    frames 10-15  grey smoke, thinning from its wisps until it is gone

Each frame is a ball whose edge is pushed in and out by fractal value noise
in (x, y, time), so neighbouring frames match and the sheet plays smoothly.
Its colour is a heat ramp, heat falling with time and toward the edge; its
alpha the ball's density. Every cell's border is transparent, so filtering
(and the smaller mips) never shows the next frame.

The file is written as RenderCore's TextureAsset.cpp reads one: NTEX holding
THDR (version 3), TPIX and TMIP -- sRGB RGBA8, linear filtering, clamped, a
full Box mip chain averaged in linear light. Deterministic: running it again
writes the same bytes. Needs numpy. Usage:

    python Games/LightLab/Tools/make_flipbook_texture.py
"""

import struct
from pathlib import Path

import numpy as np

CELL = 128
GRID = 4
SIZE = CELL * GRID
FRAMES = GRID * GRID
OUTPUT = Path(__file__).resolve().parent.parent / "Content" / "Textures" / "ExplosionFlipbook.nmtexture"

# --- noise ------------------------------------------------------------------------------------

LATTICE = 32
RANDOM = np.random.default_rng(1977).random((LATTICE, LATTICE, LATTICE))


def value_noise(x, y, z):
    """Smooth value noise in [0, 1], tiling every LATTICE units."""
    xi, yi, zi = np.floor(x).astype(int), np.floor(y).astype(int), np.floor(z).astype(int)
    fx, fy, fz = x - xi, y - yi, z - zi
    fx, fy, fz = (f * f * (3.0 - 2.0 * f) for f in (fx, fy, fz))
    total = 0.0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                corner = RANDOM[(xi + dx) % LATTICE, (yi + dy) % LATTICE, (zi + dz) % LATTICE]
                weight = (fx if dx else 1.0 - fx) * (fy if dy else 1.0 - fy) * (fz if dz else 1.0 - fz)
                total = total + corner * weight
    return total


def fbm(x, y, z, octaves=4):
    total, amplitude, norm = 0.0, 1.0, 0.0
    for _ in range(octaves):
        total = total + value_noise(x, y, z) * amplitude
        norm += amplitude
        x, y, z, amplitude = x * 2.0 + 17.0, y * 2.0 + 31.0, z * 1.7 + 7.0, amplitude * 0.5
    return total / norm


def smoothstep(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# --- frames -----------------------------------------------------------------------------------

# Heat to colour (sRGB, 0-1): smoke, ember red, orange, yellow, white.
RAMP_HEAT = np.array([0.0, 0.2, 0.45, 0.7, 1.0])
RAMP_COLOR = np.array([[0.36, 0.33, 0.31],
                       [0.55, 0.10, 0.03],
                       [1.00, 0.42, 0.06],
                       [1.00, 0.80, 0.30],
                       [1.00, 0.97, 0.88]])


def frame(index):
    """Cell index's RGBA, floats 0-1, CELL x CELL x 4."""
    t = index / (FRAMES - 1)
    coords = (np.arange(CELL) + 0.5) / CELL * 2.0 - 1.0
    px, py = np.meshgrid(coords, coords)            # py: down the image
    radius = np.sqrt(px * px + py * py)

    reach = 0.42 + 0.44 * (1.0 - (1.0 - t) ** 2)    # swells fast, then slows
    n = fbm(px * 2.8 + 5.3, py * 2.8 + 9.7 + t * 1.5, t * 2.0)   # billows drift up the frame
    shape = 1.0 - radius / reach + (n - 0.5) * 0.7
    density = smoothstep(0.0, 0.35, shape)

    # Late on, the thin parts go first: the threshold rises through the noise.
    erode = 0.2 + t ** 3 * 0.4
    density = density * smoothstep(erode - 0.2, erode + 0.1, n)
    density = density * (1.0 - 0.7 * smoothstep(0.8, 1.0, t))

    heat = np.clip((shape * 1.3 + (n - 0.5) * 0.8 + 0.25) * max(0.0, 1.0 - t * 1.25), 0.0, 1.0)
    rgb = np.stack([np.interp(heat, RAMP_HEAT, RAMP_COLOR[:, c]) for c in range(3)], axis=-1)
    smoke = 0.7 + 0.6 * n                           # smoke lighter on its billows
    rgb = np.where((heat < 0.2)[..., None], rgb * smoke[..., None], rgb)

    alpha = density * (0.8 + 0.2 * smoothstep(0.0, 0.3, heat))
    border = smoothstep(1.0, 0.9, np.maximum(np.abs(px), np.abs(py)))
    alpha = alpha * border
    return np.concatenate([np.clip(rgb, 0.0, 1.0), alpha[..., None]], axis=-1)


def build_sheet():
    sheet = np.zeros((SIZE, SIZE, 4))
    for index in range(FRAMES):
        row, column = divmod(index, GRID)
        sheet[row * CELL:(row + 1) * CELL, column * CELL:(column + 1) * CELL] = frame(index)
    return sheet


# --- mips ---------------------------------------------------------------------------------------

def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0.0), 1.0 / 2.4) - 0.055)


def box_mips(level0):
    """Every level after level0 down to 1 x 1, as TextureAsset's Box mips: colour in linear light."""
    levels = []
    linear = np.concatenate([to_linear(level0[..., :3]), level0[..., 3:]], axis=-1)
    while linear.shape[0] > 1:
        h, w = linear.shape[0] // 2, linear.shape[1] // 2
        linear = linear.reshape(h, 2, w, 2, 4).mean(axis=(1, 3))
        levels.append(np.concatenate([to_srgb(linear[..., :3]), linear[..., 3:]], axis=-1))
    return levels


def to_bytes(image):
    return (np.clip(image, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).tobytes()


# --- .nmtexture -----------------------------------------------------------------------------------

def tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


def chunk(name, payload, version=1):
    return struct.pack("<IHI", tag(name), version, len(payload)) + payload


def serialize(level0, mips):
    # Width, Height, Format RGBA8, sRGB, Filter Linear, Wrap Clamp, MipGen Box,
    # Compression None, MipCount, no blocks.
    header = struct.pack("<iiBBBBBBBB", SIZE, SIZE, 0, 1, 1, 1, 1, 0, 1 + len(mips), 0)
    return chunk("NTEX", chunk("THDR", header, version=3) + chunk("TPIX", to_bytes(level0))
                 + chunk("TMIP", b"".join(to_bytes(m) for m in mips)))


def main():
    sheet = build_sheet()
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(serialize(sheet, box_mips(sheet)))
    print(f"wrote {OUTPUT} ({SIZE} x {SIZE}, {FRAMES} frames)")


if __name__ == "__main__":
    main()
