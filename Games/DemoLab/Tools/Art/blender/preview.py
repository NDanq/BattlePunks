"""
preview.py -- contact sheets of what the generators wrote: every .nmmesh in a manifest read
back (so the sheets check the exported files, not the Blender scene), drawn with Principled
BSDF stand-ins for its materials over the same PNG textures (matdefs.py), rendered with
Cycles, and laid out in a labelled grid.

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/preview.py -- \
        <kit> [<out png>] [--size 320] [--samples 24]

<kit> names Saved/ArtCache/manifest_<kit>.json (or "all": every manifest in assets.json).
Sheets go to Saved/ArtCache/Sheets/<kit>.png unless an output is given.
"""

import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

ART = Path(__file__).resolve().parents[1]
GAME = ART.parents[1]
sys.path.insert(0, str(ART / "lib"))
import nmformats as nm      # noqa: E402
from matdefs import MATS    # noqa: E402

CACHE = GAME / "Saved" / "ArtCache"
TEX = CACHE / "Textures"


def load_mesh(path, name):
    m = nm.read_mesh(path)
    P = m["positions"][:, [0, 2, 1]] / 100.0
    N = m["normals"][:, [0, 2, 1]]
    idx = m["indices"].reshape(-1, 3)[:, [0, 2, 1]]
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(P))
    me.vertices.foreach_set("co", P.astype(np.float32).ravel())
    nt = len(idx)
    me.loops.add(nt * 3)
    me.loops.foreach_set("vertex_index", idx.astype(np.int32).ravel())
    me.polygons.add(nt)
    me.polygons.foreach_set("loop_start", np.arange(0, nt * 3, 3, dtype=np.int32))
    me.polygons.foreach_set("loop_total", np.full(nt, 3, np.int32))
    uv = m["uv0"].copy()
    uv[:, 1] = 1.0 - uv[:, 1]
    lay = me.uv_layers.new(name="UVMap")
    lay.data.foreach_set("uv", uv[idx.ravel()].astype(np.float32).ravel())
    if "colours" in m:
        ca = me.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
        ca.data.foreach_set("color", m["colours"][idx.ravel()].astype(np.float32).ravel())
    # Materials per section.
    mats = []
    tri_mat = np.zeros(nt, np.int32)
    for (sname, mpath, first, count) in m["sections"]:
        mname = Path(mpath).name.split(".")[0]
        if mname not in mats:
            mats.append(mname)
        tri_mat[first // 3:(first + count) // 3] = mats.index(mname)
    me.polygons.foreach_set("material_index", tri_mat)
    me.update()
    me.normals_split_custom_set_from_vertices(N.astype(np.float32))
    for mname in mats:
        me.materials.append(preview_material(mname))
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def _img(name, colour=True):
    path = TEX / f"{name}.png"
    if not path.exists():
        return None
    img = bpy.data.images.load(str(path), check_existing=True)
    if not colour:
        img.colorspace_settings.name = "Non-Color"
    return img


def preview_material(name):
    m = bpy.data.materials.get("pv_" + name)
    if m:
        return m
    if name.startswith("SteamDummy_"):
        d = {"name": name, "set": name, "unit": True}
    else:
        d = MATS.get(name, {"name": name})
    m = bpy.data.materials.new("pv_" + name)
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    links = nt.links
    vc = nt.nodes.new("ShaderNodeVertexColor")
    vc.layer_name = "Col"
    grime = nt.nodes.new("ShaderNodeMix")
    grime.data_type = "RGBA"
    grime.blend_type = "MULTIPLY"
    grime.inputs["Factor"].default_value = 0.6
    links.new(vc.outputs["Color"], grime.inputs[7])
    base_out = None
    if "set" in d or "tex" in d:
        tc = nt.nodes.new("ShaderNodeTexCoord")
        mp = nt.nodes.new("ShaderNodeMapping")
        s = 1.0 if d.get("unit") else 1.0 / d["metres"]
        mp.inputs["Scale"].default_value = (s, s, 1)
        links.new(tc.outputs["UV"], mp.inputs["Vector"])
        def sample(img):
            t = nt.nodes.new("ShaderNodeTexImage")
            t.image = img
            links.new(mp.outputs["Vector"], t.inputs["Vector"])
            return t
        if "set" in d:
            bc = sample(_img(f"{d['set']}_BaseColor"))
            orm_img = _img(f"{d['set']}_ORM", False)
            nrm_img = _img(f"{d['set']}_Normal", False)
            col = bc.outputs["Color"]
            tint = d.get("paint", d.get("tint", d.get("base") if d.get("domain") == "Translucent" else None))
            if tint is not None:
                mul = nt.nodes.new("ShaderNodeMix")
                mul.data_type = "RGBA"
                mul.blend_type = "MULTIPLY"
                mul.inputs["Factor"].default_value = 1.0
                links.new(col, mul.inputs[6])
                mul.inputs[7].default_value = (*tint, 1)
                col = mul.outputs[2]
            base_out = col
            if orm_img:
                orm = sample(orm_img)
                sep = nt.nodes.new("ShaderNodeSeparateColor")
                links.new(orm.outputs["Color"], sep.inputs["Color"])
                r = sep.outputs["Green"]
                if "rough_mul" in d or "rough_add" in d:
                    mm = nt.nodes.new("ShaderNodeMath")
                    mm.operation = "MULTIPLY_ADD"
                    links.new(r, mm.inputs[0])
                    mm.inputs[1].default_value = d.get("rough_mul", 1.0)
                    mm.inputs[2].default_value = d.get("rough_add", 0.0)
                    r = mm.outputs[0]
                links.new(r, bsdf.inputs["Roughness"])
                if "metal" in d:
                    bsdf.inputs["Metallic"].default_value = d["metal"]
                elif "paint" in d:
                    inv = nt.nodes.new("ShaderNodeMath")
                    inv.operation = "SUBTRACT"
                    inv.inputs[0].default_value = 1.0
                    links.new(bc.outputs["Alpha"], inv.inputs[1])
                    links.new(inv.outputs[0], bsdf.inputs["Metallic"])
                else:
                    links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
            if nrm_img:
                nn = sample(nrm_img)
                nmap = nt.nodes.new("ShaderNodeNormalMap")
                links.new(nn.outputs["Color"], nmap.inputs["Color"])
                links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
            if d.get("domain") == "Masked":
                links.new(bc.outputs["Alpha"], bsdf.inputs["Alpha"])
        else:
            bc = sample(_img(d["tex"]))
            base_out = bc.outputs["Color"]
            bsdf.inputs["Roughness"].default_value = d.get("rough", 0.5)
            if d.get("domain") == "Masked":
                links.new(bc.outputs["Alpha"], bsdf.inputs["Alpha"])
    else:
        bsdf.inputs["Roughness"].default_value = d.get("rough", 0.5)
        bsdf.inputs["Metallic"].default_value = d.get("metal", 0.0)
        grime.inputs[6].default_value = (*d.get("base", (0.5, 0.5, 0.5)), 1)
    if base_out is not None:
        links.new(base_out, grime.inputs[6])
    links.new(grime.outputs[2], bsdf.inputs["Base Color"])
    model = d.get("model", "Default")
    if model == "ClearCoat":
        bsdf.inputs["Coat Weight"].default_value = d.get("clearcoat", 1.0)
        bsdf.inputs["Coat Roughness"].default_value = d.get("coat_rough", 0.05)
    elif model == "Anisotropic":
        bsdf.inputs["Anisotropic"].default_value = d.get("anisotropy", 0.7)
    elif model == "Cloth":
        bsdf.inputs["Sheen Weight"].default_value = 1.0
        bsdf.inputs["Sheen Tint"].default_value = (*d.get("sheen", (0.5, 0.5, 0.5)), 1)
    elif model == "Subsurface":
        bsdf.inputs["Subsurface Weight"].default_value = 1.0 - d.get("thickness", 0.5) * 0.6
        bsdf.inputs["Subsurface Radius"].default_value = d.get("sss", (1, 0.5, 0.25))
        bsdf.inputs["Subsurface Scale"].default_value = 0.02
    if d.get("domain") == "Translucent":
        bsdf.inputs["Transmission Weight"].default_value = 1.0 - d.get("opacity", 0.3)
        bsdf.inputs["IOR"].default_value = d.get("refraction", 1.45) if d.get("refraction", 1) > 1.01 else 1.05
    if "emissive" in d:
        (ec, ei) = d["emissive"]
        bsdf.inputs["Emission Color"].default_value = (*ec, 1)
        bsdf.inputs["Emission Strength"].default_value = min(ei, 6.0) * (0.4 if "tex" in d else 1.0)
    return m


def setup_scene(size, samples):
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.samples = samples
    sc.cycles.use_denoising = True
    sc.cycles.device = "CPU"
    sc.render.threads_mode = "FIXED"
    sc.render.threads = 4
    sc.render.resolution_x = size
    sc.render.resolution_y = size
    sc.render.film_transparent = False
    sc.view_settings.view_transform = "AgX"
    world = bpy.data.worlds.new("W")
    sc.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    sky = world.node_tree.nodes.new("ShaderNodeTexSky")
    try:
        sky.sky_type = "HOSEK_WILKIE"
    except Exception:
        pass
    world.node_tree.links.new(sky.outputs["Color"], bg.inputs["Color"])
    bg.inputs["Strength"].default_value = 0.35
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 3.5
    sun.data.angle = math.radians(3)
    sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(-35))
    sc.collection.objects.link(sun)
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    ground = bpy.data.objects.new("Ground", bpy.data.meshes.new("Ground"))
    import bmesh
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=500)
    bm.to_mesh(ground.data)
    gm = bpy.data.materials.new("GroundMat")
    gm.use_nodes = True
    gm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.32, 0.31, 0.3, 1)
    gm.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.8
    ground.data.materials.append(gm)
    sc.collection.objects.link(ground)
    return sc, cam, ground


