#!/usr/bin/env python3
"""
make_courtyard_materials.py -- writes the Courtyard level's material graphs,
Content/Materials/Courtyard/*.material.nmgraph, and its post process stack,
Content/PostProcess/Courtyard/Courtyard.pp.nmgraph, as the editor saves them
-- without their generated code. Then fill <gen_code> as the editor does on
save (or open and save each in the editor):

    python Games/WorldsLab/Tools/Courtyard/make_courtyard_materials.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/Courtyard/*.material.nmgraph \\
                            Games/WorldsLab/Content/PostProcess/Courtyard/*.pp.nmgraph

Most are the same PBR graph over one of make_courtyard_textures.py's sets:
the set's BaseColor (times a Tint parameter), Normal (OpenGL) and Surface
(roughness, ambient occlusion, metallic), at the size one repeat covers in
the world -- the meshes' UVs are in metres. Large surfaces also multiply
in Macro_Noise sampled across several metres, so the repeat does not show,
and the outside stucco darkens toward the ground (splash-back) and streaks.

    Glass   Translucent Lit, two-sided: old wavy glass. A dark tinted metal
            (so its reflection survives the alpha blend: what it shows is
            Opacity x the reflection), its Opacity rising from 0.13 face-on
            to nearly 1 at grazing angles (Schlick's Fresnel, from the
            Camera Position node), dust raising roughness and diffuse.
    Water   the same idea, its normal two ripple maps drifting over Time.
    Foliage Masked, two-sided leaf cards: the texture's alpha clips; a
            gentle wind sways them (World Position Offset).
    Flame, Embers  emissive past 1, which Bloom spreads.
    HearthFlame the fireplace's flame sprites: a tongue drawn in the quad,
            flickering on scrolling noise, Translucent Unlit.
    WaterSpray the fountain's falling sheets and splashes: glossy streaks,
            Translucent Lit.

Node ids come from each graph's name, so running it again writes the same
files (and NodeGraphGenerate leaves an unchanged graph's code alone).
"""

import hashlib
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
MAT_DIR  = GAME_DIR / "Content" / "Materials" / "Courtyard"
PP_DIR   = GAME_DIR / "Content" / "PostProcess" / "Courtyard"
TEX      = "Content/Textures/Courtyard"


def vec(*values):
    return " ".join(repr(float(v)).rstrip("0").rstrip(".") if v != int(v) else str(int(v)) for v in values)


class Graph:
    def __init__(self, name, schema="Material"):
        self.name, self.schema = name, schema
        self.nodes, self.links, self.count = [], [], 0

    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"Courtyard/{self.name}/{self.count}".encode()).hexdigest()
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


def fresnel(g, power=5.0):
    """Schlick's (1 - |V.N|)^power, V toward the camera, N the vertex normal (either side)."""
    cam = g.node("Input.CameraPosition")
    wp = g.node("Input.WorldPosition")
    d = g.node("Math.Subtract", wildcard="vec3")
    g.link(cam, "Position", d, "A")
    g.link(wp, "Position", d, "B")
    v = g.node("Vector.Normalize", wildcard="vec3")
    g.link(d, "Out", v, "V")
    n = g.node("Input.VertexNormal")
    dt = g.node("Vector.Dot", wildcard="vec3")
    g.link(v, "Out", dt, "A")
    g.link(n, "Normal", dt, "B")
    ab = g.node("Math.Abs", wildcard="float")
    g.link(dt, "Out", ab, "X")
    om = g.node("Math.OneMinus", wildcard="float")
    g.link(ab, "Out", om, "X")
    pw = g.node("Math.Power", consts={"B": vec(power)}, wildcard="float")
    g.link(om, "Out", pw, "A")
    return (pw, "Out")


