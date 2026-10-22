#!/usr/bin/env python3
"""
make_lightinglab.py -- the LightingLab level (Engine/Modules/RenderCore/
GLOBAL_ILLUMINATION.md): what each phase of the lighting work is checked on.
Writes the materials, Content/Materials/LightingLab/*.material.nmgraph, without
their generated code, and the level, Content/Levels/LightingLab(.nmworld).
Then fill <gen_code>:

    python Games/WorldsLab/Tools/LightingLab/make_lightinglab.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/LightingLab/*.material.nmgraph

    The room     a Cornell box, 1200 wide and deep, 600 high, open toward
                 the camera (-Z) and lit by a low sun through it: a red wall
                 left, a green one right, white elsewhere -- for bounced light.
    The spheres  gold, roughness 0.1 to 1 left to right, along the room's
                 back: rough metals' energy.
    The field    beside the room (+X), a floor under a 12 x 12 grid of small
                 coloured point lights: many lights.
    Area lights  a softbox in the ceiling (a Rect Light shining down), a warm
                 panel on the green wall, and a bulb with a 40-unit sphere.

Cameras (the editor's "camera x y z yaw pitch"):
    room   0 300 -1500 0 -6
    field  2600 700 -900 0 -30
"""

import hashlib
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Courtyard"))
import make_courtyard_materials as cy  # noqa: E402

MAT_DIR   = GAME_DIR / "Content" / "Materials" / "LightingLab"
LEVEL_DIR = GAME_DIR / "Content" / "Levels" / "LightingLab"
M         = "Content/Materials/LightingLab/"
BOX       = "Engine/Content/Meshes/Box.nmmesh"
SPHERE    = "Engine/Content/Meshes/Sphere.nmmesh"


class Graph(cy.Graph):
    """Courtyard's graph, its node ids from LightingLab's names instead."""
    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"LightingLab/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard,
                           (self.count % 6) * 220 - 600, (self.count // 6) * 160 - 400))
        return nid


def material(name, consts, model="Default"):
    g = Graph(name)
    g.node("Output.Material", props={"ClipValue": "0.5", "Domain": "Opaque", "MaxOffset": "0", "Shading": "Lit",
                                     "ShadingModel": model, "TwoSided": "false"}, consts=consts)
    return g


GOLD = cy.vec(1.0, 0.78, 0.34)
MATERIALS = [
    material("White", {"BaseColor": cy.vec(0.8, 0.8, 0.8), "Roughness": "0.9"}),
    material("Red", {"BaseColor": cy.vec(0.75, 0.08, 0.06), "Roughness": "0.9"}),
    material("Green", {"BaseColor": cy.vec(0.12, 0.6, 0.12), "Roughness": "0.9"}),
    material("Floor", {"BaseColor": cy.vec(0.5, 0.5, 0.52), "Roughness": "0.35"}),
] + [material(f"Gold{int(r * 10):02d}", {"BaseColor": GOLD, "Metallic": "1", "Roughness": cy.vec(r)})
     for r in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0)]


# --- The level ---------------------------------------------------------------

guids = []


def guid(name):
    return hashlib.md5(("LightingLab/" + name).encode()).hexdigest()


def transform(pos, rot=(0, 0, 0), scale=(1, 1, 1)):
    return f"""  <Component Type="Transform" Version="1">
    <Position X="{pos[0]}" Y="{pos[1]}" Z="{pos[2]}" />
    <Rotation X="{rot[0]}" Y="{rot[1]}" Z="{rot[2]}" />
    <Scale X="{scale[0]}" Y="{scale[1]}" Z="{scale[2]}" />
  </Component>
"""


def render(mesh, material_path, shadows=True):
    slots = "".join(f'    <Material{i} Value="{material_path if i == 0 else ""}" />\n' for i in range(8))
    return f"""  <Component Type="Render" Version="5">
    <Visible Value="true" />
{slots}    <Mesh Value="{mesh}" />
    <ParameterCount Value="0" />
    <CastShadows Value="{'true' if shadows else 'false'}" />
    <CastInsetShadow Value="false" />
    <Occluder Value="false" />
  </Component>
"""


def point_light(color, intensity, radius, shadows=False, source_radius=6):
    return f"""  <Component Type="Point Light" Version="2">
    <Color R="{color[0]}" G="{color[1]}" B="{color[2]}" A="255" />
    <Intensity Value="{intensity}" />
    <Radius Value="{radius}" />
    <Active Value="true" />
    <CastShadows Value="{'true' if shadows else 'false'}" />
    <ShadowTechnique Value="0" />
    <ShadowTint R="40" G="50" B="80" A="255" />
    <ShadowBias Value="0.004" />
    <ShadowNormalBias Value="2" />
    <SourceRadius Value="{source_radius}" />
  </Component>
"""


