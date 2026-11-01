#!/usr/bin/env python3
"""
make_l1_art.py -- L1 Foothills' own art (everything named L1_), beside the shared kit:

    Content/Materials/Kit/L1_Grade.material.nmgraph   PostProcess: warm golden-hour grade, contrast,
                                                      saturation, vignette
    Content/PostProcess/L1_Foothills.pp.nmgraph       tone map, auto exposure, bloom, SSAO, SSR, SSGI,
                                                      volumetric fog, TAA, the grade
    Content/PostProcess/L1_Menu.pp.nmgraph            the same for the menu backdrop (dusk: more fog, warmer)
    Content/Meshes/L1/L1_GuardRail_Z.nmmesh           the kit's GuardRail and FencePost turned to run along +Z,
    Content/Meshes/L1/L1_FencePost_Z.nmmesh           as a Spline Path spawner's Align to Path wants them;
    Content/Meshes/L1/L1_RoadLamp_X.nmmesh            the RoadLamp with its lantern hanging toward -X (the road)

and the L1 foliage (l1_foliage/: spruces, grass, wildflowers). The sea and the steam are the kit's
(make_materials.py, make_particles.py). Then fill the graphs' <gen_code>:

    python Games/DemoLab/Tools/Art/make_l1_art.py [--bin <dir with NodeGraphGenerate>]
"""

import argparse
import glob
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
import graphs as gr        # noqa: E402
import nmformats as nm     # noqa: E402

GAME_DIR = HERE.parents[1]
REPO = GAME_DIR.parents[1]
KIT_MAT = GAME_DIR / "Content" / "Materials" / "Kit"


# --- post process ---------------------------------------------------------------------------

def grade_material(name, tint, contrast, saturation, vignette):
    """A PostProcess grade over the scene: tint, contrast about mid grey, saturation, vignette."""
    g = gr.Graph(name)
    out = g.node("Output.Material", props={"ClipValue": "0.5", "Domain": "PostProcess", "MaxOffset": "0",
                                           "Shading": "Unlit", "TwoSided": "false"})
    scene = (g.node("Input.SceneTexture"), "Color")
    luma = g.node("Vector.Dot", consts={"B": "0.2126 0.7152 0.0722 0"}, wildcard="vec4")
    g.link(*scene, luma, "A")
    grey = g.node("Vector.Make4")
    for pin in "XYZW":
        g.link(luma, "Out", grey, pin)
    sat = g.lerp((grey, "Out"), scene, (g.param("Saturation", "float", gr.vec(saturation)), "Value"), "vec4")
    tinted = g.mul(sat, (g.param("Tint", "vec4", tint + " 1"), "Value"), "vec4")
    centred = g.math("Subtract", tinted, consts={"B": "0.18 0.18 0.18 0"}, wildcard="vec4")
    con = g.mul(centred, (g.param("Contrast", "float", gr.vec(contrast)), "Value"), "vec4")
    back = g.math("Max", g.math("Add", con, consts={"B": "0.18 0.18 0.18 0"}, wildcard="vec4"),
                  consts={"B": "0 0 0 0"}, wildcard="vec4")
    dist = g.node("Vector.Distance", consts={"B": "0.5 0.5"}, wildcard="vec2")
    g.link(g.node("Input.TexCoord"), "UV", dist, "A")
    edge = g.mul(g.smooth((dist, "Out"), 0.35, 0.9), (g.param("Vignette", "float", gr.vec(vignette)), "Value"))
    keep = g.math("OneMinus", edge)
    final = g.mul(back, keep, "vec4")
    g.link(*final, out, "Emissive")
    return g


def stack(name, nodes):
    """A post-process stack (.pp.nmgraph): nodes = [(type, {prop: value})]."""
    g = gr.Graph(name, schema="PostProcess")
    for type_, props in nodes:
        g.node(type_, props={k: str(v) for k, v in props.items()})
    return g