def glass():
    g = Graph("Glass")
    out = output(g, domain="Translucent", two_sided=True)
    uv = g.uv(1.0 / 0.6)
    surface = g.texture(f"{TEX}/Glass_Surface.nmtexture", uv)
    nm = g.normal_map(f"{TEX}/Glass_Normal.nmtexture", uv, 0.7)
    g.link(nm, "Normal", out, "Normal")
    g.link(surface, "R", out, "Roughness")
    grime = g.math("Multiply", (surface, "G"), consts={"B": "0.7"})
    tint = g.param("Tint", "vec3", vec(0.30, 0.35, 0.32))
    base = g.node("Math.Lerp", consts={"B": vec(0.52, 0.49, 0.44)}, wildcard="vec3")
    g.link(tint, "Value", base, "A")
    g.link(g.splat3((grime, "Out"))[0], "Out", base, "T")
    g.link(base, "Out", out, "BaseColor")
    metal = g.node("Math.OneMinus", wildcard="float")
    g.link(grime, "Out", metal, "X")
    g.link(metal, "Out", out, "Metallic")
    f = fresnel(g)
    lift = g.node("Math.Lerp", consts={"A": "0.13", "B": "0.96"}, wildcard="float")
    g.link(f[0], f[1], lift, "T")
    dusty = g.math("Multiply", (grime, "Out"), consts={"B": "0.3"})
    op = g.math("Add", (lift, "Out"), (dusty, "Out"))
    sat = g.node("Math.Saturate", wildcard="float")
    g.link(op, "Out", sat, "X")
    g.link(sat, "Out", out, "Opacity")
    return g


def water():
    """Clear fountain water (Engine/Modules/RenderCore/LIT_TRANSLUCENCY.md, Phases 3
    and 9-12): a dielectric (2 % at normal incidence, Fresnel from the BRDF) whose
    reflections keep their strength whatever its opacity (Translucency Lighting
    Surface), a light body, and the basin seen through it bent by the waves
    (Refraction 1.33) and tinted (Transmission Tint)."""
    g = Graph("Water")
    out = output(g, domain="Translucent")
    next(n for n in g.nodes if n[0] == out)[3]["TranslucencyLighting"] = "Surface"
    time = g.node("Input.Time")
    t2 = g.node("Vector.Make2")
    g.link(time, "Time", t2, "X")
    g.link(time, "Time", t2, "Y")
    samples = []
    for scale, drift in ((1.3, (0.035, 0.021)), (0.55, (-0.018, 0.03))):
        tc = g.node("Input.TexCoord")
        m = g.node("Math.Multiply", consts={"B": vec(scale, scale)}, wildcard="vec2")
        g.link(tc, "UV", m, "A")
        dt = g.node("Math.Multiply", consts={"B": vec(*drift)}, wildcard="vec2")
        g.link(t2, "Out", dt, "A")
        uv = g.node("Math.Add", wildcard="vec2")
        g.link(m, "Out", uv, "A")
        g.link(dt, "Out", uv, "B")
        samples.append((g.texture(f"{TEX}/Water_Normal.nmtexture", (uv, "Out")), uv))
    mix = g.node("Math.Lerp", consts={"T": vec(0.5, 0.5, 0.5)}, wildcard="vec3")
    g.link(samples[0][0], "RGB", mix, "A")
    g.link(samples[1][0], "RGB", mix, "B")
    nm = g.normal_map(None, (samples[0][1], "Out"), 0.45, color=(mix, "Out"))
    g.link(nm, "Normal", out, "Normal")
    g.link(g.param("Tint", "vec3", vec(0.20, 0.25, 0.23)), "Value", out, "BaseColor")
    g.link(g.const("0.25", "float"), "Out", out, "Specular")
    g.link(g.const("0.03", "float"), "Out", out, "Roughness")
    g.link(g.const("0.15", "float"), "Out", out, "Opacity")
    g.link(g.const("1.33", "float"), "Out", out, "Refraction")
    g.link(g.param("TransmissionTint", "vec3", vec(0.78, 0.9, 0.86)), "Value", out, "TransmissionTint")
    return g


