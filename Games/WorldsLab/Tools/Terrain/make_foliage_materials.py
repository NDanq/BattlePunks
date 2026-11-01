#!/usr/bin/env python3
"""
make_foliage_materials.py -- the terrain foliage's material graphs,
Content/Materials/Foliage/*.material.nmgraph, over make_foliage_textures.py's
textures, without their generated code. Then fill <gen_code>:

    python Games/WorldsLab/Tools/Terrain/make_foliage_materials.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/Foliage/*.material.nmgraph

    PineNeedles  Masked, Subsurface: the spruce's frond cards. Light comes
                 through them (Subsurface Color from the needles' own colour, thin),
                 the mesh's vertex colour darkens the inside of the crown.
    PineBark     Opaque: the trunk, tiling bark (the mesh's UVs are in repeats).
    Grass        Masked, Subsurface: grass tufts, darker at the roots.
    Shrub        Masked, Subsurface: bushes, over Courtyard's leaf cards.

The cards are one-sided: their meshes carry both faces, each with the plant's
outward normal (a Two Sided material would turn a back face's round). The
meshes (make_foliage_meshes.py) also carry the wind: vertex colour A is how far a
vertex bends with its plant (0 at the roots, 1 at the top), and the frond cards'
UV u runs 0 at the twig to 1 at the tip, how much it flutters. Every material's
sway is the same function of time and where the vertex is, so a trunk and its
branches move together, and plants a few metres apart do not move in step.
"""

import hashlib
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Courtyard"))
import make_courtyard_materials as cy  # noqa: E402

MAT_DIR = GAME_DIR / "Content" / "Materials" / "Foliage"
TEX = "Content/Textures/Foliage"
vec = cy.vec


