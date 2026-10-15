#!/usr/bin/env python3
"""
make_grasslab_materials.py -- the GrassLab level's materials,
Content/Materials/GrassLab/*.material.nmgraph, written as the editor saves
them, without their generated code; then fill <gen_code>:

    python Games/WorldsLab/Tools/GrassLab/make_grasslab_materials.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/GrassLab/*.material.nmgraph

    GrassBlade        the blades of make_grasslab_meshes.py's clumps (Opaque,
                      two-sided; a ramp from deep green roots to yellow-green
                      tips, a breeze in gusts) bent by the ball's trail: World Position
                      Offset samples Content/RenderTargets/GrassTrail.nmrt2d,
                      which GrassLab's custom render pass draws each frame
                      before the scene (WorldsLabGrassLab.cpp). Its texel at
                      the blade's root says which way it is pushed (rg), how
                      hard (b) and how flat (a) -- all zeros at rest, as an
                      undrawn target reads; the push grows toward the tip
                      (1 - UV.v: a blade's tip is v = 0) and the tip
                      drops as it bends, so a blade lies down rather than
                      stretching.
    Ball              the rolling ball: a glossy orange.

The trail covers the field FIELD (make_grasslab_level.py's, and the pass's
in WorldsLabGrassLab.cpp: keep the three the same).
"""

import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Courtyard"))
import make_courtyard_materials as cy  # noqa: E402

MAT_DIR = GAME_DIR / "Content" / "Materials" / "GrassLab"
TEX     = "Content/Textures/Courtyard"
TRAIL   = "Content/RenderTargets/GrassTrail.nmrt2d"

# The field the trail covers: lowest world X, Z and its size (cm).
FIELD = (-2400.0, -2400.0, 4800.0, 4800.0)

BEND  = 30.0   # how far a fully pushed tip moves sideways (cm, at instance scale 1)
DROOP = 20.0   # and how far it drops


