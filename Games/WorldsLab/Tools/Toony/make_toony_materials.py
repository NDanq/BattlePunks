#!/usr/bin/env python3
"""
make_toony_materials.py -- writes Content/Materials/Toony/: the Toony style's
material graphs, as the editor's material graph editor saves them, then has
the engine's NodeGraphGenerate compile each one's <gen_code>.

    ToonPaint     every prop and building: the mesh's vertex colours, Lit, very
                  rough, barely specular -- flat candy paint
    ToonDummyRed  the crash dummy in a team's colour: its vertex colours times
    ToonDummyBlue the team's tint, a little emissive so it pops in shade
    ToonGun       the pistol: dark, satin
    ToonInk       the post process (PostProcess, after the effects): ink lines
                  where the scene's depth jumps (Fwidth of the window depth,
                  relative to the distance) and where its colour does, light banding of
                  the brightness into a few steps, and a touch more saturation

Usage (after building the editor once, so NodeGraphGenerate exists):

    python Games/WorldsLab/Tools/Toony/make_toony_materials.py
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import toony_kit as kit          # noqa: E402

OUT = kit.GAME_DIR / "Content" / "Materials" / kit.STYLE


class Graph:
    def __init__(self, name):
        self.name = name
        self.nodes, self.links = [], []
        self.count = 0

    def node(self, type_name, x, y, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        node_id = hashlib.md5(f"Toony/{self.name}/{self.count}".encode()).hexdigest()
        attrs = f" Wildcard=\"{wildcard}\"" if wildcard else ""
        body = ""
        for key, value in sorted((props or {}).items()):
            body += f"      <Prop Name=\"{key}\" Value=\"{value}\" />\n"
        for pin, value in (consts or {}).items():
            body += f"      <Const Pin=\"{pin}\" Value=\"{value}\" />\n"
        head = f"    <Node Id=\"{node_id}\" Type=\"{type_name}\" Version=\"{version}\" X=\"{x}\" Y=\"{y}\"{attrs}"
        self.nodes.append(head + (" />\n" if not body else f">\n{body}    </Node>\n"))
        return node_id

    def link(self, src, pin, dst, dst_pin):
        self.links.append(f"    <Link From=\"{src}.{pin}\" To=\"{dst}.{dst_pin}\" />\n")

    def param(self, name, type_name, default, x, y):
        return self.node("Parameter", x, y, {"Name": name, "Type": type_name, "Default": default})

    def output(self, x, y, domain="Opaque", shading="Lit"):
        return self.node("Output.Material", x, y, {"Domain": domain, "Shading": shading, "ClipValue": "0.5",
                                                   "MaxOffset": "0", "TwoSided": "false"})

    def write(self, comment):
        text = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<NodeGraph Version=\"1\" Schema=\"Material\">\n  <Nodes>\n" + "".join(self.nodes) + "  </Nodes>\n"
                "  <Links>\n" + "".join(self.links) + "  </Links>\n"
                "  <Comments>\n"
                f"    <Comment X=\"-700\" Y=\"-420\" Width=\"640\" Height=\"90\" Text=\"{comment}\" />\n"
                "  </Comments>\n</NodeGraph>\n")
        path = OUT / f"{self.name}.material.nmgraph"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, newline="\n")
        return path


def paint(name, tint, roughness, specular, emissive, comment, metallic=0.0):
    """Vertex colour x Tint, Lit; Emissive a fraction of the same (shade never goes black)."""
    g = Graph(name)
    out = g.output(700, 0)
    vcol = g.node("Input.VertexColor", 0, -100)
    tint_p = g.param("Tint", "vec3", " ".join(f"{c:g}" for c in tint), 0, 40)
    colour = g.node("Math.Multiply", 250, -60, wildcard="vec3")
    g.link(vcol, "RGB", colour, "A")
    g.link(tint_p, "Value", colour, "B")
    g.link(colour, "Out", out, "BaseColor")
    glow = g.param("Glow", "float", f"{emissive:g}", 250, 120)
    self_lit = g.node("Math.Multiply", 450, 80, wildcard="vec3")
    g.link(colour, "Out", self_lit, "A")
    g.link(glow, "Value", self_lit, "B")
    g.link(self_lit, "Out", out, "Emissive")
    rough = g.param("Roughness", "float", f"{roughness:g}", 450, 200)
    g.link(rough, "Value", out, "Roughness")
    spec = g.param("Specular", "float", f"{specular:g}", 450, 300)
    g.link(spec, "Value", out, "Specular")
    metal = g.param("Metallic", "float", f"{metallic:g}", 450, 400)
    g.link(metal, "Value", out, "Metallic")
    return g.write(comment)


def ink():
    g = Graph("ToonInk")
    out = g.output(1900, 0, domain="PostProcess", shading="Unlit")
    scene = g.node("Input.SceneTexture", -1000, 0)
    depth = g.node("Input.SceneDepth", -1000, 400)

    # Depth edges: how much the depth changes around the pixel, as a fraction
    # of the distance -- the same line weight near and far. With window depth
    # d, distance goes as 1 / (1 - d), so its relative change is
    # fwidth(d) / (1 - d).
    dw = g.node("Math.Fwidth", -800, 400, wildcard="float")
    g.link(depth, "Depth", dw, "X")
    far = g.node("Math.OneMinus", -900, 520, wildcard="float")
    g.link(depth, "Depth", far, "X")
    dist = g.node("Math.Max", -800, 520, consts={"B": "0.00001"}, wildcard="float")
    g.link(far, "Out", dist, "A")
    rel = g.node("Math.Divide", -600, 440, wildcard="float")
    g.link(dw, "Out", rel, "A")
    g.link(dist, "Out", rel, "B")
    d0 = g.param("DepthEdge0", "float", "0.03", -600, 560)
    d1 = g.param("DepthEdge1", "float", "0.08", -600, 640)
    depth_ink = g.node("Math.Smoothstep", -400, 440, wildcard="float")
    g.link(d0, "Value", depth_ink, "Edge0")
    g.link(d1, "Value", depth_ink, "Edge1")
    g.link(rel, "Out", depth_ink, "X")

    # Colour edges: where one flat colour meets another (the chamfers, a
    # wall on the grass).
    lum = g.node("Vector.Dot", -800, 160, consts={"B": "0.2126 0.7152 0.0722 0"}, wildcard="vec4")
    g.link(scene, "Color", lum, "A")
    lw = g.node("Math.Fwidth", -600, 220, wildcard="float")
    g.link(lum, "Out", lw, "X")
    colour_ink = g.node("Math.Smoothstep", -400, 220, consts={"Edge0": "0.06", "Edge1": "0.16"}, wildcard="float")
    g.link(lw, "Out", colour_ink, "X")
    colour_weight = g.param("ColourInk", "float", "0.7", -400, 320)
    colour_ink_w = g.node("Math.Multiply", -200, 260, wildcard="float")
    g.link(colour_ink, "Out", colour_ink_w, "A")
    g.link(colour_weight, "Value", colour_ink_w, "B")
    ink_amount = g.node("Math.Max", 0, 340, wildcard="float")
    g.link(depth_ink, "Out", ink_amount, "A")
    g.link(colour_ink_w, "Out", ink_amount, "B")
    strength = g.param("InkStrength", "float", "0.9", 0, 460)
    ink_final = g.node("Math.Multiply", 200, 360, wildcard="float")
    g.link(ink_amount, "Out", ink_final, "A")
    g.link(strength, "Value", ink_final, "B")

    # Banding: the brightness rounded to Bands steps, the colour scaled to
    # match, blended in by BandStrength.
    bands = g.param("Bands", "float", "4", -600, -200)
    scaled = g.node("Math.Multiply", -400, -160, wildcard="float")
    g.link(lum, "Out", scaled, "A")
    g.link(bands, "Value", scaled, "B")
    half = g.node("Math.Add", -250, -160, consts={"B": "0.5"}, wildcard="float")
    g.link(scaled, "Out", half, "A")
    stepped = g.node("Math.Floor", -100, -160, wildcard="float")
    g.link(half, "Out", stepped, "X")
    quant = g.node("Math.Divide", 50, -160, wildcard="float")
    g.link(stepped, "Out", quant, "A")
    g.link(bands, "Value", quant, "B")
    safe = g.node("Math.Max", 50, -40, consts={"B": "0.001"}, wildcard="float")
    g.link(lum, "Out", safe, "A")
    ratio = g.node("Math.Divide", 200, -100, wildcard="float")
    g.link(quant, "Out", ratio, "A")
    g.link(safe, "Out", ratio, "B")
    band_strength = g.param("BandStrength", "float", "0.35", 200, 20)
    factor = g.node("Math.Lerp", 400, -100, consts={"A": "1"}, wildcard="float")
    g.link(ratio, "Out", factor, "B")
    g.link(band_strength, "Value", factor, "T")
    banded = g.node("Math.Multiply", 600, -40, wildcard="vec4")
    g.link(scene, "Color", banded, "A")
    g.link(factor, "Out", banded, "B")

    # A little more saturation.
    grey = g.node("Math.Multiply", 600, 120, consts={"A": "1 1 1 1"}, wildcard="vec4")
    g.link(lum, "Out", grey, "B")
    sat = g.param("Saturation", "float", "1.15", 600, 220)
    vivid = g.node("Math.Lerp", 800, 40, wildcard="vec4")
    g.link(grey, "Out", vivid, "A")
    g.link(banded, "Out", vivid, "B")
    g.link(sat, "Value", vivid, "T")
    floor_ = g.node("Math.Max", 1000, 40, consts={"B": "0 0 0 0"}, wildcard="vec4")
    g.link(vivid, "Out", floor_, "A")

    ink_colour = g.param("InkColor", "vec4", "0.08 0.06 0.12 1", 1000, 300)
    final = g.node("Math.Lerp", 1300, 100, wildcard="vec4")
    g.link(floor_, "Out", final, "A")
    g.link(ink_colour, "Value", final, "B")
    g.link(ink_final, "Out", final, "T")
    if os.environ.get("TOONY_DEBUG_DEPTH"):
        probe = g.node("Math.Multiply", 1500, 300, consts={"B": "300"}, wildcard="float")
        g.link(far, "Out", probe, "A")
        g.link(probe, "Out", out, "Emissive")
    else:
        g.link(final, "Out", out, "Emissive")
    return g.write("Toony ToonInk: ink where the scene's depth jumps (Fwidth of its depth, relative) "
                   "and where flat colours meet, the brightness banded into a few steps, a little more saturation.")


def main():
    paths = [
        paint("ToonPaint", (1, 1, 1), 0.92, 0.12, 0.06,
              "Toony ToonPaint: the meshes' vertex colours as flat candy paint -- Lit, very rough, barely "
              "specular, a little self-lit so no shade goes black."),
        paint("ToonDummyRed", kit.RED_LIGHT, 0.8, 0.2, 0.12,
              "Toony ToonDummyRed: the crash dummy's colours tinted the red team's."),
        paint("ToonDummyBlue", kit.BLUE_LIGHT, 0.8, 0.2, 0.12,
              "Toony ToonDummyBlue: the crash dummy's colours tinted the blue team's."),
        paint("ToonGun", (0.35, 0.35, 0.42), 0.5, 0.3, 0.02, "Toony ToonGun: the pistol, dark satin.", metallic=0.3),
        ink(),
    ]
    for path in paths:
        print(f"  {path.relative_to(kit.GAME_DIR)}")
    gen = kit.generator()
    if not gen.exists():
        sys.exit(f"{gen} not found: build the editor-debug preset first, then run this again")
    result = subprocess.run([str(gen)] + [str(p) for p in paths], capture_output=True, text=True)
    print(result.stdout + result.stderr)
    if result.returncode != 0:
        sys.exit(f"NodeGraphGenerate failed ({result.returncode})")


if __name__ == "__main__":
    main()
