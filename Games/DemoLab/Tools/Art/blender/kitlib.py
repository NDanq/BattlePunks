"""
kitlib.py -- modelling helpers and the .nmmesh exporter for Demo Lab's Blender generators
(make_l1.py, make_l2.py, ...). Runs inside Blender (bpy, bmesh, numpy):

    blender -b --factory-startup --python Games/DemoLab/Tools/Art/blender/make_l1.py

Frames. Blender is right-handed, Z up, metres; the engine left-handed, Y up, centimetres,
+Z away from the viewer. A Blender point (x, y, z) is the engine's (x, z, y) x 100, so the
kit is modelled facing Blender's -Y (its front view), which is the engine's -Z -- the way
imported characters face (CrashDummy faces -Z). The swap is a mirror: the exporter reverses
each triangle's winding, and the tangent's w (MikkTSpace's bitangent sign) keeps its sign
because the image's V flips too (checked per mesh: see _check_frame).

UV0. Faces a helper laid out itself (lathes, tori, sweeps, decals) carry the face attribute
"uvkeep" = 1 and keep their UVs; every other face is box-projected at export in metres of
the asset (1 UV = 1 m), so materials tile by 1 / the metres their set covers and a texture
runs on across the parts of one asset. V is flipped for the engine (its V runs down the
image), so the same PNG previews the same in Blender.

Vertex colour. RGB: ambient occlusion baked per vertex by casting rays at the whole asset
(its parts occlude each other; ground=True adds the floor). A: how convex the vertex is,
0 flat or hollow, 1 a sharp outside edge -- the materials wear paint off there.

Every export is recorded for the manifest (assets.json): its mesh, materials, size in
metres (engine axes), pivot, and its moving parts with their offsets.
"""

import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

ART = Path(__file__).resolve().parents[1]
GAME = ART.parents[1]
sys.path.insert(0, str(ART / "lib"))
import nmformats as nm          # noqa: E402
from matdefs import MATS, material_path   # noqa: E402

CACHE = GAME / "Saved" / "ArtCache"
SHARP = math.radians(35.0)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)


# ==============================================================================================
# Materials (Blender material names are matdefs.py's)
# ==============================================================================================

def material(name):
    if name not in MATS:
        raise KeyError(f"no material {name} in matdefs.py")
    m = bpy.data.materials.get(name)
    if m is None:
        m = bpy.data.materials.new(name)
    return m


def _finish(ob, mat, smooth=True, sharp=SHARP):
    me = ob.data
    if mat is not None:
        me.materials.append(material(mat))
    if smooth:
        me.shade_smooth()
        me.set_sharp_from_angle(angle=sharp)
    return ob


def obj_from_bm(name, bm, mat=None, smooth=True, sharp=SHARP, keep_uv=False):
    ngons = [f for f in bm.faces if len(f.verts) > 4]
    if ngons:
        bmesh.ops.triangulate(bm, faces=ngons, quad_method="BEAUTY", ngon_method="BEAUTY")
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    if keep_uv:
        attr = me.attributes.new("uvkeep", "INT", "FACE")
        attr.data.foreach_set("value", np.ones(len(me.polygons), dtype=np.int32))
    return _finish(ob, mat, smooth, sharp)


def place(ob, loc=(0, 0, 0), rot=(0, 0, 0), scale=None):
    ob.location = loc
    ob.rotation_euler = [math.radians(a) for a in rot]
    if scale is not None:
        ob.scale = scale if hasattr(scale, "__len__") else (scale, scale, scale)
    return ob


def dup(ob, loc=None, rot=None, scale=None):
    """A copy sharing ob's mesh, moved."""
    c = ob.copy()
    bpy.context.scene.collection.objects.link(c)
    if loc is not None:
        c.location = loc
    if rot is not None:
        c.rotation_euler = [math.radians(a) for a in rot]
    if scale is not None:
        c.scale = scale if hasattr(scale, "__len__") else (scale, scale, scale)
    return c


def set_material(ob, mat, faces=None):
    """Give ob a second material on some faces (a list of polygon indices)."""
    me = ob.data
    if material(mat).name not in [m.name for m in me.materials]:
        me.materials.append(material(mat))
    idx = [m.name for m in me.materials].index(mat)
    for i in (faces if faces is not None else range(len(me.polygons))):
        me.polygons[i].material_index = idx


# ==============================================================================================
# Primitives (metres; centred on loc unless said)
# ==============================================================================================

