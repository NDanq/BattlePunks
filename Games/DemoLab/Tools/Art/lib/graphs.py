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

    @staticmethod
    def _k(k, w):
        if isinstance(k, str):
            return k
        if isinstance(k, (tuple, list)):
            return vec(*k)
        n = {"vec2": 2, "vec3": 3, "vec4": 4}.get(w, 1)
        return vec(*([k] * n))

    def mulc(self, a, k, w="float"):
        return self.math("Multiply", a, consts={"B": self._k(k, w)}, wildcard=w)

    def add(self, a, b, w="float"):
        return self.math("Add", a, b, wildcard=w)

    def addc(self, a, k, w="float"):
        return self.math("Add", a, consts={"B": self._k(k, w)}, wildcard=w)

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

    def splat2(self, f):
        m = self.node("Vector.Make2")
        for pin in "XY":
            self.link(f[0], f[1], m, pin)
        return (m, "Out")

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
        w = d.get("weather") if domain == "Opaque" else None
        base = g.texture(f"{tex_dir}/{s}_BaseColor.nmtexture", uv)
        orm = g.texture(f"{tex_dir}/{s}_ORM.nmtexture", uv)
        ntex = g.texture(f"{tex_dir}/{s}_Normal.nmtexture", uv)
        ncol = (ntex, "RGB")
        if w and w.get("detail", 0.0) > 0:
            # Detail normal (0.5 m a repeat on the metre UVs), added in tangent space.
            k = w["detail"]
            dn = g.texture(f"{tex_dir}/Detail_Normal.nmtexture", g.uv(1.0 / w.get("detail_metres", 0.5)))
            ncol = g.add(ncol, g.addc(g.mulc((dn, "RGB"), (k, k, 0), "vec3"), (-0.5 * k, -0.5 * k, 0), "vec3"), "vec3")
        g.link(*g.normal_map(ncol, uv, d.get("normal", 1.0)), out, "Normal")
        gm = None
        if w:
            gm = g.texture(f"{tex_dir}/Grime_Mask.nmtexture", g.uv(1.0 / w.get("grime_metres", 3.0)))
        metal = None
        if "base" in d and "set" in d and domain == "Translucent":
            colour = (g.param("Tint", "vec3", vec(*d["base"])), "Value")
            colour = g.mul(colour, (base, "RGB"), "vec3")
        elif "paint" in d:
            # Paint where BaseColor A says, chipped through to steel on convex edges (the
            # grime mask's chips break the wear up when the material weathers).
            tint = (g.param("Tint", "vec3", vec(*d["paint"])), "Value")
            wear_in = g.mul(edge, (orm, "R"))
            if gm is not None:
                wear_in = g.mul(wear_in, g.addc(g.mulc((gm, "B"), 1.2), 0.3))
            wear = g.smooth(wear_in, 0.45, 0.8)
            paint = g.mul((base, "A"), g.math("OneMinus", wear))
            painted = g.mul((base, "RGB"), tint, "vec3")
            colour = g.lerp(None, painted, paint, "vec3", ca=vec(0.40, 0.39, 0.37))
            metal = g.math("OneMinus", paint)
        else:
            tint = (g.param("Tint", "vec3", vec(*d.get("tint", (1, 1, 1)))), "Value")
            colour = g.mul((base, "RGB"), tint, "vec3")
        rough = (orm, "G")
        if "rough_mul" in d:
            rough = g.mulc(rough, d["rough_mul"])
        if "rough_add" in d:
            rough = g.addc(rough, d["rough_add"])
        if metal is None:
            metal = (g.const(vec(d["metal"]), "float"), "Out") if "metal" in d else (orm, "B")
        if w:
            colour, rough, metal = weathering(g, w, gm, colour, rough, metal, occl, edge, tex_dir)
        g.link(*rough, out, "Roughness")
        g.link(*metal, out, "Metallic")
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
            if "wisp" in d:
                # Wisps: the grime mask's clouds in world space (metres), two scales rising at
                # different speeds, eat the puffs away to curls; it thins out toward the top
                # (World Y over the mesh's pivot is not known here: the noise's own fade does it).
                metres, rise = d["wisp"]
                wsp = g.split((g.node("Input.WorldPosition"), "Position"))
                time = (g.node("Input.Time"), "Time")

                def cloud(k, speed, drift):
                    uvn = g.node("Vector.Make2")
                    g.link(*g.add(g.mulc((wsp, "X"), 0.01 / (metres * k)), g.mulc((wsp, "Z"), 0.007 / (metres * k))),
                           uvn, "X")
                    g.link(*g.add(g.mulc((wsp, "Y"), 0.01 / (metres * k)), g.mulc(time, -speed)), uvn, "Y")
                    uvd = g.add((uvn, "Out"), g.mulc(g.splat2(time), (drift, 0.0), "vec2"), "vec2")
                    return (g.texture(f"{tex_dir}/Grime_Mask.nmtexture", uvd), "R")
                n = g.mul(cloud(1.0, rise, 0.013), g.addc(g.mulc(cloud(0.37, rise * 1.9, -0.031), 1.2), 0.2))
                op = g.mul(op, g.smooth(n, 0.06, 0.5))
            g.link(*op, out, "Opacity")
        elif "set" in d:
            # Smudges and dust (BaseColor A) are less clear.
            op = g.addc(g.mulc((base, "A"), 0.3), d.get("opacity", 0.3))
            g.link(*op, out, "Opacity")
        else:
            g.link(g.const(vec(d.get("opacity", 0.3)), "float"), "Out", out, "Opacity")
    return g


