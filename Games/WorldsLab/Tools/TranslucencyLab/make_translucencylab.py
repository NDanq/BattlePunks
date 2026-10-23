#!/usr/bin/env python3
"""
make_translucencylab.py -- the TranslucencyLab level (Engine/Modules/RenderCore/
LIT_TRANSLUCENCY.md): what each phase of the lit translucency and lit particle
work is checked on. Writes the materials, Content/Materials/TranslucencyLab/
*.material.nmgraph, without their generated code; the particle systems,
Content/Particles/TranslucencyLab/*.nmparticle; and the level,
Content/Levels/TranslucencyLab(.nmworld). Then fill <gen_code>:

    python Games/WorldsLab/Tools/TranslucencyLab/make_translucencylab.py
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/Materials/TranslucencyLab/*.material.nmgraph
    <bin>/NodeGraphGenerate Games/WorldsLab/Content/PostProcess/TranslucencyLab/*.pp.nmgraph

    The glass   three panes of clear glass (Translucent Lit, opacity 0.12,
                smooth) in front of a dark backdrop, the left half of them
                in an overhead slab's sun shadow: shadows on forward Lit (Phase 2),
                specular over opacity (Phase 3).
    The water   a smooth translucent strip on the floor below a red wall:
                reflections on forward Lit (Phase 4).
    The plume   a column of lit smoke crossing a shadowed spot light's cone
                and the sun, over a pale floor it shadows, with sparks
                (Translucent Unlit) through it: one translucent stage
                (Phase 1), the Volume model and per-particle lighting
                (Phases 5 and 6), translucent shadows and self-shadowing
                (Phases 7 and 8).
    Everything  inside a GI Volume (DDGI, Phase 3), under thin volumetric
                fog (shafts through the plume, Phase 7).

The game mode (WorldsLabTranslucencyLab.cpp) puts the camera at one of four
fixed views, WorldsLab.TranslucencyLabView 0-3: the whole lab, the glass, the
plume, the water.
"""

import hashlib
import re
import sys
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAME_DIR / "Tools" / "Courtyard"))
import make_courtyard_materials as cy  # noqa: E402

MAT_DIR      = GAME_DIR / "Content" / "Materials" / "TranslucencyLab"
PARTICLE_DIR = GAME_DIR / "Content" / "Particles" / "TranslucencyLab"
LEVEL_DIR    = GAME_DIR / "Content" / "Levels" / "TranslucencyLab"
M            = "Content/Materials/TranslucencyLab/"
P            = "Content/Particles/TranslucencyLab/"
BOX          = "Engine/Content/Meshes/Box.nmmesh"


