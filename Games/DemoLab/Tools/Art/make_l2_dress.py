#!/usr/bin/env python3
"""
make_l2_dress.py -- the assets Level 2's dressing (Tools/Levels/dress_l2_laboratory.py) adds to
the art kit, every one named L2_*:

    Content/Textures/L2_Laboratory/   L2_Plaques_BaseColor (the sample plaques, an atlas),
                                      L2_Leaf_{BaseColor,Normal,ORM}
    Content/Materials/L2_Laboratory/  L2_Plaque, L2_Leaf, L2_DomeGlass, L2_Water, L2_AlarmLens, L2_StripGlow,
                                      L2_Soil, L2_Plaster
    Content/Meshes/L2_Laboratory/     L2_Plaque_<Model> (one per sample), L2_Curtain, L2_CurtainRod,
                                      L2_Fern, L2_Palm, L2_Planter, L2_Tank, L2_Beacon,
                                      L2_BeaconReflector, L2_LightStrip, L2_AtriumRing, L2_Rubble,
                                      L2_HandLamp
    Content/Particles/L2_Laboratory/  L2_Smoke, L2_Mist, L2_Bubbles
    Content/PostProcess/L2_Laboratory/ L2_Calm.pp.nmgraph, L2_Alarm.pp.nmgraph

Pure Python (numpy, Pillow). Then fill the graphs' <gen_code> and cook, as for the kit:

    python3 Games/DemoLab/Tools/Art/make_l2_dress.py [--bin <dir with NodeGraphGenerate>]
    <bin>/NimbleCook shaders -project Games/DemoLab -module DemoLabGame -editor

Frame and units as the kit's: engine space (left-handed, +Y up), centimetres, fronts toward -Z;
UV0 in metres unless a mesh maps a decal (the plaques, the leaves) in 0-1.
"""

import glob
import math
import subprocess
import sys
from pathlib import Path

import numpy as np

ART = Path(__file__).resolve().parent
sys.path.insert(0, str(ART / "lib"))
import graphs                  # noqa: E402
import nmformats as nm         # noqa: E402
import texlib as tl            # noqa: E402

GAME = ART.parents[1]
ROOT = GAME.parents[1]
TEX_DIR = GAME / "Content" / "Textures" / "L2_Laboratory"
MAT_DIR = GAME / "Content" / "Materials" / "L2_Laboratory"
MESH_DIR = GAME / "Content" / "Meshes" / "L2_Laboratory"
PART_DIR = GAME / "Content" / "Particles" / "L2_Laboratory"
PP_DIR = GAME / "Content" / "PostProcess" / "L2_Laboratory"
TEX = "Content/Textures/L2_Laboratory"
KIT = "Content/Materials/Kit/{}.material.nmgraph"
MAT = "Content/Materials/L2_Laboratory/{}.material.nmgraph"

# The workshop's samples: (kit mesh, shading model, what it is). Plaque i is atlas cell i.
SAMPLES = [
    ("Sample_BrushedBrass", "ANISOTROPIC", "Brushed brass"),
    ("Sample_Walnut", "CLEAR COAT", "Lacquered walnut"),
    ("Sample_Enamel", "CLEAR COAT", "Car-paint enamel"),
    ("Sample_Velvet", "CLOTH", "Crushed velvet"),
    ("Sample_Leather", "CLOTH", "Bound leather"),
    ("Sample_Marble", "SUBSURFACE", "White marble"),
    ("Sample_Wax", "SUBSURFACE", "Candle wax"),
    ("Sample_LeafPanel", "SUBSURFACE", "Backlit fern leaf"),
    ("Sample_FrostedGlass", "LIT TRANSLUCENCY", "Frosted glass"),
    ("Sample_Steam", "VOLUME", "Steam"),
]
PLAQUE_COLS, PLAQUE_ROWS = 2, 8          # 512 x 256 cells in a 1024 x 2048 atlas
PLAQUE_W, PLAQUE_H = 22.0, 11.0          # cm, the face (over the pedestal's 20 x 8 label plate)


def plaque_name(i):
    return "L2_Plaque_" + SAMPLES[i][0].split("_", 1)[1]


