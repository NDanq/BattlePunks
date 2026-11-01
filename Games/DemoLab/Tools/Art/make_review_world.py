#!/usr/bin/env python3
"""
make_review_world.py -- scratch worlds for checking the art in the engine: the SteamDummy
looks beside the shading-model samples, and a line-up of kit hero pieces, under a low sun.
Written to a folder you choose (not the project's Levels: these are throwaway), e.g. a
staged editor's Content/Levels:

    python Games/DemoLab/Tools/Art/make_review_world.py <Content/Levels folder>
    NimbleEditor --rhi 2 <folder>/ArtReview_Characters.nmworld --command "after 8000 windowshot <png>"

Every asset comes from assets.json, so parts sit at their manifest offsets.
"""

import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import nmformats as nm      # noqa: E402

ART = Path(__file__).resolve().parent
ASSETS = {a["name"]: a for a in json.loads((ART / "assets.json").read_text())["assets"]}


def render(mesh, material=""):
    slots = "".join(f'    <Material{i} Value="{material if i == 0 else ""}" />\n' for i in range(8))
    return ("  <Component Type=\"Render\" Version=\"5\">\n    <Visible Value=\"true\" />\n" + slots +
            f"    <Mesh Value=\"{mesh}\" />\n    <ParameterCount Value=\"0\" />\n    <CastShadows Value=\"true\" />\n"
            "    <CastInsetShadow Value=\"false\" />\n    <Occluder Value=\"false\" />\n  </Component>\n")


class World:
    def __init__(self, name, out):
        self.name, self.out = name, Path(out)
        self.dir = self.out / name
        self.dir.mkdir(parents=True, exist_ok=True)
        for old in self.dir.glob("*.nmentity"):
            old.unlink()
        self.guids = []

    def entity(self, name, body, mobility=None):
        g = hashlib.md5(f"{self.name}/{name}/{len(self.guids)}".encode()).hexdigest()
        self.guids.append(g)
        mob = f' Mobility="{mobility}"' if mobility else ""
        (self.dir / f"{g}.nmentity").write_text(
            f'<?xml version="1.0" encoding="UTF-8"?>\n<Entity Guid="{g}" Name="{name}"{mob}>\n{body}</Entity>\n')

    def asset(self, name, pos, yaw=0.0, material="", scale=1.0):
        a = ASSETS[name]
        self.entity(name, render(a["mesh"], material) + nm.transform_xml(pos, (0, yaw, 0), (scale,) * 3))
        import math
        c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        for p in a.get("parts", []):
            ox, oy, oz = (v * scale for v in p["offset_cm"])
            # Yaw about Y (left-handed: +yaw turns +Z toward +X).
            rx, rz = ox * c + oz * s, -ox * s + oz * c
            self.entity(f"{name}.{p['name']}", render(p["mesh"]) +
                        nm.transform_xml((pos[0] + rx, pos[1] + oy, pos[2] + rz), (0, yaw, 0), (scale,) * 3))

    def lights(self, camera_pos, camera_rot, sun_rot=(28, 140, 0)):
        self.entity("Camera", "  <Component Type=\"Camera\" Version=\"2\">\n    <ZNear Value=\"5\" />\n"
                    "    <ZFar Value=\"50000\" />\n    <Active Value=\"true\" />\n    <Projection Value=\"1\" />\n"
                    "    <FovY Value=\"45\" />\n  </Component>\n" + nm.transform_xml(camera_pos, camera_rot))
        self.entity("Sun", """  <Component Type="Directional Light" Version="3">
    <Color R="255" G="236" B="205" A="255" />
    <Intensity Value="5" />
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
""" + nm.transform_xml((0, 4000, 0), sun_rot))
        self.entity("Sky", """  <Component Type="Sky Light" Version="2">
    <SkyColor R="150" G="168" B="195" A="255" />
    <GroundColor R="70" G="64" B="56" A="255" />
    <Intensity Value="0.5" />
    <Active Value="true" />
    <Environment Value="" />
    <EnvironmentIntensity Value="0.6" />
    <EnvironmentRotation Value="0" />
    <DrawBackground Value="true" />
  </Component>
""" + nm.transform_xml((0, 0, 0)), "Static")
        self.entity("Ground", render("Engine/Content/Meshes/Box.nmmesh", "Content/Materials/Kit/Concrete.material.nmgraph")
                    + nm.transform_xml((0, -10, 0), scale=(6000, 20, 6000)), "Static")

    def save(self):
        with open(self.out / f"{self.name}.nmworld", "w", newline="\n") as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?>\n<World Format="1">\n')
            for g in self.guids:
                f.write(f'    <Entity Guid="{g}" />\n')
            f.write("</World>\n")
        print(f"{self.out / self.name}.nmworld: {len(self.guids)} entities")


def main(out):
    w = World("ArtReview_Characters", out)
    w.lights((0, 190, -620), (8, 0, 0))
    for i, look in enumerate(("Player", "Watchman", "Brute")):
        w.asset(f"SteamDummy_{look}", ((i - 1) * 190, 0, 60), 0,
                f"Content/Materials/Characters/SteamDummy_{look}.material.nmgraph", 1.25 if look == "Brute" else 1.0)
    samples = ["BrushedBrass", "Walnut", "Enamel", "Velvet", "Leather", "Marble", "Wax", "LeafPanel", "FrostedGlass"]
    for i, s in enumerate(samples):
        x = (i - 4) * 75
        w.asset("SamplePedestal", (x, 0, -170))
        w.asset(f"Sample_{s}", (x, 92, -170))
    w.save()
    w = World("ArtReview_Kit", out)
    w.lights((0, 900, -3200), (10, 0, 0))
    w.asset("Facade_Wall", (-1100, 0, 600))
    w.asset("Facade_Arch", (-300, 0, 600))
    w.asset("BlastDoor", (-300, 0, 600))
    w.asset("Gatehouse", (900, 0, 300), 0)
    w.asset("RoadLamp", (300, 0, -200))
    w.asset("Rock_C", (500, 0, -600), 30)
    w.asset("Rock_B", (-700, 0, -500), 70)
    w.asset("Orrery", (-1300, 0, -400))
    w.asset("Turbine", (1500, 0, -700), 20)
    w.asset("ReactorCore", (2600, 0, 900))
    w.asset("Locker", (-500, 0, -900))
    w.asset("Workbench", (0, 0, -1000))
    w.save()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(ART.parents[1] / "Saved" / "ArtCache" / "Levels"))
