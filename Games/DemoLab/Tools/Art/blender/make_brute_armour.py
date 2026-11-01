"""
make_brute_armour.py -- the Brute's armour plates as separate meshes that can break off
(Content/Characters/SteamDummy/Armour/Brute_<Plate>.nmmesh), without touching the skeleton:

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_brute_armour.py

Each plate is cut from SteamDummy_PBR.nmmesh's shell where one bone carries it (the
vertex's heaviest weight) inside a region, pushed out off the body, given thickness and a
rolled edge, and riveted round its rim. It is written in that bone's own frame (the bind
pose's inverse, scale-free), so an entity with an **Anim Socket** (Bone = the plate's bone,
offsets 0) parented to the Brute holds it exactly where it was cut from, and follows the
bone. To break one off: remove its socket (or reparent it to the world) at its current
transform and hand it to physics. Records go into the Characters manifest (assets.json:
Brute_Armour_*, with "bone").

The Brute's body texture still has its baked plates under them; a plate that comes off
shows the body's own painted plate beneath (read as the under-layer).
"""

import json
import math
import struct
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kitlib as K      # noqa: E402

nm = K.nm
SRC = K.GAME / "Content" / "Characters" / "SteamDummy"
SHELL, TRIM = (0.62, 0.42, 0.12), (0.58, 0.24, 0.10)     # the plates and their edging

# name, bone, region test on the engine-space vertex (cm: x right, y up, z back; the
# character faces -Z), push-off (cm), thickness (cm)
PLATES = [
    ("ChestPlate", "DEF-spine.003", lambda p: p[2] < -1.0 and 137 < p[1] < 176, 0.5, 1.2),
    ("BackPlate", "DEF-spine.003", lambda p: p[2] > 4.0 and 137 < p[1] < 176, 0.5, 1.2),
    ("Pauldron.L", "DEF-shoulder.L", lambda p: p[1] > 159.5, 0.6, 1.0),
    ("Pauldron.R", "DEF-shoulder.R", lambda p: p[1] > 159.5, 0.6, 1.0),
    ("ThighGuard.L", "DEF-thigh.L", lambda p: 60 < p[1] < 104, 0.5, 1.0),
    ("ThighGuard.R", "DEF-thigh.R", lambda p: 60 < p[1] < 104, 0.5, 1.0),
    ("Greave.L", "DEF-shin.L", lambda p: 16 < p[1] < 56, 0.5, 1.0),
    ("Greave.R", "DEF-shin.R", lambda p: 16 < p[1] < 56, 0.5, 1.0),
]


def skeleton_names(path):
    d = path.read_bytes()
    top = nm._read_chunks(d, 0, len(d))
    o, s = top["NSKL"]
    c = nm._read_chunks(d, o, o + s)
    o = c["SBON"][0]
    n = struct.unpack_from("<I", d, o)[0]
    o += 4
    names = []
    for _ in range(n):
        ln = struct.unpack_from("<I", d, o)[0]
        names.append(d[o + 4:o + 4 + ln].decode())
        o += 4 + ln + 4 + 40          # name, parent, translation, rotation, scale
    return names