class Graph(cy.Graph):
    """Courtyard's graph, its node ids from TranslucencyLab's names instead."""
    def node(self, type_, props=None, consts=None, wildcard=None, version=1):
        self.count += 1
        nid = hashlib.md5(f"TranslucencyLab/{self.name}/{self.count}".encode()).hexdigest()
        self.nodes.append((nid, type_, version, props or {}, consts or {}, wildcard,
                           (self.count % 6) * 220 - 600, (self.count // 6) * 160 - 400))
        return nid


def opaque(name, consts):
    g = Graph(name)
    cy.output(g, consts=consts)
    return g


def translucent(name, consts, two_sided=False, surface=True):
    """surface: Translucency Lighting Surface, the specular over the opacity (Phase 3)."""
    g = Graph(name)
    out = cy.output(g, domain="Translucent", two_sided=two_sided, consts=consts)
    if surface:
        next(n for n in g.nodes if n[0] == out)[3]["TranslucencyLighting"] = "Surface"
    return g


def smoke():
    """A soft round puff, its colour and opacity the particle's: Steampunk's
    Steam, Lit (the Volume model from Phase 5 on)."""
    g = Graph("Smoke")
    out = cy.output(g, domain="Translucent", consts={"Roughness": "1"})
    tc = g.node("Input.TexCoord")
    dist = g.node("Vector.Distance", consts={"B": cy.vec(0.5, 0.5)})
    g.link(tc, "UV", dist, "A")
    edge = g.math("Multiply", (dist, "Out"), consts={"B": "2"})
    soft = g.math("OneMinus", (edge, "Out"))
    sat = g.math("Saturate", (soft, "Out"))
    square = g.math("Multiply", (sat, "Out"), (sat, "Out"))
    vc = g.node("Input.VertexColor")
    alpha = g.math("Multiply", (square, "Out"), (vc, "A"))
    g.link(alpha, "Out", out, "Opacity")
    g.link(vc, "RGB", out, "BaseColor")
    return g


MATERIALS = [
    opaque("Floor", {"BaseColor": cy.vec(0.62, 0.62, 0.6), "Roughness": "0.8"}),
    opaque("Wall", {"BaseColor": cy.vec(0.7, 0.7, 0.7), "Roughness": "0.9"}),
    opaque("Backdrop", {"BaseColor": cy.vec(0.06, 0.06, 0.07), "Roughness": "0.9"}),
    opaque("Red", {"BaseColor": cy.vec(0.75, 0.08, 0.06), "Roughness": "0.6"}),
    translucent("Glass", {"BaseColor": cy.vec(0.9, 0.95, 0.95), "Roughness": "0.05", "Opacity": "0.12"}, two_sided=True),
    translucent("Water", {"BaseColor": cy.vec(0.05, 0.12, 0.11), "Roughness": "0.03", "Opacity": "0.45"}),
    smoke(),
]


# --- The particle systems ----------------------------------------------------

def emitter_block(text, name):
    """One emitter's lines (its header included) out of a .nmparticle."""
    blocks = re.split(r"(?m)^(?=Emitter )", text)
    for block in blocks:
        if block.startswith(f"Emitter {name}"):
            return block.rstrip() + "\n"
    raise SystemExit(f"no emitter {name}")


def setting(block, key, value):
    out, n = re.subn(rf"(?m)^(    {key}) .*$", rf"\g<1> {value}", block)
    if n != 1:
        raise SystemExit(f"setting {key}: {n} matches")
    return out


def particles():
    PARTICLE_DIR.mkdir(parents=True, exist_ok=True)
    chimney = (GAME_DIR / "Content" / "Particles" / "Steampunk" / "ChimneySmoke.nmparticle").read_text()
    hearth = (GAME_DIR / "Content" / "Particles" / "Courtyard" / "Hearth.nmparticle").read_text()

    plume = emitter_block(chimney, re.search(r"(?m)^Emitter (\S+)", chimney).group(1))
    plume = re.sub(r"(?m)^Emitter \S+", "Emitter Plume", plume, count=1)
    for key, value in (("Material", M + "Smoke.material.nmgraph"), ("Seed", "17"), ("PauseOffscreen", "false"),
                       ("Lifetime", "9"), ("MaxParticles", "160"),
                       ("Velocity", "0,140,0"), ("CullDistance", "0"), ("EndSize", "380"),
                       ("StartColor", "205,203,198,190"), ("EndColor", "185,183,180,0"), ("SpawnRate", "16"),
                       ("Acceleration", "6,20,3"), ("Drag", "0.1"), ("StartSize", "60")):
        plume = setting(plume, key, value)
    sparks = emitter_block(hearth, "Embers")
    sparks = re.sub(r"(?m)^Emitter \S+", "Emitter Sparks", sparks, count=1)
    for key, value in (("Seed", "23"), ("PauseOffscreen", "false"), ("SpawnRate", "14"), ("Velocity", "0,160,0"),
                       ("BoxSize", "60,2,60")):
        sparks = setting(sparks, key, value)
    (PARTICLE_DIR / "Plume.nmparticle").write_text("NimbleParticleSystem 1\n\n" + plume + "\n" + sparks, newline="\n")


# --- The level ---------------------------------------------------------------

guids = []


def guid(name):
    return hashlib.md5(("TranslucencyLab/" + name).encode()).hexdigest()


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


def spot_light(color, intensity, light_range, inner, outer):
    return f"""  <Component Type="Spot Light" Version="2">
    <Color R="{color[0]}" G="{color[1]}" B="{color[2]}" A="255" />
    <Intensity Value="{intensity}" />
    <Range Value="{light_range}" />
    <InnerConeAngle Value="{inner}" />
    <OuterConeAngle Value="{outer}" />
    <Active Value="true" />
    <CastShadows Value="true" />
    <ShadowTechnique Value="1" />
    <ShadowTint R="20" G="22" B="28" A="255" />
    <ShadowBias Value="0.002" />
    <ShadowNormalBias Value="1.5" />
    <SourceRadius Value="10" />
  </Component>
"""


def particle_system(path, seed):
    return f"""  <Component Type="Particle System" Version="3">
    <System Value="{path}" />
    <Emitting Value="true" />
    <Tint R="255" G="255" B="255" A="255" />
    <SpawnRateScale Value="1" />
    <TimeScale Value="1" />
    <Seed Value="{seed}" />
    <UseBeamTarget Value="false" />
    <BeamTarget X="0" Y="0" Z="0" />
  </Component>
"""


def reflection_probe(radius):
    return f"""  <Component Type="Reflection Probe" Version="1">
    <Radius Value="{radius}" />
    <BoxExtent X="0" Y="0" Z="0" />
    <Intensity Value="1" />
    <Resolution Value="128" />
    <Frequency Value="2" />
    <Interval Value="1" />
    <NearClip Value="5" />
    <FarClip Value="20000" />
    <CaptureShadows Value="true" />
    <Enabled Value="true" />
  </Component>
"""


# Screen-space reflections, thin volumetric fog, TAA and tone mapping; no auto
# exposure, so captures compare frame to frame.
PP_STACK = """<?xml version="1.0" encoding="UTF-8"?>
<NodeGraph Version="1" Schema="PostProcess">
  <Nodes>
    <Node Id="7d2c1f0e8b4a4c6e9f1a2b3c4d5e6f01" Type="Effect.ToneMapping" Version="1" X="-380" Y="-400">
      <Prop Name="Exposure" Value="0" />
      <Prop Name="Tone Mapper" Value="ACES" />
    </Node>
    <Node Id="7d2c1f0e8b4a4c6e9f1a2b3c4d5e6f02" Type="Effect.Antialiasing" Version="1" X="-380" Y="-240">
      <Prop Name="Method" Value="TAA" />
      <Prop Name="Quality" Value="Low" />
    </Node>
    <Node Id="7d2c1f0e8b4a4c6e9f1a2b3c4d5e6f03" Type="Effect.ScreenSpaceReflections" Version="1" X="-160" Y="-400">
      <Prop Name="Intensity" Value="1" />
      <Prop Name="Max Distance" Value="2000" />
      <Prop Name="Max Roughness" Value="0.8" />
      <Prop Name="Steps" Value="32" />
      <Prop Name="Thickness" Value="25" />
    </Node>
    <Node Id="7d2c1f0e8b4a4c6e9f1a2b3c4d5e6f04" Type="Effect.VolumetricFog" Version="1" X="60" Y="-400">
      <Prop Name="Albedo" Value="1 1 1" />
      <Prop Name="Anisotropy" Value="0.6" />
      <Prop Name="Base Height" Value="0" />
      <Prop Name="Density" Value="0.00006" />
      <Prop Name="Height Falloff" Value="0.0008" />
      <Prop Name="Light Scattering" Value="2" />
      <Prop Name="Max Distance" Value="8000" />
      <Prop Name="Shadows" Value="true" />
      <Prop Name="Sky Scattering" Value="0.5" />
      <Prop Name="Steps" Value="24" />
      <Prop Name="Sun Scattering" Value="1.5" />
    </Node>
  </Nodes>
</NodeGraph>
"""
PP_PATH = "Content/PostProcess/TranslucencyLab/TranslucencyLab.pp.nmgraph"


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

    # View 0; the game mode moves it (WorldsLab.TranslucencyLabView).
    entity("Camera", """  <Component Type="Camera" Version="2">
    <ZNear Value="5" />
    <ZFar Value="30000" />
    <Active Value="true" />
    <Projection Value="1" />
    <FovY Value="55" />
  </Component>
""" + transform((400, 650, -2300), (12, 0, 0)))

    # The sun from the camera's side (-Z), high and a little to the left: shadows fall toward +Z.
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
""" + transform((0, 4000, -4000), (40, 15, 0)))

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

    entity("Ground", render(BOX, M + "Floor.material.nmgraph") + transform((400, -10, 400), scale=(6000, 20, 6000)))

    # The glass: three panes at z 0, x -800 to -200, a dark backdrop behind; an overhead
    # slab on the sun's side shadows the left of them.
    for index in range(3):
        entity(f"Pane{index}", render(BOX, M + "Glass.material.nmgraph", shadows=False)
               + transform((-700 + index * 220, 160, 0), scale=(200, 300, 4)))
    entity("Backdrop", render(BOX, M + "Backdrop.material.nmgraph") + transform((-480, 220, 260), scale=(900, 440, 20)))
    # Out of the glass view: the light travels (0.2, -0.64, 0.74), so its shadow lands 1.15 x the height above a
    # surface farther in +Z: on the panes and the ground before them.
    entity("ShadowSlab", render(BOX, M + "Wall.material.nmgraph") + transform((-1000, 560, -640), scale=(700, 20, 500)))

    # The water: a strip on the floor below a red wall.
    entity("Water", render(BOX, M + "Water.material.nmgraph", shadows=False) + transform((500, 3, 0), scale=(700, 4, 600)))
    entity("RedWall", render(BOX, M + "Red.material.nmgraph") + transform((500, 250, 330), scale=(700, 500, 30)))

    # The plume: smoke and sparks rising through a shadowed spot light's cone, a post in
    # the cone for its shadow.
    entity("Plume", particle_system(P + "Plume.nmparticle", 0) + transform((1500, 20, 250)))
    entity("Spot", spot_light((255, 230, 200), 14, 2200, 18, 30) + transform((1500, 900, -300), (58, 0, 0)))
    entity("Post", render(BOX, M + "Wall.material.nmgraph") + transform((1380, 120, 60), scale=(40, 240, 40)))

    for index, x in enumerate((-500, 500, 1500)):
        entity(f"Probe{index}", reflection_probe(900) + transform((x, 250, 0)))
    entity("GIVolume", """  <Component Type="GI Volume" Version="1">
    <Extent X="1500" Y="400" Z="700" />
    <ProbeSpacing Value="200" />
    <Intensity Value="1" />
    <Enabled Value="true" />
  </Component>
""" + transform((400, 400, 200)))
    entity("PostProcess", f"""  <Component Type="Post Process" Version="2">
    <Stack Value="{PP_PATH}" />
    <Priority Value="0" />
    <Enabled Value="true" />
    <BlendDistance Value="0" />
  </Component>
""" + transform((0, 0, 0)))

    with open(GAME_DIR / "Content" / "Levels" / "TranslucencyLab.nmworld", "w", newline="\n") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<World Format="1">\n')
        f.write('  <Settings GameMode="WorldsLabTranslucencyLab" />\n')
        for g in guids:
            f.write(f'    <Entity Guid="{g}" />\n')
        f.write("</World>\n")


def main():
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    for graph in MATERIALS:
        graph.write(MAT_DIR / f"{graph.name}.material.nmgraph")
    pp = GAME_DIR / PP_PATH
    pp.parent.mkdir(parents=True, exist_ok=True)
    if not pp.exists():   # NodeGraphGenerate adds its gen_code; keep it
        pp.write_text(PP_STACK, newline="\n")
    particles()
    level()
    print(f"  {len(MATERIALS)} materials; Content/Levels/TranslucencyLab.nmworld: {len(guids)} entities")


if __name__ == "__main__":
    main()
