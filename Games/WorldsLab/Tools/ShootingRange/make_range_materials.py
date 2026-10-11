#!/usr/bin/env python3
"""
make_range_materials.py -- writes Content/Materials/ShootingRange/: the
material graphs of the Shooting Range warehouse, as the editor's material
graph editor saves them, then has the engine's NodeGraphGenerate compile each
one's <gen_code> (what the editor's Save writes).

The surfaces share Steampunk's graph shape (make_steampunk_materials.py):
Texture Coordinate x UVScale (the meshes' UV0 is in metres), BaseColor x the
vertex colour (baked grime and occlusion) x Tint, ORM to AO / Roughness x
RoughnessScale / Metallic, Normal through a Normal Map node. Then:

    Grating      the surface shape, Masked: the BaseColor's alpha is the
                 OpacityMask (bar grating you see the range through), two-sided
    Glass        skylight glass: Translucent Lit, dirty, Opacity a parameter
    Fluorescent  a tube's diffuser: Emissive = Color x Intensity (bloom)
    TargetPaper  the silhouette card, UV 0-1, faintly self-lit so it reads
    MuzzleFlash  a particle's flash: Translucent Unlit, Emissive = the
                 particle's colour x Intensity, opacity a soft round falloff

Usage (after building the editor once, so NodeGraphGenerate exists):

    python Games/WorldsLab/Tools/ShootingRange/make_range_materials.py
"""

import hashlib
import subprocess
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
OUT = GAME_DIR / "Content" / "Materials" / "ShootingRange"
TEX = "Content/Textures/ShootingRange"
GENERATOR = GAME_DIR / "Binaries" / "Linux_Debug" / "NodeGraphGenerate"


