"""
make_steamdummy.py -- the SteamDummy's PBR textures and the mesh they fit.

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_steamdummy.py [-- --size 2048]

Source: Content/Characters/SteamDummy/SteamDummy.glb (BattlePunks fad1c7d6, byte-identical),
skinned to CrashDummy's rig. Its parts are told apart by their vertex colours -- ten flat
colours, one per part -- and only its CRT eye screens had UVs (0-1 each, read by the eye
glow). So:

1. UV: every face but the eyes is unwrapped (Smart UV Project) into one 0-1 atlas; the eyes
   keep their own 0-1 UVs, which the material's CRT effect reads (vertex colour A = 0 marks
   them, and the material draws them as dark glass whatever the atlas holds there).
2. Bake (Cycles): each part gets a procedural surface per look -- brass hammered and
   tarnished, copper, bronze rivets, pebble leather, rubber bellows, gunmetal, enamel with
   livery bands, riveted steel plate -- in object space (no seams), baked into the atlas:
   base colour, roughness, metal, tangent-space normal, ambient occlusion, and a mask (clear
   coat for the Watchman's enamel, anisotropy for the Brute's plate).
3. Write Content/Characters/SteamDummy/SteamDummy_PBR.nmmesh: the same vertices with the
   atlas UVs, MikkTSpace tangents and the original vertex colours; the skin (skeleton path,
   inverse binds, each vertex's bones and weights) copied from SteamDummy.nmmesh by
   position, so the rig and bone names are exactly the import's.

Textures (Content/Characters/SteamDummy/SteamDummy_<Look>_<Map>.nmtexture), per look:
BaseColor (A: the look's mask), Normal, ORM (R occlusion, G roughness, B metal). Player's
BaseColor and Normal are 2048, everything else 1024.
"""

import math
import sys
from pathlib import Path

import bpy
import numpy as np

ART = Path(__file__).resolve().parents[1]
GAME = ART.parents[1]
sys.path.insert(0, str(ART / "lib"))
import nmformats as nm      # noqa: E402

SRC = GAME / "Content" / "Characters" / "SteamDummy"
CACHE = GAME / "Saved" / "ArtCache"
BAKE = 2048

CLASSES = {
    (0.62, 0.42, 0.12): "shell", (0.58, 0.24, 0.10): "trim", (0.36, 0.24, 0.08): "rivet",
    (0.16, 0.07, 0.04): "leather", (0.04, 0.02, 0.01): "eye", (0.95, 0.75, 0.02): "decal_y",
    (0.01, 0.01, 0.01): "decal_k", (0.07, 0.07, 0.07): "iron", (0.09, 0.05, 0.03): "grille",
    (0.10, 0.06, 0.05): "glove",
}

# Per look, per part: base colour (sRGB), roughness, metal, mask, bump pattern, extras.
BRASS = dict(base=(0.80, 0.62, 0.33), rough=0.32, metal=1.0, bump="hammer", tarnish=(0.45, 0.36, 0.2))
COPPER = dict(base=(0.85, 0.48, 0.32), rough=0.3, metal=1.0, bump="brushed", tarnish=(0.42, 0.24, 0.16))
BRONZE = dict(base=(0.48, 0.36, 0.22), rough=0.38, metal=1.0, bump="brushed")
LEATHER = dict(base=(0.30, 0.16, 0.08), rough=0.6, metal=0.0, bump="leather")
BLACK_LEATHER = dict(base=(0.06, 0.055, 0.05), rough=0.5, metal=0.0, bump="leather")
RUBBER = dict(base=(0.04, 0.04, 0.04), rough=0.8, metal=0.0, bump="rubber")
GUNMETAL = dict(base=(0.28, 0.28, 0.30), rough=0.42, metal=1.0, bump="brushed")
GLASS = dict(base=(0.02, 0.02, 0.02), rough=0.05, metal=0.0, bump="none")

