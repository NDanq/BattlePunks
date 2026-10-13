#!/usr/bin/env python3
"""
make_toony_textures.py -- writes Content/Textures/Toony/: the Toony style's
sky. Everything else in the style is vertex colour, so this is all it needs.

    Sky_HDR   a cartoon midday: a bright cyan-to-blue gradient, a warm band at
              the horizon, a few flat, banded puffs of cloud, a hard white sun

Run from the repository root (needs numpy):

    python Games/WorldsLab/Tools/Toony/make_toony_textures.py
"""

import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import toony_kit as kit          # noqa: E402
nm = kit.nm

OUT = kit.GAME_DIR / "Content" / "Textures" / kit.STYLE
SUN_ELEVATION = 52.0             # degrees above the horizon
SUN_AZIMUTH   = 35.0             # degrees round from +Z toward +X


def make_sky(width=1024, height=512):
    """Equirectangular, row 0 straight up, azimuth 0 at the centre column (+Z)."""
    ys, xs = np.mgrid[0:height, 0:width] + 0.5
    lon = (xs / width - 0.5) * 2 * math.pi
    lat = (0.5 - ys / height) * math.pi
    dx, dy, dz = np.cos(lat) * np.sin(lon), np.sin(lat), np.cos(lat) * np.cos(lon)
    el, az = math.radians(SUN_ELEVATION), math.radians(SUN_AZIMUTH)
    sx, sy, sz = math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)
    cos_sun = np.clip(dx * sx + dy * sy + dz * sz, -1, 1)

    up = np.clip(dy, 0, 1)
    zenith  = np.array([0.10, 0.36, 0.95])
    horizon = np.array([0.55, 0.85, 1.00])
    warm    = np.array([1.00, 0.86, 0.62])
    # Banded, as a cartoon sky is: the gradient in a few flat steps.
    t = np.floor(np.power(up, 0.55) * 6.0) / 6.0
    sky = horizon * (1 - t)[..., None] + zenith * t[..., None]
    sky = np.where((up < 0.06)[..., None], warm * 0.9 + horizon * 0.1, sky)

    # Flat cloud puffs: a few soft circles on the dome, cut hard.
    rng = np.random.default_rng(9)
    cover = np.zeros_like(dy)
    for _ in range(26):
        c_az = rng.uniform(-math.pi, math.pi)
        c_el = rng.uniform(math.radians(8), math.radians(40))
        cx, cy, cz = math.cos(c_el) * math.sin(c_az), math.sin(c_el), math.cos(c_el) * math.cos(c_az)
        size = rng.uniform(0.05, 0.12)
        d = 1.0 - (dx * cx + dy * cy + dz * cz)
        for k in range(3):
            ox, oz = rng.uniform(-0.6, 0.6) * size, rng.uniform(-0.2, 0.2) * size
            cover = np.maximum(cover, (d + ox * dx + oz * dz < size * size * (1.0 - 0.25 * k)).astype(np.float64))
    cover *= (dy > 0.03)
    sky = sky * (1 - cover[..., None]) + np.array([1.4, 1.4, 1.45]) * cover[..., None]

    disc = (cos_sun > math.cos(math.radians(2.2)))[..., None]
    sky = np.where(disc, np.array([40.0, 38.0, 32.0]), sky)
    ground = np.array([0.30, 0.55, 0.25]) * (0.7 + 0.3 * np.exp(dy * 4))[..., None]
    sky = np.where((dy < 0)[..., None], ground, sky)
    nm.write_hdri(OUT / "Sky_HDR.nmtexture", sky.astype(np.float64))
    print(f"  Sky: {width} x {height} HDR")


def main():
    print(f"Writing {OUT}")
    make_sky()


if __name__ == "__main__":
    main()