def wind(g, out):
    """A gentle breeze as World Position Offset, in world units: the crown
    sways along the wind (two slow sines, each plant its own phase from
    where it stands) and each leaf card flutters a little faster on top (its
    phase from its own position), less near the pot than up in the crown."""
    split = g.node("Vector.Split")
    g.link(g.node("Input.WorldPosition"), "Position", split, "Vector")
    time = (g.node("Input.Time"), "Time")
    x, y, z = (split, "X"), (split, "Y"), (split, "Z")
    scaled = lambda a, k: (g.math("Multiply", a, consts={"B": vec(k)}), "Out")
    added = lambda a, b: (g.math("Add", a, b), "Out")
    sine = lambda a, op="Sin": (g.math(op, a), "Out")
    # The sway: per plant, so the crowns do not move in step.
    phase = added(scaled(x, 0.004), scaled(z, 0.005))
    sway = added(scaled(sine(added(scaled(time, 1.05), phase)), 2.0),
                 scaled(sine(added(scaled(time, 1.73), scaled(phase, 1.6))), 0.7))
    # The flutter: per card.
    leaf = added(added(scaled(x, 0.09), scaled(z, 0.07)), scaled(y, 0.05))
    flutter_a = scaled(sine(added(scaled(time, 6.8), leaf)), 0.45)
    flutter_b = scaled(sine(added(scaled(time, 8.3), scaled(leaf, 1.3)), "Cos"), 0.35)
    # Stiffer low down: 0.35 at the pots' rims, all of it 1.5 m up.
    rise = g.node("Math.Saturate", wildcard="float")
    g.link(*scaled((g.math("Subtract", y, consts={"B": "60"}), "Out"), 1.0 / 150.0), rise, "X")
    weight = g.node("Math.Lerp", consts={"A": "0.35", "B": "1"}, wildcard="float")
    g.link(rise, "Out", weight, "T")
    w = (weight, "Out")
    offset = g.node("Vector.Make3")
    # The wind blows toward +X +Z (0.8, 0, 0.6).
    g.link(*(g.math("Multiply", added(scaled(sway, 0.8), flutter_a), w), "Out"), offset, "X")
    g.link(*(g.math("Multiply", scaled(flutter_b, 0.6), w), "Out"), offset, "Y")
    g.link(*(g.math("Multiply", added(scaled(sway, 0.6), flutter_b), w), "Out"), offset, "Z")
    g.link(offset, "Out", out, "WorldPositionOffset")


def foliage():
    g = Graph("Foliage")
    out = output(g, domain="Masked", two_sided=True, clip=0.5, max_offset=5.0)
    wind(g, out)
    base = g.texture(f"{TEX}/Foliage_BaseColor.nmtexture", None)
    surface = g.texture(f"{TEX}/Foliage_Surface.nmtexture", None)
    nm = g.normal_map(f"{TEX}/Foliage_Normal.nmtexture", None)
    tint = g.param("Tint", "vec3", "1 1 1")
    g.link(g.math("Multiply", (base, "RGB"), (tint, "Value"), wildcard="vec3"), "Out", out, "BaseColor")
    g.link(base, "A", out, "OpacityMask")
    g.link(surface, "R", out, "Roughness")
    g.link(surface, "G", out, "AmbientOcclusion")
    g.link(nm, "Normal", out, "Normal")
    return g


def emissive(name, colour, base=(0, 0, 0), roughness=0.6, flicker=None):
    g = Graph(name)
    out = output(g, consts={"BaseColor": vec(*base), "Roughness": vec(roughness)})
    e = g.param("Emissive", "vec3", vec(*colour))
    if flicker:
        uv = g.uv(1.0 / flicker)
        noise = g.texture(f"{TEX}/Macro_Noise.nmtexture", uv)
        lerp = g.node("Math.Lerp", consts={"A": "0.25", "B": "1.6"}, wildcard="float")
        g.link(noise, "G", lerp, "T")
        g.link(g.math("Multiply", (e, "Value"), g.splat3((lerp, "Out")), wildcard="vec3"), "Out", out, "Emissive")
    else:
        g.link(e, "Value", out, "Emissive")
    return g


