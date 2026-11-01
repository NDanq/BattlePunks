#!/usr/bin/env python3
"""
make_l1_foliage_textures.py -- L1 Foothills' foliage textures, Content/Textures/Foliage/L1_*
(copied from WorldsLab Tools/Terrain/make_foliage_textures.py, plus the wildflowers):
procedural, deterministic, written as make_courtyard_textures.py writes its sets
(whose helpers this borrows: noise, normals, coverage-kept alpha mips).

    PineFrond_BaseColor   a spruce branch seen from above, base at the left (u = 0),
                          tip at the right: a twig, side twigs and their needles, the
                          new growth at the tips lighter; A = coverage. Clamped.
    PineFrond_Normal      each needle rounded across its width (OpenGL convention).
    Bark_BaseColor        pine bark, tiling: reddish plates split by dark fissures
    Bark_Normal           running up the trunk (v), one repeat 0.8 m x 1.6 m.
    Bark_Surface          R roughness, G ambient occlusion, B metallic (0).
    Grass_BaseColor       a clump of curved blades rising from the bottom edge;
                          A = coverage. Clamped.
    Wildflower_BaseColor  thin stems with heads of yellow, white and violet flowers
                          rising from the bottom edge; A = coverage. Clamped.

Foliage alpha keeps its coverage down the mips, so a distant tree does not thin out.
PNG previews go to --preview DIR.

    python Games/DemoLab/Tools/Art/l1_foliage/make_l1_foliage_textures.py [--preview DIR]
"""

import argparse
import math
import sys
from pathlib import Path

import numpy as np

GAME_DIR = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctlib as ct  # noqa: E402

OUT = GAME_DIR / "Content" / "Textures" / "Foliage"


# --- Strokes ----------------------------------------------------------------------------------------

class Canvas:
    """Colour (linear), coverage and height, painted with anti-aliased capsules, later on top."""

    def __init__(self, h, w):
        self.h, self.w = h, w
        self.rgb = np.zeros((h, w, 3))
        self.a = np.zeros((h, w))
        self.height = np.zeros((h, w))

    def capsule(self, p0, p1, r0, r1, colour, lift=0.0):
        """A tapered stroke from p0 (radius r0) to p1 (radius r1); points are (x, y) pixels.
        Its height is a half cylinder across it, raised by lift."""
        x0 = int(max(math.floor(min(p0[0], p1[0]) - max(r0, r1) - 2), 0))
        x1 = int(min(math.ceil(max(p0[0], p1[0]) + max(r0, r1) + 2), self.w))
        y0 = int(max(math.floor(min(p0[1], p1[1]) - max(r0, r1) - 2), 0))
        y1 = int(min(math.ceil(max(p0[1], p1[1]) + max(r0, r1) + 2), self.h))
        if x1 <= x0 or y1 <= y0:
            return
        ys, xs = np.mgrid[y0:y1, x0:x1] + 0.5
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        ll = dx * dx + dy * dy + 1e-9
        t = np.clip(((xs - p0[0]) * dx + (ys - p0[1]) * dy) / ll, 0.0, 1.0)
        d = np.hypot(xs - (p0[0] + t * dx), ys - (p0[1] + t * dy))
        r = r0 + (r1 - r0) * t
        cover = np.clip(r - d + 0.5, 0.0, 1.0)
        top = lift + r * np.sqrt(np.clip(1.0 - (d / np.maximum(r, 1e-3)) ** 2, 0.0, 1.0))
        win = (slice(y0, y1), slice(x0, x1))
        self.rgb[win] = self.rgb[win] * (1 - cover[..., None]) + np.asarray(colour) * cover[..., None]
        self.a[win] = np.maximum(self.a[win], cover)
        self.height[win] = np.where(cover > 0.5, np.maximum(self.height[win] * 0.6, top), self.height[win])

    def curve(self, points, r0, r1, colour, lift=0.0):
        n = len(points) - 1
        for i in range(n):
            self.capsule(points[i], points[i + 1], r0 + (r1 - r0) * i / n, r0 + (r1 - r0) * (i + 1) / n, colour, lift)


