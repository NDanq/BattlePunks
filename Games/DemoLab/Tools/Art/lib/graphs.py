"""
graphs.py -- material graphs (.material.nmgraph) written from Python, as the editor saves
them but without their generated code (NodeGraphGenerate fills <gen_code>). The Graph
class is WorldsLab Courtyard's (make_courtyard_materials.py), its node ids seeded from
"DemoLab/<graph name>" so a run writes the same bytes.

    kit_material(defn)      a matdefs.py definition -> Graph
    terrain_material(...)   four height-blended layers for the island
"""

import hashlib

UNARY = {"OneMinus", "Abs", "Saturate", "Sqrt", "Negate", "Fract", "Floor", "Sin", "Cos"}


def vec(*values):
    return " ".join(repr(float(v)).rstrip("0").rstrip(".") if v != int(v) else str(int(v)) for v in values)


class Graph:
    def __init__(self, name, schema="Material"):
        self.name, self.schema = name, schema
        self.nodes, self.links, self.count = [], [], 0

    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"DemoLab/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard,
                           (self.count % 8) * 220 - 1400, (self.count // 8) * 150 - 600))
        return nid

    def link(self, src, out, dst, pin):
        self.links.append((f"{src}.{out}", f"{dst}.{pin}"))

    def const(self, value, wildcard):
        return self.node("Constant", consts={"Value": value}, wildcard=wildcard)

    def param(self, name, type_, default):
        return self.node("Parameter", props={"Default": default, "Name": name, "Type": type_})

    def math(self, op, a, b=None, wildcard="float", consts=None):
        """op on (node, pin) inputs; a constant goes in consts. Returns (node, 'Out')."""
        n = self.node(f"Math.{op}", consts=consts, wildcard=wildcard)
        if a is not None:
            self.link(a[0], a[1], n, "A" if b is not None or (consts and "B" in consts) else ("X" if op in UNARY else "A"))
        if b is not None:
            self.link(b[0], b[1], n, "B")
        return (n, "Out")

    def mul(self, a, b, w="float"):
        return self.math("Multiply", a, b, wildcard=w)

    def mulc(self, a, k, w="float"):
        return self.math("Multiply", a, consts={"B": k if isinstance(k, str) else vec(k)}, wildcard=w)

    def add(self, a, b, w="float"):
        return self.math("Add", a, b, wildcard=w)

    def addc(self, a, k, w="float"):
        return self.math("Add", a, consts={"B": vec(k)}, wildcard=w)

    def lerp(self, a, b, t, w="float", ca=None, cb=None):
        consts = {}
        if ca is not None:
            consts["A"] = ca
        if cb is not None:
            consts["B"] = cb
        n = self.node("Math.Lerp", consts=consts or None, wildcard=w)
        if a is not None:
            self.link(a[0], a[1], n, "A")
        if b is not None:
            self.link(b[0], b[1], n, "B")
        self.link(t[0], t[1], n, "T")
        return (n, "Out")

    def smooth(self, x, e0, e1):
        n = self.node("Math.Smoothstep", consts={"Edge0": vec(e0), "Edge1": vec(e1)}, wildcard="float")
        self.link(x[0], x[1], n, "X")
        return (n, "Out")

    def split(self, v):
        n = self.node("Vector.Split")
        self.link(v[0], v[1], n, "Vector")
        return n

    def splat3(self, f):
        m = self.node("Vector.Make3")
        for pin in "XYZ":
            self.link(f[0], f[1], m, pin)
        return (m, "Out")

    def uv(self, scale):
        tc = self.node("Input.TexCoord")
        if scale == 1.0:
            return (tc, "UV")
        return self.math("Multiply", (tc, "UV"), consts={"B": vec(scale, scale)}, wildcard="vec2")

    def texture(self, path, uv):
        obj = self.node("Texture.Object", props={"Filter": "Default", "Texture": path, "Wrap": "Default"})
        s = self.node("Texture.Sample", version=2)
        self.link(obj, "Texture", s, "Texture")
        if uv is not None:
            self.link(uv[0], uv[1], s, "UV")
        return s

    def normal_map(self, color, uv, strength=1.0):
        n = self.node("Texture.NormalMap", props={"Format": "OpenGL"},
                      consts={"Strength": vec(strength)} if strength != 1.0 else None)
        self.link(color[0], color[1], n, "Color")
        if uv is not None:
            self.link(uv[0], uv[1], n, "UV")
        return (n, "Normal")

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
        if self.links:
            lines.append("  <Links>")
            for src, dst in sorted(self.links):
                lines.append(f'    <Link From="{src}" To="{dst}" />')
            lines.append("  </Links>")
        lines.append("</NodeGraph>")
        text = "\n".join(lines) + "\n"
        # Keep the code a previous NodeGraphGenerate stored when the graph is unchanged.
        if path.exists():
            old = path.read_text()
            at = old.find("  <gen_code>")
            if at >= 0 and old[:at] + "</NodeGraph>\n" == text:
                return False
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, newline="\n")
        return True


