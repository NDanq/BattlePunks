#!/usr/bin/env python3
"""
make_courtyard_sky.py -- writes Content/Textures/Courtyard/SunsetSky.nmtexture,
the Courtyard level's sky at sunset: an equirectangular HDRI (RGBE8) that the
Sky Light draws behind the scene and lights by (image-based lighting).

    the sun     a reddened disk just over the west-south-west horizon, exactly
                where make_courtyard_level.py's Sun shines from (SUN_YAW,
                SUN_PITCH: the Directional Light's rotation), and its glow
    the sky     deep blue overhead, a warm band low toward the sun, dusky
                violet opposite it
    clouds      thin streaks of altocumulus on a plane above the house, lit
                orange and pink from below near the sun, grey-violet away
    the ground  dark earth below the horizon, hazed into it

Directions map as IBLCommon.glsl's IBL_SampleEquirect reads them: u =
atan(x, z) / 2pi + 0.5, v = acos(y) / pi, row 0 straight up. Linear radiance,
at the scale of Content/Textures/Sky/ProceduralSky.nmtexture (its zenith
about 1, its sun 2500), so the Sky Light's intensities mean the same.
Written as TextureAsset.cpp reads one: NTEX { THDR v3, TPIX }, RGBE8, no
mips (RGBE cannot be filtered). Deterministic. Needs numpy. Usage:

    python Games/WorldsLab/Tools/Courtyard/make_courtyard_sky.py
"""

import math
import struct
from pathlib import Path

import numpy as np

OUTPUT = Path(__file__).resolve().parents[2] / "Content" / "Textures" / "Courtyard" / "SunsetSky.nmtexture"
WIDTH, HEIGHT = 2048, 1024

# The Sun's Transform rotation in make_courtyard_level.py: pitch (down) and yaw.
SUN_PITCH = 6.5
SUN_YAW = 78.0


def sun_direction():
    """Toward the sun: the light's forward (+Z turned by pitch, then yaw), reversed."""
    p, y = math.radians(SUN_PITCH), math.radians(SUN_YAW)
    forward = np.array([math.sin(y) * math.cos(p), -math.sin(p), math.cos(y) * math.cos(p)])
    return -forward


def directions():
    v = (np.arange(HEIGHT) + 0.5) / HEIGHT
    u = (np.arange(WIDTH) + 0.5) / WIDTH
    theta = v[:, None] * math.pi                      # from straight up
    phi = (u[None, :] - 0.5) * 2.0 * math.pi          # atan(x, z)
    st = np.sin(theta)
    return np.stack([st * np.sin(phi), np.cos(theta) * np.ones_like(phi), st * np.cos(phi)], axis=-1)


# --- Noise for the clouds -------------------------------------------------------------------

RNG = np.random.default_rng(2077)
LATTICE = 64
TABLE = RNG.random((LATTICE, LATTICE))


def value_noise(x, y):
    xi, yi = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - xi, y - yi
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = TABLE[xi % LATTICE, yi % LATTICE]
    b = TABLE[(xi + 1) % LATTICE, yi % LATTICE]
    c = TABLE[xi % LATTICE, (yi + 1) % LATTICE]
    d = TABLE[(xi + 1) % LATTICE, (yi + 1) % LATTICE]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(x, y, octaves=6):
    total, amp, norm = 0.0, 1.0, 0.0
    for _ in range(octaves):
        total = total + value_noise(x, y) * amp
        norm += amp
        x, y, amp = x * 2.03 + 11.7, y * 2.03 + 5.3, amp * 0.5
    return total / norm


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# --- The sky ----------------------------------------------------------------------------------

