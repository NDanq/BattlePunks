#!/usr/bin/env python3
"""
make_shadingmodellab.py -- the ShadingModelLab level (Engine/Modules/RenderCore/
SHADING_MODELS.md): one sphere per shading model on a grey floor, lit by a low
sun from behind and to the left (so transmission and sheen show) and an HDRI
sky, and a leaf card held up against the sun. Writes the materials,
Content/Materials/ShadingModelLab/*.material.nmgraph, without their generated
code, and the level, Content/Levels/ShadingModelLab(.nmworld). Then fill
<gen_code>:

    python Games/WorldsLab/Tools/ShadingModelLab/make_shadingmodellab.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/ShadingModelLab/*.material.nmgraph

Left to right:

    Plastic       Default: red plastic, the reference
    CarPaint      Clear Coat: a dark red metallic base under a glossy clear coat
    BrushedSteel  Anisotropic: steel brushed along the sphere's UV (round it)
    Velvet        Cloth: deep plum with a pink sheen at its rim
    Wax           Subsurface: candle wax the sun glows through
    Skin          Skin: blurred in screen space where the scene is HDR

    Leaf          Subsurface, two-sided, thin: a card held up against the sun
"""

import hashlib
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Courtyard"))
import make_courtyard_materials as cy  # noqa: E402

MAT_DIR   = GAME_DIR / "Content" / "Materials" / "ShadingModelLab"
LEVEL_DIR = GAME_DIR / "Content" / "Levels" / "ShadingModelLab"
M         = "Content/Materials/ShadingModelLab/"