def _bevel_all(bm, width, segments=3, angle=math.radians(30)):
    if width <= 0:
        return
    edges = [e for e in bm.edges if e.calc_face_angle(0.0) > angle]
    if edges:
        bmesh.ops.bevel(bm, geom=edges, offset=width, offset_type="OFFSET", segments=segments, profile=0.5,
                        affect="EDGES", clamp_overlap=True)


def box(name, size, loc=(0, 0, 0), mat=None, bevel=0.01, rot=(0, 0, 0), segments=3):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    _bevel_all(bm, min(bevel, min(size) * 0.45), segments)
    return place(obj_from_bm(name, bm, mat), loc, rot)


def box_minmax(name, lo, hi, mat=None, bevel=0.01):
    size = [hi[i] - lo[i] for i in range(3)]
    centre = [(hi[i] + lo[i]) / 2 for i in range(3)]
    return box(name, size, centre, mat, bevel)


def cyl(name, r, h, loc=(0, 0, 0), mat=None, seg=32, bevel=0.004, rot=(0, 0, 0), r2=None, caps=True, swap_uv=False):
    """A cylinder (or cone, r2 at the top) along Z, centred, its side UVs laid round it."""
    r2 = r if r2 is None else r2
    prof = [(0.0, -h / 2), (r, -h / 2), (r2, h / 2), (0.0, h / 2)] if caps else [(r, -h / 2), (r2, h / 2)]
    ob = lathe(name, prof, mat=mat, seg=seg, bevel=bevel, swap_uv=swap_uv)
    return place(ob, loc, rot)


def lathe(name, profile, loc=(0, 0, 0), mat=None, seg=32, rot=(0, 0, 0), bevel=0.0, sharp=SHARP, swap_uv=False):
    """A surface of revolution about Z: profile [(radius, z)] bottom to top. UVs: u round the
    circumference in metres at the larger radius, v along the profile in metres (so brushed
    metal on an Anisotropic material runs round it)."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    rings = []
    for (r, z) in profile:
        ring = []
        for k in range(seg):
            a = 2 * math.pi * k / seg
            ring.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z)) if r > 1e-6 else None)
        if r <= 1e-6:
            ring = [bm.verts.new((0, 0, z))] * seg
        rings.append(ring)
    vacc = 0.0
    vs = [0.0]
    for i in range(1, len(profile)):
        vacc += math.hypot(profile[i][0] - profile[i - 1][0], profile[i][1] - profile[i - 1][1])
        vs.append(vacc)
    rmax = max(p[0] for p in profile) or 1.0
    for i in range(len(profile) - 1):
        ra, rb = rings[i], rings[i + 1]
        for k in range(seg):
            k1 = (k + 1) % seg
            quad = [ra[k], ra[k1], rb[k1], rb[k]]
            uniq = []
            for v in quad:
                if v not in uniq:
                    uniq.append(v)
            if len(uniq) < 3:
                continue
            try:
                f = bm.faces.new(uniq)
            except ValueError:
                continue
            u0 = 2 * math.pi * rmax * k / seg
            u1 = 2 * math.pi * rmax * (k + 1) / seg
            coords = {id(ra[k]): (u0, vs[i]), id(ra[k1]): (u1, vs[i]), id(rb[k1]): (u1, vs[i + 1]),
                      id(rb[k]): (u0, vs[i + 1])}
            for lp, v in zip(f.loops, uniq):
                lp[uv].uv = coords[id(v)][::-1] if swap_uv else coords[id(v)]
            # A collapsed centre vertex appears in two corners: give each its own u.
            if len(uniq) == 3:
                for lp in f.loops:
                    if lp.vert is ra[k] and ra[k] is ra[k1]:
                        lp[uv].uv = ((u0 + u1) / 2, vs[i])
                    if lp.vert is rb[k] and rb[k] is rb[k1]:
                        lp[uv].uv = ((u0 + u1) / 2, vs[i + 1])
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    if bevel > 0:
        _bevel_all(bm, bevel, 2, math.radians(40))
    ob = obj_from_bm(name, bm, mat, keep_uv=True, sharp=sharp)
    return place(ob, loc, rot)


def torus(name, R, r, loc=(0, 0, 0), mat=None, seg=48, rseg=12, rot=(0, 0, 0), arc=360.0):
    """A torus about Z (or an arc of one, open ends capped). UV u along the ring (metres)."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    full = arc >= 359.9
    nseg = seg if full else max(2, int(seg * arc / 360.0))
    rings = []
    for k in range(nseg + (0 if full else 1)):
        a = math.radians(arc) * k / (seg if full else nseg)
        ca, sa = math.cos(a), math.sin(a)
        ring = []
        for j in range(rseg):
            b = 2 * math.pi * j / rseg
            rr = R + r * math.cos(b)
            ring.append(bm.verts.new((rr * ca, rr * sa, r * math.sin(b))))
        rings.append(ring)
    count = nseg if full else nseg
    for k in range(count):
        ra, rb = rings[k], rings[(k + 1) % len(rings)]
        for j in range(rseg):
            j1 = (j + 1) % rseg
            f = bm.faces.new([ra[j], rb[j], rb[j1], ra[j1]])
            u0 = math.radians(arc) * R * k / (seg if full else nseg)
            u1 = math.radians(arc) * R * (k + 1) / (seg if full else nseg)
            v0, v1 = 2 * math.pi * r * j / rseg, 2 * math.pi * r * (j + 1) / rseg
            for lp, c in zip(f.loops, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]):
                lp[uv].uv = c
    if not full:
        for ring in (rings[0], rings[-1]):
            f = bm.faces.new(ring)
            for lp in f.loops:
                lp[uv].uv = (lp.vert.co.z, lp.vert.co.x)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = obj_from_bm(name, bm, mat, keep_uv=True, sharp=math.radians(60))
    return place(ob, loc, rot)