LOOKS = {
    "Player": {
        "shell": BRASS, "trim": COPPER, "rivet": BRONZE, "leather": LEATHER, "glove": LEATHER | dict(base=(0.20, 0.11, 0.06)),
        "rubber": RUBBER, "iron": GUNMETAL, "grille": GUNMETAL | dict(base=(0.12, 0.12, 0.13)),
        "decal_y": dict(base=(0.90, 0.70, 0.08), rough=0.4, metal=0.0, bump="none"),
        "decal_k": dict(base=(0.03, 0.03, 0.03), rough=0.4, metal=0.0, bump="none"), "eye": GLASS,
    },
    "Watchman": {
        # Security livery: navy enamel (clear coat) with cream bands, polished brass fittings.
        "shell": dict(base=(0.05, 0.09, 0.20), rough=0.35, metal=0.0, mask=1.0, bump="enamel",
                      bands=[(1.38, 1.46, (0.86, 0.80, 0.66)), (0.60, 0.66, (0.86, 0.80, 0.66)),
                             (1.80, 1.84, (0.70, 0.08, 0.05))]),
        "trim": BRASS | dict(base=(0.86, 0.68, 0.38), rough=0.22, tarnish=None, bump="brushed"),
        "rivet": BRASS | dict(tarnish=None, bump="none"), "leather": BLACK_LEATHER, "glove": BLACK_LEATHER,
        "rubber": RUBBER, "iron": GUNMETAL | dict(base=(0.16, 0.17, 0.19)),
        "grille": GUNMETAL | dict(base=(0.08, 0.08, 0.09)),
        "decal_y": dict(base=(0.86, 0.80, 0.66), rough=0.3, metal=0.0, mask=1.0, bump="none"),
        "decal_k": dict(base=(0.60, 0.06, 0.04), rough=0.3, metal=0.0, mask=1.0, bump="none"), "eye": GLASS,
    },
    "Brute": {
        # Riveted steel plate, brushed (anisotropic), rust in the seams; blackened iron trim.
        "shell": dict(base=(0.52, 0.53, 0.55), rough=0.3, metal=1.0, mask=1.0, bump="plate",
                      tarnish=(0.30, 0.17, 0.09)),
        "trim": GUNMETAL | dict(base=(0.18, 0.18, 0.19), mask=0.6),
        "rivet": dict(base=(0.62, 0.62, 0.62), rough=0.28, metal=1.0, mask=0.5, bump="none"),
        "leather": BLACK_LEATHER | dict(base=(0.12, 0.08, 0.05)), "glove": RUBBER, "rubber": RUBBER,
        "iron": GUNMETAL | dict(base=(0.20, 0.20, 0.21)), "grille": GUNMETAL | dict(base=(0.1, 0.1, 0.1)),
        "decal_y": dict(base=(0.80, 0.55, 0.05), rough=0.5, metal=0.0, bump="none"),
        "decal_k": dict(base=(0.03, 0.03, 0.03), rough=0.5, metal=0.0, bump="none"), "eye": GLASS,
    },
}
SIZES = {"Player": (2048, 2048, 1024), "Watchman": (1024, 1024, 1024), "Brute": (1024, 1024, 1024)}


def srgb_to_lin(c):
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)


# ----------------------------------------------------------------------------------------------

def load():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    bpy.ops.import_scene.gltf(filepath=str(SRC / "SteamDummy.glb"))
    for ob in list(bpy.data.objects):
        if ob.type == "MESH" and ob.name != "SteamDummy_Body":
            bpy.data.objects.remove(ob)
    return bpy.data.objects["SteamDummy_Body"]


def classify(body):
    me = body.data
    ca = me.color_attributes["Color"]
    cols = np.empty(len(me.loops) * 4, np.float32)
    ca.data.foreach_get("color", cols)
    cols = cols.reshape(-1, 4)
    starts = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_start", starts)
    centres = np.empty(len(me.polygons) * 3, np.float32)
    me.polygons.foreach_get("center", centres)
    centres = centres.reshape(-1, 3)
    names = []
    for i, s in enumerate(starts):
        key = tuple(round(float(x), 2) for x in cols[s, :3])
        c = CLASSES.get(key)
        if c is None:
            best = min(CLASSES, key=lambda k: sum((a - b) ** 2 for a, b in zip(k, cols[s, :3])))
            c = CLASSES[best]
        if c == "iron":
            x, y, z = centres[i]
            if abs(x) < 0.2 and 0.95 < z < 1.32:
                c = "rubber"          # the waist bellows
        names.append(c)
    return names, cols