def weathering(g, w, gm, colour, rough, metal, occl, edge, tex_dir):
    """The second art pass's surface variation over a set (matdefs.py "weather"), from the
    shared Grime_Mask (R grime clouds, G streaks down V, B chips, A fine) and the vertex
    colour (RGB baked occlusion, A convex edges):

        grime   soot and dust in clouds and in occluded corners: darker, rougher
        streak  (r, g, b, amount): rust or water stains running down (multiplies the
                colour; a metal's stain is rust, so it is not metal there)
        edge    "polish" (worn bright metal on convex edges), "wear" (lighter, scuffed
                wood or stone), or None
        top     (texture path, metres, amount): moss / grass on upward faces (world
                normal Y), its own BaseColor sampled at UV0 / metres
    Returns (colour, roughness, metallic)."""
    amount = w.get("grime", 0.0)
    if amount > 0:
        occ_dirt = g.mulc(g.math("OneMinus", occl), 1.5)
        t = g.math("Saturate", g.mulc(g.add(g.mulc((gm, "R"), 0.8), occ_dirt), amount))
        dirt = (g.const(vec(*w.get("grime_tint", (0.52, 0.47, 0.42))), "vec3"), "Out")
        colour = g.lerp(colour, g.mul(colour, dirt, "vec3"), g.splat3(t), "vec3")
        rough = g.add(rough, g.mulc(t, 0.18))
    if w.get("streak"):
        r, gg, b, k = w["streak"]
        st = g.math("Saturate", g.mulc((gm, "G"), k))
        stain = (g.const(vec(r, gg, b), "vec3"), "Out")
        if w.get("rust", False):
            colour = g.lerp(colour, stain, g.splat3(g.mulc(st, 0.85)), "vec3")
            metal = g.mul(metal, g.math("OneMinus", st))
            rough = g.add(rough, g.mulc(st, 0.3))
        else:
            colour = g.lerp(colour, g.mul(colour, stain, "vec3"), g.splat3(st), "vec3")
            rough = g.add(rough, g.mulc(st, -0.12))
    mode = w.get("edge")
    if mode:
        e = g.smooth(g.mul(edge, g.addc(g.mulc((gm, "B"), 0.9), 0.5)), 0.3, 0.65)
        if mode == "polish":
            colour = g.lerp(colour, g.mulc(colour, 1.35, "vec3"), g.splat3(e), "vec3")
            rough = g.lerp(rough, g.mulc(rough, 0.55), e)
        else:
            colour = g.lerp(colour, g.mulc(colour, 1.25, "vec3"), g.splat3(g.mulc(e, 0.7)), "vec3")
            rough = g.add(rough, g.mulc(e, 0.08))
    if w.get("top"):
        path, metres, k = w["top"]
        moss = g.texture(f"{path}_BaseColor.nmtexture", g.uv(1.0 / metres))
        ny = g.split((g.node("Input.VertexNormal"), "Normal"))
        up = g.smooth((ny, "Y"), 0.45, 0.85)
        brk = g.smooth(g.add((moss, "A"), g.mulc((gm, "R"), 0.6)), 0.35, 0.75)
        t = g.math("Saturate", g.mulc(g.mul(up, brk), k))
        colour = g.lerp(colour, g.mulc((moss, "RGB"), 0.8, "vec3"), g.splat3(t), "vec3")
        rough = g.lerp(rough, None, t, cb="0.92")
        metal = g.mul(metal, g.math("OneMinus", t))
    rough = g.math("Saturate", g.add(rough, g.addc(g.mulc((gm, "A"), 0.12), -0.06)))
    return colour, rough, metal


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


