#!/usr/bin/env python3
"""
make_testing_materials.py -- the materials RenderCore.Smoke's translucency checks
draw (Engine/Modules/RenderCore/LIT_TRANSLUCENCY.md, "Validation"): engine
content, so the Engine shader cache holds them and the checks need no game.
Writes Engine/Content/Materials/Testing/*.material.nmgraph without their
generated code; then

    <bin>/NodeGraphGenerate Engine/Content/Materials/Testing/*.material.nmgraph

and cook (NimbleCook shaders).
"""

import hashlib
from pathlib import Path

HERE = Path(__file__).resolve().parent


def vec(*values):
    return " ".join(f"{v:g}" for v in values)


def material(name, domain, shading, consts, two_sided=False, props=None):
    nid = hashlib.md5(f"Testing/{name}".encode()).hexdigest()
    pins = "".join(f'      <Const Pin="{pin}" Value="{value}" />\n' for pin, value in sorted(consts.items()))
    # Sorted by name, as the editor and NodeGraphGenerate save them, so an unchanged graph keeps its code.
    every = {"ClipValue": "0.5", "Domain": domain, "MaxOffset": "0", "Shading": shading,
             "TwoSided": "true" if two_sided else "false", **(props or {})}
    named = "".join(f'      <Prop Name="{key}" Value="{value}" />\n' for key, value in sorted(every.items()))
    text = f"""<?xml version="1.0" encoding="UTF-8"?>
<NodeGraph Version="1" Schema="Material">
  <Nodes>
    <Node Id="{nid}" Type="Output.Material" Version="1" X="300" Y="0">
{pins}{named}    </Node>
  </Nodes>
</NodeGraph>
"""
    path = HERE / f"{name}.material.nmgraph"
    if path.exists():
        old = path.read_text()
        at = old.find("  <gen_code>")
        if at >= 0 and old[:at] + "</NodeGraph>\n" == text:
            return
    path.write_text(text, newline="\n")


MATERIALS = [
    # Phase 1: which of two overlapping quads is in front.
    ("TransLitRed", "Translucent", "Lit", {"BaseColor": vec(0.8, 0.05, 0.05), "Roughness": "0.8", "Opacity": "1"}, {}),
    ("TransUnlitGreen", "Translucent", "Unlit", {"Emissive": vec(0.05, 0.8, 0.05), "Opacity": "1"}, {}),
    # Phase 2: a white translucent quad, half in an opaque blocker's shadow.
    ("TransLitWhite", "Translucent", "Lit", {"BaseColor": vec(0.9, 0.9, 0.9), "Roughness": "0.9", "Opacity": "0.5"}, {}),
    # Phase 3: a nearly clear, shiny pane, its specular over its opacity (Surface) or faded by it (Legacy).
    ("TransLitShiny", "Translucent", "Lit", {"Roughness": "0.3", "Opacity": "0.02"}, {"TranslucencyLighting": "Surface"}),
    ("TransLitShinyLegacy", "Translucent", "Lit", {"Roughness": "0.3", "Opacity": "0.02"}, {"TranslucencyLighting": "Legacy"}),
    # Phase 5: a Volume quad, scattering forward and back.
    ("VolumeForward", "Translucent", "Lit", {"BaseColor": vec(0.8, 0.8, 0.8), "Opacity": "0.6", "PhaseAnisotropy": "0.6"},
     {"ShadingModel": "Volume"}),
    ("VolumeBack", "Translucent", "Lit", {"BaseColor": vec(0.8, 0.8, 0.8), "Opacity": "0.6", "PhaseAnisotropy": "-0.6"},
     {"ShadingModel": "Volume"}),
    # Phase 7: particles that draw nothing themselves, only their shadow.
    ("Invisible", "Translucent", "Unlit", {"Opacity": "0"}, {}),
    # Glass casting: a pane whose see-through shadow stops half the light.
    ("GlassCaster", "Translucent", "Lit", {"Roughness": "0.1", "Opacity": "0.2"},
     {"CastTranslucentShadow": "true", "TranslucentShadowOpacity": "0.5"}),
    # Ambient self-occlusion: an even medium, its sky light darkened by the medium toward the sun or not.
    ("VolumeOccluded", "Translucent", "Lit", {"BaseColor": vec(0.8, 0.8, 0.8), "Opacity": "0.6", "SelfOcclusion": "1"},
     {"ShadingModel": "Volume"}),
    ("VolumeOpen", "Translucent", "Lit", {"BaseColor": vec(0.8, 0.8, 0.8), "Opacity": "0.6"}, {"ShadingModel": "Volume"}),
    # Opaque surroundings.
    ("OpaqueWhite", "Opaque", "Lit", {"BaseColor": vec(0.8, 0.8, 0.8), "Roughness": "0.9"}, {}),
    ("OpaqueRed", "Opaque", "Lit", {"BaseColor": vec(0.8, 0.05, 0.05), "Roughness": "0.9"}, {}),
    ("OpaqueBlack", "Opaque", "Lit", {"BaseColor": vec(0.02, 0.02, 0.02), "Roughness": "0.9"}, {}),
]


def main():
    for name, domain, shading, consts, props in MATERIALS:
        material(name, domain, shading, consts, props=props)
    print(f"  {len(MATERIALS)} testing materials")


if __name__ == "__main__":
    main()