def unwrap(body, classes):
    me = body.data
    atlas = me.uv_layers.new(name="Atlas")
    # Start from the original UVs (the eyes' 0-1 screens), then unwrap everything else.
    src = me.uv_layers["UVMap"]
    uv = np.empty(len(me.loops) * 2, np.float32)
    src.data.foreach_get("uv", uv)
    atlas.data.foreach_set("uv", uv)
    me.uv_layers.active = atlas
    bpy.context.view_layer.objects.active = body
    body.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="DESELECT")
    bpy.ops.object.mode_set(mode="OBJECT")
    for poly, c in zip(me.polygons, classes):
        poly.select = c != "eye"
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.004, area_weight=0.0,
                             correct_aspect=True, scale_to_bounds=False)
    bpy.ops.uv.pack_islands(margin=0.004, rotate=True)
    bpy.ops.object.mode_set(mode="OBJECT")
    me.uv_layers.active = atlas
    atlas.active_render = True


# ----------------------------------------------------------------------------------------------
# Bake materials

def _node(nt, kind, **kw):
    n = nt.nodes.new(kind)
    for k, v in kw.items():
        setattr(n, k, v)
    return n


def make_material(name, spec, target):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    L = nt.links
    bsdf = nt.nodes["Principled BSDF"]
    out = nt.nodes["Material Output"]
    tc = _node(nt, "ShaderNodeTexCoord")
    obj = tc.outputs["Object"]
    # Colour: base x broad variation, tarnish blotches, livery bands.
    var = _node(nt, "ShaderNodeTexNoise")
    var.inputs["Scale"].default_value = 5.0
    var.inputs["Detail"].default_value = 6.0
    L.new(obj, var.inputs["Vector"])
    vr = _node(nt, "ShaderNodeMapRange")
    vr.inputs["To Min"].default_value = 0.82
    vr.inputs["To Max"].default_value = 1.12
    L.new(var.outputs["Fac"], vr.inputs["Value"])
    base = _node(nt, "ShaderNodeMix", data_type="RGBA", blend_type="MULTIPLY")
    base.inputs["Factor"].default_value = 1.0
    base.inputs[6].default_value = (*srgb_to_lin(spec["base"]), 1)
    L.new(vr.outputs["Result"], base.inputs[7])
    colour = base.outputs[2]
    rough_val = spec["rough"]
    rough_node = _node(nt, "ShaderNodeMapRange")
    rough_node.inputs["To Min"].default_value = rough_val * 0.85
    rough_node.inputs["To Max"].default_value = min(1.0, rough_val * 1.25)
    L.new(var.outputs["Fac"], rough_node.inputs["Value"])
    rough = rough_node.outputs["Result"]
    if spec.get("tarnish"):
        tn = _node(nt, "ShaderNodeTexNoise")
        tn.inputs["Scale"].default_value = 9.0
        tn.inputs["Detail"].default_value = 8.0
        tn.inputs["Roughness"].default_value = 0.65
        L.new(obj, tn.inputs["Vector"])
        ramp = _node(nt, "ShaderNodeMapRange")
        ramp.inputs["From Min"].default_value = 0.52
        ramp.inputs["From Max"].default_value = 0.68
        L.new(tn.outputs["Fac"], ramp.inputs["Value"])
        mix = _node(nt, "ShaderNodeMix", data_type="RGBA")
        L.new(ramp.outputs["Result"], mix.inputs["Factor"])
        L.new(colour, mix.inputs[6])
        mix.inputs[7].default_value = (*srgb_to_lin(spec["tarnish"]), 1)
        colour = mix.outputs[2]
        radd = _node(nt, "ShaderNodeMath", operation="MULTIPLY_ADD")
        L.new(ramp.outputs["Result"], radd.inputs[0])
        radd.inputs[1].default_value = 0.3
        L.new(rough, radd.inputs[2])
        rough = radd.outputs[0]
    for (zlo, zhi, bc) in spec.get("bands", []):
        sep = _node(nt, "ShaderNodeSeparateXYZ")
        L.new(obj, sep.inputs["Vector"])
        a = _node(nt, "ShaderNodeMath", operation="GREATER_THAN")
        a.inputs[1].default_value = zlo
        b = _node(nt, "ShaderNodeMath", operation="LESS_THAN")
        b.inputs[1].default_value = zhi
        L.new(sep.outputs["Z"], a.inputs[0])
        L.new(sep.outputs["Z"], b.inputs[0])
        both = _node(nt, "ShaderNodeMath", operation="MULTIPLY")
        L.new(a.outputs[0], both.inputs[0])
        L.new(b.outputs[0], both.inputs[1])
        mix = _node(nt, "ShaderNodeMix", data_type="RGBA")
        L.new(both.outputs[0], mix.inputs["Factor"])
        L.new(colour, mix.inputs[6])
        mix.inputs[7].default_value = (*srgb_to_lin(bc), 1)
        colour = mix.outputs[2]
    L.new(colour, bsdf.inputs["Base Color"])
    L.new(rough, bsdf.inputs["Roughness"])
    bsdf.inputs["Metallic"].default_value = spec["metal"]
    # Height pattern -> Bump.
    kind = spec.get("bump", "none")
    height = None
    strength, dist = 0.4, 0.002
    if kind == "hammer":
        v = _node(nt, "ShaderNodeTexVoronoi", feature="SMOOTH_F1")
        v.inputs["Scale"].default_value = 60.0
        L.new(obj, v.inputs["Vector"])
        height = v.outputs["Distance"]
        strength, dist = 0.25, 0.002
    elif kind == "brushed":
        mp = _node(nt, "ShaderNodeMapping")
        mp.inputs["Scale"].default_value = (300, 300, 8)
        L.new(obj, mp.inputs["Vector"])
        n = _node(nt, "ShaderNodeTexNoise")
        n.inputs["Scale"].default_value = 1.0
        n.inputs["Detail"].default_value = 4
        L.new(mp.outputs["Vector"], n.inputs["Vector"])
        height = n.outputs["Fac"]
        strength, dist = 0.15, 0.0006
    elif kind == "leather":
        v = _node(nt, "ShaderNodeTexVoronoi", feature="DISTANCE_TO_EDGE")
        v.inputs["Scale"].default_value = 260.0
        L.new(obj, v.inputs["Vector"])
        height = v.outputs["Distance"]
        strength, dist = 0.5, 0.0015
    elif kind == "rubber":
        n = _node(nt, "ShaderNodeTexNoise")
        n.inputs["Scale"].default_value = 400.0
        L.new(obj, n.inputs["Vector"])
        height = n.outputs["Fac"]
        strength, dist = 0.1, 0.0005
    elif kind == "enamel":
        n = _node(nt, "ShaderNodeTexNoise")
        n.inputs["Scale"].default_value = 120.0
        L.new(obj, n.inputs["Vector"])
        height = n.outputs["Fac"]
        strength, dist = 0.05, 0.0005      # orange peel, under the coat
    elif kind == "plate":
        # Plate seams (bricks in object space) and a grid of rivet heads.
        br = _node(nt, "ShaderNodeTexBrick")
        br.inputs["Scale"].default_value = 3.5
        br.inputs["Mortar Size"].default_value = 0.012
        br.inputs["Mortar Smooth"].default_value = 0.5
        br.inputs["Brick Width"].default_value = 0.9
        br.inputs["Row Height"].default_value = 0.45
        L.new(obj, br.inputs["Vector"])
        rv = _node(nt, "ShaderNodeTexVoronoi", feature="F1")
        rv.inputs["Scale"].default_value = 22.0
        rv.inputs["Randomness"].default_value = 0.0
        L.new(obj, rv.inputs["Vector"])
        dot = _node(nt, "ShaderNodeMapRange")
        dot.inputs["From Min"].default_value = 0.16
        dot.inputs["From Max"].default_value = 0.08
        L.new(rv.outputs["Distance"], dot.inputs["Value"])
        add = _node(nt, "ShaderNodeMath", operation="MULTIPLY_ADD")
        seamless = _node(nt, "ShaderNodeMath", operation="SUBTRACT")     # Fac is 1 in the seams
        seamless.inputs[0].default_value = 1.0
        L.new(br.outputs["Fac"], seamless.inputs[1])
        L.new(dot.outputs["Result"], add.inputs[0])
        add.inputs[1].default_value = 0.6
        L.new(seamless.outputs[0], add.inputs[2])
        height = add.outputs[0]
        strength, dist = 0.6, 0.004
        # Rust gathers in the seams.
        rust = _node(nt, "ShaderNodeMix", data_type="RGBA")
        inv = _node(nt, "ShaderNodeMath", operation="SUBTRACT")
        inv.inputs[0].default_value = 1.0
        L.new(br.outputs["Fac"], inv.inputs[1])
        fac = _node(nt, "ShaderNodeMath", operation="MULTIPLY")
        L.new(br.outputs["Fac"], fac.inputs[0])
        fac.inputs[1].default_value = 0.85
        L.new(fac.outputs[0], rust.inputs["Factor"])
        L.new(colour, rust.inputs[6])
        rust.inputs[7].default_value = (*srgb_to_lin((0.28, 0.14, 0.07)), 1)
        L.new(rust.outputs[2], bsdf.inputs["Base Color"])
    if height is not None:
        bump = _node(nt, "ShaderNodeBump")
        bump.inputs["Strength"].default_value = strength
        bump.inputs["Distance"].default_value = dist
        L.new(height, bump.inputs["Height"])
        L.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    # Emission of a value, for the metal and mask bakes; of the colour, for the base colour.
    em = _node(nt, "ShaderNodeEmission")
    em.name = "DataEmit"
    ce = _node(nt, "ShaderNodeEmission")
    ce.name = "ColorEmit"
    src = [l.from_socket for l in nt.links if l.to_socket == bsdf.inputs["Base Color"]]
    if src:
        L.new(src[0], ce.inputs["Color"])
    m["metal"] = spec["metal"]
    m["mask"] = spec.get("mask", 0.0)
    img = _node(nt, "ShaderNodeTexImage")
    img.image = target
    nt.nodes.active = img
    return m