def hearth_flame():
    """The fireplace's flame sprites (Content/Particles/Courtyard/Hearth.nmparticle):
    Translucent Unlit. A tongue drawn in the quad -- wide at the foot, a point
    at the top -- its edge pushed sideways by Macro_Noise scrolling up over
    Time (each particle its own, from where it is in the world), a hot core
    inside; coloured and faded by the particle's colour (Vertex Color)."""
    g = Graph("HearthFlame")
    out = output(g, domain="Translucent", shading="Unlit")
    uvs = g.node("Vector.Split")
    g.link(g.node("Input.TexCoord"), "UV", uvs, "Vector")
    u, v = (uvs, "X"), (uvs, "Y")
    wps = g.node("Vector.Split")
    g.link(g.node("Input.WorldPosition"), "Position", wps, "Vector")
    time = (g.node("Input.Time"), "Time")
    scaled = lambda a, k: (g.math("Multiply", a, consts={"B": vec(k)}), "Out")
    added = lambda a, b: (g.math("Add", a, b), "Out")
    unary = lambda op, a: (g.math(op, a), "Out")
    # The noise, rising: per particle from its world place.
    seed = added(scaled((wps, "X"), 0.021), scaled((wps, "Z"), 0.017))
    nuv = g.node("Vector.Make2")
    g.link(*added(scaled(u, 0.35), seed), nuv, "X")
    g.link(*added(added(scaled(v, 0.45), scaled(time, 0.9)), scaled(seed, 0.6)), nuv, "Y")
    noise = g.texture(f"{TEX}/Macro_Noise.nmtexture", (nuv, "Out"))
    height = unary("OneMinus", v)                                  # 0 at the foot, 1 at the top
    wobble = g.math("Multiply", (g.math("Subtract", (noise, "G"), consts={"B": "0.5"}), "Out"),
                    scaled(height, 1.1))
    across = unary("Abs", added(scaled((g.math("Subtract", u, consts={"B": "0.5"}), "Out"), 2.0), (wobble, "Out")))
    width = (g.math("Add", scaled((g.math("Power", unary("OneMinus", height), consts={"B": "0.65"}), "Out"), 0.9),
                    consts={"B": "0.04"}), "Out")
    body = unary("Saturate", unary("OneMinus", (g.math("Divide", across, width), "Out")))
    foot = g.node("Math.Smoothstep", consts={"Edge0": "0", "Edge1": "0.18"}, wildcard="float")
    g.link(*height, foot, "X")
    shape = (g.math("Multiply", body, (foot, "Out")), "Out")
    core = (g.math("Power", shape, consts={"B": "3"}), "Out")
    vc = g.node("Input.VertexColor")
    glow = added(scaled(shape, 2.2), scaled(core, 5.0))
    g.link(g.math("Multiply", (vc, "RGB"), g.splat3(glow), wildcard="vec3"), "Out", out, "Emissive")
    g.link(*unary("Saturate", (g.math("Multiply", (g.math("Power", shape, consts={"B": "0.8"}), "Out"), (vc, "A")), "Out")), out, "Opacity")
    return g


def water_spray():
    """The fountain's falling water and splashes (Content/Particles/Courtyard/Fountain.nmparticle):
    Translucent Lit, two-sided streaks -- clear across their width (the
    quad's U), glossy, so the sun, the lanterns and the sky's reflection
    catch them: a greyed base, so reflection carries more of it than the
    sunset's diffuse -- faded by the particle's colour (Vertex Color)."""
    g = Graph("WaterSpray")
    out = output(g, domain="Translucent", two_sided=True,
                 consts={"BaseColor": vec(0.4, 0.44, 0.46), "Roughness": "0.05", "Specular": "1"})
    uvs = g.node("Vector.Split")
    g.link(g.node("Input.TexCoord"), "UV", uvs, "Vector")
    centred = g.math("Abs", (g.math("Subtract", (uvs, "X"), consts={"B": "0.5"}), "Out"))
    across = g.math("Saturate", (g.math("OneMinus", (g.math("Multiply", (centred, "Out"), consts={"B": "2"}), "Out")), "Out"))
    profile = g.math("Power", (across, "Out"), consts={"B": "1.6"})
    vc = g.node("Input.VertexColor")
    opacity = g.math("Multiply", (g.math("Multiply", (profile, "Out"), (vc, "A")), "Out"), consts={"B": "0.55"})
    g.link(opacity, "Out", out, "Opacity")
    g.link(g.math("Multiply", (vc, "RGB"), consts={"B": vec(0.03, 0.03, 0.03)}, wildcard="vec3"), "Out", out, "Emissive")
    return g


def wax():
    g = Graph("Wax")
    output(g, consts={"BaseColor": vec(0.92, 0.87, 0.74), "Roughness": "0.42", "Emissive": vec(0.05, 0.035, 0.015)})
    return g