def build():
    D = directions()
    S = sun_direction()
    up = np.clip(D[..., 1], -1.0, 1.0)
    cos_g = np.clip(D @ S, -1.0, 1.0)
    gamma = np.arccos(cos_g)                          # angle to the sun
    # How much toward the sun's side of the sky, round the horizon (0 opposite, 1 under it).
    flat = D.copy()
    flat[..., 1] = 0.0
    flat /= np.maximum(np.linalg.norm(flat, axis=-1, keepdims=True), 1e-6)
    s_flat = np.array([S[0], 0.0, S[2]]) / math.hypot(S[0], S[2])
    side = (flat @ s_flat) * 0.5 + 0.5

    zenith = np.array([0.030, 0.062, 0.185])
    mid = np.array([0.11, 0.13, 0.27])
    horizon_sun = np.array([1.55, 0.70, 0.26])
    horizon_away = np.array([0.30, 0.21, 0.30])
    horizon = horizon_away + (horizon_sun - horizon_away) * (side ** 2.2)[..., None]

    h = np.clip(up, 0.0, 1.0)
    t_low = smoothstep(0.0, 0.28, h ** 0.75)[..., None]
    t_high = smoothstep(0.2, 1.0, h)[..., None]
    sky = horizon * (1 - t_low) + mid * t_low
    sky = sky * (1 - t_high) + zenith * t_high

    # The sun's glow: a broad warm halo and a tight bright one (forward scattering).
    sky = sky + np.array([2.6, 1.15, 0.36]) * np.exp(-gamma / 0.32)[..., None] * (0.35 + 0.65 * (1 - h))[..., None]
    sky = sky + np.array([16.0, 7.5, 2.2]) * np.exp(-gamma / 0.045)[..., None]

    # Clouds on a plane above: streaks stretched along the wind, thin, broken.
    above = up > 0.015
    k = 1.0 / np.maximum(up, 0.015)
    px, pz = D[..., 0] * k, D[..., 2] * k
    stretch_x, stretch_z = px * 0.9 + pz * 0.35, (-px * 0.35 + pz * 0.9) * 3.2
    n = fbm(stretch_x * 1.6 + 7.0, stretch_z * 1.6 + 3.0)
    detail = fbm(px * 6.0 + 1.0, pz * 6.0 + 9.0, octaves=4)
    density = smoothstep(0.56, 0.76, n * 0.8 + detail * 0.3) * above
    density *= smoothstep(0.015, 0.09, up) * (1 - smoothstep(0.55, 0.95, up) * 0.7)
    lit_sun = np.array([2.3, 0.95, 0.42])
    lit_away = np.array([0.15, 0.11, 0.17])
    toward = (np.clip(cos_g, 0, 1) ** 3)[..., None]
    cloud = lit_away + (lit_sun - lit_away) * (0.25 * side[..., None] ** 3 + 0.75 * toward)
    cloud = cloud * (0.75 + 0.5 * detail[..., None])
    a = (density * 0.85)[..., None]
    sky = sky * (1 - a) + cloud * a

    # The disk itself, reddened, softened at its rim; clouds may veil it a little.
    disk = smoothstep(math.radians(0.62), math.radians(0.45), gamma) * (1 - 0.6 * density)
    sky = sky + np.array([2400.0, 1150.0, 380.0]) * disk[..., None]

    # Below the horizon: dark ground, the haze fading into it.
    below = np.clip(-up, 0.0, 1.0)
    ground = np.array([0.055, 0.042, 0.034])
    haze = smoothstep(0.0, 0.06, below)[..., None]
    sky = np.where((up < 0)[..., None], horizon * 0.55 * (1 - haze) + ground * haze, sky)
    return np.maximum(sky, 0.0)


def to_rgbe(image):
    """IBLCommon.glsl's DecodeRGBE in reverse: rgb = (byte + 0.5) * 2^(e - 136)."""
    m = image.max(axis=-1)
    e = np.where(m > 1e-12, np.floor(np.log2(np.maximum(m, 1e-30))) + 1.0, 0.0)
    scale = np.exp2(8.0 - e)
    mant = np.clip(np.floor(image * scale[..., None]), 0, 255)
    out = np.zeros(image.shape[:2] + (4,), np.uint8)
    out[..., :3] = mant.astype(np.uint8)
    out[..., 3] = np.where(m > 1e-12, np.clip(e + 128, 0, 255), 0).astype(np.uint8)
    return out


def tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


def chunk(name, payload, version=1):
    return struct.pack("<IHI", tag(name), version, len(payload)) + payload


def main():
    sky = build()
    # Width, Height, Format RGBE8, not sRGB, Filter Nearest, Wrap Repeat, no mips, no compression, 1 level, no blocks.
    header = struct.pack("<iiBBBBBBBB", WIDTH, HEIGHT, 1, 0, 0, 0, 0, 0, 1, 0)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(chunk("NTEX", chunk("THDR", header, version=3) + chunk("TPIX", to_rgbe(sky).tobytes())))
    row = lambda deg: sky[int((90 - deg) / 180 * HEIGHT)].mean(0).round(3)
    print(f"wrote {OUTPUT} ({WIDTH} x {HEIGHT}); zenith {row(89)}, 20 deg {row(20)}, horizon {row(1)}, max {sky.max():.0f}")


if __name__ == "__main__":
    main()