def set_mode(mats, mode):
    for m in mats:
        nt = m.node_tree
        out = nt.nodes["Material Output"]
        if mode == "surface":
            nt.links.new(nt.nodes["Principled BSDF"].outputs[0], out.inputs["Surface"])
        elif mode == "colour":
            nt.links.new(nt.nodes["ColorEmit"].outputs[0], out.inputs["Surface"])
        else:
            em = nt.nodes["DataEmit"]
            v = m[mode]
            em.inputs["Color"].default_value = (v, v, v, 1)
            nt.links.new(em.outputs[0], out.inputs["Surface"])


def bake(body, kind, image, **kw):
    for ob in bpy.context.view_layer.objects:
        ob.select_set(False)
    body.select_set(True)
    bpy.context.view_layer.objects.active = body
    uvs = body.data.uv_layers
    uvs.active_index = [l.name for l in uvs].index("BakeUV")
    uvs["BakeUV"].active_render = True
    bpy.ops.object.bake(type=kind, margin=16, margin_type="EXTEND", use_clear=True, target="IMAGE_TEXTURES", **kw)
    a = np.array(image.pixels[:], dtype=np.float32).reshape(image.size[1], image.size[0], 4)
    return a[::-1].copy()       # row 0 the image's top (UV v = 1 in Blender, 0 in the engine)