def stacks():
    common = [
        ("Effect.ToneMapping", {"Exposure": 0.3, "Tone Mapper": "ACES"}),
        ("Effect.AutoExposure", {"Key": 0.14, "Min EV": -2, "Max EV": 3, "Speed Up": 2.5, "Speed Down": 1,
                                 "Centre Weight": 0.5}),
        ("Effect.AmbientOcclusion", {"Intensity": 1.6, "Radius": 80, "Samples": 12, "Power": 1.4, "Blur": "true"}),
        ("Effect.ScreenSpaceReflections", {"Intensity": 1, "Max Roughness": 0.45, "Max Distance": 6000,
                                           "Thickness": 60, "Steps": 32}),
        ("Effect.ScreenSpaceGI", {"Intensity": 1, "Rays": 4, "Radius": 400}),
        ("Effect.Antialiasing", {"Method": "TAA", "Quality": "Medium", "Sharpness": 0.35, "Edge Threshold": 0.125}),
    ]
    l1 = stack("L1_Foothills", common + [
        ("Effect.Bloom", {"Threshold": 0.85, "Intensity": 0.5, "Radius": 0.7, "Levels": 5, "Tint": "1 0.92 0.8"}),
        ("Effect.VolumetricFog", {"Density": 0.00006, "Height Falloff": 0.0009, "Base Height": -100,
                                  "Max Distance": 16000, "Anisotropy": 0.72, "Sun Scattering": 2.2,
                                  "Sky Scattering": 0.7, "Light Scattering": 1.5, "Albedo": "1 0.95 0.88",
                                  "Steps": 24, "Shadows": "true"}),
        ("Effect.Material", {"Material": "Content/Materials/Kit/L1_Grade.material.nmgraph", "Stage": "After Effects",
                             "Order": 0}),
    ])
    menu = stack("L1_Menu", common + [
        ("Effect.Bloom", {"Threshold": 0.75, "Intensity": 0.7, "Radius": 0.75, "Levels": 5, "Tint": "1 0.9 0.75"}),
        ("Effect.VolumetricFog", {"Density": 0.00012, "Height Falloff": 0.0012, "Base Height": 0,
                                  "Max Distance": 9000, "Anisotropy": 0.7, "Sun Scattering": 2.0,
                                  "Sky Scattering": 0.8, "Light Scattering": 2.5, "Albedo": "1 0.94 0.86",
                                  "Steps": 24, "Shadows": "true"}),
        ("Effect.Material", {"Material": "Content/Materials/Kit/L1_Grade.material.nmgraph", "Stage": "After Effects",
                             "Order": 0}),
    ])
    return l1, menu


def turned_meshes():
    """GuardRail and FencePost run along X; a Spline Path spawner aligns a mesh's +Z with the road. Writes
    copies turned 90 degrees (X -> Z) so they line up; rerun after the kit meshes change."""
    out = []
    for src, dst, sign in (("GuardRail", "L1_GuardRail_Z", 1), ("FencePost", "L1_FencePost_Z", 1),
                           ("RoadLamp", "L1_RoadLamp_X", -1)):
        m = nm.read_mesh(GAME_DIR / f"Content/Meshes/L1/{src}.nmmesh")
        out.append(nm_turn(m, GAME_DIR / f"Content/Meshes/L1/{dst}.nmmesh", sign))
    return out


def nm_turn(mesh, path, sign):
    """Turn a read mesh a quarter about Y and write it: sign 1, (x, y, z) -> (-z, y, x) (+X becomes +Z);
    -1, (x, y, z) -> (z, y, -x) (the RoadLamp's lantern, toward -Z, hangs toward -X: over the road when
    the lamp stands on the road's right)."""
    def turn(a):
        a = np.array(a, dtype=np.float64).reshape(-1, 3)
        return np.stack([-sign * a[:, 2], a[:, 1], sign * a[:, 0]], axis=1)
    m = {k: mesh[k] for k in ("positions", "normals", "uv0", "indices", "sections", "colours", "uv1", "tangents")
         if k in mesh}
    for key in ("positions", "normals"):
        m[key] = turn(m[key])
    t = np.array(m["tangents"], dtype=np.float64).reshape(-1, 4)
    t[:, :3] = turn(t[:, :3])
    m["tangents"] = t
    nm.write_mesh_arrays(path, **m)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=None, help="a directory with NodeGraphGenerate")
    ap.add_argument("--no-foliage", action="store_true")
    args = ap.parse_args()

    if not args.no_foliage:
        for script in ("make_l1_foliage_textures.py", "make_l1_foliage_meshes.py", "make_l1_foliage_materials.py"):
            subprocess.run([sys.executable, str(HERE / "l1_foliage" / script)], check=True)
    graphs = []
    for g in (grade_material("L1_Grade", "1.04 1.0 0.93", 1.08, 1.05, 0.45),):
        path = KIT_MAT / f"{g.name}.material.nmgraph"
        g.write(path)
        graphs.append(path)
    for g in stacks():
        path = GAME_DIR / "Content" / "PostProcess" / f"{g.name}.pp.nmgraph"
        g.write(path)
        graphs.append(path)
    graphs += [Path(p) for p in sorted(glob.glob(str(GAME_DIR / "Content/Materials/Foliage/L1_*.material.nmgraph")))]
    for p in turned_meshes():
        print(f"  {Path(p).relative_to(GAME_DIR)}")

    gen = None
    dirs = [Path(args.bin)] if args.bin else sorted(Path(p) for p in glob.glob(str(GAME_DIR / "Binaries" / "*")))
    for d in dirs:
        if (d / "NodeGraphGenerate").exists():
            gen = d / "NodeGraphGenerate"
            break
    if gen is None:
        print("  NodeGraphGenerate not found: fill <gen_code> later (build_all.py generate)")
        return
    subprocess.run([str(gen)] + [str(p) for p in graphs], check=True, cwd=REPO)
    for p in graphs:
        print(f"  {p.relative_to(GAME_DIR)}")


if __name__ == "__main__":
    main()