def rect_light(color, intensity, width, height, light_range):
    return f"""  <Component Type="Rect Light" Version="1">
    <Color R="{color[0]}" G="{color[1]}" B="{color[2]}" A="255" />
    <Intensity Value="{intensity}" />
    <Width Value="{width}" />
    <Height Value="{height}" />
    <Range Value="{light_range}" />
    <Active Value="true" />
  </Component>
"""


def reflection_probe(radius, frequency=2, resolution=128):
    """Frequency: 0 every frame, 2 once (ESceneCaptureFrequency)."""
    return f"""  <Component Type="Reflection Probe" Version="1">
    <Radius Value="{radius}" />
    <BoxExtent X="0" Y="0" Z="0" />
    <Intensity Value="1" />
    <Resolution Value="{resolution}" />
    <Frequency Value="{frequency}" />
    <Interval Value="1" />
    <NearClip Value="5" />
    <FarClip Value="20000" />
    <CaptureShadows Value="true" />
    <Enabled Value="true" />
  </Component>
"""


# Phase 4: screen-space reflections up to roughness 0.8, TAA and tone mapping; Phase 5: screen-space GI.
PP_STACK = """<?xml version="1.0" encoding="UTF-8"?>
<NodeGraph Version="1" Schema="PostProcess">
  <Nodes>
    <Node Id="4c1b0e6f9a3d4e8bb0d1a2c3e4f50601" Type="Effect.ToneMapping" Version="1" X="-380" Y="-400">
      <Prop Name="Exposure" Value="0" />
      <Prop Name="Tone Mapper" Value="ACES" />
    </Node>
    <Node Id="4c1b0e6f9a3d4e8bb0d1a2c3e4f50602" Type="Effect.Antialiasing" Version="1" X="-380" Y="-240">
      <Prop Name="Method" Value="TAA" />
      <Prop Name="Quality" Value="Low" />
    </Node>
    <Node Id="4c1b0e6f9a3d4e8bb0d1a2c3e4f50603" Type="Effect.ScreenSpaceReflections" Version="1" X="-160" Y="-400">
      <Prop Name="Intensity" Value="1" />
      <Prop Name="Max Distance" Value="2000" />
      <Prop Name="Max Roughness" Value="0.8" />
      <Prop Name="Steps" Value="32" />
      <Prop Name="Thickness" Value="25" />
    </Node>
    <Node Id="4c1b0e6f9a3d4e8bb0d1a2c3e4f50604" Type="Effect.ScreenSpaceGI" Version="1" X="60" Y="-400">
      <Prop Name="Intensity" Value="1" />
      <Prop Name="Radius" Value="300" />
      <Prop Name="Rays" Value="4" />
    </Node>
  </Nodes>
  <gen_code>
    <Output Extension=".ppstack"><![CDATA[# Post process stack LightingLab
Effect ScreenSpaceReflections Intensity=1 MaxRoughness=0.8 MaxDistance=2000 Thickness=25 Steps=32
Effect ScreenSpaceGI Intensity=1 Rays=4 Radius=300
Effect ToneMapping Exposure=0 ToneMapper=ACES
Effect Antialiasing EdgeThreshold=0.125 Sharpness=0.35 Method=TAA Quality=Low
]]></Output>
  </gen_code>
</NodeGraph>
"""
PP_PATH = "Content/PostProcess/LightingLab/LightingLab.pp.nmgraph"


def entity(name, body, mobility=None):
    g = guid(name)
    guids.append(g)
    mob = f' Mobility="{mobility}"' if mobility else ""
    (LEVEL_DIR / (g + ".nmentity")).write_text(
        f'<?xml version="1.0" encoding="UTF-8"?>\n<Entity Guid="{g}" Name="{name}"{mob}>\n{body}</Entity>\n', newline="\n")


def hue(index, count):
    """A bright colour round the wheel."""
    import colorsys
    r, g, b = colorsys.hsv_to_rgb(index / count, 0.75, 1.0)
    return int(r * 255), int(g * 255), int(b * 255)