class Graph(cy.Graph):
    """Courtyard's graph, its node ids from GrassLab's names instead."""
    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = cy.hashlib.md5(f"GrassLab/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard,
                           (self.count % 6) * 220 - 600, (self.count // 6) * 160 - 400))
        return nid


def wind(g, tip2):
    """A breeze through the field, in world units, all of it at the tips (tip2:
    (1 - v)^2): a slow sway rolling across the field in gusts (its phase from
    where the clump stands), and a quicker flutter of each blade on top."""
    split = g.node("Vector.Split")
    g.link(g.node("Input.WorldPosition"), "Position", split, "Vector")
    time = (g.node("Input.Time"), "Time")
    x, z = (split, "X"), (split, "Z")
    scaled = lambda a, k: (g.math("Multiply", a, consts={"B": cy.vec(k)}), "Out")
    added = lambda a, b: (g.math("Add", a, b), "Out")
    sine = lambda a, op="Sin": (g.math(op, a), "Out")
    # Gusts travel along the wind (+X +Z): the phase falls with distance downwind.
    travel = added(scaled(x, -0.0016), scaled(z, -0.0012))
    gust = added(scaled(sine(added(scaled(time, 0.9), travel)), 0.5), scaled(sine(added(scaled(time, 0.37), scaled(travel, 0.6))), 0.5))
    sway = added(scaled(gust, 7.0), scaled((g.const("1", "float"), "Out"), 3.0))   # leans downwind a little even when calm
    blade_phase = added(scaled(x, 0.21), scaled(z, 0.17))
    flutter = scaled(sine(added(scaled(time, 4.7), blade_phase)), 1.4)
    flutter_z = scaled(sine(added(scaled(time, 5.9), scaled(blade_phase, 1.3)), "Cos"), 1.2)
    w = tip2
    offset = g.node("Vector.Make3")
    g.link(*(g.math("Multiply", added(scaled(sway, 0.8), flutter), w), "Out"), offset, "X")
    g.link(*(g.math("Multiply", scaled(gust, -1.5), w), "Out"), offset, "Y")
    g.link(*(g.math("Multiply", added(scaled(sway, 0.6), flutter_z), w), "Out"), offset, "Z")
    return (offset, "Out")


def trail(g):
    """The ball's push at this blade's root, toward its tip: (offset vec3, flatten float)."""
    split = g.node("Vector.Split")
    g.link(g.node("Input.WorldPosition"), "Position", split, "Vector")
    u = g.math("Divide", (g.math("Subtract", (split, "X"), consts={"B": cy.vec(FIELD[0])}), "Out"),
               consts={"B": cy.vec(FIELD[2])})
    v = g.math("Divide", (g.math("Subtract", (split, "Z"), consts={"B": cy.vec(FIELD[1])}), "Out"),
               consts={"B": cy.vec(FIELD[3])})
    uv = g.node("Vector.Make2")
    g.link(u, "Out", uv, "X")
    g.link(v, "Out", uv, "Y")
    obj = g.node("Texture.Object", props={"Filter": "Linear", "Texture": TRAIL, "Wrap": "Clamp"})
    sample = g.node("Texture.Sample", version=2)
    g.link(obj, "Texture", sample, "Texture")
    g.link(uv, "Out", sample, "UV")

    # 1 at the tip, 0 at the root.
    tex = g.node("Vector.Split")
    g.link(g.node("Input.TexCoord"), "UV", tex, "Vector")
    tip = g.node("Math.OneMinus", wildcard="float")
    g.link(tex, "Y", tip, "X")
    tip2 = g.math("Multiply", (tip, "Out"), (tip, "Out"))

    # How hard, toward the tip: b x (1 - v)^2 x BEND.
    reach = g.math("Multiply", (g.math("Multiply", (sample, "B"), (tip2, "Out")), "Out"), consts={"B": cy.vec(BEND)})

    def push(channel):
        centred = g.math("Subtract", (g.math("Multiply", (sample, channel), consts={"B": "2"}), "Out"), consts={"B": "1"})
        return g.math("Multiply", (centred, "Out"), (reach, "Out"))

    drop = g.math("Multiply", (g.math("Multiply", (sample, "A"), (tip, "Out")), "Out"), consts={"B": cy.vec(-DROOP)})
    offset = g.node("Vector.Make3")
    g.link(push("R"), "Out", offset, "X")
    g.link(drop, "Out", offset, "Y")
    g.link(push("G"), "Out", offset, "Z")
    return (offset, "Out"), (sample, "A"), (tip, "Out"), (tip2, "Out")


def grass():
    g = Graph("GrassBlade")
    out = cy.output(g, two_sided=True, max_offset=BEND + 14.0)
    bent, flatten, tip, tip2 = trail(g)
    # The wind stills where the grass is pressed flat.
    calm = g.node("Math.OneMinus", wildcard="float")
    g.link(flatten[0], flatten[1], calm, "X")
    breeze = g.math("Multiply", wind(g, tip2), g.splat3((calm, "Out")), wildcard="vec3")
    total = g.math("Add", (breeze, "Out"), bent, wildcard="vec3")
    g.link(total, "Out", out, "WorldPositionOffset")

    # Deep green at the root, a sunlit yellow-green toward the tip; each blade's
    # own shade (vertex colour, times the spawner's per-clump tint) over it.
    ramp = g.node("Math.Lerp", consts={"A": "0.11 0.17 0.045", "B": "0.36 0.52 0.13"}, wildcard="vec3")
    g.link(tip[0], tip[1], ramp, "T")
    vc = g.node("Input.VertexColor")
    tint = g.param("Tint", "vec3", "1 1 1")
    coloured = g.math("Multiply", (g.math("Multiply", (ramp, "Out"), (vc, "RGB"), wildcard="vec3"), "Out"), (tint, "Value"),
                      wildcard="vec3")
    # Pressed grass shows its paler, sunlit side, as a trodden path through a
    # field does: lighter and yellower where the trail flattens it, most at the tips.
    press = g.math("Multiply", flatten, tip)
    pale = g.node("Math.Lerp", consts={"A": "1 1 1", "B": "1.6 1.45 1.1"}, wildcard="vec3")
    g.link(press, "Out", pale, "T")
    g.link(g.math("Multiply", (coloured, "Out"), (pale, "Out"), wildcard="vec3"), "Out", out, "BaseColor")
    # Shadowed down in the clump, open at the tips.
    ao = g.node("Math.Lerp", consts={"A": "0.6", "B": "1"}, wildcard="float")
    g.link(tip[0], tip[1], ao, "T")
    g.link(ao, "Out", out, "AmbientOcclusion")
    rough = g.node("Math.Lerp", consts={"A": "0.85", "B": "0.5"}, wildcard="float")
    g.link(tip[0], tip[1], rough, "T")
    g.link(rough, "Out", out, "Roughness")
    return g


def ball():
    g = Graph("Ball")
    out = cy.output(g)
    colour = g.param("Color", "vec3", "1 0.36 0.08")
    g.link(colour, "Value", out, "BaseColor")
    g.link(g.const("0.3", "float"), "Out", out, "Roughness")
    return g


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    for graph in (grass(), ball()):
        path = MAT_DIR / f"{graph.name}.material.nmgraph"
        graph.write(path)
        print(f"  {path.relative_to(GAME_DIR)}")


if __name__ == "__main__":
    main()
