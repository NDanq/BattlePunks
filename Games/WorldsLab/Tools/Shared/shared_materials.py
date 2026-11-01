#!/usr/bin/env python3
"""
shared_materials.py -- the material-graph writer WorldsLab's generators share,
and the shared materials themselves: Content/Materials/Shared/Wood and Stone
(the DestructionLab's crates and pillars, and what [Destruction_Wood] and
[Destruction_Stone] in Config/DefaultEngine.ini draw their cuts with), over
make_shared_textures.py's sets in Content/Textures/Shared. Written as the
editor saves them, without their generated code; then fill <gen_code> as the
editor does on save:

    python Games/WorldsLab/Tools/Shared/shared_materials.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/Shared/Wood.material.nmgraph \\
                            Games/WorldsLab/Content/Materials/Shared/Stone.material.nmgraph

Content/Materials/Shared/ParticleStreak.material.nmgraph (Translucent Unlit
streaks for sparks and embers) is hand-made, not written here.

The other labs' generators import this module for Graph (subclassed with
their own node-id seed), vec and output. The PBR graph samples a set's
BaseColor (times a Tint parameter), Normal (OpenGL) and Surface (roughness,
ambient occlusion, metallic) at the size one repeat covers in the world --
the meshes' UVs are in metres -- and multiplies in Macro_Noise sampled across
several metres, so the repeat does not show.

Node ids come from each graph's name, so running it again writes the same
files (and NodeGraphGenerate leaves an unchanged graph's code alone).
"""

import hashlib
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
MAT_DIR  = GAME_DIR / "Content" / "Materials" / "Shared"
TEX      = "Content/Textures/Shared"


def vec(*values):
    return " ".join(repr(float(v)).rstrip("0").rstrip(".") if v != int(v) else str(int(v)) for v in values)


class Graph:
    def __init__(self, name, schema="Material"):
        self.name, self.schema = name, schema
        self.nodes, self.links, self.count = [], [], 0

    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"Shared/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard, (self.count % 6) * 220 - 600, (self.count // 6) * 160 - 400))
        return nid

    def link(self, src, out, dst, pin):
        self.links.append((f"{src}.{out}", f"{dst}.{pin}"))

    # --- Shorthands ----------------------------------------------------------------------------
    def const(self, value, wildcard):
        return self.node("Constant", consts={"Value": value}, wildcard=wildcard)

    def param(self, name, type_, default):
        return self.node("Parameter", props={"Default": default, "Name": name, "Type": type_})

    def math(self, op, a, b=None, wildcard="float", consts=None):
        """op on (node, pin) inputs; a constant goes in consts."""
        n = self.node(f"Math.{op}", consts=consts, wildcard=wildcard)
        if a is not None:
            self.link(a[0], a[1], n, "A" if b is not None or (consts and "B" in consts) else ("X" if op in UNARY else "A"))
        if b is not None:
            self.link(b[0], b[1], n, "B")
        return n

    def uv(self, scale):
        """Texture Coordinate x scale (metres to repeats)."""
        tc = self.node("Input.TexCoord")
        m = self.node("Math.Multiply", consts={"B": vec(scale, scale)}, wildcard="vec2")
        self.link(tc, "UV", m, "A")
        return (m, "Out")

    def texture(self, path, uv):
        obj = self.node("Texture.Object", props={"Filter": "Default", "Texture": path, "Wrap": "Default"})
        s = self.node("Texture.Sample", version=2)
        self.link(obj, "Texture", s, "Texture")
        if uv is not None:
            self.link(uv[0], uv[1], s, "UV")
        return s

    def normal_map(self, path, uv, strength=1.0, color=None):
        n = self.node("Texture.NormalMap", props={"Format": "OpenGL"}, consts={"Strength": vec(strength)} if strength != 1.0 else None)
        if color is None:
            s = self.texture(path, uv)
            self.link(s, "RGB", n, "Color")
        else:
            self.link(color[0], color[1], n, "Color")
        if uv is not None:
            self.link(uv[0], uv[1], n, "UV")
        return n

    def splat3(self, f):
        """A float as a vec3 (r, r, r)."""
        m = self.node("Vector.Make3")
        for pin in "XYZ":
            self.link(f[0], f[1], m, pin)
        return (m, "Out")

    def write(self, path):
        lines = ['<?xml version="1.0" encoding="UTF-8"?>', f'<NodeGraph Version="1" Schema="{self.schema}">', "  <Nodes>"]
        for nid, type_, version, props, consts, wildcard, x, y in sorted(self.nodes):
            wc = f' Wildcard="{wildcard}"' if wildcard else ""
            head = f'    <Node Id="{nid}" Type="{type_}" Version="{version}" X="{x}" Y="{y}"{wc}'
            if not props and not consts:
                lines.append(head + " />")
                continue
            lines.append(head + ">")
            for pin, value in sorted(consts.items()):
                lines.append(f'      <Const Pin="{pin}" Value="{value}" />')
            for key, value in sorted(props.items()):
                lines.append(f'      <Prop Name="{key}" Value="{value}" />')
            lines.append("    </Node>")
        lines.append("  </Nodes>")
        if self.links:    # as the editor saves: no empty element
            lines.append("  <Links>")
            for src, dst in sorted(self.links):
                lines.append(f'    <Link From="{src}" To="{dst}" />')
            lines.append("  </Links>")
        lines.append("</NodeGraph>")
        text = "\n".join(lines) + "\n"
        # Keep the code a previous NodeGraphGenerate stored, so an unchanged graph stays unchanged.
        if path.exists():
            old = path.read_text()
            at = old.find("  <gen_code>")
            if at >= 0 and old[:at] + "</NodeGraph>\n" == text:
                return
        path.write_text(text, newline="\n")