def bleed(rgb, a):
    """The colour spread out under the alpha's edge, so mips and filtering do not
    darken a card's outline."""
    filled = rgb.copy()
    for sigma in (2, 6, 16, 40):
        bled = ct.blur(rgb * a[..., None], sigma) / (ct.blur(a, sigma) + 1e-6)[..., None]
        filled = np.where(a[..., None] > 0.01, filled, np.where(filled.sum(-1, keepdims=True) > 0, filled, bled))
    return filled


def shrink(img, kind, width):
    """Halve img (as its mips are made) until it is width wide; it need not be square."""
    while img.shape[1] > width:
        img = ct._half(img, kind)
    return img


def needle_colour(rng, young):
    """Spruce needles, linear: blue-green old growth, yellow-green new growth at the tips."""
    old = ct.lin(0.20, 0.30, 0.14) * rng.uniform(0.75, 1.15)
    new = ct.lin(0.42, 0.56, 0.20) * rng.uniform(0.85, 1.1)
    return old + (new - old) * young


def twig(c, rng, start, angle, length, width, needle_len, side_bias):
    """A twig from start along angle (radians, image y down), needles both sides, leaning
    toward the tip. Returns its points."""
    pts, bend = [], rng.uniform(-0.25, 0.25)
    steps = max(int(length / 6), 2)
    for k in range(steps + 1):
        s = k / steps
        a = angle + bend * s
        if k == 0:
            pts.append(start)
        else:
            px, py = pts[-1]
            pts.append((px + math.cos(a) * length / steps, py + math.sin(a) * length / steps))
    c.curve(pts, width * 0.9, width * 0.45, ct.lin(0.26, 0.17, 0.10), lift=0.5)
    # Needles over the twig: every few pixels, both sides, shorter near the tip.
    spacing = needle_len * 0.12
    k = 0.0
    while k < length:
        s = k / length
        i = min(int(s * steps), steps - 1)
        f = s * steps - i
        px = pts[i][0] + (pts[i + 1][0] - pts[i][0]) * f
        py = pts[i][1] + (pts[i + 1][1] - pts[i][1]) * f
        a = angle + bend * s
        young = ct.smoothstep(0.55, 0.95, s) if side_bias is not None else ct.smoothstep(0.7, 1.0, s)
        for side in (-1, 1):
            spread = rng.uniform(0.55, 1.35) * side
            na = a + spread
            nl = needle_len * rng.uniform(0.75, 1.1) * (1.0 - 0.45 * s * s)
            tip = (px + math.cos(na) * nl, py + math.sin(na) * nl)
            c.capsule((px, py), tip, width * 0.8, width * 0.25, needle_colour(rng, young), lift=1.0)
        k += spacing * rng.uniform(0.7, 1.3)
    return pts