# =============================================================================
# Textures
# =============================================================================
def font(size, bold=True):
    from PIL import ImageFont
    for name in (["DejaVuSerif-Bold.ttf", "LiberationSerif-Bold.ttf"] if bold else
                 ["DejaVuSerif-Italic.ttf", "LiberationSerif-Italic.ttf", "DejaVuSerif.ttf"]):
        for path in glob.glob(f"/usr/share/fonts/**/{name}", recursive=True):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def make_plaques():
    """Engraved brass plates: the shading model in capitals, what the sample is in italics, a
    rule and corner screws. Letters are dark enamel filled into the brass."""
    from PIL import Image, ImageDraw
    cw, ch = 512, 256
    w, h = cw * PLAQUE_COLS, ch * PLAQUE_ROWS
    noise = tl.Noise(207, w, h)
    grain = noise.fbm((64, 8), octaves=4, cells_v=None) if False else noise.fbm(32, octaves=4)
    streak = tl.Noise(208, w, h).fbm((256,), octaves=3) if False else tl.Noise(208, w, h).value(256, 4)
    base = np.zeros((h, w, 3))
    brass = np.array([0.72, 0.55, 0.28])
    base[:] = brass * (0.85 + 0.15 * grain[..., None]) * (0.95 + 0.05 * streak[..., None])
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    title, sub, small = font(54), font(38, bold=False), font(22)
    for i, (_, model, what) in enumerate(SAMPLES):
        x0, y0 = (i % PLAQUE_COLS) * cw, (i // PLAQUE_COLS) * ch
        d.rectangle([x0 + 14, y0 + 14, x0 + cw - 15, y0 + ch - 15], outline=255, width=4)
        d.rectangle([x0 + 24, y0 + 24, x0 + cw - 25, y0 + ch - 25], outline=200, width=1)
        for sx, sy in ((x0 + 40, y0 + 40), (x0 + cw - 40, y0 + 40), (x0 + 40, y0 + ch - 40), (x0 + cw - 40, y0 + ch - 40)):
            d.ellipse([sx - 7, sy - 7, sx + 7, sy + 7], fill=150)
        d.text((x0 + cw / 2, y0 + 62), "SHADING MODEL", font=small, fill=230, anchor="mm")
        size = 54
        f = title
        while d.textlength(model, font=f) > cw - 90 and size > 30:
            size -= 2
            f = font(size)
        d.text((x0 + cw / 2, y0 + 118), model, font=f, fill=255, anchor="mm")
        d.line([x0 + 120, y0 + 158, x0 + cw - 120, y0 + 158], fill=220, width=2)
        d.text((x0 + cw / 2, y0 + 196), what, font=sub, fill=255, anchor="mm")
    m = np.asarray(mask, dtype=np.float64) / 255.0
    ink = np.array([0.035, 0.03, 0.025])
    img = base * (1.0 - m[..., None]) + ink * m[..., None]
    tl.write(TEX_DIR, "L2_Plaques_BaseColor", img, True, wrap="clamp", png=False)


def make_leaf():
    """A tileable-free leaf card: an elliptical blade with a midrib and veins, alpha cut out."""
    n = 256
    v, u = np.mgrid[0:n, 0:n] / (n - 1.0)          # v down the card (tip at the top), u across
    t = 1.0 - v                                    # 0 at the stem, 1 at the tip
    half = 0.46 * np.sin(np.pi * np.clip(t, 0, 1)) ** 0.8 * (1.0 - 0.25 * t)
    x = (u - 0.5)
    inside = np.abs(x) < half
    edge = np.clip((half - np.abs(x)) / 0.02, 0, 1)
    rib = np.exp(-(x / 0.012) ** 2)
    veins = np.clip(np.sin((t * 18.0 - np.abs(x) * 24.0) * np.pi) * 0.5 + 0.5, 0, 1) ** 8 * (np.abs(x) < half * 0.92)
    noise = tl.Noise(311, n).fbm(8, octaves=4)
    green = np.array([0.16, 0.36, 0.08]) * (0.8 + 0.4 * noise[..., None])
    tipc = np.array([0.30, 0.45, 0.10])
    col = green * (1 - 0.3 * t[..., None]) + tipc * 0.3 * t[..., None]
    col = col * (1 - 0.35 * rib[..., None] - 0.12 * veins[..., None]) + np.array([0.55, 0.6, 0.25]) * 0.35 * rib[..., None]
    alpha = (inside * edge).astype(np.float64)
    height = 0.5 + 0.3 * rib - 0.1 * veins
    rough = 0.55 - 0.15 * rib
    tl.save_set(TEX_DIR, "L2_Leaf", col, height, 2.0, rough, np.zeros_like(rough), 1.0 - 0.2 * veins, alpha=alpha)


# =============================================================================
# Materials
# =============================================================================
MATDEFS = [
    dict(name="L2_Plaque", tex="L2_Plaques_BaseColor", unit=True, rough=0.32, metal=0.85),
    dict(name="L2_Leaf", set="L2_Leaf", metres=1.0, model="Subsurface", sss=(0.9, 1.3, 0.45), thickness=0.15,
         domain="Masked", two_sided=True),
    dict(name="L2_DomeGlass", base=(0.75, 0.8, 0.8), rough=0.04, domain="Translucent", opacity=0.05, refraction=1.0),
    dict(name="L2_Water", base=(0.08, 0.28, 0.25), rough=0.04, domain="Translucent", opacity=0.3, refraction=1.06),
    dict(name="L2_AlarmLens", base=(0.5, 0.03, 0.02), rough=0.25, emissive=((1.0, 0.07, 0.02), 24.0)),
    dict(name="L2_StripGlow", base=(1.0, 0.95, 0.88), rough=0.3, emissive=((1.0, 0.86, 0.66), 10.0)),
    dict(name="L2_Soil", base=(0.10, 0.07, 0.045), rough=0.95),
    dict(name="L2_Plaster", base=(0.62, 0.58, 0.52), rough=0.92),
]


def make_materials():
    written = []
    for d in MATDEFS:
        g = graphs.kit_material(d, TEX)
        path = MAT_DIR / f"{d['name']}.material.nmgraph"
        g.write(path)
        written.append(path)
    return written


# =============================================================================
# Meshes (MeshBuilder: engine space, cm)
# =============================================================================
def mb(name):
    return nm.MeshBuilder(name)


def save(m, name):
    lo, hi = m.write(MESH_DIR / f"{name}.nmmesh", lightmap=False)
    print(f"  {name}: {m.triangle_count()} tris, {tuple(round(v) for v in lo)}..{tuple(round(v) for v in hi)}")


def make_plaque_meshes():
    """A plate 30 x 15 cm on a 1.2 cm brass back, the face toward -Z at z = -1.6; pivot at the
    centre of its back (z = 0): stick it on a face."""
    hw, hh = PLAQUE_W / 2, PLAQUE_H / 2
    for i in range(len(SAMPLES)):
        m = mb(plaque_name(i))
        col, row = i % PLAQUE_COLS, i // PLAQUE_COLS
        u0, u1 = col / PLAQUE_COLS, (col + 1) / PLAQUE_COLS
        v0, v1 = row / PLAQUE_ROWS, (row + 1) / PLAQUE_ROWS
        z = -1.6
        # Seen from -Z, +X is to the viewer's left... the front faces -Z, so the viewer's right is -X.
        m.polygon(MAT.format("L2_Plaque"), [(hw, hh, z), (-hw, hh, z), (-hw, -hh, z), (hw, -hh, z)],
                  normal=(0, 0, -1), uvs=[(u1, v0), (u0, v0), (u0, v1), (u1, v1)])
        m.box(KIT.format("BrassAged"), (-hw - 0.8, -hh - 0.8, -1.5), (hw + 0.8, hh + 0.8, 0.0), skip=("-z",))
        m.polygon(KIT.format("BrassAged"), [(hw + 0.8, hh + 0.8, -1.5), (-hw - 0.8, hh + 0.8, -1.5),
                                            (-hw - 0.8, hh, -1.5), (hw + 0.8, hh, -1.5)], normal=(0, 0, -1))
        m.polygon(KIT.format("BrassAged"), [(hw + 0.8, -hh, -1.5), (-hw - 0.8, -hh, -1.5),
                                            (-hw - 0.8, -hh - 0.8, -1.5), (hw + 0.8, -hh - 0.8, -1.5)], normal=(0, 0, -1))
        m.polygon(KIT.format("BrassAged"), [(hw + 0.8, hh, -1.5), (hw, hh, -1.5), (hw, -hh, -1.5), (hw + 0.8, -hh, -1.5)],
                  normal=(0, 0, -1))
        m.polygon(KIT.format("BrassAged"), [(-hw, hh, -1.5), (-hw - 0.8, hh, -1.5), (-hw - 0.8, -hh, -1.5),
                                            (-hw, -hh, -1.5)], normal=(0, 0, -1))
        save(m, plaque_name(i))


def make_curtain():
    """A velvet drape 170 wide and 470 tall hanging from its top centre (y 0 down to -470), deep
    pleats toward -Z that gather at a tie-back; a rod and rings come separately."""
    m = mb("L2_Curtain")
    nu, nv = 48, 24
    width, height, pleats = 170.0, 470.0, 9
    def point(a, b):
        y = -b * height
        gather = 1.0 - 0.45 * math.exp(-((b - 0.62) / 0.12) ** 2)      # the tie-back pinches it
        x = (a - 0.5) * width * gather + 25.0 * (1 - gather) * (a - 0.5)
        z = -7.0 * math.sin(a * pleats * 2 * math.pi) * (0.6 + 0.4 * b) - 3.0
        if b > 0.62:                                                     # it bellies out below
            z -= 18.0 * math.sin(math.pi * (b - 0.62) / 0.38) * (0.5 + 0.5 * math.sin(a * math.pi))
        return (x, y, z)
    grid = [[point(i / nu, j / nv) for i in range(nu + 1)] for j in range(nv + 1)]
    def normal(i, j):
        a = grid[j][min(i + 1, nu)]
        b = grid[j][max(i - 1, 0)]
        c = grid[min(j + 1, nv)][i]
        e = grid[max(j - 1, 0)][i]
        n = nm.v_cross(nm.v_sub(a, b), nm.v_sub(c, e))
        n = nm.v_norm(n)
        return n if n[2] < 0 else nm.v_mul(n, -1.0) if False else n
    vel = KIT.format("Velvet")
    for j in range(nv):
        for i in range(nu):
            quad = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
            verts = [(grid[b][a], normal(a, b), (a / nu * width / 100.0, b / nv * height / 100.0), (1, 1, 1, 0.5))
                     for a, b in quad]
            m.triangle_raw(vel, [verts[0], verts[1], verts[2]])
            m.triangle_raw(vel, [verts[0], verts[2], verts[3]])
    # A pelmet strip at the top (walnut) hides where it gathers on the rod.
    m.box(KIT.format("Walnut"), (-width / 2 - 6, -22, -16), (width / 2 + 6, 4, 0))
    save(m, "L2_Curtain")

    r = mb("L2_CurtainRod")
    r.tube(KIT.format("BrassAged"), [(-200, 0, 0), (200, 0, 0)], 2.2, segments=12)
    for x in (-206, 206):
        r.lathe(KIT.format("BrassAged"), [(0, -5), (4.5, -4), (5, 0), (4.5, 4), (0, 5)], centre=(x, 0, 0), segments=12,
                axis="x")
    for x in (-203, 203):
        r.box(KIT.format("BrassAged"), (x - 1.5, -2, 0), (x + 1.5, 2, 14))
    save(r, "L2_CurtainRod")


def leaf_card(m, mat, base, direction, length, width, droop, twist=0.0, segments=4):
    """A bending leaf from base along direction (unit, roughly horizontal), sagging by droop; UVs
    0-1 (v from the tip at 0 to the stem at 1, as the texture is drawn)."""
    dx, dy, dz = direction
    side = nm.v_norm(nm.v_cross(direction, (0, 1, 0))) if abs(dy) < 0.95 else (1, 0, 0)
    side = (side[0] * math.cos(twist) + 0 * math.sin(twist), side[1] * math.cos(twist) + math.sin(twist) * 0.3, side[2])
    pts = []
    for k in range(segments + 1):
        t = k / segments
        p = (base[0] + dx * length * t, base[1] + dy * length * t - droop * t * t, base[2] + dz * length * t)
        pts.append(p)
    for k in range(segments):
        t0, t1 = k / segments, (k + 1) / segments
        a, b = pts[k], pts[k + 1]
        h = width / 2
        quad = [nm.v_add(a, nm.v_mul(side, -h)), nm.v_add(a, nm.v_mul(side, h)), nm.v_add(b, nm.v_mul(side, h)),
                nm.v_add(b, nm.v_mul(side, -h))]
        tangent = nm.v_norm(nm.v_sub(b, a))
        n = nm.v_norm(nm.v_cross(side, tangent))
        if n[1] < 0:
            n = nm.v_mul(n, -1.0)
        uvs = [(0, 1 - t0), (1, 1 - t0), (1, 1 - t1), (0, 1 - t1)]
        verts = [(q, n, uv, (1, 1, 1, 0.5)) for q, uv in zip(quad, uvs)]
        m.triangle_raw(mat, [verts[0], verts[1], verts[2]])
        m.triangle_raw(mat, [verts[0], verts[2], verts[3]])


def pot(m, radius, height, mat):
    m.lathe(mat, [(0, 0), (radius * 0.7, 0), (radius, height * 0.9), (radius * 1.08, height), (radius * 1.08, height * 1.05),
                  (radius * 0.95, height * 1.05)], segments=20, caps=True)
    m.lathe(MAT.format("L2_Soil"), [(0, height * 0.97), (radius * 0.95, height * 0.97)], segments=20)


def make_plants():
    rng = np.random.default_rng(42)
    leaf = MAT.format("L2_Leaf")
    # A fern: fronds arching out of a copper pot.
    m = mb("L2_Fern")
    pot(m, 32, 50, KIT.format("Copper"))
    for k in range(26):
        a = k * 2.39996 + rng.uniform(-0.2, 0.2)
        up = rng.uniform(0.25, 0.75)
        d = nm.v_norm((math.cos(a), up, math.sin(a)))
        length = rng.uniform(70, 120)
        leaf_card(m, leaf, (math.cos(a) * 6, 50, math.sin(a) * 6), d, length, rng.uniform(22, 32), length * rng.uniform(0.5, 0.8),
                  segments=5)
    save(m, "L2_Fern")
    # A palm: a few stems up to broad leaves.
    p = mb("L2_Palm")
    pot(p, 40, 60, KIT.format("BrassAged"))
    for k in range(9):
        a = k * 2.39996
        top = (math.cos(a) * rng.uniform(15, 40), 60 + rng.uniform(110, 200), math.sin(a) * rng.uniform(15, 40))
        p.tube(KIT.format("WoodBeam"), [(0, 55, 0), (top[0] * 0.5, (55 + top[1]) / 2, top[2] * 0.5), top], 2.0, segments=6)
        for j in range(5):
            b = a + (j - 2) * 0.7
            d = nm.v_norm((math.cos(b), rng.uniform(0.0, 0.5), math.sin(b)))
            leaf_card(p, leaf, top, d, rng.uniform(60, 90), rng.uniform(26, 36), rng.uniform(25, 50), segments=4)
    save(p, "L2_Palm")


def make_planter():
    """A long walnut planter 240 x 60 x 70 with brass corners, soil top; pivot bottom centre."""
    m = mb("L2_Planter")
    m.box(KIT.format("Walnut"), (-120, 0, -30), (120, 70, 30), skip=("+y",))
    m.box(MAT.format("L2_Soil"), (-115, 60, -25), (115, 64, 25), skip=("-y",))
    m.box(KIT.format("Walnut"), (-124, 66, -34), (124, 72, 34), skip=())
    for x in (-121, 121):
        for z in (-31, 31):
            m.box(KIT.format("BrassAged"), (x - 3, 0, z - 3), (x + 3, 72, z + 3))
    save(m, "L2_Planter")


def make_tank():
    """An aquarium 220 x 150 x 90 on a walnut stand: brass frame, water, sand, weeds; pivot bottom
    centre, the long side facing -Z."""
    m = mb("L2_Tank")
    w, d = 110.0, 45.0
    m.box(KIT.format("Walnut"), (-w - 8, 0, -d - 8), (w + 8, 70, d + 8))
    y0, y1 = 70.0, 220.0
    brass = KIT.format("BrassAged")
    for x in (-w, w):
        for z in (-d, d):
            m.box(brass, (x - 3, y0, z - 3), (x + 3, y1, z + 3))
    for y in (y0 + 3, y1 - 3):
        for z in (-d, d):
            m.box(brass, (-w, y - 3, z - 3), (w, y + 3, z + 3))
        for x in (-w, w):
            m.box(brass, (x - 3, y - 3, -d), (x + 3, y + 3, d))
    m.box(KIT.format("Concrete"), (-w + 2, y0, -d + 2), (w - 2, y0 + 12, d - 2), skip=("-y",))
    m.box(MAT.format("L2_Water"), (-w + 1, y0 + 12, -d + 1), (w - 1, y1 - 18, d - 1))
    rng = np.random.default_rng(9)
    for k in range(14):
        x, z = rng.uniform(-w + 15, w - 15), rng.uniform(-d + 10, d - 10)
        leaf_card(m, MAT.format("L2_Leaf"), (x, y0 + 12, z), nm.v_norm((rng.uniform(-0.2, 0.2), 1, rng.uniform(-0.2, 0.2))),
                  rng.uniform(40, 100), rng.uniform(10, 16), -rng.uniform(5, 15), segments=4)
    m.box(KIT.format("Walnut"), (-w - 6, y1, -d - 6), (w + 6, y1 + 8, d + 6))
    save(m, "L2_Tank")


def make_beacon():
    """A ceiling or wall alarm beacon: a steel cup and a red lens dome on its axis (+Y out of the
    mount: hang it pitched 180 from a ceiling); the reflector inside spins (separate mesh, same pivot)."""
    m = mb("L2_Beacon")
    m.lathe(KIT.format("SteelDark"), [(0, 0), (16, 0), (16, 3), (13, 4), (13, 9), (11.5, 10), (0, 10)], segments=20, caps=True)
    prof = [(11, 10)] + [(11 * math.cos(a), 10 + 13 * math.sin(a)) for a in np.linspace(0.05, math.pi / 2, 8)]
    m.lathe(MAT.format("L2_AlarmLens"), prof, segments=20)
    for a in range(0, 360, 45):
        r = math.radians(a)
        m.tube(KIT.format("BrassAged"), [(11.6 * math.cos(r), 10, 11.6 * math.sin(r)),
                                         (8 * math.cos(r), 21, 8 * math.sin(r)), (0.1 * math.cos(r), 23.4, 0.1 * math.sin(r))],
               0.7, segments=5)
    save(m, "L2_Beacon")
    r = mb("L2_BeaconReflector")
    for k in range(10):
        a0, a1 = math.radians(-70 + k * 14), math.radians(-70 + (k + 1) * 14)
        r.quad(KIT.format("SteelBrushed"), (7 * math.sin(a0), 11, 7 * math.cos(a0)), (7 * math.sin(a1), 11, 7 * math.cos(a1)),
               (7 * math.sin(a1), 20, 7 * math.cos(a1)), (7 * math.sin(a0), 20, 7 * math.cos(a0)), normal=(0, 0, -1))
    r.lathe(MAT.format("L2_AlarmLens"), [(0, 12), (3, 12), (3, 18), (0, 18)], segments=10, caps=True)
    save(r, "L2_BeaconReflector")


def make_light_strip():
    """A ceiling light bar 300 long (X), 16 wide, 12 deep, hung by two rods; the diffuser faces -Y.
    Pivot at its top centre (hang it from the ceiling)."""
    m = mb("L2_LightStrip")
    m.box(KIT.format("SteelDark"), (-150, -12, -8), (150, 0, 8), skip=("-y",))
    m.box(KIT.format("BrassAged"), (-152, -13, -9), (152, -11, 9), skip=())
    m.polygon(MAT.format("L2_StripGlow"), [(-147, -12.5, -6), (147, -12.5, -6), (147, -12.5, 6), (-147, -12.5, 6)],
              normal=(0, -1, 0))
    for x in (-120, 120):
        m.tube(KIT.format("SteelDark"), [(x, 0, 0), (x, 80, 0)], 1.2, segments=6)
    save(m, "L2_LightStrip")


def make_atrium_ring(apothem=1230.0, inner=1185.0, depth=45.0):
    """The ceiling band between the octagon's wall tops and the dome's base circle: an octagon
    (apothem to the walls' outside) with a round hole; cream underside, walnut inner fascia and a
    brass lip. Pivot at the centre of its underside."""
    m = mb("L2_AtriumRing")
    n = 96
    def outer(a):
        k = round(a / (math.pi / 4))
        return apothem / math.cos(a - k * math.pi / 4)
    for i in range(n):
        a0, a1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
        # engine: x = r sin(a), z = r cos(a) (compass)
        def p(r, a, y):
            return (r * math.sin(a), y, r * math.cos(a))
        r0, r1 = outer(a0), outer(a1)
        m.polygon(KIT.format("PanelCream"), [p(inner, a0, 0), p(r0, a0, 0), p(r1, a1, 0), p(inner, a1, 0)], normal=(0, -1, 0))
        m.polygon(KIT.format("BrassAged"), [p(inner, a0, depth), p(inner, a1, depth), p(r1, a1, depth), p(r0, a0, depth)],
                  normal=(0, 1, 0))
        mid = (a0 + a1) / 2
        inward = (-math.sin(mid), 0, -math.cos(mid))
        m.polygon(KIT.format("Walnut"), [p(inner, a0, 0), p(inner, a1, 0), p(inner, a1, depth), p(inner, a0, depth)],
                  normal=inward)
        m.polygon(KIT.format("BrassAged"), [p(inner - 6, a0, -6), p(inner - 6, a1, -6), p(inner - 6, a1, 0), p(inner - 6, a0, 0)],
                  normal=inward)
        m.polygon(KIT.format("BrassAged"), [p(inner - 6, a0, -6), p(inner, a0, -6), p(inner, a1, -6), p(inner - 6, a1, -6)],
                  normal=(0, -1, 0))
    save(m, "L2_AtriumRing")


def make_rubble():
    """A heap of plaster and panel chunks, a broken pipe stub and a few books, about 160 x 40 x 120;
    pivot bottom centre."""
    rng = np.random.default_rng(77)
    m = mb("L2_Rubble")
    for k in range(26):
        sx, sy, sz = rng.uniform(8, 30), rng.uniform(4, 14), rng.uniform(8, 26)
        r = rng.uniform(0, 1) ** 0.6
        a = rng.uniform(0, 2 * math.pi)
        cx, cz = math.cos(a) * r * 70, math.sin(a) * r * 50
        cy = (1 - r) * 22 + sy / 2
        yaw = rng.uniform(0, math.pi)
        ax = ((math.cos(yaw), rng.uniform(-0.25, 0.25), math.sin(yaw)), (0, 1, 0), (-math.sin(yaw), 0, math.cos(yaw)))
        ax = (nm.v_norm(ax[0]), nm.v_norm(nm.v_cross(ax[2], ax[0])), nm.v_norm(ax[2]))
        mat = [MAT.format("L2_Plaster"), KIT.format("PanelGreen"), KIT.format("PanelCream"), KIT.format("Walnut")][k % 4]
        m.oriented_box(mat, (cx, cy, cz), ax, (sx / 2, sy / 2, sz / 2))
    m.tube(KIT.format("Copper"), [(-60, 8, 30), (20, 10, 45)], 8, segments=10)
    save(m, "L2_Rubble")


def make_hand_lamp():
    """A brass hurricane lantern 40 tall: the glass globe glows (LampGlow); pivot bottom centre."""
    m = mb("L2_HandLamp")
    m.lathe(KIT.format("BrassAged"), [(0, 0), (11, 0), (11, 4), (8, 6), (8, 9), (0, 9)], segments=16, caps=True)
    m.lathe(KIT.format("LampGlow"), [(0, 9), (6, 10), (8.5, 17), (7, 25), (4, 28), (0, 28)], segments=16)
    m.lathe(KIT.format("BrassAged"), [(0, 28), (7, 28), (7, 31), (3, 34), (0, 35)], segments=16, caps=True)
    for a in (0, 120, 240):
        r = math.radians(a)
        m.tube(KIT.format("BrassAged"), [(9 * math.cos(r), 6, 9 * math.sin(r)), (10 * math.cos(r), 17, 10 * math.sin(r)),
                                         (7.5 * math.cos(r), 29, 7.5 * math.sin(r))], 0.6, segments=5)
    m.tube(KIT.format("BrassAged"), [(-7, 33, 0), (-5, 42, 0), (0, 44, 0), (5, 42, 0), (7, 33, 0)], 0.5, segments=5)
    save(m, "L2_HandLamp")


# =============================================================================
# Particles
# =============================================================================
EMITTER_DEFAULTS = dict(
    Enabled="true", Type="Sprite", Modules="Shape Velocity Forces", Mesh="Engine/Content/Meshes/SpriteOctagon.nmmesh",
    Material="Content/Materials/Effects/Steam.material.nmgraph", Offset="0,0,0", Billboard="FaceCamera",
    VelocityStretch="0", MeshOrientation="RandomSpin", RandomRotation="true", RotationRate="0.5", SpawnMode="Continuous",
    MaxParticles="256", Shape="Sphere", ShapeSurface="false", BoxSize="10,10,10", ConeAngle="25", RingWidth="0",
    ShapeSpeed="0", SpawnRate="10", BurstCount="0", BurstInterval="0", BurstCycles="1", Lifetime="2",
    LifetimeVariance="0.3", EmitterRadius="5", Seed="0", CullDistance="6000", PauseOffscreen="true", Velocity="0,0,0",
    VelocitySpread="0", Acceleration="0,0,0", Drag="0", Turbulence="0", CollisionResponse="None",
    CollisionHeight="-100000", Bounce="0.3", Friction="0.2", FlipbookColumns="1", FlipbookRows="1", FlipbookFrames="0",
    FlipbookRate="0", FlipbookCycles="1", FlipbookRandomStart="false", BeamEnd="0,200,0", BeamSegments="16",
    BeamNoise="0", BeamNoiseSpeed="4", SizeMode="StartEnd", StartSize="10", EndSize="10", SizeKeys="",
    ColorMode="StartEnd", StartColor="255,255,255,255", EndColor="255,255,255,0", ColorCurve="0.6", FadeIn="0.0",
    FadeOut="0.5", ColorKeys="")
FLASH = "Content/Materials/Effects/MuzzleFlash.material.nmgraph"


def particle(name, emitters):
    out = ["NimbleParticleSystem 1", ""]
    for ename, fields in emitters:
        out.append(f"Emitter {ename}")
        for k, v in {**EMITTER_DEFAULTS, **fields}.items():
            out.append(f"    {k} {v}".rstrip())
        out.append("")
    PART_DIR.mkdir(parents=True, exist_ok=True)
    (PART_DIR / f"{name}.nmparticle").write_text("\n".join(out), newline="\n")
    print(f"  {name}.nmparticle")


def make_particles():
    # (Dust motes, sprinklers, steam jets and sparks are the kit's: Content/Particles/*.nmparticle.)
    # Smoke rising from a burnt-out panel, dark and slow.
    particle("L2_Smoke", [("Smoke", dict(Shape="Sphere", EmitterRadius="25", SpawnRate="10", MaxParticles="96",
                                         Lifetime="6", Velocity="0,45,0", VelocitySpread="12", Drag="0.3",
                                         Turbulence="18", StartSize="40", EndSize="260", StartColor="55,52,50,120",
                                         EndColor="70,68,66,0", ColorCurve="0.5", CullDistance="8000"))])
    # The conservatory's warm humid air: slow mist low over the beds.
    particle("L2_Mist", [("Mist", dict(Shape="Box", BoxSize="1600,60,1600", SpawnRate="10", MaxParticles="96",
                                       Lifetime="9", Velocity="0,6,0", VelocitySpread="6", Turbulence="8",
                                       StartSize="120", EndSize="320", StartColor="230,240,235,40",
                                       EndColor="230,240,235,0", CullDistance="8000"))])
    # Bubbles in the tank.
    particle("L2_Bubbles", [("Bubbles", dict(Material=FLASH, Shape="Box", BoxSize="180,4,60", SpawnRate="14",
                                             MaxParticles="96", Lifetime="1.5", Velocity="0,85,0", VelocitySpread="6",
                                             Turbulence="12", StartSize="1.4", EndSize="2.2",
                                             StartColor="40,60,60,120", EndColor="40,60,60,0"))])


# =============================================================================
# Post process stacks
# =============================================================================
def pp_stack(name, comment, effects):
    g = graphs.Graph(name, schema="PostProcess")
    for type_, props in effects:
        g.node(f"Effect.{type_}", props=props)
    path = PP_DIR / f"{name}.pp.nmgraph"
    g.write(path)
    text = path.read_text()
    text = text.replace("  </Nodes>", "  </Nodes>", 1)
    if "<Comments>" not in text and comment:
        text = text.replace("</NodeGraph>", f"  <Comments>\n    <Comment X=\"-350\" Y=\"-120\" Width=\"1100\" Height=\"90\" "
                                            f"Text=\"{comment}\" />\n  </Comments>\n</NodeGraph>")
        path.write_text(text, newline="\n")
    return path


def make_stacks():
    common = [
        ("AmbientOcclusion", {"Blur": "true", "Intensity": "1", "Power": "1.5", "Radius": "60", "Samples": "12"}),
        ("ScreenSpaceGI", {"Intensity": "1", "Rays": "2", "Radius": "400"}),
        ("ScreenSpaceReflections", {"Intensity": "1", "Max Distance": "2500", "Max Roughness": "0.6", "Steps": "24",
                                    "Thickness": "25"}),
        ("Antialiasing", {"Edge Threshold": "0.125", "Method": "TAA", "Quality": "High", "Sharpness": "0.35"}),
    ]
    calm = pp_stack("L2_Calm", "Level 2, calm: SSGI over the DDGI volumes (the C3 bounce arrives live), AO, SSR on brass and "
                    "marble, the dome's sun shafting through a dusty haze, warm bloom, eye adaptation held so the "
                    "unpowered lab stays dark, ACES, TAA.", common + [
        ("VolumetricFog", {"Albedo": "1 0.97 0.93", "Anisotropy": "0.7", "Base Height": "0", "Density": "0.00007",
                           "Height Falloff": "0", "Light Scattering": "0.5", "Max Distance": "5000", "Shadows": "true",
                           "Sky Scattering": "0.15", "Steps": "16", "Sun Scattering": "4"}),
        ("Bloom", {"Intensity": "0.5", "Levels": "6", "Radius": "0.8", "Threshold": "0.9", "Tint": "1 0.95 0.88"}),
        ("ToneMapping", {"Exposure": "-1.5", "Tone Mapper": "ACES"}),
        ("AutoExposure", {"Centre Weight": "0.5", "Key": "0.15", "Max EV": "0.3", "Min EV": "-1.5", "Speed Down": "1.2",
                          "Speed Up": "2.5"}),
    ])
    alarm = pp_stack("L2_Alarm", "Level 2 on alarm: thicker smoky haze the red beacons sweep through, hotter bloom, "
                     "a little darker exposure.", common + [
        ("VolumetricFog", {"Albedo": "0.9 0.86 0.82", "Anisotropy": "0.55", "Base Height": "0", "Density": "0.0001",
                           "Height Falloff": "0", "Light Scattering": "0.6", "Max Distance": "5000", "Shadows": "true",
                           "Sky Scattering": "0.2", "Steps": "16", "Sun Scattering": "1.5"}),
        ("Bloom", {"Intensity": "0.75", "Levels": "6", "Radius": "0.85", "Threshold": "0.85", "Tint": "1 0.85 0.8"}),
        ("ToneMapping", {"Exposure": "-1.2", "Tone Mapper": "ACES"}),
        ("AutoExposure", {"Centre Weight": "0.5", "Key": "0.13", "Max EV": "0.5", "Min EV": "-1.5", "Speed Down": "1",
                          "Speed Up": "2.5"}),
    ])
    return [calm, alarm]


# =============================================================================
def find_generate(explicit=None):
    if explicit:
        return Path(explicit) / "NodeGraphGenerate"
    roots = [GAME, ROOT, Path("/home/ndanque/Projects/NimbleEngine")]
    found = sorted((f for r in roots for f in glob.glob(str(r / "Games" / "*" / "Binaries" / "*" / "NodeGraphGenerate"))),
                   key=lambda p: Path(p).stat().st_mtime, reverse=True)
    return Path(found[0]) if found else None


def main():
    args = sys.argv[1:]
    bin_dir = args[args.index("--bin") + 1] if "--bin" in args else None
    print("textures")
    make_plaques()
    make_leaf()
    print("materials")
    graphs_written = make_materials()
    print("meshes")
    make_plaque_meshes()
    make_curtain()
    make_plants()
    make_planter()
    make_tank()
    make_beacon()
    make_light_strip()
    make_atrium_ring()
    make_rubble()
    make_hand_lamp()
    print("particles")
    make_particles()
    print("post process")
    graphs_written += make_stacks()
    gen = find_generate(bin_dir)
    if gen and gen.exists():
        subprocess.run([str(gen)] + [str(p) for p in graphs_written], check=True)
    else:
        print("NodeGraphGenerate not found: <gen_code> left empty (pass --bin)")


if __name__ == "__main__":
    main()