class Graph(cy.Graph):
    """Courtyard's graph, its node ids from Foliage's names instead."""
    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"Foliage/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard,
                           (self.count % 6) * 220 - 600, (self.count // 6) * 160 - 400))
        return nid


def output(g, domain, model, two_sided, max_offset, consts=None):
    return g.node("Output.Material", props={"ClipValue": "0.5", "Domain": domain, "MaxOffset": vec(max_offset),
                                            "Shading": "Lit", "ShadingModel": model,
                                            "TwoSided": "true" if two_sided else "false"}, consts=consts)


def wind(g, out, colour, bend, flutter=0.0, flutter_weight=None):
    """World Position Offset: the plant leans and sways along the wind (the Wind node:
    the Weather plugin's direction, times its strength), bend world units at vertex
    colour A = 1 and Strength 1 (more in gusts, never back past upright by much); its
    tips flutter up and down, flutter units at flutter_weight = 1 (a (node, pin), 0-1)."""
    split = g.node("Vector.Split")
    g.link(g.node("Input.WorldPosition"), "Position", split, "Vector")
    time = (g.node("Input.Time"), "Time")
    x, y, z = (split, "X"), (split, "Y"), (split, "Z")
    scaled = lambda a, k: (g.math("Multiply", a, consts={"B": vec(k)}), "Out")
    added = lambda a, b: (g.math("Add", a, b), "Out")
    plus = lambda a, k: (g.math("Add", a, consts={"B": vec(k)}), "Out")
    sine = lambda a: (g.math("Sin", a), "Out")
    # Per plant: changes little across one tree (0.2 rad over 3 m), a lot between trees.
    phase = added(scaled(x, 0.0007), scaled(z, 0.0009))
    gust = plus(added(scaled(sine(added(scaled(time, 0.9), phase)), 0.55),
                      scaled(sine(added(scaled(time, 2.1), scaled(phase, 1.7))), 0.2)), 0.45)
    # The world's wind (the Weather plugin's; a breeze of 1 toward (0.8, 0, 0.6) without one):
    # the plants lean its way, as far as its strength.
    wind_node = g.node("Input.Wind")
    wind_dir = g.node("Vector.Split")
    g.link(wind_node, "Direction", wind_dir, "Vector")
    strength = (wind_node, "Strength")
    times = lambda a, b: (g.math("Multiply", a, b), "Out")
    lean = (g.math("Multiply", gust, (colour, "A")), "Out")
    lean = times(scaled(lean, bend), strength)
    offset = g.node("Vector.Make3")
    if flutter and flutter_weight is not None:
        leaf = added(added(scaled(x, 0.021), scaled(z, 0.017)), scaled(y, 0.013))
        bob = sine(added(scaled(time, 3.3), leaf))
        bob = times((g.math("Multiply", scaled(bob, flutter), flutter_weight), "Out"), strength)
        g.link(*added(times(lean, (wind_dir, "X")), scaled(bob, 0.3)), offset, "X")
        g.link(*bob, offset, "Y")
        g.link(*added(times(lean, (wind_dir, "Z")), scaled(bob, 0.2)), offset, "Z")
    else:
        g.link(*times(lean, (wind_dir, "X")), offset, "X")
        g.link(*times(lean, (wind_dir, "Z")), offset, "Z")
    g.link(offset, "Out", out, "WorldPositionOffset")


def u_of_uv(g):
    split = g.node("Vector.Split")
    g.link(g.node("Input.TexCoord"), "UV", split, "Vector")
    return (split, "X")


def card_material(name, base_tex, normal_tex, sss_tint, thickness, roughness, bend, flutter, flutter_from_u,
                  tint="1 1 1", specular=0.25):
    """A Masked Subsurface card: base colour x vertex colour x Tint, Subsurface Color the
    base colour brightened and tinted (light through a leaf is yellower). One-sided: the
    meshes give each card both faces, each with the card's own (outward) normal; Two
    Sided would turn a back face's normal round, and a crown seen from below go dark.
    Specular low: thin blades and needles reflect little, and a card's up-leaning normal
    would otherwise catch the sky white at grazing angles."""
    g = Graph(name)
    out = output(g, "Masked", "Subsurface", False, bend + flutter + 5.0,
                 consts={"Roughness": vec(roughness), "Specular": vec(specular), "Thickness": vec(thickness)})
    colour = g.node("Input.VertexColor")
    base = g.texture(base_tex, None)
    tint = g.param("Tint", "vec3", tint)
    albedo = g.math("Multiply", (base, "RGB"), (colour, "RGB"), wildcard="vec3")
    albedo = g.math("Multiply", (albedo, "Out"), (tint, "Value"), wildcard="vec3")
    g.link(albedo, "Out", out, "BaseColor")
    g.link(base, "A", out, "OpacityMask")
    sss = g.math("Multiply", (base, "RGB"), None, wildcard="vec3", consts={"B": vec(*sss_tint)})
    g.link(sss, "Out", out, "SubsurfaceColor")
    ao = g.node("Vector.Split")
    g.link(colour, "RGB", ao, "Vector")
    g.link(ao, "X", out, "AmbientOcclusion")
    if normal_tex:
        g.link(g.normal_map(normal_tex, None), "Normal", out, "Normal")
    wind(g, out, colour, bend, flutter, u_of_uv(g) if flutter_from_u else (colour, "A"))
    return g


def pine_bark():
    g = Graph("PineBark")
    out = output(g, "Opaque", "Default", False, 35.0)
    colour = g.node("Input.VertexColor")
    base = g.texture(f"{TEX}/Bark_BaseColor.nmtexture", None)
    surface = g.texture(f"{TEX}/Bark_Surface.nmtexture", None)
    g.link(g.math("Multiply", (base, "RGB"), (colour, "RGB"), wildcard="vec3"), "Out", out, "BaseColor")
    g.link(surface, "R", out, "Roughness")
    ao = g.node("Vector.Split")
    g.link(colour, "RGB", ao, "Vector")
    g.link(g.math("Multiply", (surface, "G"), (ao, "X")), "Out", out, "AmbientOcclusion")
    g.link(g.normal_map(f"{TEX}/Bark_Normal.nmtexture", None), "Normal", out, "Normal")
    wind(g, out, colour, 30.0)
    return g


MATERIALS = [
    lambda: card_material("PineNeedles", f"{TEX}/PineFrond_BaseColor.nmtexture", f"{TEX}/PineFrond_Normal.nmtexture",
                          (1.5, 1.7, 0.9), 0.3, 0.7, 30.0, 6.0, True),
    pine_bark,
    lambda: card_material("Grass", f"{TEX}/Grass_BaseColor.nmtexture", None, (1.1, 1.25, 0.8), 0.45, 0.9, 6.0, 3.0, False),
    lambda: card_material("Shrub", "Content/Textures/Courtyard/Foliage_BaseColor.nmtexture",
                          "Content/Textures/Courtyard/Foliage_Normal.nmtexture", (1.1, 1.25, 0.8), 0.45, 0.8, 5.0, 3.0,
                          False, tint="0.72 0.78 0.66"),
]


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    for make in MATERIALS:
        g = make()
        path = MAT_DIR / f"{g.name}.material.nmgraph"
        g.write(path)
        print(f"  {path.relative_to(GAME_DIR)}: {len(g.nodes)} nodes")


if __name__ == "__main__":
    main()