def frame(cam, obs, yaw=-35.0, pitch=18.0, lens=50):
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for ob in obs:
        for c in ob.bound_box:
            w = ob.matrix_world @ Vector(c)
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
    centre = (lo + hi) / 2
    radius = max((hi - lo).length / 2, 0.05)
    cam.data.lens = lens
    dist = radius / math.tan(math.atan(18 / lens)) * 1.08
    y, p = math.radians(yaw), math.radians(pitch)
    d = Vector((math.sin(y) * math.cos(p), -math.cos(y) * math.cos(p), math.sin(p)))
    cam.location = centre + d * dist
    cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    cam.data.clip_start = dist * 0.01
    cam.data.clip_end = dist * 10
    return lo, hi


def render_records(records, out_png, size=320, samples=24, cols=5, title=""):
    from PIL import Image, ImageDraw
    sc, cam, ground = setup_scene(size, samples)
    tiles = []
    tmp = CACHE / "Sheets" / "tiles" / Path(out_png).stem     # per sheet: runs in parallel do not mix
    tmp.mkdir(parents=True, exist_ok=True)
    for i, rec in enumerate(records):
        obs = []
        for entry in [rec] + rec.get("parts", []):
            ob = load_mesh(GAME / entry["mesh"], entry["name"])
            if "preview_material" in entry:
                ob.data.materials.clear()
                ob.data.materials.append(preview_material(entry["preview_material"]))
            off = entry.get("offset_cm", [0, 0, 0])
            ob.location = (off[0] / 100.0, off[2] / 100.0, off[1] / 100.0)
            obs.append(ob)
        bpy.context.view_layer.update()      # the parts' matrix_world, before framing on their bounds
        lo, hi = frame(cam, obs, yaw=rec.get("view_yaw", -35.0))
        ground.location = (0, 0, lo.z - 0.001)
        sc.render.filepath = str(tmp / f"{i:03d}.png")
        bpy.ops.render.render(write_still=True)
        tiles.append((rec["name"], rec["size_m"], sc.render.filepath))
        for ob in obs:
            me = ob.data
            bpy.data.objects.remove(ob)
            bpy.data.meshes.remove(me)
    rows = (len(tiles) + cols - 1) // cols
    head = 30 if title else 0
    sheet = Image.new("RGB", (cols * size, head + rows * (size + 18)), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    if title:
        dr.text((8, 8), title, fill=(230, 230, 230))
    for i, (name, sz, path) in enumerate(tiles):
        x, y = (i % cols) * size, head + (i // cols) * (size + 18)
        sheet.paste(Image.open(path).convert("RGB"), (x, y + 18))
        dr.text((x + 4, y + 3), f"{name}  {sz[0]:.2f} x {sz[1]:.2f} x {sz[2]:.2f} m", fill=(230, 230, 230))
    Path(out_png).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_png)
    print(f"sheet: {out_png} ({len(tiles)} assets)")


def main(argv):
    kit = argv[0]
    out = None
    size, samples, only, cols = 320, 24, None, 5
    rest = argv[1:]
    i = 0
    while i < len(rest):
        if rest[i] == "--size":
            size = int(rest[i + 1]); i += 2
        elif rest[i] == "--samples":
            samples = int(rest[i + 1]); i += 2
        elif rest[i] == "--only":
            only = rest[i + 1].split(","); i += 2
        elif rest[i] == "--cols":
            cols = int(rest[i + 1]); i += 2
        else:
            out = rest[i]; i += 1
    if kit == "all":
        records = json.loads((ART / "assets.json").read_text())["assets"]
    else:
        records = json.loads((CACHE / f"manifest_{kit}.json").read_text())
    if only:
        records = [r for r in records if r["name"] in only]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    render_records(records, out or str(CACHE / "Sheets" / f"{kit}.png"), size, samples, cols=cols,
                   title=f"Demo Lab kit: {kit}")


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