# ==============================================================================================
# The sea and the particle materials (second art pass)
# ==============================================================================================

# Swell: (amplitude m, wavelength m, direction x, direction z); speeds from deep-water
# dispersion (w = sqrt(g k)).
SEA_WAVES = [(0.24, 38.0, 0.96, 0.28), (0.12, 17.0, 0.71, -0.71), (0.06, 7.5, 0.2, 0.98)]


def _world_xz_m(g):
    """World X and Z in metres as a vec2."""
    wp = g.split((g.node("Input.WorldPosition"), "Position"))
    m = g.node("Vector.Make2")
    g.link(wp, "X", m, "X")
    g.link(wp, "Z", m, "Y")
    return g.mulc((m, "Out"), 0.01, "vec2"), wp


def _swell(g, xz, time, scale):
    """The summed sine swell at xz (metres): (height in metres x scale)."""
    import math as _m
    h = None
    for (amp, length, dx, dz) in SEA_WAVES:
        k = 2 * _m.pi / length
        w = _m.sqrt(9.81 * k)
        dn = _m.hypot(dx, dz)
        d = g.node("Vector.Dot", consts={"B": vec(dx / dn, dz / dn)}, wildcard="vec2")
        g.link(xz[0], xz[1], d, "A")
        phase = g.add(g.mulc((d, "Out"), k), g.mulc(time, w))
        wave = g.mulc(g.math("Sin", phase), amp)
        h = wave if h is None else g.add(h, wave)
    return g.mul(h, scale)


def sea_material(d, tex_dir):
    """Sea: Translucent Lit (Surface) water. The swell is World Position Offset -- three
    sine trains in world space, metres, deep-water speeds (Max Offset 60 cm); two copies of
    Sea_Normal pan across it at different scales and directions; a Fresnel term (from the
    view direction) turns it from see-through blue-green looking down to an opaque mirror at
    grazing angles; crest foam (Sea_Foam) where the swell peaks. Parameters: SwellScale,
    DeepColor, ShallowColor, NormalStrength."""
    g = Graph(d["name"])
    out = output(g, "Translucent", "Default", False,
                 {"Refraction": "1.33", "Specular": "0.25", "TransmissionTint": vec(0.62, 0.86, 0.8)},
                 extra={"MaxOffset": "60"})
    time = (g.node("Input.Time"), "Time")
    scale = (g.param("SwellScale", "float", "1"), "Value")
    xz, wp = _world_xz_m(g)
    h = _swell(g, xz, time, scale)
    off = g.node("Vector.Make3", consts={"X": "0", "Z": "0"})
    g.link(*g.mulc(h, 100.0), off, "Y")
    g.link(off, "Out", out, "WorldPositionOffset")
    # Ripples: two pans of the normal field.
    uva = g.add(g.mulc(xz, 1.0 / 9.0, "vec2"), g.mulc(g.splat2(time), (0.021, 0.008), "vec2"), "vec2")
    uvb = g.add(g.mulc(xz, 1.0 / 23.0, "vec2"), g.mulc(g.splat2(time), (-0.006, 0.013), "vec2"), "vec2")
    na = g.texture(f"{tex_dir}/Sea_Normal.nmtexture", uva)
    nb = g.texture(f"{tex_dir}/Sea_Normal.nmtexture", uvb)
    ncol = g.addc(g.add((na, "RGB"), (nb, "RGB"), "vec3"), (-0.5, -0.5, 0.0), "vec3")
    strength = (g.param("NormalStrength", "float", "0.9"), "Value")
    nm_ = g.node("Texture.NormalMap", props={"Format": "OpenGL"})
    g.link(*ncol, nm_, "Color")
    g.link(*strength, nm_, "Strength")
    g.link(nm_, "Normal", out, "Normal")
    # Fresnel from the view direction against up.
    cam = g.node("Input.CameraPosition")
    view = g.node("Vector.Normalize", wildcard="vec3")
    g.link(*g.math("Subtract", (cam, "Position"), (g.node("Input.WorldPosition"), "Position"), wildcard="vec3"),
           view, "V")
    vy = g.split((view, "Out"))
    fres = g.math("Power", g.math("OneMinus", g.math("Saturate", (vy, "Y"))), consts={"B": "4"})
    deep = (g.param("DeepColor", "vec3", vec(0.012, 0.055, 0.07)), "Value")
    shallow = (g.param("ShallowColor", "vec3", vec(0.035, 0.16, 0.15)), "Value")
    colour = g.lerp(shallow, deep, g.splat3(g.math("Sqrt", fres)), "vec3")
    # Crests: lighter where the swell is high, foam on the highest.
    crest = g.smooth(h, 0.12, 0.38)
    colour = g.lerp(colour, g.mulc(shallow, 1.6, "vec3"), g.splat3(g.mulc(crest, 0.5)), "vec3")
    fuv = g.add(g.mulc(xz, 0.25, "vec2"), g.mulc(g.splat2(time), (0.01, 0.004), "vec2"), "vec2")
    foam_t = g.texture(f"{tex_dir}/Sea_Foam.nmtexture", fuv)
    foam = g.math("Saturate", g.mul(g.smooth((foam_t, "A"), 0.5, 0.85), g.smooth(h, 0.3, 0.42)))
    colour = g.lerp(colour, (foam_t, "RGB"), g.splat3(foam), "vec3")
    g.link(*colour, out, "BaseColor")
    g.link(*g.addc(g.mulc(foam, 0.6), 0.03), out, "Roughness")
    g.link(g.const("0", "float"), "Out", out, "Metallic")
    op = g.math("Saturate", g.add(g.lerp(None, None, fres, ca="0.72", cb="0.97"), foam))
    g.link(*op, out, "Opacity")
    return g


