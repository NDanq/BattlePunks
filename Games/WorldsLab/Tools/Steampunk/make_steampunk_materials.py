#!/usr/bin/env python3
"""
make_steampunk_materials.py -- writes Content/Materials/Steampunk/: the
material graphs of the Steampunk street corner, as the editor's material
graph editor saves them, then has the engine's own NodeGraphGenerate compile
each one's <gen_code> (exactly what the editor's Save writes).

The surfaces share one graph shape (make_steampunk_textures.py's sets):

    Wetness     the vertex colour's alpha scales Roughness: 1 dry, less
                where the ground is wet (puddles, gutters)
    UV          Texture Coordinate x UVScale (a vec2 parameter: 1 / the
                metres the texture covers -- the meshes' UV0 is in metres)
    BaseColor   <Set>_BaseColor x the vertex colour (baked grime and
                occlusion) x Tint (a vec3 parameter, white: per entity)
    ORM         <Set>_ORM: R -> Ambient Occlusion, G x RoughnessScale ->
                Roughness, B -> Metallic
    Normal      <Set>_Normal through a Normal Map node (OpenGL)

and the rest are small:

    WindowLit   dark glass whose Emissive is WindowLit_Emissive x Glow
    LampGlow    a gas mantle's glass: Emissive = Color x Intensity (bloom)
    ClockFace   the dial's texture, lit, and faintly backlit (Emissive)
    Gauge       a pressure gauge's dial
    Steam       a particle's soft puff: Translucent Lit, colour and opacity
                from the particle's colour
    Fabric      an airship's envelope: Tint, rough, with the Paint set's
                normal faint

Usage (after building the editor once, so NodeGraphGenerate exists):

    python Games/WorldsLab/Tools/Steampunk/make_steampunk_materials.py
"""

import hashlib
import subprocess
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
OUT = GAME_DIR / "Content" / "Materials" / "Steampunk"
TEX = "Content/Textures/Steampunk"
GENERATOR = GAME_DIR / "Binaries" / "Linux_Debug" / "NodeGraphGenerate"


