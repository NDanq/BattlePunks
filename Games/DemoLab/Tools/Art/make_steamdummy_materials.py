#!/usr/bin/env python3
"""
make_steamdummy_materials.py -- the SteamDummy's three looks, for SteamDummy_PBR.nmmesh:

    Content/Materials/Characters/SteamDummy_Player.material.nmgraph     Default: brass, copper,
                                                                        leather, rubber; amber eyes
    Content/Materials/Characters/SteamDummy_Watchman.material.nmgraph   Clear Coat on the navy
                                                                        enamel livery; red eyes
    Content/Materials/Characters/SteamDummy_Brute.material.nmgraph      Anisotropic riveted steel
                                                                        plate; orange-red eyes

Each is BattlePunks' SteamDummy graph (lib/SteamDummyEyes.template.nmgraph, verbatim: the
CRT eyes -- EyeColor, EyeIntensity, BlinkInterval, BlinkDuration, BlinkOffset, ScanlineCount,
ScanlineSpeed, RollSpeed -- glowing where vertex colour A is 0, over the eyes' own 0-1 UVs)
with its flat vertex-colour surface replaced by make_steamdummy.py's baked textures:
BaseColor (A: the look's mask -- clear coat or anisotropy), Normal, ORM. On the eye screens
(vertex colour A = 0) the surface is dark glass instead, whatever the atlas holds there.
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import graphs                     # noqa: E402
from graphs import vec            # noqa: E402

ART = Path(__file__).resolve().parent
GAME_DIR = ART.parents[1]
TEMPLATE = ART / "lib" / "SteamDummyEyes.template.nmgraph"
OUT = GAME_DIR / "Content" / "Materials" / "Characters"
TEX = "Content/Characters/SteamDummy"

LOOKS = {
    "Player": dict(model="Default", eye=(1.0, 0.42, 0.05)),
    "Watchman": dict(model="ClearCoat", eye=(1.0, 0.07, 0.03)),
    "Brute": dict(model="Anisotropic", eye=(1.0, 0.2, 0.02)),
}


def load_template(g):
    root = ET.parse(TEMPLATE).getroot()
    out_id = colour_id = None
    drop = set()
    for n in root.find("Nodes"):
        nid, type_ = n.get("Id"), n.get("Type")
        props = {p.get("Name"): p.get("Value") for p in n.findall("Prop")}
        consts = {c.get("Pin"): c.get("Value") for c in n.findall("Const")}
        if type_ == "Parameter" and props.get("Name") in ("Roughness", "Metallic"):
            drop.add(nid)
            continue
        if type_ == "Output.Material":
            out_id = nid
        if type_ == "Input.VertexColor":
            colour_id = nid
        g.nodes.append((nid, type_, int(n.get("Version")), props, consts, n.get("Wildcard"),
                        int(n.get("X")), int(n.get("Y"))))
    for l in root.find("Links"):
        src, dst = l.get("From"), l.get("To")
        if src.split(".")[0] in drop or dst in (f"{out_id}.BaseColor", f"{out_id}.Roughness", f"{out_id}.Metallic"):
            continue
        g.links.append((src, dst))
    return out_id, colour_id


def look(name, spec):
    g = graphs.Graph(f"SteamDummy_{name}")
    out, vc = load_template(g)
    # The template's output: set the shading model, and the eye colour's default.
    for i, node in enumerate(g.nodes):
        nid, type_, version, props, consts, wc, x, y = node
        if nid == out:
            props = dict(props, ShadingModel=spec["model"])
        if type_ == "Parameter" and props.get("Name") == "EyeColor":
            props = dict(props, Default=vec(*spec["eye"]))
        g.nodes[i] = (nid, type_, version, props, consts, wc, x, y)
    prefix = f"{TEX}/SteamDummy_{name}"
    base = g.texture(f"{prefix}_BaseColor.nmtexture", None)
    orm = g.texture(f"{prefix}_ORM.nmtexture", None)
    nrm = g.texture(f"{prefix}_Normal.nmtexture", None)
    eye = g.math("OneMinus", (vc, "A"))                       # 1 on the CRT screens
    glass = (g.const(vec(0.015, 0.015, 0.018), "vec3"), "Out")
    g.link(*g.lerp((base, "RGB"), glass, eye, "vec3"), out, "BaseColor")
    g.link(*g.lerp((orm, "G"), None, eye, cb="0.05"), out, "Roughness")
    g.link(*g.lerp((orm, "B"), None, eye, cb="0"), out, "Metallic")
    g.link(*g.lerp((orm, "R"), None, eye, cb="1"), out, "AmbientOcclusion")
    flat = (g.const(vec(0.5, 0.5, 1.0), "vec3"), "Out")
    g.link(*g.normal_map(g.lerp((nrm, "RGB"), flat, eye, "vec3"), None), out, "Normal")
    mask = g.lerp((base, "A"), None, eye, cb="0")
    if spec["model"] == "ClearCoat":
        g.link(*mask, out, "ClearCoat")
        g.link(g.param("CoatRoughness", "float", "0.04"), "Value", out, "ClearCoatRoughness")
    elif spec["model"] == "Anisotropic":
        g.link(*g.mul(mask, (g.param("Anisotropy", "float", "0.7"), "Value")), out, "Anisotropy")
    return g


def main():
    paths = []
    for name, spec in LOOKS.items():
        path = OUT / f"SteamDummy_{name}.material.nmgraph"
        look(name, spec).write(path)
        paths.append(path)
        print(f"  {path.relative_to(GAME_DIR)}")
    return paths


if __name__ == "__main__":
    main()