def materials():
    return [
        pbr("Stucco", "Stucco", 2.0, normal=0.45, macro=(0.11, 9.0), grime=True),
        pbr("Plaster", "Stucco", 2.0, tint=(1.0, 0.975, 0.93), normal=0.35, macro=(0.06, 7.0)),
        pbr("RoofTile", "RoofTile", 1.0, macro=(0.2, 4.5)),
        pbr("Terracotta", "RoofTile", 0.6, tint=(1.0, 0.93, 0.86)),
        pbr("FloorTile", "FloorTile", 1.2, macro=(0.07, 6.0)),
        pbr("Paving", "Paving", 2.4, macro=(0.12, 11.0), normal=1.2),
        pbr("Ground", "Ground", 4.0, macro=(0.22, 23.0)),
        pbr("Soil", "Ground", 1.0, tint=(0.46, 0.37, 0.29), rough_const=0.95),
        pbr("Wood", "Wood", 2.0, macro=(0.1, 6.0)),
        pbr("GreenPaint", "GreenPaint", 1.0, macro=(0.05, 5.0)),
        pbr("Iron", "Iron", 1.0),
        pbr("Stone", "Stone", 1.5, macro=(0.1, 7.0)),
        pbr("StoneWet", "Stone", 1.5, tint=(0.6, 0.58, 0.54), rough_scale=0.35),
        pbr("Soot", "Stone", 1.5, tint=(0.07, 0.065, 0.06), rough_const=0.95),
        pbr("Talavera", "Talavera", 0.6),
        glass(), water(), foliage(), wax(), hearth_flame(), water_spray(),
        emissive("Flame", (9.0, 5.2, 1.9)),
        emissive("Embers", (1.5, 0.38, 0.07), base=(0.05, 0.04, 0.035), roughness=0.9, flicker=0.35),
    ]


def post_stack():
    g = Graph("CourtyardPost", schema="PostProcess")
    g.node("Effect.ToneMapping", props={"Exposure": "0", "Tone Mapper": "ACES"})
    # Eye adaptation: the dim hall and the bright sunset sky each exposed in turn, within a few stops.
    g.node("Effect.AutoExposure", props={"Key": "0.12", "Min EV": "-2", "Max EV": "2.5", "Speed Up": "2.5",
                                         "Speed Down": "1", "Centre Weight": "0.5"})
    g.node("Effect.Bloom", props={"Intensity": "0.6", "Radius": "0.65", "Threshold": "1"})
    # Dusk haze: the low sun glows through it and throws shafts; the lanterns and chandeliers glow in it.
    g.node("Effect.VolumetricFog", props={"Density": "0.0001", "Height Falloff": "0.0005", "Base Height": "0",
                                          "Max Distance": "9000", "Anisotropy": "0.65", "Sun Scattering": "1.5",
                                          "Sky Scattering": "0.6", "Light Scattering": "2", "Albedo": "1 0.96 0.92",
                                          "Steps": "24", "Shadows": "true"})
    # The waxed floors and the fountain's stone catch the room and the arcade, where the probes cannot place them.
    g.node("Effect.ScreenSpaceReflections", props={"Intensity": "1", "Max Roughness": "0.5", "Max Distance": "1500",
                                                   "Thickness": "25", "Steps": "32"})
    g.node("Effect.AmbientOcclusion", props={"Blur": "true", "Intensity": "3", "Power": "1.4", "Radius": "60", "Samples": "16"})
    g.node("Effect.Antialiasing", props={"Method": "TAA", "Quality": "Low"})   # TAA: FXAA stands in where it cannot run
    return g


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    PP_DIR.mkdir(parents=True, exist_ok=True)
    names = set()
    for g in materials():
        g.write(MAT_DIR / f"{g.name}.material.nmgraph")
        names.add(f"{g.name}.material.nmgraph")
    for stale in MAT_DIR.glob("*.material.nmgraph"):
        if stale.name not in names:
            stale.unlink()
    post_stack().write(PP_DIR / "Courtyard.pp.nmgraph")
    print(f"{MAT_DIR}: {len(names)} materials; {PP_DIR / 'Courtyard.pp.nmgraph'}")


if __name__ == "__main__":
    main()
