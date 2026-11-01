#!/usr/bin/env python3
"""Writes Games/WorldsLab/Content/Levels/DestructionLab: a floor, a sun and sky,
and things to break -- a concrete wall (structural), a stack of wooden crates,
glass panes and stone pillars (structural) -- and the Player its mode
(WorldsLabDestruction.cpp) flies and shoots from.

Usage: make_destructionlab.py <WorldsLab dir>"""
import hashlib
import os
import sys

root = sys.argv[1]
level_dir = os.path.join(root, "Content", "Levels", "DestructionLab")
os.makedirs(level_dir, exist_ok=True)
for name in os.listdir(level_dir):
    os.remove(os.path.join(level_dir, name))

guids = []


def guid(name):
    return hashlib.md5(("DestructionLab/" + name).encode()).hexdigest()


def transform(pos, rot=(0, 0, 0), scale=(1, 1, 1)):
    return f"""  <Component Type="Transform" Version="1">
    <Position X="{pos[0]}" Y="{pos[1]}" Z="{pos[2]}" />
    <Rotation X="{rot[0]}" Y="{rot[1]}" Z="{rot[2]}" />
    <Scale X="{scale[0]}" Y="{scale[1]}" Z="{scale[2]}" />
  </Component>
"""


def render(mesh, material, tint=None):
    params = '    <ParameterCount Value="0" />\n'
    if tint:
        params = f"""    <ParameterCount Value="1" />
    <Parameter0Name Value="Tint" />
    <Parameter0Slot Value="0" />
    <Parameter0X Value="{tint[0]}" />
    <Parameter0Y Value="{tint[1]}" />
    <Parameter0Z Value="{tint[2]}" />
    <Parameter0W Value="0" />
"""
    slots = "".join(f'    <Material{i} Value="{material if i == 0 else ""}" />\n' for i in range(8))
    return f"""  <Component Type="Render" Version="5">
    <Visible Value="true" />
{slots}    <Mesh Value="{mesh}" />
{params}    <CastShadows Value="true" />
    <CastInsetShadow Value="false" />
    <Occluder Value="false" />
  </Component>
"""


def body(motion, mass=0, friction=0.6):
    return f"""  <Component Type="Rigid Body" Version="1">
    <Enabled Value="true" />
    <Motion Value="{motion}" />
    <Shape Value="0" />
    <ShapeSource Value="0" />
    <Mass Value="{mass}" />
    <Friction Value="{friction}" />
    <Restitution Value="0" />
    <LinearDamping Value="0.05" />
    <AngularDamping Value="0.05" />
    <GravityFactor Value="1" />
    <Continuous Value="false" />
    <Sensor Value="false" />
    <AllowSleeping Value="true" />
    <Layer Value="1" />
    <Mask Value="-1" />
    <InitialLinearVelocity X="0" Y="0" Z="0" />
    <InitialAngularVelocity X="0" Y="0" Z="0" />
  </Component>
"""


def destructible(material, structural=False, pieces=0, seed=0):
    return f"""  <Component Type="Destructible" Version="1">
    <Enabled Value="true" />
    <Mode Value="0" />
    <Structural Value="{'true' if structural else 'false'}" />
    <Anchored Value="true" />
    <Material Value="{material}" />
    <Fracture Value="" />
    <Pieces Value="{pieces}" />
    <Seed Value="{seed}" />
    <Strength Value="0" />
    <Health Value="0" />
    <DebrisLifetime Value="0" />
  </Component>
"""


def entity(name, body_xml, mobility=None):
    g = guid(name)
    guids.append(g)
    mob = f' Mobility="{mobility}"' if mobility else ""
    with open(os.path.join(level_dir, g + ".nmentity"), "w", newline="\n") as f:
        f.write(f'<?xml version="1.0" encoding="UTF-8"?>\n<Entity Guid="{g}" Name="{name}"{mob}>\n{body_xml}</Entity>\n')