def level():
    LEVEL_DIR.mkdir(parents=True, exist_ok=True)
    for old in LEVEL_DIR.glob("*.nmentity"):
        old.unlink()

    entity("Camera", """  <Component Type="Camera" Version="2">
    <ZNear Value="5" />
    <ZFar Value="30000" />
    <Active Value="true" />
    <Projection Value="1" />
    <FovY Value="55" />
  </Component>
""" + transform((0, 300, -1500), (6, 0, 0)))

    entity("Sun", """  <Component Type="Directional Light" Version="3">
    <Color R="255" G="242" B="220" A="255" />
    <Intensity Value="6" />
    <Active Value="true" />
    <CastShadows Value="true" />
    <ShadowTechnique Value="2" />
    <ShadowTint R="25" G="28" B="36" A="255" />
    <ShadowBias Value="0.0012" />
    <ShadowNormalBias Value="2" />
    <ShadowDistance Value="6000" />
    <CascadeCount Value="4" />
    <CascadeSplitLambda Value="0.75" />
    <SourceAngle Value="0.5" />
  </Component>
""" + transform((0, 4000, -4000), (35, 20, 0)))

    entity("Sky", """  <Component Type="Sky Light" Version="2">
    <SkyColor R="150" G="165" B="190" A="255" />
    <GroundColor R="60" G="56" B="50" A="255" />
    <Intensity Value="0.3" />
    <Active Value="true" />
    <Environment Value="Content/Textures/ShootingRange/Sky_HDR.nmtexture" />
    <EnvironmentIntensity Value="0.5" />
    <EnvironmentRotation Value="0" />
    <DrawBackground Value="true" />
  </Component>
""" + transform((0, 0, 0)), "Static")

    # The ground under everything, and the room.
    entity("Ground", render(BOX, M + "Floor.material.nmgraph") + transform((1200, -10, 0), scale=(8000, 20, 8000)))
    entity("BackWall", render(BOX, M + "White.material.nmgraph") + transform((0, 300, 610), scale=(1240, 600, 20)))
    entity("Ceiling", render(BOX, M + "White.material.nmgraph") + transform((0, 610, 0), scale=(1240, 20, 1240)))
    entity("LeftWall", render(BOX, M + "Red.material.nmgraph") + transform((-610, 300, 0), scale=(20, 600, 1240)))
    entity("RightWall", render(BOX, M + "Green.material.nmgraph") + transform((610, 300, 0), scale=(20, 600, 1240)))

    # Gold spheres along the back, smooth to rough.
    for index in range(10):
        x = -495 + index * 110
        entity(f"Gold{index + 1:02d}", render(SPHERE, M + f"Gold{index + 1:02d}.material.nmgraph")
               + transform((x, 50, 450), scale=(90, 90, 90)))
    entity("WhiteBlock", render(BOX, M + "White.material.nmgraph") + transform((-200, 150, 150), (0, 25, 0), (220, 300, 220)))

    # Area lights (Phase 3): a softbox in the ceiling shining down, a warm panel on the
    # right wall facing in, and a bulb with a big sphere over the gold spheres.
    entity("Softbox", rect_light((255, 250, 240), 3, 400, 200, 1400) + transform((0, 590, 200), (90, 0, 0)))
    entity("WarmPanel", rect_light((255, 170, 90), 4, 160, 300, 1200) + transform((590, 250, 300), (0, -90, 0)))
    entity("Bulb", point_light((200, 220, 255), 6, 600, source_radius=40) + transform((300, 220, 350)))

    # Reflection probes (Phase 4): more than the old four -- a 3 x 3 grid over the room,
    # overlapping so every corner is inside one,
    # captured once, and one over the field captured every frame (its capture spread
    # over frames, [Rendering] ProbeFacesPerFrame).
    for row in range(3):
        for col in range(3):
            entity(f"RoomProbe{row * 3 + col}", reflection_probe(800)
                   + transform((-400 + col * 400, 300, -400 + row * 400)))
    entity("FieldProbe", reflection_probe(1400, frequency=0) + transform((2380, 200, -20)))
    # Dynamic GI (Phase 6): a GI Volume filling the room, 9 x 4 x 9 probes.
    entity("GIVolume", """  <Component Type="GI Volume" Version="1">
    <Extent X="600" Y="290" Z="600" />
    <ProbeSpacing Value="150" />
    <Intensity Value="1" />
    <Enabled Value="true" />
  </Component>
""" + transform((0, 300, 0)))
    entity("PostProcess", f"""  <Component Type="Post Process" Version="2">
    <Stack Value="{PP_PATH}" />
    <Priority Value="0" />
    <Enabled Value="true" />
    <BlendDistance Value="0" />
  </Component>
""" + transform((0, 0, 0)))

    # The field: 144 small coloured point lights over the ground beside the room.
    for row in range(12):
        for col in range(12):
            index = row * 12 + col
            entity(f"FieldLight{index:03d}", point_light(hue(index, 144), 4, 180)
                   + transform((1500 + col * 160, 40, -900 + row * 160)))

    with open(GAME_DIR / "Content" / "Levels" / "LightingLab.nmworld", "w", newline="\n") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<World Format="1">\n')
        for g in guids:
            f.write(f'    <Entity Guid="{g}" />\n')
        f.write("</World>\n")


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    for graph in MATERIALS:
        path = MAT_DIR / f"{graph.name}.material.nmgraph"
        graph.write(path)
    pp = GAME_DIR / PP_PATH
    pp.parent.mkdir(parents=True, exist_ok=True)
    pp.write_text(PP_STACK, newline="\n")
    level()
    print(f"  {len(MATERIALS)} materials; Content/Levels/LightingLab.nmworld: {len(guids)} entities")


if __name__ == "__main__":
    main()