UNARY = {"OneMinus", "Abs", "Saturate", "Sqrt", "Negate", "Fract", "Floor", "Sin", "Cos"}


def output(g, domain="Opaque", shading="Lit", two_sided=False, consts=None, clip=0.5, max_offset=0.0):
    return g.node("Output.Material", props={"ClipValue": vec(clip), "Domain": domain, "MaxOffset": vec(max_offset), "Shading": shading,
                                            "TwoSided": "true" if two_sided else "false"}, consts=consts)


def pbr(name, tex_set, size, tint=(1, 1, 1), normal=1.0, macro=None, grime=False, rough_scale=None, rough_const=None,
        metal=True, tex_name=None):
    """The textured PBR graph. macro: (amount, metres) of Macro_Noise
    brightness variation; grime: darkened toward the ground and streaked."""
    g = Graph(name)
    out = output(g)
    uv = g.uv(1.0 / size)
    base = g.texture(f"{TEX}/{tex_set}_BaseColor.nmtexture", uv)
    surface = g.texture(f"{TEX}/{tex_set}_Surface.nmtexture", uv)
    nm = g.normal_map(f"{TEX}/{tex_set}_Normal.nmtexture", uv, normal)
    g.link(nm, "Normal", out, "Normal")

    tint_p = g.param("Tint", "vec3", vec(*tint))
    colour = (g.math("Multiply", (base, "RGB"), (tint_p, "Value"), wildcard="vec3"), "Out")
    if macro:
        amount, metres = macro
        muv = g.uv(1.0 / metres)
        noise = g.texture(f"{TEX}/Macro_Noise.nmtexture", muv)
        lerp = g.node("Math.Lerp", consts={"A": vec(1 - amount), "B": vec(1 + amount)}, wildcard="float")
        g.link(noise, "R", lerp, "T")
        colour = (g.math("Multiply", colour, g.splat3((lerp, "Out")), wildcard="vec3"), "Out")
        if grime:
            # Splash-back toward the ground and rain streaks down from above.
            wp = g.node("Input.WorldPosition")
            split = g.node("Vector.Split")
            g.link(wp, "Position", split, "Vector")
            step = g.node("Math.Smoothstep", consts={"Edge0": "4", "Edge1": "95"}, wildcard="float")
            g.link(split, "Y", step, "X")
            low = g.node("Math.Lerp", consts={"A": "0.66", "B": "1"}, wildcard="float")
            g.link(step, "Out", low, "T")
            streak = g.node("Math.Lerp", consts={"A": "0.86", "B": "1.03"}, wildcard="float")
            g.link(noise, "B", streak, "T")
            both = g.math("Multiply", (low, "Out"), (streak, "Out"))
            colour = (g.math("Multiply", colour, g.splat3((both, "Out")), wildcard="vec3"), "Out")
    g.link(colour[0], colour[1], out, "BaseColor")

    if rough_const is not None:
        r = g.const(vec(rough_const), "float")
        g.link(r, "Out", out, "Roughness")
    elif rough_scale is not None:
        r = g.math("Multiply", (surface, "R"), consts={"B": vec(rough_scale)})
        g.link(r, "Out", out, "Roughness")
    else:
        g.link(surface, "R", out, "Roughness")
    g.link(surface, "G", out, "AmbientOcclusion")
    if metal:
        g.link(surface, "B", out, "Metallic")
    return g


def materials():
    return [
        pbr("Wood", "Wood", 2.0, macro=(0.1, 6.0)),
        pbr("Stone", "Stone", 1.5, macro=(0.1, 7.0)),
    ]


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    for g in materials():
        g.write(MAT_DIR / f"{g.name}.material.nmgraph")
    print(f"{MAT_DIR}: {', '.join(g.name for g in materials())}")


if __name__ == "__main__":
    main()