BOX = "Engine/Content/Meshes/Box.nmmesh"
M = "Content/Materials/"

entity("Player", """  <Component Type="Camera" Version="2">
    <ZNear Value="5" />
    <ZFar Value="20000" />
    <Active Value="true" />
    <Projection Value="1" />
    <FovY Value="65" />
  </Component>
  <Component Type="Input" Version="1">
    <Map Value="Destruction" />
    <PlayerIndex Value="0" />
    <Enabled Value="true" />
  </Component>
""" + transform((80, 330, -980), (12, 0, 0)))

entity("Sun", """  <Component Type="Directional Light" Version="3">
    <Color R="255" G="244" B="226" A="255" />
    <Intensity Value="9" />
    <Active Value="true" />
    <CastShadows Value="true" />
    <ShadowTechnique Value="2" />
    <ShadowTint R="25" G="28" B="36" A="255" />
    <ShadowBias Value="0.0012" />
    <ShadowNormalBias Value="2" />
    <ShadowDistance Value="5000" />
    <CascadeCount Value="4" />
    <CascadeSplitLambda Value="0.75" />
    <SourceAngle Value="0.5" />
  </Component>
""" + transform((0, 6000, 0), (50, 160, 0)), "Static")

entity("Sky", """  <Component Type="Sky Light" Version="2">
    <SkyColor R="150" G="165" B="190" A="255" />
    <GroundColor R="60" G="56" B="50" A="255" />
    <Intensity Value="0.35" />
    <Active Value="true" />
    <Environment Value="Content/Textures/ShootingRange/Sky_HDR.nmtexture" />
    <EnvironmentIntensity Value="0.5" />
    <EnvironmentRotation Value="0" />
    <DrawBackground Value="true" />
  </Component>
""" + transform((0, 0, 0)), "Static")

entity("Floor", render(BOX, M + "ShootingRange/FloorPaint.material.nmgraph") + body(2) + transform((0, -10, 0), scale=(5000, 20, 5000)), "Static")

# A concrete wall, structural: hits break it around them.
entity("ConcreteWall", render(BOX, M + "ShootingRange/Concrete.material.nmgraph") + body(2) + destructible("Concrete", True, 48, 11)
       + transform((-520, 150, 320), scale=(520, 300, 40)))

# A stack of plywood crates.
crates = [(-20, 40, 60), (70, 40, 60), (25, 120, 60), (-20, 40, 150), (70, 40, 150), (25, 200, 60)]
for i, p in enumerate(crates):
    entity(f"Crate{i + 1}", render(BOX, M + "ShootingRange/Plywood.material.nmgraph") + body(0, 30) + destructible("Wood", seed=100 + i)
           + transform(p, (0, (i * 7) % 15, 0), (80, 80, 80)))

# Two glass panes on frames.
for i, x in enumerate((330, 560)):
    entity(f"GlassPane{i + 1}", render(BOX, M + "ShootingRange/Glass.material.nmgraph") + body(2) + destructible("Glass", seed=200 + i)
           + transform((x, 120, 260), scale=(200, 220, 6)))
    entity(f"PaneBase{i + 1}", render(BOX, M + "ShootingRange/Steel.material.nmgraph") + body(2)
           + transform((x, 5, 260), scale=(220, 10, 30)), "Static")

# Stone pillars, structural.
for i, x in enumerate((820, 980)):
    entity(f"StonePillar{i + 1}", render(BOX, M + "Shared/Stone.material.nmgraph") + body(2) + destructible("Stone", True, 20, 300 + i)
           + transform((x, 160, 420), scale=(80, 320, 80)))

with open(os.path.join(root, "Content", "Levels", "DestructionLab.nmworld"), "w", newline="\n") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n<World Format="1">\n  <Settings GameMode="WorldsLabDestruction" />\n')
    for g in guids:
        f.write(f'    <Entity Guid="{g}" />\n')
    f.write("</World>\n")
print(f"{len(guids)} entities")