def output(g, domain="Opaque", model="Default", two_sided=False, consts=None, extra=None):
    props = {"ClipValue": "0.5", "Domain": domain, "MaxOffset": "0", "Shading": "Lit", "ShadingModel": model,
             "TwoSided": "true" if two_sided else "false"}
    if domain == "Translucent":
        props["TranslucencyLighting"] = "Surface"
    props.update(extra or {})
    return g.node("Output.Material", props=props, consts=consts)


def kit_material(d, tex_dir):
    """A matdefs.py definition as a graph (see matdefs.py for the keys)."""
    g = Graph(d["name"])
    model = d.get("model", "Default")
    domain = d.get("domain", "Opaque")
    consts = {}
    if model == "ClearCoat":
        consts.update({"ClearCoat": vec(d.get("clearcoat", 1.0)), "ClearCoatRoughness": vec(d.get("coat_rough", 0.05))})
    if model == "Anisotropic":
        consts["Anisotropy"] = vec(d.get("anisotropy", 0.7))
    if model == "Cloth":
        consts["SheenColor"] = vec(*d.get("sheen", (0.5, 0.5, 0.5)))
        consts["SubsurfaceColor"] = vec(*d.get("sss", (0.5, 0.5, 0.5)))
    if model == "Subsurface":
        consts["Thickness"] = vec(d.get("thickness", 0.5))
    if model == "Volume":
        consts["PhaseAnisotropy"] = vec(d.get("phase", 0.3))
        consts["NormalInfluence"] = "0.3"
    if domain == "Translucent" and model != "Volume":
        consts["Refraction"] = vec(d.get("refraction", 1.0))
    out = output(g, domain, model, d.get("two_sided", False), consts)

    vc = g.node("Input.VertexColor")
    vcs = g.split((vc, "RGB"))
    occl = (vcs, "X")
    edge = (vc, "A")

    if "set" in d or "tex" in d:
        uv = g.uv(1.0 if d.get("unit") else 1.0 / d["metres"])
    if "set" in d:
        s = d["set"]
        base = g.texture(f"{tex_dir}/{s}_BaseColor.nmtexture", uv)
        orm = g.texture(f"{tex_dir}/{s}_ORM.nmtexture", uv)
        ntex = g.texture(f"{tex_dir}/{s}_Normal.nmtexture", uv)
        g.link(*g.normal_map((ntex, "RGB"), uv, d.get("normal", 1.0)), out, "Normal")
        if "base" in d and "set" in d and domain == "Translucent":
            colour = (g.param("Tint", "vec3", vec(*d["base"])), "Value")
            colour = g.mul(colour, (base, "RGB"), "vec3")
        elif "paint" in d:
            # Paint where BaseColor A says, worn through to steel on convex edges.
            tint = (g.param("Tint", "vec3", vec(*d["paint"])), "Value")
            wear = g.smooth(g.mul(edge, (orm, "R")), 0.45, 0.8)
            paint = g.mul((base, "A"), g.math("OneMinus", wear))
            painted = g.mul((base, "RGB"), tint, "vec3")
            colour = g.lerp(None, painted, paint, "vec3", ca=vec(0.40, 0.39, 0.37))
            metal = g.math("OneMinus", paint)
            g.link(*metal, out, "Metallic")
        else:
            tint = (g.param("Tint", "vec3", vec(*d.get("tint", (1, 1, 1)))), "Value")
            colour = g.mul((base, "RGB"), tint, "vec3")
        rough = (orm, "G")
        if "rough_mul" in d:
            rough = g.mulc(rough, d["rough_mul"])
        if "rough_add" in d:
            rough = g.addc(rough, d["rough_add"])
        g.link(*rough, out, "Roughness")
        if "paint" not in d:
            if "metal" in d:
                g.link(g.const(vec(d["metal"]), "float"), "Out", out, "Metallic")
            else:
                g.link(orm, "B", out, "Metallic")
        g.link(*g.mul((orm, "R"), occl), out, "AmbientOcclusion")
        if domain == "Masked":
            g.link(base, "A", out, "OpacityMask")
    elif "tex" in d:
        base = g.texture(f"{tex_dir}/{d['tex']}.nmtexture", uv)
        colour = (base, "RGB")
        g.link(g.const(vec(d.get("rough", 0.5)), "float"), "Out", out, "Roughness")
        g.link(g.const(vec(d.get("metal", 0.0)), "float"), "Out", out, "Metallic")
        g.link(*occl, out, "AmbientOcclusion")
    else:
        colour = (g.param("BaseColor", "vec3", vec(*d.get("base", (0.5, 0.5, 0.5)))), "Value")
        g.link(g.param("Roughness", "float", vec(d.get("rough", 0.5))), "Value", out, "Roughness")
        if model != "Volume":
            g.link(g.const(vec(d.get("metal", 0.0)), "float"), "Out", out, "Metallic")
            g.link(*occl, out, "AmbientOcclusion")

    # Baked occlusion darkens the albedo a little too (crevices collect grime).
    if model != "Volume":
        grime = g.lerp(None, None, occl, ca="0.6", cb="1")
        colour = g.mul(colour, g.splat3(grime), "vec3")
    g.link(*colour, out, "BaseColor")

    if model == "Subsurface":
        sss = g.mul(colour, (g.const(vec(*d.get("sss", (1, 1, 1))), "vec3"), "Out"), "vec3")
        g.link(*sss, out, "SubsurfaceColor")

    if "emissive" in d:
        (ec, ei) = d["emissive"]
        col = (g.param("EmissiveColor", "vec3", vec(*ec)), "Value")
        inten = (g.param("EmissiveIntensity", "float", vec(ei)), "Value")
        glow = g.mul(col, g.splat3(inten), "vec3")
        if "tex" in d:
            glow = g.mul(glow, colour, "vec3")
        if "pulse" in d:
            speed = (g.param("PulseSpeed", "float", vec(d["pulse"])), "Value")
            t = g.mul((g.node("Input.Time"), "Time"), speed)
            wave = g.addc(g.mulc(g.math("Sin", g.mulc(t, 6.2831853)), 0.25), 0.75)
            glow = g.mul(glow, g.splat3(wave), "vec3")
        g.link(*glow, out, "Emissive")

    if domain == "Translucent":
        if model == "Volume":
            # Thicker toward the silhouette's middle: Opacity x (N.V)^2.
            cam = g.node("Input.CameraPosition")
            wp = g.node("Input.WorldPosition")
            view = g.node("Vector.Normalize", wildcard="vec3")
            g.link(*g.math("Subtract", (cam, "Position"), (wp, "Position"), wildcard="vec3"), view, "V")
            ndv = g.node("Vector.Dot", wildcard="vec3")
            g.link(view, "Out", ndv, "A")
            g.link(g.node("Input.VertexNormal"), "Normal", ndv, "B")
            facing = g.math("Saturate", g.math("Abs", (ndv, "Out")))
            op = g.mulc(g.mul(facing, facing), d.get("opacity", 0.4))
            g.link(*op, out, "Opacity")
        elif "set" in d:
            # Smudges and dust (BaseColor A) are less clear.
            op = g.addc(g.mulc((base, "A"), 0.3), d.get("opacity", 0.3))
            g.link(*op, out, "Opacity")
        else:
            g.link(g.const(vec(d.get("opacity", 0.3)), "float"), "Out", out, "Opacity")
    return g