class Graph(cy.Graph):
    """Courtyard's graph, its node ids from ShadingModelLab's names instead."""
    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"ShadingModelLab/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard,
                           (self.count % 6) * 220 - 600, (self.count // 6) * 160 - 400))
        return nid


def material(name, model, consts, two_sided=False):
    """A Material Output alone, its pins constants."""
    g = Graph(name)
    g.node("Output.Material", props={"ClipValue": "0.5", "Domain": "Opaque", "MaxOffset": "0", "Shading": "Lit",
                                     "ShadingModel": model, "TwoSided": "true" if two_sided else "false"},
           consts=consts)
    return g


def brushed_steel():
    """Brushed round the sphere: its Tangent the vertex normal across +Y, smooth
    everywhere but the poles. (Unlinked, Tangent follows the UV's U from screen
    derivatives, which is faceted on a low-poly curve: meshes carry no tangents yet.)"""
    g = material("BrushedSteel", "Anisotropic", {"BaseColor": cy.vec(0.9, 0.9, 0.92), "Metallic": "1",
                                                 "Roughness": "0.35", "Anisotropy": "0.85"})
    out = g.nodes[0][0]
    normal = g.node("Input.VertexNormal")
    across = g.node("Vector.Cross", consts={"B": cy.vec(0, 1, 0)})
    g.link(normal, "Normal", across, "A")
    g.link(across, "Out", out, "Tangent")
    return g


MATERIALS = [
    material("Plastic", "Default", {"BaseColor": cy.vec(0.6, 0.06, 0.05), "Roughness": "0.35"}),
    material("CarPaint", "ClearCoat", {"BaseColor": cy.vec(0.45, 0.02, 0.03), "Metallic": "0.6", "Roughness": "0.45",
                                       "ClearCoat": "1", "ClearCoatRoughness": "0.03"}),
    brushed_steel(),
    material("Velvet", "Cloth", {"BaseColor": cy.vec(0.22, 0.03, 0.1), "Roughness": "0.75",
                                 "SheenColor": cy.vec(0.95, 0.55, 0.75)}),
    material("Wax", "Subsurface", {"BaseColor": cy.vec(0.93, 0.85, 0.62), "Roughness": "0.4",
                                   "SubsurfaceColor": cy.vec(1.0, 0.55, 0.2), "Thickness": "0.35"}),
    material("Skin", "Skin", {"BaseColor": cy.vec(0.8, 0.56, 0.45), "Roughness": "0.5",
                              "SubsurfaceColor": cy.vec(1.0, 0.3, 0.2), "Thickness": "0.7"}),
    material("Leaf", "Subsurface", {"BaseColor": cy.vec(0.18, 0.42, 0.08), "Roughness": "0.6",
                                    "SubsurfaceColor": cy.vec(0.7, 0.95, 0.2), "Thickness": "0.1"}, two_sided=True),
    material("Floor", "Default", {"BaseColor": cy.vec(0.42, 0.42, 0.42), "Roughness": "0.8"}),
]


# --- The level ---------------------------------------------------------------

guids = []


def guid(name):
    return hashlib.md5(("ShadingModelLab/" + name).encode()).hexdigest()


def transform(pos, rot=(0, 0, 0), scale=(1, 1, 1)):
    return f"""  <Component Type="Transform" Version="1">
    <Position X="{pos[0]}" Y="{pos[1]}" Z="{pos[2]}" />
    <Rotation X="{rot[0]}" Y="{rot[1]}" Z="{rot[2]}" />
    <Scale X="{scale[0]}" Y="{scale[1]}" Z="{scale[2]}" />
  </Component>
"""


def render(mesh, material_path):
    slots = "".join(f'    <Material{i} Value="{material_path if i == 0 else ""}" />\n' for i in range(8))
    return f"""  <Component Type="Render" Version="5">
    <Visible Value="true" />
{slots}    <Mesh Value="{mesh}" />
    <ParameterCount Value="0" />
    <CastShadows Value="true" />
    <CastInsetShadow Value="false" />
    <Occluder Value="false" />
  </Component>
"""


def entity(name, body, mobility=None):
    g = guid(name)
    guids.append(g)
    mob = f' Mobility="{mobility}"' if mobility else ""
    (LEVEL_DIR / (g + ".nmentity")).write_text(
        f'<?xml version="1.0" encoding="UTF-8"?>\n<Entity Guid="{g}" Name="{name}"{mob}>\n{body}</Entity>\n', newline="\n")


def level():
    LEVEL_DIR.mkdir(parents=True, exist_ok=True)
    for old in LEVEL_DIR.glob("*.nmentity"):
        old.unlink()

    entity("Camera", """  <Component Type="Camera" Version="2">
    <ZNear Value="5" />
    <ZFar Value="20000" />
    <Active Value="true" />
    <Projection Value="1" />
    <FovY Value="50" />
  </Component>
""" + transform((0, 160, -760), (8, 0, 0)))

    # Low, from behind and to the left: rims, sheen, and light through the wax, skin and leaf.
    entity("Sun", """  <Component Type="Directional Light" Version="3">
    <Color R="255" G="240" B="215" A="255" />
    <Intensity Value="6" />
    <Active Value="true" />
    <CastShadows Value="true" />
    <ShadowTechnique Value="2" />
    <ShadowTint R="25" G="28" B="36" A="255" />
    <ShadowBias Value="0.0012" />
    <ShadowNormalBias Value="2" />
    <ShadowDistance Value="3000" />
    <CascadeCount Value="4" />
    <CascadeSplitLambda Value="0.75" />
    <SourceAngle Value="0.5" />
  </Component>
""" + transform((0, 4000, 0), (22, 150, 0)))

    entity("Sky", """  <Component Type="Sky Light" Version="2">
    <SkyColor R="150" G="165" B="190" A="255" />
    <GroundColor R="60" G="56" B="50" A="255" />
    <Intensity Value="0.35" />
    <Active Value="true" />
    <Environment Value="Content/Textures/ShootingRange/Sky_HDR.nmtexture" />
    <EnvironmentIntensity Value="0.6" />
    <EnvironmentRotation Value="0" />
    <DrawBackground Value="true" />
  </Component>
""" + transform((0, 0, 0)), "Static")

    entity("Floor", render("Engine/Content/Meshes/Box.nmmesh", M + "Floor.material.nmgraph")
           + transform((0, -10, 0), scale=(3000, 20, 3000)), "Static")

    spheres = ["Plastic", "CarPaint", "BrushedSteel", "Velvet", "Wax", "Skin"]
    for index, name in enumerate(spheres):
        x = (index - (len(spheres) - 1) / 2.0) * 130.0
        entity(name, render("Engine/Content/Meshes/Sphere.nmmesh", M + f"{name}.material.nmgraph")
               + transform((x, 55, 0), scale=(100, 100, 100)))

    # A leaf card, upright between the spheres and the sun, to show light through it.
    entity("Leaf", render("Engine/Content/Meshes/Quad.nmmesh", M + "Leaf.material.nmgraph")
           + transform((0, 230, 180), (0, 0, 0), (120, 160, 1)))

    with open(GAME_DIR / "Content" / "Levels" / "ShadingModelLab.nmworld", "w", newline="\n") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<World Format="1">\n')
        for g in guids:
            f.write(f'    <Entity Guid="{g}" />\n')
        f.write("</World>\n")


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    for graph in MATERIALS:
        path = MAT_DIR / f"{graph.name}.material.nmgraph"
        graph.write(path)
        print(f"  {path.relative_to(GAME_DIR)}")
    level()
    print(f"  Content/Levels/ShadingModelLab.nmworld: {len(guids)} entities")


if __name__ == "__main__":
    main()