class Graph:
    def __init__(self, name):
        self.name = name
        self.nodes, self.links = [], []
        self.count = 0

    def node(self, type_name, x, y, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        node_id = hashlib.md5(f"ShootingRange/{self.name}/{self.count}".encode()).hexdigest()
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


def surface(name, texture_set, metres, roughness_scale=1.0, normal_strength=1.0, comment="", masked=False):
    g = Graph(name)
    out = g.output(900, 0, domain="Masked" if masked else "Opaque", two_sided=masked)
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
    if masked:
        g.link(base, "A", out, "OpacityMask")
    rscale = g.param("RoughnessScale", "float", f"{roughness_scale:g}", 220, 60)
    rough = g.node("Math.Multiply", 420, 20, wildcard="float")
    g.link(orm, "G", rough, "A")
    g.link(rscale, "Value", rough, "B")
    g.link(rough, "Out", out, "Roughness")
    g.link(orm, "R", out, "AmbientOcclusion")
    g.link(orm, "B", out, "Metallic")
    nmap = g.node("Texture.NormalMap", 420, 260, {"Format": "OpenGL"}, consts={"Strength": f"{normal_strength:g}"})
    g.link(normal, "RGB", nmap, "Color")
    g.link(uv, "Out", nmap, "UV")
    g.link(nmap, "Normal", out, "Normal")
    return g.write(comment or f"Shooting Range {name}: the {texture_set} set, {metres} m a tile on the meshes' "
                              "metre UVs; base colour times the vertex colour (baked grime) and Tint.")


def glass():
    g = Graph("Glass")
    out = g.output(900, 0, domain="Translucent", shading="Lit", two_sided=True)
    coord = g.node("Input.TexCoord", -700, 0)
    scale = g.param("UVScale", "vec2", "1 1", -700, 120)
    uv = g.node("Math.Multiply", -480, 40, wildcard="vec2")
    g.link(coord, "UV", uv, "A")
    g.link(scale, "Value", uv, "B")
    base = g.texture(f"{TEX}/Glass_BaseColor.nmtexture", -300, -200, (uv, "Out"))
    orm = g.texture(f"{TEX}/Glass_ORM.nmtexture", -300, 60, (uv, "Out"))
    g.link(base, "RGB", out, "BaseColor")
    g.link(orm, "G", out, "Roughness")
    opacity = g.param("Opacity", "float", "0.28", 300, 200)
    g.link(opacity, "Value", out, "Opacity")
    return g.write("Shooting Range Glass: wired skylight glass, frosted and filmed with years of dirt; "
                   "Translucent so the sun comes through. Opacity sets how much it veils.")


def fluorescent():
    g = Graph("Fluorescent")
    out = g.output(600, 0)
    colour = g.param("Color", "vec3", "0.92 0.97 1", 0, 0)
    inten = g.param("Intensity", "float", "9", 0, 120)
    mul = g.node("Math.Multiply", 300, 40, wildcard="vec3")
    g.link(colour, "Value", mul, "A")
    g.link(inten, "Value", mul, "B")
    g.link(mul, "Out", out, "Emissive")
    base = g.param("BaseColor", "vec3", "0.9 0.92 0.92", 300, -150)
    g.link(base, "Value", out, "BaseColor")
    return g.write("Shooting Range Fluorescent: a tube fixture's diffuser, cool white. Emissive is Color x "
                   "Intensity, past 1 so Bloom spreads it.")


def target_paper():
    g = Graph("TargetPaper")
    out = g.output(700, 0, two_sided=True)
    tex = g.texture(f"{TEX}/TargetPaper_BaseColor.nmtexture", -200, 0, wrap="Clamp")
    g.link(tex, "RGB", out, "BaseColor")
    glow = g.param("Backlight", "float", "0.06", 0, 200)
    mul = g.node("Math.Multiply", 300, 160, wildcard="vec3")
    g.link(tex, "RGB", mul, "A")
    g.link(glow, "Value", mul, "B")
    g.link(mul, "Out", out, "Emissive")
    rough = g.param("Roughness", "float", "0.9", 300, -80)
    g.link(rough, "Value", out, "Roughness")
    return g.write("Shooting Range TargetPaper: a silhouette target on card (UV 0-1), matte, a touch of "
                   "Backlight so it reads down a dim lane.")


def muzzle_flash():
    g = Graph("MuzzleFlash")
    out = g.output(700, 0, domain="Translucent", shading="Unlit")
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
    inten = g.param("Intensity", "float", "12", 200, -200)
    em = g.node("Math.Multiply", 400, -120, wildcard="vec3")
    g.link(vcol, "RGB", em, "A")
    g.link(inten, "Value", em, "B")
    g.link(em, "Out", out, "Emissive")
    return g.write("Shooting Range MuzzleFlash: a soft round flash for the pistol's and the impacts' particles; "
                   "Emissive is the particle's colour x Intensity (blooms), opacity a round falloff x its alpha.")


def grit():
    """The range's grade, run by Range.pp.nmgraph after its effects: a little
    desaturated, cooled, contrast pulled, a vignette, and fine animated grain."""
    g = Graph("Grit")
    out = g.output(1500, 0, domain="PostProcess", shading="Unlit")
    scene = g.node("Input.SceneTexture", -900, 0)
    lum = g.node("Vector.Dot", -700, 160, consts={"B": "0.2126 0.7152 0.0722 0"}, wildcard="vec4")
    g.link(scene, "Color", lum, "A")
    grey = g.node("Math.Multiply", -500, 160, consts={"A": "1 1 1 1"}, wildcard="vec4")
    g.link(lum, "Out", grey, "B")
    sat = g.param("Saturation", "float", "0.78", -500, 300)
    desat = g.node("Math.Lerp", -300, 60, wildcard="vec4")
    g.link(grey, "Out", desat, "A")
    g.link(scene, "Color", desat, "B")
    g.link(sat, "Value", desat, "T")
    tint = g.param("Tint", "vec4", "0.96 1 1.04 1", -300, -120)
    toned = g.node("Math.Multiply", -100, 0, wildcard="vec4")
    g.link(desat, "Out", toned, "A")
    g.link(tint, "Value", toned, "B")
    # Contrast about mid grey: (c - 0.18) * k + 0.18, kept above 0.
    mid = g.node("Math.Subtract", 80, 0, consts={"B": "0.18 0.18 0.18 0"}, wildcard="vec4")
    g.link(toned, "Out", mid, "A")
    contrast = g.param("Contrast", "float", "1.12", 80, 160)
    k = g.node("Math.Multiply", 260, 0, wildcard="vec4")
    g.link(mid, "Out", k, "A")
    g.link(contrast, "Value", k, "B")
    back = g.node("Math.Add", 440, 0, consts={"B": "0.18 0.18 0.18 0"}, wildcard="vec4")
    g.link(k, "Out", back, "A")
    floor_ = g.node("Math.Max", 620, 0, consts={"B": "0 0 0 0"}, wildcard="vec4")
    g.link(back, "Out", floor_, "A")
    # Vignette: 1 - smoothstep(0.3, 0.85, distance(uv, centre)) x Strength.
    uv = g.node("Input.TexCoord", 260, 300)
    dist = g.node("Vector.Distance", 440, 300, consts={"B": "0.5 0.5"}, wildcard="vec2")
    g.link(uv, "UV", dist, "A")
    edge = g.node("Math.Smoothstep", 620, 300, consts={"Edge0": "0.3", "Edge1": "0.85"}, wildcard="float")
    g.link(dist, "Out", edge, "X")
    strength = g.param("Vignette", "float", "0.5", 620, 440)
    dark = g.node("Math.Multiply", 800, 300, wildcard="float")
    g.link(edge, "Out", dark, "A")
    g.link(strength, "Value", dark, "B")
    keep = g.node("Math.OneMinus", 960, 300, wildcard="float")
    g.link(dark, "Out", keep, "X")
    vignetted = g.node("Math.Multiply", 1120, 0, wildcard="vec4")
    g.link(floor_, "Out", vignetted, "A")
    g.link(keep, "Out", vignetted, "B")
    # Grain: a per-pixel hash that changes every frame, (n - 0.5) x Grain.
    frag = g.node("Input.FragCoord", 260, 620)
    time = g.node("Input.Time", 260, 760)
    tscale = g.node("Math.Multiply", 440, 760, consts={"B": "61.7"}, wildcard="float")
    g.link(time, "Time", tscale, "A")
    seed = g.node("Math.Add", 620, 640, wildcard="vec4")
    g.link(frag, "Coord", seed, "A")
    g.link(tscale, "Out", seed, "B")
    dot = g.node("Vector.Dot", 800, 640, consts={"B": "12.9898 78.233 0 0"}, wildcard="vec4")
    g.link(seed, "Out", dot, "A")
    sine = g.node("Math.Sin", 960, 640, wildcard="float")
    g.link(dot, "Out", sine, "X")
    big = g.node("Math.Multiply", 1120, 640, consts={"B": "43758.5453"}, wildcard="float")
    g.link(sine, "Out", big, "A")
    frac = g.node("Math.Fract", 1280, 640, wildcard="float")
    g.link(big, "Out", frac, "X")
    centred = g.node("Math.Subtract", 1280, 780, consts={"B": "0.5"}, wildcard="float")
    g.link(frac, "Out", centred, "A")
    amount = g.param("Grain", "float", "0.035", 1120, 900)
    grain = g.node("Math.Multiply", 1400, 780, wildcard="float")
    g.link(centred, "Out", grain, "A")
    g.link(amount, "Value", grain, "B")
    final = g.node("Math.Add", 1300, 0, wildcard="vec4")
    g.link(vignetted, "Out", final, "A")
    g.link(grain, "Out", final, "B")
    g.link(final, "Out", out, "Emissive")
    return g.write("Shooting Range Grit: the warehouse's grade -- desaturated a touch, cooled, contrast pulled "
                   "about mid grey, a vignette, and fine film grain that changes every frame.")


def vertex_paint(name, roughness, metallic, comment, specular=0.5):
    """A mesh's own colours (baked into its vertex colours on import), with a
    roughness and metalness: the crash dummy's paint, the pistol's finish."""
    g = Graph(name)
    out = g.output(600, 0)
    vcol = g.node("Input.VertexColor", 0, -100)
    tint = g.param("Tint", "vec3", "1 1 1", 0, 40)
    mul = g.node("Math.Multiply", 300, -60, wildcard="vec3")
    g.link(vcol, "RGB", mul, "A")
    g.link(tint, "Value", mul, "B")
    g.link(mul, "Out", out, "BaseColor")
    rough = g.param("Roughness", "float", f"{roughness:g}", 300, 120)
    g.link(rough, "Value", out, "Roughness")
    metal = g.param("Metallic", "float", f"{metallic:g}", 300, 220)
    g.link(metal, "Value", out, "Metallic")
    spec = g.param("Specular", "float", f"{specular:g}", 300, 320)
    g.link(spec, "Value", out, "Specular")
    return g.write(comment)


def main():
    paths = [
        surface("Concrete", "Concrete", 3.0),
        surface("Block", "Block", 1.6),
        surface("Corrugated", "Corrugated", 2.0),
        surface("Steel", "Steel", 1.0),
        surface("Grating", "Grating", 0.5, masked=True,
                comment="Shooting Range Grating: galvanised bar grating, 0.5 m a tile; Masked on the texture's "
                        "alpha so the range shows through the catwalk floor."),
        surface("DiamondPlate", "DiamondPlate", 0.6),
        surface("Plywood", "Plywood", 1.2),
        surface("Rubber", "Rubber", 1.0),
        surface("FloorPaint", "FloorPaint", 1.0),
        surface("SteelTarget", "SteelTarget", 0.5),
        glass(), fluorescent(), target_paper(), muzzle_flash(), grit(),
        vertex_paint("DummyPaint", 0.85, 0.0, "Shooting Range DummyPaint: the crash dummy's colours (its vertex "
                     "colours) as scuffed satin paint, not DefaultLit's polish.", specular=0.2),
        vertex_paint("GunMetal", 0.38, 0.55, "Shooting Range GunMetal: the pistol's colours as worn black "
                     "finish over steel."),
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