def build_plate(name, m, sel_tris, push, thick):
    """A Blender object (Blender frame, metres) from the body's triangles sel_tris."""
    P, N = m["positions"], m["normals"]
    used = np.unique(sel_tris)
    remap = {int(v): i for i, v in enumerate(used)}
    bm = bmesh.new()
    vs = []
    for v in used:
        p = P[v] + N[v] * push
        vs.append(bm.verts.new((p[0] / 100.0, p[2] / 100.0, p[1] / 100.0)))
    for t in sel_tris:
        try:
            bm.faces.new([vs[remap[int(t[0])]], vs[remap[int(t[2])]], vs[remap[int(t[1])]]])
        except ValueError:
            pass
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0015)
    # Keep the pieces of 16 faces or more (drop crumbs of the selection).
    bm.faces.ensure_lookup_table()
    seen, best = set(), []
    kept = []
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, comp = [f], []
        seen.add(f.index)
        while stack:
            g = stack.pop()
            comp.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        stack.append(h)
        if len(comp) >= 16:
            kept += comp
    keep = set(f.index for f in kept)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index not in keep], context="FACES")
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    bm.normal_update()          # the body's winding, mirrored with the axes: normals point out
    rim = [e for e in bm.edges if e.is_boundary]
    rim_pts = [(e.verts[0].co.copy(), e.verts[1].co.copy(), e.link_faces[0].normal.copy()) for e in rim]
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    sol = ob.modifiers.new("Solidify", "SOLIDIFY")
    sol.thickness = thick / 100.0
    sol.offset = -1.0
    sol.use_rim = True
    sol.use_even_offset = True
    bev = ob.modifiers.new("Bevel", "BEVEL")
    bev.width = 0.0025
    bev.segments = 2
    bev.limit_method = "ANGLE"
    bev.angle_limit = math.radians(50)
    ob.modifiers.new("Triangulate", "TRIANGULATE")
    K._finish(ob, "PlateSteel", True, math.radians(40))
    # Rivets round the rim, every ~5 cm, a little in from the edge.
    rivets = []
    acc = 0.0
    for a, b, n in rim_pts:
        L = (b - a).length
        acc += L
        if acc < 0.05:
            continue
        acc = 0.0
        mid = (a + b) / 2
        r = K.rivet(0.0075, "SteelDark")
        r.location = mid + n * 0.001
        K.orient(r, n)
        rivets.append(r)
    return [ob] + rivets


def main():
    K.reset()
    m = nm.read_mesh(SRC / "SteamDummy_PBR.nmmesh")
    names = skeleton_names(SRC / "SteamDummy_Skeleton.nmskel")
    ib = np.frombuffer(m["inverse_binds"], np.float32).reshape(-1, 3, 4)
    dom = m["joints"][np.arange(len(m["joints"])), np.argmax(m["weights"], axis=1)]
    col = m["colours"][:, :3]
    shell = (np.linalg.norm(col - np.array(SHELL), axis=1) < 0.06) | (np.linalg.norm(col - np.array(TRIM), axis=1) < 0.06)
    tris = m["indices"].reshape(-1, 3)
    S = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))     # engine <-> Blender axes
    records = []
    for name, bone, region, push, thick in PLATES:
        K.clear()
        b = names.index(bone)
        ok = (dom == b) & shell & np.array([region(p) for p in m["positions"]])
        sel = tris[ok[tris].all(axis=1)]
        if len(sel) < 20:
            print(f"WARNING {name}: only {len(sel)} triangles")
            continue
        obs = build_plate(name, m, sel, push, thick)
        # Into the bone's frame: engine p_local = R^T (p - t), the bind pose's rotation and
        # head (no scale; an Anim Socket drops the bone's scale too).
        bind = np.linalg.inv(np.vstack([ib[b], [0, 0, 0, 1]]))
        R = bind[:3, :3] / np.linalg.norm(bind[:3, :3], axis=0)
        t = bind[:3, 3] / 100.0
        to_local = Matrix.Identity(4)
        for i in range(3):
            for j in range(3):
                to_local[i][j] = float(R[j, i])
        to_local = to_local @ Matrix.Translation(Vector((-t[0], -t[1], -t[2])))
        M = S @ to_local @ S
        root = bpy.data.objects.new("PlateRoot", None)
        bpy.context.scene.collection.objects.link(root)
        root.matrix_world = M
        for ob in obs:
            if ob.parent is None:
                ob.parent = root
        safe = name.replace(".", "_")
        rec = K.export(f"Brute_Armour_{safe}", obs, f"Characters/SteamDummy/Armour/Brute_{safe}.nmmesh",
                       kit="Characters", ao_dist=0.08, record=False,
                       pivot_note=f"the bone {bone}'s head, in its frame: an Anim Socket with Bone {bone} and zero "
                                  "offsets on a child of the Brute holds it in place")
        rec["bone"] = bone
        rec["notes"] = ("Brute armour plate (breakable): child entity of the Brute with Render (this mesh) + Anim "
                        f"Socket (Bone {bone}); to knock it off, drop the socket at its current transform and add "
                        "a physics body")
        rec["view_yaw"] = -25.0
        records.append(rec)
    path = K.CACHE / "manifest_Characters.json"
    old = json.loads(path.read_text()) if path.exists() else []
    old = [r for r in old if not r["name"].startswith("Brute_Armour_")]
    path.write_text(json.dumps(old + records, indent=1))
    print(f"Characters: {len(records)} armour plates -> {path}")


main()