def resize(img, size):
    f = img.shape[0] // size
    if f <= 1:
        return img
    return img.reshape(size, f, size, f, -1).mean(axis=(1, 3))


# ----------------------------------------------------------------------------------------------

def write_mesh(body, classes):
    me = body.data
    me.calc_tangents(uvmap="Atlas")
    me.calc_loop_triangles()
    nl = len(me.loops)
    M = np.asarray(body.matrix_world)
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]
    lv = np.empty(nl, np.int32)
    me.loops.foreach_get("vertex_index", lv)
    cn = np.empty(nl * 3, np.float32)
    me.corner_normals.foreach_get("vector", cn)
    cn = cn.reshape(-1, 3) @ M[:3, :3].T
    cn /= np.linalg.norm(cn, axis=1, keepdims=True)
    uv = np.empty(nl * 2, np.float32)
    me.uv_layers["Atlas"].data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    tan = np.empty(nl * 3, np.float32)
    me.loops.foreach_get("tangent", tan)
    tan = tan.reshape(-1, 3) @ M[:3, :3].T
    sign = np.empty(nl, np.float32)
    me.loops.foreach_get("bitangent_sign", sign)
    col = np.empty(nl * 4, np.float32)
    me.color_attributes["Color"].data.foreach_get("color", col)
    col = col.reshape(-1, 4)
    tri = np.empty(len(me.loop_triangles) * 3, np.int32)
    me.loop_triangles.foreach_get("loops", tri)
    tri = tri.reshape(-1, 3)[:, [0, 2, 1]].ravel()        # the mirror reverses winding
    P = co[lv][:, [0, 2, 1]] * 100.0
    N = cn[:, [0, 2, 1]]
    T = np.concatenate([tan[:, [0, 2, 1]], sign[:, None]], axis=1)
    UV = np.stack([uv[:, 0], 1.0 - uv[:, 1]], axis=1)
    key = np.concatenate([np.round(P, 3), np.round(N, 4), np.round(UV, 5), np.round(T, 3), np.round(col, 3)], axis=1)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.reshape(-1)
    indices = inverse[tri].astype(np.uint32)
    P, N, UV, T, C = P[first], N[first], UV[first], T[first], col[first]
    # Skin from the import, matched by position.
    old = nm.read_mesh(SRC / "SteamDummy.nmmesh")
    lut = {}
    for p, j, w in zip(np.round(old["positions"], 2), old["joints"], old["weights"]):
        lut.setdefault(tuple(p), (j, w))
    joints = np.zeros((len(P), 4), np.uint16)
    weights = np.zeros((len(P), 4), np.float32)
    missing = 0
    for i, p in enumerate(np.round(P, 2)):
        jw = lut.get(tuple(p))
        if jw is None:
            # Nearest old vertex (rounding at a boundary).
            d = np.linalg.norm(old["positions"] - P[i], axis=1)
            k = int(np.argmin(d))
            jw = (old["joints"][k], old["weights"][k])
            missing += 1
        joints[i], weights[i] = jw
    path = SRC / "SteamDummy_PBR.nmmesh"
    nm.write_mesh_arrays(path, P, N, UV, indices, [("SteamDummy_Mesh", "Content/Materials/Characters/SteamDummy_Player.material.nmgraph", 0, len(indices))],
                         colours=C, tangents=T,
                         skin=(old["skeleton"], old["inverse_binds"], old["bone_count"], joints, weights))
    print(f"SteamDummy_PBR.nmmesh: {len(P)} vertices ({len(old['positions'])} in the import), "
          f"{len(indices) // 3} triangles, {missing} skinned by nearest; bounds {P.min(0)} {P.max(0)}")
    lo_old, hi_old = old["positions"].min(0), old["positions"].max(0)
    assert np.allclose(P.min(0), lo_old, atol=0.05) and np.allclose(P.max(0), hi_old, atol=0.05), "frame mismatch"