class Graph:
    def __init__(self, name):
        self.name = name
        self.nodes, self.links = [], []
        self.count = 0

    def node(self, type_name, x, y, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        node_id = hashlib.md5(f"Steampunk/{self.name}/{self.count}".encode()).hexdigest()
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

    def texture(self, path, x, y, uv=None, wrap="Default"):
        obj = self.node("Texture.Object", x, y, {"Texture": path, "Filter": "Default", "Wrap": wrap})
        sample = self.node("Texture.Sample", x + 220, y, version=2)
        self.link(obj, "Texture", sample, "Texture")
        if uv:
            self.link(uv[0], uv[1], sample, "UV")
        return sample

    def output(self, x, y, domain="Opaque", shading="Lit", two_sided=False):
        return self.node("Output.Material", x, y, {"Domain": domain, "Shading": shading, "ClipValue": "0.5",
                                                   "MaxOffset": "0", "TwoSided": "true" if two_sided else "false"})

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


def surface(name, texture_set, metres, roughness_scale=1.0, normal_strength=1.0, comment="", two_sided=False):
    g = Graph(name)
    out = g.output(900, 0, two_sided=two_sided)
    coord = g.node("Input.TexCoord", -700, 0)
    scale = g.param("UVScale", "vec2", f"{1.0 / metres:.6g} {1.0 / metres:.6g}", -700, 120)
    uv = g.node("Math.Multiply", -480, 40, wildcard="vec2")
    g.link(coord, "UV", uv, "A")
    g.link(scale, "Value", uv, "B")
    base = g.texture(f"{TEX}/{texture_set}_BaseColor.nmtexture", -300, -260, (uv, "Out"))
    orm = g.texture(f"{TEX}/{texture_set}_ORM.nmtexture", -300, 0, (uv, "Out"))
    normal = g.texture(f"{TEX}/{texture_set}_Normal.nmtexture", -300, 260, (uv, "Out"))
    vcol = g.node("Input.VertexColor", 0, -400)
    tint = g.param("Tint", "vec3", "1 1 1", 0, -300)
    m1 = g.node("Math.Multiply", 220, -300, wildcard="vec3")
    g.link(base, "RGB", m1, "A")
    g.link(vcol, "RGB", m1, "B")
    m2 = g.node("Math.Multiply", 420, -300, wildcard="vec3")
    g.link(m1, "Out", m2, "A")
    g.link(tint, "Value", m2, "B")
    g.link(m2, "Out", out, "BaseColor")
    rscale = g.param("RoughnessScale", "float", f"{roughness_scale:g}", 220, 60)
    rough = g.node("Math.Multiply", 420, 20, wildcard="float")
    g.link(orm, "G", rough, "A")
    g.link(rscale, "Value", rough, "B")
    wet = g.node("Math.Multiply", 620, 20, wildcard="float")      # vertex alpha: 1 dry, lower wet
    g.link(rough, "Out", wet, "A")
    g.link(vcol, "A", wet, "B")
    g.link(wet, "Out", out, "Roughness")
    g.link(orm, "R", out, "AmbientOcclusion")
    g.link(orm, "B", out, "Metallic")
    nmap = g.node("Texture.NormalMap", 420, 260, {"Format": "OpenGL"}, consts={"Strength": f"{normal_strength:g}"})
    g.link(normal, "RGB", nmap, "Color")
    g.link(uv, "Out", nmap, "UV")
    g.link(nmap, "Normal", out, "Normal")
    return g.write(comment or f"Steampunk {name}: the {texture_set} set, {metres} m a tile on the meshes' metre UVs; "
                              "base colour times the vertex colour (baked grime) and Tint.")


def window_lit():
    g = Graph("WindowLit")
    out = g.output(700, 0)
    tex = g.texture(f"{TEX}/WindowLit_Emissive.nmtexture", -200, 0)
    glow = g.param("Glow", "float", "1.6", 0, 200)
    mul = g.node("Math.Multiply", 300, 100, wildcard="vec3")
    g.link(tex, "RGB", mul, "A")
    g.link(glow, "Value", mul, "B")
    g.link(mul, "Out", out, "Emissive")
    base = g.param("BaseColor", "vec3", "0.02 0.02 0.02", 300, -150)
    g.link(base, "Value", out, "BaseColor")
    rough = g.param("Roughness", "float", "0.08", 300, -60)
    g.link(rough, "Value", out, "Roughness")
    return g.write("Steampunk WindowLit: a lit window seen from the street -- dark, glossy glass glowing with "
                   "lamplight through net curtains (UV 0-1 a pane). Glow scales it; past 1 it blooms.")


def lamp_glow():
    g = Graph("LampGlow")
    out = g.output(600, 0)
    colour = g.param("Color", "vec3", "1 0.62 0.3", 0, 0)
    inten = g.param("Intensity", "float", "6", 0, 120)
    mul = g.node("Math.Multiply", 300, 40, wildcard="vec3")
    g.link(colour, "Value", mul, "A")
    g.link(inten, "Value", mul, "B")
    g.link(mul, "Out", out, "Emissive")
    base = g.param("BaseColor", "vec3", "0.9 0.75 0.5", 300, -150)
    g.link(base, "Value", out, "BaseColor")
    return g.write("Steampunk LampGlow: a gas mantle behind glass. Emissive is Color x Intensity, far past 1 "
                   "so Bloom spreads it.")


def clock_face():
    g = Graph("ClockFace")
    out = g.output(700, 0)
    tex = g.texture(f"{TEX}/ClockFace_BaseColor.nmtexture", -200, 0)
    g.link(tex, "RGB", out, "BaseColor")
    glow = g.param("Backlight", "float", "0.35", 0, 200)
    mul = g.node("Math.Multiply", 300, 160, wildcard="vec3")
    g.link(tex, "RGB", mul, "A")
    g.link(glow, "Value", mul, "B")
    g.link(mul, "Out", out, "Emissive")
    rough = g.param("Roughness", "float", "0.35", 300, -80)
    g.link(rough, "Value", out, "Roughness")
    return g.write("Steampunk ClockFace: the tower's enamel dial (UV 0-1), faintly backlit (Backlight).")


def gauge():
    g = Graph("Gauge")
    out = g.output(700, 0)
    tex = g.texture(f"{TEX}/Gauge_BaseColor.nmtexture", -200, 0)
    g.link(tex, "RGB", out, "BaseColor")
    rough = g.param("Roughness", "float", "0.15", 300, -80)
    g.link(rough, "Value", out, "Roughness")
    return g.write("Steampunk Gauge: a pressure gauge's dial behind glass (UV 0-1).")


def steam():
    g = Graph("Steam")
    out = g.output(700, 0, domain="Translucent", shading="Lit")
    coord = g.node("Input.TexCoord", -600, 120)
    dist = g.node("Vector.Distance", -440, 120, consts={"B": "0.5 0.5"})
    g.link(coord, "UV", dist, "A")
    dbl = g.node("Math.Multiply", -280, 120, consts={"B": "2"}, wildcard="float")
    g.link(dist, "Out", dbl, "A")
    inv = g.node("Math.OneMinus", -130, 120, wildcard="float")
    g.link(dbl, "Out", inv, "X")
    sat = g.node("Math.Saturate", 20, 120, wildcard="float")
    g.link(inv, "Out", sat, "X")
    sq = g.node("Math.Multiply", 170, 120, wildcard="float")
    g.link(sat, "Out", sq, "A")
    g.link(sat, "Out", sq, "B")
    vcol = g.node("Input.VertexColor", -80, -60)
    op = g.node("Math.Multiply", 360, 60, wildcard="float")
    g.link(sq, "Out", op, "A")
    g.link(vcol, "A", op, "B")
    g.link(op, "Out", out, "Opacity")
    g.link(vcol, "RGB", out, "BaseColor")
    rough = g.param("Roughness", "float", "1", 360, -120)
    g.link(rough, "Value", out, "Roughness")
    return g.write("Steampunk Steam: a soft round puff for the steam vents' particles, lit by the sun and the "
                   "lamps; colour and opacity from the particle's colour.")


def fabric():
    return surface("Fabric", "Paint", 4.0, roughness_scale=1.6, normal_strength=0.25,
                   comment="Steampunk Fabric: an airship's doped canvas envelope -- the Paint set stretched "
                           "4 m a tile, rough, faint seams; Tint colours it.")


def main():
    paths = [
        surface("Cobble", "Cobble", 1.92),
        surface("Flagstone", "Flagstone", 3.0),
        surface("Curb", "Curb", 2.0),
        surface("Brick", "Brick", 1.8),
        surface("Stone", "Stone", 2.0),
        surface("Brass", "Brass", 0.5),
        surface("Copper", "Copper", 1.0),
        surface("Iron", "Iron", 1.0),
        surface("Plate", "Plate", 2.0),
        surface("Wood", "Wood", 1.0),
        surface("Paint", "Paint", 1.0),
        surface("Slate", "Slate", 1.5),
        surface("Glass", "Glass", 1.0, normal_strength=0.3),
        window_lit(), lamp_glow(), clock_face(), gauge(), steam(), fabric(),
    ]
    for path in paths:
        print(f"  {path.relative_to(GAME_DIR)}")
    if not GENERATOR.exists():
        sys.exit(f"{GENERATOR} not found: build the editor-debug preset first, then run this again")
    result = subprocess.run([str(GENERATOR)] + [str(p) for p in paths], capture_output=True, text=True)
    print(result.stdout + result.stderr)
    if result.returncode != 0:
        sys.exit(f"NodeGraphGenerate failed ({result.returncode})")


if __name__ == "__main__":
    main()