def sea_foam_material(d, tex_dir):
    """SeaFoam: the shore strip (Sea_FoamStrip). Its V runs from 1 at the sand to 0 out to
    sea; bands of Sea_Foam roll in toward the sand and thin out, fading at both edges."""
    g = Graph(d["name"])
    out = output(g, "Translucent", "Default", False, {"Refraction": "1.0"})
    tc = g.node("Input.TexCoord")
    v = (g.split((tc, "UV")), "Y")
    time = (g.node("Input.Time"), "Time")
    uv = g.add(g.mulc((tc, "UV"), 1.5, "vec2"), g.mulc(g.splat2(time), (0.004, 0.035), "vec2"), "vec2")
    foam = g.texture(f"{tex_dir}/Sea_Foam.nmtexture", uv)
    bands = g.addc(g.mulc(g.math("Sin", g.add(g.mulc(v, 18.0), g.mulc(time, -1.4))), 0.35), 0.65)
    edge = g.mul(g.smooth(v, 0.0, 0.35), g.smooth(v, 1.0, 0.9))
    cover = g.math("Saturate", g.addc(g.mulc((foam, "A"), 1.6), -0.25))
    op = g.mul(g.mul(cover, edge), g.addc(g.mulc(g.mul(bands, v), 0.6), 0.25))
    g.link(*g.math("Saturate", op), out, "Opacity")
    g.link(foam, "RGB", out, "BaseColor")
    g.link(g.const("0.65", "float"), "Out", out, "Roughness")
    g.link(g.const("0", "float"), "Out", out, "Metallic")
    return g


def highlight_material(d, tex_dir):
    """ObjectiveHighlight: the glow shell LabWorld lays over whatever the player should use next
    (LabInteractable.cpp, "Highlight"). Unlit Translucent, pushed Push units out along the
    normals so it clears the mesh it copies; a Fresnel rim plus a faint fill, breathing at
    PulseSpeed. Glow (per entity) fades it in and out and brightens it within reach."""
    g = Graph(d["name"])
    out = g.node("Output.Material", props={"ClipValue": "0.5", "Domain": "Translucent", "MaxOffset": "4",
                                           "Shading": "Unlit", "ShadingModel": "Default", "TwoSided": "false"})
    normal = (g.node("Input.VertexNormal"), "Normal")
    push = (g.param("Push", "float", "1.5"), "Value")
    g.link(*g.mul(normal, g.splat3(push), "vec3"), out, "WorldPositionOffset")
    to_eye = g.node("Vector.Normalize", wildcard="vec3")
    g.link(*g.math("Subtract", (g.node("Input.CameraPosition"), "Position"), (g.node("Input.WorldPosition"), "Position"),
                   wildcard="vec3"), to_eye, "V")
    facing = g.node("Vector.Dot", wildcard="vec3")
    g.link(to_eye, "Out", facing, "A")
    nn = g.node("Vector.Normalize", wildcard="vec3")
    g.link(*normal, nn, "V")
    g.link(nn, "Out", facing, "B")
    rim = g.math("Power", g.math("Saturate", g.math("OneMinus", g.math("Abs", (facing, "Out")))), consts={"B": "2.5"})
    time = (g.node("Input.Time"), "Time")
    speed = (g.param("PulseSpeed", "float", "3.2"), "Value")
    pulse = g.addc(g.mulc(g.math("Sin", g.mul(time, speed)), 0.3), 0.7)
    glow = (g.param("Glow", "float", "1"), "Value")
    shape = g.mul(g.addc(rim, 0.12), g.mul(pulse, glow))
    colour = (g.param("Color", "vec3", "1 0.62 0.18"), "Value")
    inten = (g.param("Intensity", "float", "8"), "Value")
    g.link(*g.mul(g.mul(colour, g.splat3(shape), "vec3"), g.splat3(inten), "vec3"), out, "Emissive")
    g.link(*g.math("Saturate", g.mulc(shape, 0.8)), out, "Opacity")
    return g