def sweep(name, points, r, mat=None, seg=12, closed_ends=True, profile=None):
    """A tube along a polyline (points in metres), corners mitred. UV u along the path
    (metres), v round it. profile: [(x, y)] a closed cross-section instead of a circle."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    pts = [Vector(p) for p in points]
    if profile is None:
        profile = [(r * math.cos(2 * math.pi * j / seg), r * math.sin(2 * math.pi * j / seg)) for j in range(seg)]
    n = len(profile)
    dirs = []
    for i in range(len(pts)):
        if i == 0:
            d = (pts[1] - pts[0]).normalized()
        elif i == len(pts) - 1:
            d = (pts[-1] - pts[-2]).normalized()
        else:
            d = ((pts[i] - pts[i - 1]).normalized() + (pts[i + 1] - pts[i]).normalized()).normalized()
        dirs.append(d)
    ref = Vector((0, 0, 1)) if abs(dirs[0].z) < 0.9 else Vector((1, 0, 0))
    side = dirs[0].cross(ref).normalized()
    rings = []
    lengths = [0.0]
    for i in range(1, len(pts)):
        lengths.append(lengths[-1] + (pts[i] - pts[i - 1]).length)
    for i, (p, d) in enumerate(zip(pts, dirs)):
        side = (side - d * side.dot(d)).normalized()
        up = d.cross(side)
        widen = 1.0
        if 0 < i < len(pts) - 1:
            incoming = (pts[i] - pts[i - 1]).normalized()
            widen = 1.0 / max(0.35, incoming.dot(d))
            bend = incoming - d * incoming.dot(d)
            bend = bend.normalized() if bend.length > 1e-6 else None
        else:
            bend = None
        ring = []
        for (x, y) in profile:
            off = side * x + up * y
            if bend is not None:
                off = off + bend * off.dot(bend) * (widen - 1.0)
            ring.append(bm.verts.new(p + off))
        rings.append(ring)
    perim = sum(math.hypot(profile[(j + 1) % n][0] - profile[j][0], profile[(j + 1) % n][1] - profile[j][1])
                for j in range(n))
    for i in range(len(rings) - 1):
        acc = 0.0
        for j in range(n):
            j1 = (j + 1) % n
            seglen = math.hypot(profile[j1][0] - profile[j][0], profile[j1][1] - profile[j][1])
            f = bm.faces.new([rings[i][j], rings[i + 1][j], rings[i + 1][j1], rings[i][j1]])
            for lp, c in zip(f.loops, [(lengths[i], acc), (lengths[i + 1], acc), (lengths[i + 1], acc + seglen),
                                       (lengths[i], acc + seglen)]):
                lp[uv].uv = c
            acc += seglen
    if closed_ends:
        for ring in (rings[0], rings[-1]):
            f = bm.faces.new(ring)
            for lp in f.loops:
                lp[uv].uv = (lp.vert.co.x, lp.vert.co.y)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return obj_from_bm(name, bm, mat, keep_uv=True, sharp=math.radians(50))


def prism(name, outline, depth, loc=(0, 0, 0), mat=None, bevel=0.005, rot=(0, 0, 0), holes=()):
    """An outline [(x, z)] in the XZ plane (counter-clockwise seen from -Y) extruded depth
    along +Y from y = -depth / 2. holes: outlines cut through (made with a grid fill)."""
    bm = bmesh.new()
    verts = [bm.verts.new((x, -depth / 2, z)) for (x, z) in outline]
    edges = [bm.edges.new((verts[i], verts[(i + 1) % len(verts)])) for i in range(len(verts))]
    for h in holes:
        hv = [bm.verts.new((x, -depth / 2, z)) for (x, z) in h]
        edges += [bm.edges.new((hv[i], hv[(i + 1) % len(hv)])) for i in range(len(hv))]
    bmesh.ops.triangle_fill(bm, use_beauty=True, use_dissolve=False, edges=edges)
    if holes:
        # triangle_fill fills the holes too: drop faces whose centre lies inside a hole.
        from mathutils.geometry import intersect_point_tri_2d  # noqa: F401
        for f in list(bm.faces):
            c = f.calc_center_median()
            for h in holes:
                if _inside(c.x, c.z, h):
                    bm.faces.remove(f)
                    break
    bmesh.ops.dissolve_limit(bm, angle_limit=0.01, verts=bm.verts, edges=bm.edges)
    faces = list(bm.faces)
    for f in faces:
        f.normal_update()
        if f.normal.y > 0:
            f.normal_flip()
    res = bmesh.ops.extrude_face_region(bm, geom=faces)
    moved = [g for g in res["geom"] if isinstance(g, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, vec=(0, depth, 0), verts=moved)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    _bevel_all(bm, bevel, 2)
    return place(obj_from_bm(name, bm, mat), loc, rot)


def _inside(x, z, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, zi = poly[i]
        xj, zj = poly[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside


def rock(name, size, seed, loc=(0, 0, 0), mat="Rock", detail=3, flat=0.6, sharpness=0.5):
    """A boulder: a subdivided icosphere pushed out by layered noise, flattened at its base,
    with facets (planar cuts) so it reads as fractured stone rather than a potato."""
    import random
    rnd = random.Random(seed)
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=detail, radius=1.0)
    from mathutils import noise
    off = Vector((rnd.uniform(-50, 50), rnd.uniform(-50, 50), rnd.uniform(-50, 50)))
    cuts = [(Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-0.3, 1))).normalized(),
             rnd.uniform(0.62, 0.85)) for _ in range(7)]
    for v in bm.verts:
        p = v.co.copy()
        n1 = noise.fractal(p * 1.3 + off, 0.6, 2.0, 4)
        n2 = noise.fractal(p * 4.0 + off * 2, 0.5, 2.0, 3)
        rr = 1.0 + 0.28 * n1 + 0.06 * n2
        q = p * rr
        for (d, dist) in cuts:
            k = q.dot(d)
            if k > dist:
                q -= d * (k - dist) * (0.6 + 0.4 * sharpness)
        v.co = q
    for v in bm.verts:
        v.co.x *= size[0] / 2
        v.co.y *= size[1] / 2
        v.co.z *= size[2] / 2
        if v.co.z < -size[2] * 0.3:
            v.co.z = -size[2] * 0.3 + (v.co.z + size[2] * 0.3) * 0.25
    zmin = min(v.co.z for v in bm.verts)
    for v in bm.verts:
        v.co.z -= zmin + size[2] * 0.06        # sunk a little into the ground
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = obj_from_bm(name, bm, mat, sharp=math.radians(48))
    return place(ob, loc)


def rivet_ring(name, radius, count, z, mat="BrassAged", head=0.012, axis_rot=(0, 0, 0)):
    """count domed rivets round a circle (about Z) at height z."""
    obs = []
    base = lathe(name, [(0.0, 0.0), (head, 0.0), (head * 0.85, head * 0.45), (head * 0.4, head * 0.8),
                        (0.0, head * 0.9)], mat=mat, seg=8)
    for k in range(count):
        a = 2 * math.pi * k / count
        c = dup(base) if k else base
        c.location = (radius * math.cos(a), radius * math.sin(a), z)
        c.rotation_euler = (0, math.radians(90), a)
        obs.append(c)
    return obs


def rivet_row(name, start, end, count, normal_rot, mat="BrassAged", head=0.012):
    """count rivets from start to end (metres), each turned by normal_rot (degrees; a rivet
    points +Z unturned)."""
    obs = []
    base = lathe(name, [(0.0, 0.0), (head, 0.0), (head * 0.85, head * 0.45), (head * 0.4, head * 0.8),
                        (0.0, head * 0.9)], mat=mat, seg=8)
    s, e = Vector(start), Vector(end)
    for k in range(count):
        t = k / max(1, count - 1)
        c = dup(base) if k else base
        c.location = s.lerp(e, t)
        c.rotation_euler = [math.radians(a) for a in normal_rot]
        obs.append(c)
    return obs


# ==============================================================================================
# Export
# ==============================================================================================

RECORDS = []


def _mesh_arrays(ob, dg, pivot):
    """Per-corner arrays of ob's evaluated mesh, triangulated, in Blender space less pivot."""
    eo = ob.evaluated_get(dg)
    me = eo.to_mesh()
    M = ob.matrix_world.copy()
    M = Matrix.Translation(-Vector(pivot)) @ M
    R = M.to_3x3()
    nM = R.inverted_safe().transposed()
    nv = len(me.vertices)
    co = np.empty(nv * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(nv, 3)
    co = (np.asarray(R, np.float64) @ co.T).T + np.asarray(M.translation)
    nl = len(me.loops)
    if nl == 0:
        eo.to_mesh_clear()
        return None
    loop_vert = np.empty(nl, np.int32)
    me.loops.foreach_get("vertex_index", loop_vert)
    cn = np.empty(nl * 3, np.float32)
    me.corner_normals.foreach_get("vector", cn)
    cn = (np.asarray(nM, np.float64) @ cn.reshape(nl, 3).T).T
    cn /= np.maximum(np.linalg.norm(cn, axis=1, keepdims=True), 1e-9)
    npoly = len(me.polygons)
    keep = np.zeros(npoly, np.int32)
    if "uvkeep" in me.attributes:
        me.attributes["uvkeep"].data.foreach_get("value", keep)
    poly_loop_start = np.empty(npoly, np.int32)
    poly_loop_total = np.empty(npoly, np.int32)
    me.polygons.foreach_get("loop_start", poly_loop_start)
    me.polygons.foreach_get("loop_total", poly_loop_total)
    loop_poly = np.repeat(np.arange(npoly), poly_loop_total)
    pn = np.empty(npoly * 3, np.float32)
    me.polygons.foreach_get("normal", pn)
    pn = (np.asarray(nM, np.float64) @ pn.reshape(npoly, 3).T).T
    # UV0: kept, or box-projected in asset metres.
    if "UVMap" not in me.uv_layers:
        me.uv_layers.new(name="UVMap")
    uvl = me.uv_layers["UVMap"]
    uv = np.empty(nl * 2, np.float32)
    uvl.data.foreach_get("uv", uv)
    uv = uv.reshape(nl, 2).astype(np.float64)
    if (keep[loop_poly] == 1).any():
        # Kept UVs scale with the object's scale (a lathe made at 1 m and scaled 2x is 2 m).
        s = max(abs(ob.matrix_world.to_scale()[0]), 1e-6)
        uv[keep[loop_poly] == 1] *= s
    p = co[loop_vert]
    n = pn[loop_poly]
    ax = np.argmax(np.abs(n), axis=1)
    sign = np.sign(n[np.arange(nl), ax])
    u = np.where(ax == 2, p[:, 0], np.where(ax == 0, p[:, 1] * sign, -p[:, 0] * sign))
    v = np.where(ax == 2, p[:, 1] * sign, p[:, 2])
    box_uv = np.stack([u, v], axis=1)
    sel = keep[loop_poly] == 0
    uv[sel] = box_uv[sel]
    uvl.data.foreach_set("uv", uv.astype(np.float32).ravel())
    me.calc_tangents(uvmap="UVMap")
    tan = np.empty(nl * 3, np.float32)
    me.loops.foreach_get("tangent", tan)
    tan = (np.asarray(R, np.float64) @ tan.reshape(nl, 3).T).T
    tan /= np.maximum(np.linalg.norm(tan, axis=1, keepdims=True), 1e-9)
    bsign = np.empty(nl, np.float32)
    me.loops.foreach_get("bitangent_sign", bsign)
    if np.linalg.det(np.asarray(R)) < 0:
        bsign = -bsign
    me.calc_loop_triangles()
    nt = len(me.loop_triangles)
    tri = np.empty(nt * 3, np.int32)
    me.loop_triangles.foreach_get("loops", tri)
    tri = tri.reshape(nt, 3)
    tmat = np.empty(nt, np.int32)
    me.loop_triangles.foreach_get("material_index", tmat)
    names = [m.name if m else "BrassAged" for m in me.materials] or ["BrassAged"]
    tmat_names = [names[min(i, len(names) - 1)] for i in tmat]
    # Vertex data for AO: positions and vertex normals.
    vn = np.empty(nv * 3, np.float32)
    me.vertex_normals.foreach_get("vector", vn)
    vn = (np.asarray(nM, np.float64) @ vn.reshape(nv, 3).T).T
    vn /= np.maximum(np.linalg.norm(vn, axis=1, keepdims=True), 1e-9)
    edges = np.empty(len(me.edges) * 2, np.int32)
    me.edges.foreach_get("vertices", edges)
    edges = edges.reshape(-1, 2)
    out = dict(co=co, vn=vn, edges=edges, loop_vert=loop_vert, cn=cn, uv=uv, tan=tan, bsign=bsign, tri=tri,
               tmat=tmat_names, tri_world=co[loop_vert[tri]])
    eo.to_mesh_clear()
    return out


def _bake_vertex_ao(parts, rays=20, dist=0.6, ground=False, strength=0.85):
    """Ambient occlusion per vertex of every part: rays over the hemisphere about the vertex
    normal, against every part's triangles (and the ground plane z = 0)."""
    verts, polys = [], []
    for d in parts:
        base = len(verts)
        tw = d["tri_world"].reshape(-1, 3)
        verts.extend(map(tuple, tw))
        polys.extend((base + 3 * i, base + 3 * i + 1, base + 3 * i + 2) for i in range(len(d["tri"])))
    if ground:
        base = len(verts)
        g = 200.0
        verts.extend([(-g, -g, 0.0), (g, -g, 0.0), (g, g, 0.0), (-g, g, 0.0)])
        polys.extend([(base, base + 1, base + 2), (base, base + 2, base + 3)])
    bvh = BVHTree.FromPolygons(verts, polys, all_triangles=True, epsilon=0.0)
    rng = np.random.default_rng(7)
    # Cosine-weighted directions about +Z, reused (rotated) for every vertex.
    u1, u2 = rng.random(rays), rng.random(rays)
    rr = np.sqrt(u1)
    th = 2 * np.pi * u2
    local = np.stack([rr * np.cos(th), rr * np.sin(th), np.sqrt(1 - u1)], axis=1)
    for d in parts:
        co, vn = d["co"], d["vn"]
        ao = np.ones(len(co))
        for i in range(len(co)):
            n = Vector(vn[i])
            t = n.orthogonal().normalized()
            b = n.cross(t)
            o = Vector(co[i]) + n * 0.003
            occ = 0.0
            for (x, y, z) in local:
                dirv = t * x + b * y + n * z
                hit = bvh.ray_cast(o, dirv, dist)
                if hit[0] is not None:
                    occ += 1.0 - (hit[3] / dist) ** 0.5
            ao[i] = 1.0 - strength * occ / rays
        d["ao"] = np.clip(ao, 0.0, 1.0)
        # Convexity: neighbours below the tangent plane make an outside edge.
        e = d["edges"]
        acc = np.zeros(len(co))
        cnt = np.zeros(len(co))
        for a, b in ((0, 1), (1, 0)):
            dv = co[e[:, b]] - co[e[:, a]]
            dv /= np.maximum(np.linalg.norm(dv, axis=1, keepdims=True), 1e-9)
            dots = np.einsum("ij,ij->i", dv, vn[e[:, a]])
            np.add.at(acc, e[:, a], dots)
            np.add.at(cnt, e[:, a], 1)
        conv = -acc / np.maximum(cnt, 1)
        d["edge"] = np.clip((conv - 0.04) / 0.3, 0.0, 1.0)


def _check_frame(P, N, UV, TAN, idx):
    """MikkTSpace's frame, in the engine: T along +U and w x cross(N, T) along +V, for most
    triangles (a few degenerate or mirrored ones may disagree)."""
    tri = idx.reshape(-1, 3)
    p0, p1, p2 = P[tri[:, 0]], P[tri[:, 1]], P[tri[:, 2]]
    t0, t1, t2 = UV[tri[:, 0]], UV[tri[:, 1]], UV[tri[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = t1 - t0, t2 - t0
    det = d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]
    ok = np.abs(det) > 1e-9
    r = 1.0 / np.where(ok, det, 1.0)
    dpdu = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r[:, None]
    dpdv = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) * r[:, None]
    n = N[tri[:, 0]]
    t = TAN[tri[:, 0], :3]
    w = TAN[tri[:, 0], 3:4]
    b = w * np.cross(n, t)
    along_u = np.einsum("ij,ij->i", t, dpdu)[ok] > 0
    along_v = np.einsum("ij,ij->i", b, dpdv)[ok] > 0
    face = np.cross(e1, e2)
    wind = np.einsum("ij,ij->i", face, n) > 0
    return along_u.mean() if len(along_u) else 1.0, along_v.mean() if len(along_v) else 1.0, wind.mean()


def export(name, objects, mesh_rel, pivot=(0, 0, 0), ao=True, ground=False, ao_dist=0.6, record=True, kit=None,
           pivot_note="", notes="", parts=None, check=True):
    """Writes objects (their evaluated meshes, parts of one asset) as one .nmmesh at
    Content/<mesh_rel>, positions relative to pivot (Blender metres). Returns its record."""
    dg = bpy.context.evaluated_depsgraph_get()
    arrays = [a for a in (_mesh_arrays(ob, dg, pivot) for ob in objects) if a is not None]
    if ao:
        _bake_vertex_ao(arrays, dist=ao_dist, ground=ground)
    P, N, UV, TAN, COL, MAT = [], [], [], [], [], []
    for a in arrays:
        tri = a["tri"].reshape(-1)
        lv = a["loop_vert"][tri]
        P.append(a["co"][lv])
        N.append(a["cn"][tri])
        UV.append(a["uv"][tri])
        TAN.append(np.concatenate([a["tan"][tri], a["bsign"][tri][:, None]], axis=1))
        if ao:
            occ = a["ao"][lv]
            COL.append(np.stack([occ, occ, occ, a["edge"][lv]], axis=1))
        else:
            COL.append(np.ones((len(tri), 4)))
        MAT.extend(a["tmat"])
    P, N, UV, TAN, COL = (np.concatenate(x) for x in (P, N, UV, TAN, COL))
    # Engine frame: (x, y, z) -> (x, z, y) x 100; V down the image; winding reversed.
    Pe = P[:, [0, 2, 1]] * 100.0
    Ne = N[:, [0, 2, 1]]
    Te = np.concatenate([TAN[:, [0, 2, 1]], TAN[:, 3:4]], axis=1)
    UVe = np.stack([UV[:, 0], 1.0 - UV[:, 1]], axis=1)
    ntri = len(MAT)
    order_tri = np.arange(ntri * 3).reshape(ntri, 3)[:, [0, 2, 1]]
    # Sections: triangles grouped by material, in first-seen order.
    mats = []
    for m in MAT:
        if m not in mats:
            mats.append(m)
    mat_index = np.array([mats.index(m) for m in MAT])
    sort = np.argsort(mat_index, kind="stable")
    corner = order_tri[sort].reshape(-1)
    # One vertex per distinct corner.
    key = np.concatenate([np.round(Pe, 3), np.round(Ne, 4), np.round(UVe, 5), np.round(Te, 3), np.round(COL, 3)],
                         axis=1)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    inverse = inverse.reshape(-1)
    indices = inverse[corner].astype(np.uint32)
    sections = []
    at = 0
    counts = np.bincount(mat_index, minlength=len(mats))
    for i, m in enumerate(mats):
        c = int(counts[i]) * 3
        sections.append((m, material_path(m), at, c))
        at += c
    path = GAME / "Content" / mesh_rel
    lo, hi = nm.write_mesh_arrays(path, Pe[first], Ne[first], UVe[first], indices, sections, colours=COL[first],
                                  tangents=Te[first])
    if check:
        au, av, wind = _check_frame(Pe[first], Ne[first], UVe[first], Te[first], indices)
        if au < 0.9 or av < 0.9 or wind < 0.95:
            print(f"WARNING {mesh_rel}: tangent frame along U {au:.2f}, along V {av:.2f}, winding {wind:.2f}")
    rec = dict(name=name, kit=kit, mesh=f"Content/{mesh_rel}", materials=[material_path(m) for m in mats],
               size_m=[round(float(hi[i] - lo[i]) / 100.0, 3) for i in range(3)],
               bounds_cm=[[round(float(x), 1) for x in lo], [round(float(x), 1) for x in hi]],
               triangles=int(len(indices) // 3), vertices=int(len(first)), pivot=pivot_note)
    if notes:
        rec["notes"] = notes
    if parts:
        rec["parts"] = parts
    print(f"  {mesh_rel}: {rec['triangles']} tris, {rec['vertices']} verts, {rec['size_m']} m, {len(mats)} sections")
    if record:
        RECORDS.append(rec)
    return rec


def engine_offset(blender_point):
    """A Blender point (metres) as an engine offset in centimetres."""
    x, y, z = blender_point
    return [round(x * 100.0, 2), round(z * 100.0, 2), round(y * 100.0, 2)]


def engine_axis(blender_axis):
    x, y, z = blender_axis
    return [x, z, y]


def part(name, objects, mesh_rel, joint, asset_pivot=(0, 0, 0), moves="", axis=None, kit=None, ao_with=None,
         ground=False):
    """A moving part: exported with its pivot at joint (Blender metres), recorded with its
    offset from the asset's pivot (engine centimetres) and how it moves. ao_with: other
    objects that occlude it in its rest pose (their AO is not written)."""
    rec = export(name, objects, mesh_rel, pivot=joint, record=False, kit=kit, ground=ground)
    j = Vector(joint) - Vector(asset_pivot)
    out = dict(name=name, mesh=rec["mesh"], materials=rec["materials"], offset_cm=engine_offset(j),
               size_m=rec["size_m"], moves=moves)
    if axis is not None:
        out["axis"] = engine_axis(axis)
    return out


def clear():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    for me in list(bpy.data.meshes):
        bpy.data.meshes.remove(me)


def save_records(kit):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / f"manifest_{kit}.json"
    path.write_text(json.dumps(RECORDS, indent=1))
    print(f"{kit}: {len(RECORDS)} assets -> {path}")


def uv_box(name, size, loc=(0, 0, 0), mat=None, faces=None, rot=(0, 0, 0)):
    """A box whose faces take UV rectangles of an atlas: faces maps '-y', '+y', '-x', '+x',
    '-z', '+z' to (u0, v0, u1, v1) in Blender's UV (v up); others get the '*' entry."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    faces = faces or {}
    for f in bm.faces:
        n = f.normal
        ax = max(range(3), key=lambda i: abs(n[i]))
        key = ("-" if n[ax] < 0 else "+") + "xyz"[ax]
        u0, v0, u1, v1 = faces.get(key, faces.get("*", (0, 0, 1, 1)))
        # Local in-face axes: horizontal and vertical on the face.
        if ax == 2:
            ua, va = 0, 1
        else:
            ua, va = (1 if ax == 0 else 0), 2
        lo = [min(v.co[i] for v in f.verts) for i in range(3)]
        hi = [max(v.co[i] for v in f.verts) for i in range(3)]
        for lp in f.loops:
            s = (lp.vert.co[ua] - lo[ua]) / max(hi[ua] - lo[ua], 1e-9)
            t = (lp.vert.co[va] - lo[va]) / max(hi[va] - lo[va], 1e-9)
            if key in ("+y", "-x"):
                s = 1 - s
            lp[uv].uv = (u0 + (u1 - u0) * s, v0 + (v1 - v0) * t)
    ob = obj_from_bm(name, bm, mat, keep_uv=True)
    return place(ob, loc, rot)


def disc(name, r, loc=(0, 0, 0), mat=None, seg=32, rot=(0, 0, 0)):
    """A disc in the XZ plane facing -Y, UV 0-1 across it (+Z up the image)."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    vs = [bm.verts.new((r * math.cos(2 * math.pi * k / seg), 0.0, r * math.sin(2 * math.pi * k / seg)))
          for k in range(seg)]
    f = bm.faces.new(vs)
    f.normal_update()
    if f.normal.y > 0:
        f.normal_flip()
    for lp in f.loops:
        lp[uv].uv = (0.5 + lp.vert.co.x / (2 * r), 0.5 + lp.vert.co.z / (2 * r))
    ob = obj_from_bm(name, bm, mat, keep_uv=True)
    return place(ob, loc, rot)


def sphere(name, r, loc=(0, 0, 0), mat=None, seg=32, rings=16, squash=1.0):
    prof = [(r * math.sin(math.pi * i / rings), -r * squash * math.cos(math.pi * i / rings)) for i in range(rings + 1)]
    return lathe(name, prof, loc=loc, mat=mat, seg=seg, sharp=math.radians(80))