def main(argv):
    global BAKE
    if "--size" in argv:
        BAKE = int(argv[argv.index("--size") + 1])
    body = load()
    classes, _ = classify(body)
    unwrap(body, classes)
    write_mesh(body, classes)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.render.threads_mode = "FIXED"
    sc.render.threads = 6
    sc.cycles.samples = 1
    body.modifiers.clear()      # bake the rest pose
    # Bake through a copy of the atlas with the eyes (0-1 each, over everything) collapsed.
    me = body.data
    uv = np.empty(len(me.loops) * 2, np.float32)
    me.uv_layers["Atlas"].data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    starts = np.empty(len(me.polygons), np.int32)
    totals = np.empty(len(me.polygons), np.int32)
    me.polygons.foreach_get("loop_start", starts)
    me.polygons.foreach_get("loop_total", totals)
    loop_cls = np.repeat(np.array([c == "eye" for c in classes]), totals)
    uv[loop_cls] = (0.9995, 0.9995)
    bl = me.uv_layers.new(name="BakeUV")
    bl.data.foreach_set("uv", uv.ravel())
    target = bpy.data.images.new("Bake", BAKE, BAKE, float_buffer=True, alpha=False)
    target.colorspace_settings.name = "Non-Color"
    names = sorted(set(classes))
    out = SRC
    # Occlusion once (shared by the looks): a simple material everywhere.
    me = body.data
    me.materials.clear()
    ao_mat = make_material("AO", GLASS, target)
    me.materials.append(ao_mat)
    sc.cycles.samples = 64
    ao = bake(body, "AO", target)[..., 0]
    sc.cycles.samples = 1
    me.materials.clear()
    for look, spec in LOOKS.items():
        mats = {}
        me.materials.clear()
        for c in names:
            mats[c] = make_material(f"{look}_{c}", spec[c], target)
            me.materials.append(mats[c])
        idx = np.array([names.index(c) for c in classes], np.int32)
        me.polygons.foreach_set("material_index", idx)
        me.update()
        ml = list(mats.values())
        set_mode(ml, "colour")
        base = bake(body, "EMIT", target)[..., :3]
        set_mode(ml, "surface")
        rough = bake(body, "ROUGHNESS", target)[..., 0]
        normal = bake(body, "NORMAL", target, normal_space="TANGENT")[..., :3]
        set_mode(ml, "metal")
        metal = bake(body, "EMIT", target)[..., 0]
        set_mode(ml, "mask")
        mask = bake(body, "EMIT", target)[..., 0]
        # Crevices collect grime: darken by the occlusion a little.
        base = base * (0.6 + 0.4 * ao[..., None])
        base_srgb = nm.to_srgb(np.clip(base, 0, 1))
        bsz, nsz, osz = SIZES[look]
        prefix = f"SteamDummy_{look}"
        rgba = np.concatenate([resize(base_srgb, bsz), resize(mask[..., None], bsz)], axis=-1)
        nm.write_texture(out / f"{prefix}_BaseColor.nmtexture", rgba, srgb=True, wrap="clamp")
        nrm = resize(normal, nsz) * 2 - 1
        nrm /= np.maximum(np.linalg.norm(nrm, axis=-1, keepdims=True), 1e-6)
        nm.write_texture(out / f"{prefix}_Normal.nmtexture", nrm * 0.5 + 0.5, srgb=False, wrap="clamp", normal_map=True)
        orm = np.stack([ao, np.clip(rough, 0.03, 1), metal], axis=-1)
        nm.write_texture(out / f"{prefix}_ORM.nmtexture", resize(orm, osz), srgb=False, wrap="clamp")
        # PNG copies for previews.
        from PIL import Image
        (CACHE / "Textures").mkdir(parents=True, exist_ok=True)
        Image.fromarray((np.clip(resize(base_srgb, 1024), 0, 1) * 255).astype(np.uint8)).save(
            CACHE / "Textures" / f"{prefix}_BaseColor.png")
        Image.fromarray((np.clip(resize(normal, 1024), 0, 1) * 255).astype(np.uint8)).save(
            CACHE / "Textures" / f"{prefix}_Normal.png")
        Image.fromarray((np.clip(resize(orm, 1024), 0, 1) * 255).astype(np.uint8)).save(
            CACHE / "Textures" / f"{prefix}_ORM.png")
        Image.fromarray((np.clip(resize(mask[..., None].repeat(3, -1), 1024), 0, 1) * 255).astype(np.uint8)).save(
            CACHE / "Textures" / f"{prefix}_Mask.png")
        print(f"{look}: baked")
    import json
    recs = []
    for look in LOOKS:
        recs.append(dict(name=f"SteamDummy_{look}", kit="Characters",
                         mesh="Content/Characters/SteamDummy/SteamDummy_PBR.nmmesh",
                         materials=[f"Content/Materials/Characters/SteamDummy_{look}.material.nmgraph"],
                         preview_material=f"SteamDummy_{look}",
                         skeleton="Content/Characters/SteamDummy/SteamDummy_Skeleton.nmskel",
                         size_m=[1.59, 2.01, 0.35], pivot="between the feet, on the ground; faces -Z",
                         notes="Skinned to CrashDummy's 74-bone rig (the import's skeleton); set the material "
                               "in Render > Material slot 0. Brute: scale the entity 1.25." , view_yaw=-25.0))
    CACHE.mkdir(parents=True, exist_ok=True)
    (CACHE / "manifest_Characters.json").write_text(json.dumps(recs, indent=1))


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