CUSTOM = {"sea": sea_material, "sea_foam": sea_foam_material, "highlight": highlight_material}


def _radial(g):
    """1 at a sprite's centre, 0 at its rim (UV 0-1)."""
    dist = g.node("Vector.Distance", consts={"B": "0.5 0.5"}, wildcard="vec2")
    g.link(g.node("Input.TexCoord"), "UV", dist, "A")
    return g.math("Saturate", g.math("OneMinus", g.mulc((dist, "Out"), 2.0)))


def particle_material(name, kind, tex_dir):
    """Particle materials (Content/Materials/Particles): the particle's colour arrives as the
    vertex colour (RGB, A its alpha over life).

        Steam   Translucent Lit: the Puff billow, colour x particle colour (lit by the sun
                and the lamps, so steam in a light shaft glows)
        Dust    Translucent Lit: a small soft mote (dust in light shafts)
        Spray   Translucent Lit: a glossy droplet (sprinklers)
        Spark   Translucent Unlit: a hot point, Emissive = colour x Intensity (blooms)
        Arc     Translucent Unlit, for beams: a white-hot core across U fading to the
                colour at the strip's edges, flickering along V (reactor lightning)"""
    g = Graph(name)
    vc = g.node("Input.VertexColor")
    lit = kind in ("Steam", "Dust", "Spray")
    props = {"ClipValue": "0.5", "Domain": "Translucent", "MaxOffset": "0", "Shading": "Lit" if lit else "Unlit",
             "ShadingModel": "Default", "TwoSided": "true"}
    if lit:
        props["TranslucencyLighting"] = "Legacy"
    out = g.node("Output.Material", props=props)
    if kind in ("Steam", "Dust"):
        puff = g.texture(f"{tex_dir}/Puff.nmtexture", None)
        dens = (puff, "A") if kind == "Steam" else g.mul(_radial(g), _radial(g))
        g.link(*g.mul((vc, "RGB"), (puff, "RGB"), "vec3"), out, "BaseColor")
        g.link(*g.mul(dens, (vc, "A")), out, "Opacity")
        g.link(g.const("1" if kind == "Steam" else "0.8", "float"), "Out", out, "Roughness")
    elif kind == "Spray":
        r = _radial(g)
        g.link(vc, "RGB", out, "BaseColor")
        g.link(*g.mul(g.math("Sqrt", r), (vc, "A")), out, "Opacity")
        g.link(g.const("0.08", "float"), "Out", out, "Roughness")
    elif kind == "Spark":
        r = _radial(g)
        core = g.mul(r, r)
        inten = (g.param("Intensity", "float", "24"), "Value")
        g.link(*g.mul(g.mul((vc, "RGB"), g.splat3(core), "vec3"), g.splat3(inten), "vec3"), out, "Emissive")
        g.link(*g.mul(core, (vc, "A")), out, "Opacity")
    elif kind == "Arc":
        tc = g.split((g.node("Input.TexCoord"), "UV"))
        across = g.math("Saturate", g.math("OneMinus", g.mulc(g.math("Abs", g.addc((tc, "X"), -0.5)), 2.0)))
        core = g.math("Power", across, consts={"B": "3"})
        halo = g.mul(across, across)
        time = (g.node("Input.Time"), "Time")
        flick = g.addc(g.mulc(g.math("Sin", g.add(g.mulc((tc, "Y"), 23.0), g.mulc(time, 70.0))), 0.35), 0.65)
        inten = (g.param("Intensity", "float", "40"), "Value")
        hot = g.lerp((vc, "RGB"), None, g.splat3(core), "vec3", cb="1 1 1")
        glow = g.mul(g.mul(hot, g.splat3(g.mul(halo, flick)), "vec3"), g.splat3(inten), "vec3")
        g.link(*glow, out, "Emissive")
        g.link(*g.mul(halo, (vc, "A")), out, "Opacity")
    return g