def terrain_material(name, layers, sharpness=8.0):
    """Four layers, blended by the terrain's paint weights (vertex colour R G B A) sharpened
    by each layer's height (its BaseColor A): w_i' = w_i (h_i + 0.25)^sharpness, normalised,
    so grass gives way to the rock's crests first. layers: [(texture dir, set name, metres a
    repeat, roughness, has normal map)]. Opaque materials sample at most 7 textures: four
    base colours and three normal maps (the flattest layer goes without).

    UV0 repeats once every UV Tile Size units (the Terrain component's): set it to 100
    (1 m) and each layer repeats every its metres."""
    g = Graph(name)
    out = output(g)
    vc = g.node("Input.VertexColor")
    w = g.split((vc, "Color"))
    samples = []
    for i, (tex_dir, layer, metres, rough, has_normal) in enumerate(layers):
        uv = g.uv(1.0 / metres)
        base = g.texture(f"{tex_dir}/{layer}_BaseColor.nmtexture", uv)
        nrm = g.texture(f"{tex_dir}/{layer}_Normal.nmtexture", uv) if has_normal else None
        hp = g.math("Power", g.addc((base, "A"), 0.25), consts={"B": vec(sharpness)})
        wi = g.mul((w, "XYZW"[i]), hp)
        samples.append((base, nrm, rough, wi))
    total = g.addc(g.add(g.add(samples[0][3], samples[1][3]), g.add(samples[2][3], samples[3][3])), 0.0001)
    ws = [g.math("Divide", s[3], total) for s in samples]
    colour = rough = normal = ao = None
    flat = (g.const(vec(0.5, 0.5, 1.0), "vec3"), "Out")
    for (base, nrm, r, _), wi in zip(samples, ws):
        c = g.mul((base, "RGB"), g.splat3(wi), "vec3")
        colour = c if colour is None else g.add(colour, c, "vec3")
        rr = g.mulc(wi, r)
        rough = rr if rough is None else g.add(rough, rr)
        nn = g.mul((nrm, "RGB") if nrm else flat, g.splat3(wi), "vec3")
        normal = nn if normal is None else g.add(normal, nn, "vec3")
        hh = g.mul(g.addc(g.mulc((base, "A"), 0.5), 0.5), wi)
        ao = hh if ao is None else g.add(ao, hh)
    g.link(*colour, out, "BaseColor")
    g.link(*rough, out, "Roughness")
    g.link(*ao, out, "AmbientOcclusion")
    g.link(*g.normal_map(normal, None), out, "Normal")
    return g