def pine_frond(size=(2048, 1024)):
    """At twice the written size. The frond fills a leaf-shaped outline, widest a little
    past the middle, so the card's polygon (make_foliage_meshes.py) can hug it."""
    W, H = size
    rng = np.random.default_rng(31)
    c = Canvas(H, W)
    mid = H * 0.5
    x0, x1 = W * 0.02, W * 0.97
    needle = H * 0.055
    # The main twig under everything, needled too.
    twig(c, rng, (x0, mid), 0.0, x1 - x0, H * 0.009, needle * 1.1, None)
    # Side twigs: alternating, forward at ~50 degrees, their length the outline's half-width.
    t = 0.05
    side = 1
    while t < 0.95:
        env = math.sin(math.pi * min(t / 0.98, 1.0) ** 0.75)   # 0 at both ends, widest past the middle
        x = x0 + (x1 - x0) * t
        length = env * H * 0.44 * rng.uniform(0.85, 1.05)
        if length > needle * 1.2:
            ang = side * rng.uniform(0.7, 0.95)   # downward (+y) or upward (-y), toward +x
            pts = twig(c, rng, (x, mid + rng.uniform(-3, 3)), ang, length, H * 0.006, needle * rng.uniform(0.8, 1.0), side)
            # Sub-twigs off the longer ones, forward and outward.
            for s in (0.3, 0.55):
                if length > H * 0.2:
                    p = pts[int(s * (len(pts) - 1))]
                    twig(c, rng, p, ang + side * rng.uniform(0.5, 0.8) * rng.choice([-1, 1]) * 0.8 - 0.2 * side,
                         length * rng.uniform(0.3, 0.45), H * 0.004, needle * 0.8, side)
        side = -side
        t += rng.uniform(0.022, 0.032)
    # Light from above a branch: a little self-shading toward the twigs' undersides.
    h = c.height
    ao = np.clip(0.55 + 0.45 * h / max(h.max(), 1e-6), 0, 1)
    rgb = c.rgb * ao[..., None]
    a = c.a
    # Fill the colour under the alpha edge (bleed), so mips do not darken the outline.
    base = np.concatenate([ct.to_srgb(np.clip(bleed(rgb, a), 0, 1)), a[..., None]], axis=-1)
    base = shrink(base, "foliage", W // 2)
    # Height in mm: the needles are ~1 mm round; one texel here is ~1.2 mm of a 2.5 m frond.
    hmm = ct.blur(c.height, 0.7) * 0.35
    n = ct.normal_from_height(hmm, 1.0, strength=1.0)
    normal = np.concatenate([n * 0.5 + 0.5, np.ones(hmm.shape)[..., None]], axis=-1)
    normal = shrink(normal, "normal", W // 4)
    return base, normal


def bark(S=1024):
    """Pine bark, tiling: plates (Voronoi cells, tall) lifted between deep fissures; one
    repeat 0.8 m across, 1.6 m up (u around the trunk, v up it), so the cells are stretched."""
    rng = np.random.default_rng(77)
    shape = (S, S)
    texel_mm = 800.0 / S
    f1, f2, cid, _, _ = ct.voronoi(rng, shape, 9, 7, jitter=0.9)
    edge = f2 - f1
    plate = ct.smoothstep(2.0, 16.0, edge)
    rough = ct.spectral(rng, shape, lo=4, hi=200, beta=1.1, sy=0.4)
    flake = ct.spectral(rng, shape, lo=20, hi=400, beta=0.8, sy=0.5)
    lift = rng.uniform(0.6, 1.0, 9 * 7)[cid]
    h = plate * (6.0 + 3.0 * lift) + rough * 1.2 + flake * 0.5 * plate
    fissure = 1.0 - plate
    # Colour: reddish-grey plates, dark brown fissures, a little lichen-grey.
    plate_col = ct.lin(0.42, 0.30, 0.24)
    grey = ct.lin(0.40, 0.38, 0.35)
    dark = ct.lin(0.10, 0.07, 0.05)
    tone = ct.rescale(ct.spectral(rng, shape, lo=2, hi=30, beta=1.0), 0, 1)
    rgb = ct.mix(np.broadcast_to(plate_col, shape + (3,)), grey, ct.smoothstep(0.55, 0.85, tone) * 0.6)
    rgb = rgb * (0.8 + 0.25 * lift[..., None]) * (0.9 + 0.1 * flake[..., None])
    rgb = ct.mix(rgb, dark, ct.smoothstep(0.3, 1.0, fissure))
    ao = ct.cavity_ao(h, [(4, 0.6), (16, 0.4)], 0.12)
    base = ct.rgba(ct.finish_albedo(rgb * ao[..., None]))
    n = ct.normal_from_height(h, texel_mm, strength=1.4)
    normal = np.concatenate([n * 0.5 + 0.5, np.ones(shape)[..., None]], axis=-1)
    surf = ct.surface(np.clip(0.82 + 0.12 * fissure + 0.04 * rough, 0, 1), ao)
    return (ct.downsample_to(base, "color", 512), ct.downsample_to(normal, "normal", 512),
            ct.downsample_to(surf, "data", 512))


def grass(S=1024):
    """A clump of blades rising from the bottom edge, curving out and over; darker at the
    roots, drier toward a few tips. At twice the written size."""
    rng = np.random.default_rng(12)
    c = Canvas(S, S)
    for k in range(70):
        root = (S * 0.5 + rng.normal(0, S * 0.12), S - 1.0)
        lean = rng.normal(0, 0.45)
        tall = S * rng.uniform(0.45, 0.95)
        curl = rng.uniform(0.2, 0.9) * (1 if lean >= 0 else -1)
        pts = []
        for i in range(13):
            s = i / 12
            x = root[0] + (lean * 0.55 + curl * s * s * 0.5) * tall * s
            y = root[1] - tall * s * (1.0 - 0.25 * s * abs(curl))
            pts.append((x, y))
        dry = rng.random() < 0.15
        tip = ct.lin(0.46, 0.44, 0.27) if dry else ct.lin(0.29, 0.39, 0.15)
        foot = ct.lin(0.10, 0.16, 0.06)
        width = S * rng.uniform(0.008, 0.013)
        n = len(pts) - 1
        for i in range(n):
            s = (i + 0.5) / n
            colour = (foot + (tip - foot) * ct.smoothstep(0.0, 0.8, s)) * rng.uniform(0.9, 1.1)
            c.capsule(pts[i], pts[i + 1], width * (1 - 0.9 * i / n), width * (1 - 0.9 * (i + 1) / n), colour, lift=0.3)
    base = np.concatenate([ct.to_srgb(np.clip(bleed(c.rgb, c.a), 0, 1)), c.a[..., None]], axis=-1)
    return shrink(base, "foliage", S // 2)


def wildflowers(S=1024):
    """Stems with small flower heads (five petals round a centre) in yellow, white and
    violet, over a few grass blades; at twice the written size."""
    rng = np.random.default_rng(31)
    c = Canvas(S, S)
    for k in range(18):   # a few blades at the foot
        root = (S * 0.5 + rng.normal(0, S * 0.15), S - 1.0)
        tall = S * rng.uniform(0.25, 0.5)
        lean = rng.normal(0, 0.4)
        pts = [(root[0] + lean * tall * (i / 8) ** 1.5 * 0.6, root[1] - tall * i / 8) for i in range(9)]
        c.curve(pts, S * 0.009, S * 0.002, ct.lin(0.16, 0.26, 0.09) * rng.uniform(0.85, 1.1), lift=0.3)
    colours = [ct.lin(0.95, 0.78, 0.18), ct.lin(0.93, 0.92, 0.86), ct.lin(0.55, 0.36, 0.78), ct.lin(0.92, 0.55, 0.20)]
    for k in range(16):
        root = (S * 0.5 + rng.normal(0, S * 0.14), S - 1.0)
        tall = S * rng.uniform(0.45, 0.92)
        lean = rng.normal(0, 0.18)
        pts = [(root[0] + lean * tall * (i / 10) ** 1.3, root[1] - tall * i / 10) for i in range(11)]
        c.curve(pts, S * 0.0045, S * 0.003, ct.lin(0.14, 0.24, 0.08), lift=0.3)
        head = pts[-1]
        colour = colours[int(rng.integers(0, len(colours)))] * rng.uniform(0.85, 1.05)
        r = S * rng.uniform(0.022, 0.034)
        for p in range(5):
            a = p * 2 * math.pi / 5 + rng.uniform(0, 1)
            tip = (head[0] + math.cos(a) * r, head[1] + math.sin(a) * r * 0.7)
            c.capsule(head, tip, r * 0.35, r * 0.45, colour, lift=1.0)
        c.capsule(head, (head[0] + 0.5, head[1]), r * 0.3, r * 0.3, ct.lin(0.75, 0.55, 0.10), lift=1.5)
    base = np.concatenate([ct.to_srgb(np.clip(bleed(c.rgb, c.a), 0, 1)), c.a[..., None]], axis=-1)
    return shrink(base, "foliage", S // 2)


def write(name, image, kind, wrap=0, preview=None):
    path = OUT / f"{name}.nmtexture"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(ct.serialize(image, kind, wrap))
    print(f"  {path.relative_to(GAME_DIR)}: {image.shape[1]} x {image.shape[0]}")
    if preview:
        ct.write_png(Path(preview) / f"{name}.png", image)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None)
    args = ap.parse_args()
    if args.preview:
        Path(args.preview).mkdir(parents=True, exist_ok=True)
    base, normal = pine_frond()
    write("L1_PineFrond_BaseColor", base, "foliage", wrap=1, preview=args.preview)
    write("L1_PineFrond_Normal", normal, "normal", wrap=1, preview=args.preview)
    b, n, s = bark()
    write("L1_Bark_BaseColor", b, "color", preview=args.preview)
    write("L1_Bark_Normal", n, "normal", preview=args.preview)
    write("L1_Bark_Surface", s, "data", preview=args.preview)
    write("L1_Grass_BaseColor", grass(), "foliage", wrap=1, preview=args.preview)
    write("L1_Wildflower_BaseColor", wildflowers(), "foliage", wrap=1, preview=args.preview)


if __name__ == "__main__":
    main()
