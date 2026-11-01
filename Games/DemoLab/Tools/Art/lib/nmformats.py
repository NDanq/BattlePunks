"""
nmformats.py -- the engine's content files, written from Python, for the
Steampunk generators beside it (no engine code involved):

    .nmtexture   RenderCore's TextureAsset.cpp: NTEX { THDR v3, TPIX, TMIP }
                 -- RGBA8 (sRGB colour or linear data) with a Box mip chain,
                 or RGBE8 (an HDRI, for a Sky Light's Environment), no mips.
    .nmmesh      RenderCore's MeshAsset.cpp: NMSH { MHDR, MVTX, MIDX, MSEC,
                 MCOL, MUV1 } -- sections that name their own material.
    .nmentity    GameCore's text world: one XML file per entity, beside a
                 .nmworld manifest (as the editor's File > Save writes them).

Every helper is deterministic: running a generator again writes the same bytes.
"""

import hashlib
import math
import struct
from pathlib import Path

import numpy as np

# ==============================================================================================
# Chunks (EngineCore::Archive: tag, version, size, payload; little-endian)
# ==============================================================================================


def tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


def chunk(name, payload, version=1):
    return struct.pack("<IHI", tag(name), version, len(payload)) + payload


def string(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


# ==============================================================================================
# Textures
# ==============================================================================================

def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((np.maximum(c, 0.0) + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0.0), 1.0 / 2.4) - 0.055)


def _box_mips(level0, srgb):
    """Every level after level0 down to 1 x 1, as TextureAsset's Box mips: colour in linear light."""
    levels = []
    data = np.concatenate([to_linear(level0[..., :3]), level0[..., 3:]], axis=-1) if srgb else level0.copy()
    while data.shape[0] > 1 and data.shape[1] > 1:
        h, w = data.shape[0] // 2, data.shape[1] // 2
        data = data.reshape(h, 2, w, 2, 4).mean(axis=(1, 3))
        levels.append(np.concatenate([to_srgb(data[..., :3]), data[..., 3:]], axis=-1) if srgb else data.copy())
    return levels


def _bytes(image):
    return (np.clip(image, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).tobytes()


def write_texture(path, rgba, srgb=True, wrap="repeat", normal_map=False):
    """rgba: H x W x 4 (or x 3) floats 0-1, row 0 the image's top. A normal map's
    mips are renormalised as they shrink, so distant bumps fade rather than tilt."""
    if rgba.shape[-1] == 3:
        rgba = np.concatenate([rgba, np.ones(rgba.shape[:2] + (1,))], axis=-1)
    height, width = rgba.shape[:2]
    mips = _box_mips(rgba, srgb)
    if normal_map:
        fixed = []
        for level in mips:
            n = level[..., :3] * 2.0 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            fixed.append(np.concatenate([n * 0.5 + 0.5, level[..., 3:]], axis=-1))
        mips = fixed
    wrap_byte = {"repeat": 0, "clamp": 1, "mirror": 2}[wrap]
    # Width, Height, Format RGBA8, sRGB, Filter Linear, Wrap, MipGen Box,
    # Compression None, MipCount, no blocks.
    header = struct.pack("<iiBBBBBBBB", width, height, 0, 1 if srgb else 0, 1, wrap_byte, 1, 0, 1 + len(mips), 0)
    data = chunk("NTEX", chunk("THDR", header, version=3) + chunk("TPIX", _bytes(rgba))
                 + chunk("TMIP", b"".join(_bytes(m) for m in mips)))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def encode_rgbe(rgb):
    """Linear HDR RGB (H x W x 3) to Radiance RGBE8 bytes (H x W x 4), as TextureAsset's EncodeRgbe."""
    peak = rgb.max(axis=-1)
    out = np.zeros(rgb.shape[:2] + (4,), dtype=np.uint8)
    lit = peak > 1e-32
    mantissa, exponent = np.frexp(np.where(lit, peak, 1.0))
    factor = np.where(lit, mantissa * 256.0 / np.where(lit, peak, 1.0), 0.0)
    out[..., :3] = np.clip(rgb * factor[..., None], 0, 255).astype(np.uint8)
    out[..., 3] = np.where(lit, exponent + 128, 0).astype(np.uint8)
    return out


def write_hdri(path, rgb):
    """An equirectangular HDRI (linear RGB, row 0 straight up) as RGBE8: linear, point sampled, no mips."""
    height, width = rgb.shape[:2]
    header = struct.pack("<iiBBBBBBBB", width, height, 1, 0, 0, 0, 0, 0, 1, 0)
    data = chunk("NTEX", chunk("THDR", header, version=3) + chunk("TPIX", encode_rgbe(rgb).tobytes()))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


# ==============================================================================================
# Meshes (engine frame: left-handed, +X right, +Y up, +Z away; 1 unit = 1 cm)
# ==============================================================================================

def v_add(a, b):   return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def v_sub(a, b):   return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def v_mul(a, s):   return (a[0] * s, a[1] * s, a[2] * s)
def v_dot(a, b):   return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def v_cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def v_len(a):      return math.sqrt(v_dot(a, a))


def v_norm(a):
    length = v_len(a)
    return (a[0] / length, a[1] / length, a[2] / length) if length > 1e-12 else (0.0, 1.0, 0.0)


class MeshBuilder:
    """Triangles grouped by material: each material one section, drawn with
    that material graph unless a Render component's slot names another.

    UV0 is in metres of the surface (1 UV = 100 units), so a material's
    UVScale (1 / the metres its texture covers) tiles it the same everywhere.
    Vertex colour carries baked grime and occlusion (the materials multiply
    their base colour by it). UV1, the lightmap UVs, is laid out at the end:
    each planar chart packed into 0-1 without overlap."""

    def __init__(self, name):
        self.name = name
        self.sections = {}       # material -> list of triangles (each 3 vertices)
        self.order = []

    # A vertex: (position, normal, uv, colour, chart)
    def _tris(self, material):
        if material not in self.sections:
            self.sections[material] = []
            self.order.append(material)
        return self.sections[material]

    def polygon(self, material, points, normal=None, uvs=None, colours=None, uv_origin=None, uv_axes=None,
                chart=None):
        """A convex planar polygon (points in order, facing normal -- found from
        the winding when None: counter-clockwise seen from the front). UVs from
        its plane when not given: u along uv_axes[0], v along uv_axes[1]
        (metres), each axis picked from the normal when not given."""
        if normal is None:
            acc = (0.0, 0.0, 0.0)
            for i in range(1, len(points) - 1):
                acc = v_add(acc, v_cross(v_sub(points[i], points[0]), v_sub(points[i + 1], points[0])))
            normal = v_norm(acc)
        if uvs is None:
            u_axis, v_axis = uv_axes if uv_axes else plane_axes(normal)
            origin = uv_origin if uv_origin is not None else (0.0, 0.0, 0.0)
            uvs = [(v_dot(v_sub(p, origin), u_axis) / 100.0, v_dot(v_sub(p, origin), v_axis) / 100.0) for p in points]
        if colours is None:
            colours = [(1.0, 1.0, 1.0, 1.0)] * len(points)
        chart = chart if chart is not None else object()
        tris = self._tris(material)
        for i in range(1, len(points) - 1):
            a, b, c = 0, i, i + 1
            face = v_cross(v_sub(points[b], points[a]), v_sub(points[c], points[a]))
            if v_dot(face, face) < 1e-10:
                continue
            if v_dot(face, normal) < 0.0:
                b, c = c, b
            tris.append(tuple((points[k], normal, uvs[k], colours[k], chart) for k in (a, b, c)))

    def quad(self, material, p0, p1, p2, p3, **kw):
        self.polygon(material, [p0, p1, p2, p3], **kw)

    def triangle_raw(self, material, verts, chart=None):
        """verts: three (position, normal, uv, colour) -- smooth shading's way in."""
        chart = chart if chart is not None else object()
        tris = self._tris(material)
        a, b, c = verts
        face = v_cross(v_sub(b[0], a[0]), v_sub(c[0], a[0]))
        if v_dot(face, face) < 1e-10:
            return
        avg = v_add(v_add(a[1], b[1]), c[1])
        if v_dot(face, avg) < 0.0:
            b, c = c, b
        tris.append(tuple(v + (chart,) for v in (a, b, c)))

    def box(self, material, lo, hi, colour_fn=None, skip=(), uv_origin=None):
        """An axis-aligned box lo-hi; skip names faces left out ('-x', '+y', ...)."""
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        faces = {
            "-x": ([(x0, y0, z1), (x0, y0, z0), (x0, y1, z0), (x0, y1, z1)], (-1, 0, 0)),
            "+x": ([(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)], (1, 0, 0)),
            "-y": ([(x0, y0, z1), (x1, y0, z1), (x1, y0, z0), (x0, y0, z0)], (0, -1, 0)),
            "+y": ([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0)),
            "-z": ([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1)),
            "+z": ([(x1, y0, z1), (x0, y0, z1), (x0, y1, z1), (x1, y1, z1)], (0, 0, 1)),
        }
        for key, (points, normal) in faces.items():
            if key in skip:
                continue
            colours = [colour_fn(p, normal) for p in points] if colour_fn else None
            self.polygon(material, points, normal=normal, colours=colours, uv_origin=uv_origin)

    def oriented_box(self, material, centre, axes, half, colour=None):
        """A box along three orthonormal axes, half extents half."""
        corners = {}
        for sx in (-1, 1):
            for sy in (-1, 1):
                for sz in (-1, 1):
                    p = centre
                    p = v_add(p, v_mul(axes[0], sx * half[0]))
                    p = v_add(p, v_mul(axes[1], sy * half[1]))
                    p = v_add(p, v_mul(axes[2], sz * half[2]))
                    corners[(sx, sy, sz)] = p
        col = [colour or (1, 1, 1, 1)] * 4
        for axis in range(3):
            for sign in (-1, 1):
                keys = []
                others = [a for a in range(3) if a != axis]
                for s0, s1 in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                    k = [0, 0, 0]
                    k[axis], k[others[0]], k[others[1]] = sign, s0, s1
                    keys.append(tuple(k))
                normal = v_mul(axes[axis], sign)
                self.polygon(material, [corners[k] for k in keys], normal=normal, colours=col)

    def lathe(self, material, profile, centre=(0.0, 0.0, 0.0), segments=24, axis="y", colour=None, caps=False,
              v_scale=1.0):
        """A surface of revolution: profile is [(radius, height)] bottom to top
        (units), turned about the axis through centre. Smooth around, hard at
        each profile corner (each profile edge its own ring of normals)."""
        col = colour or (1.0, 1.0, 1.0, 1.0)
        chart = object()

        def place(r, h, angle):
            c, s = math.cos(angle), math.sin(angle)
            if axis == "y":
                return (centre[0] + r * c, centre[1] + h, centre[2] + r * s)
            if axis == "x":
                return (centre[0] + h, centre[1] + r * c, centre[2] + r * s)
            return (centre[0] + r * c, centre[1] + r * s, centre[2] + h)

        def radial(nr, nh, angle):
            c, s = math.cos(angle), math.sin(angle)
            if axis == "y":
                return v_norm((nr * c, nh, nr * s))
            if axis == "x":
                return v_norm((nh, nr * c, nr * s))
            return v_norm((nr * c, nr * s, nh))

        circumference_v = 0.0
        for i in range(len(profile) - 1):
            (r0, h0), (r1, h1) = profile[i], profile[i + 1]
            dr, dh = r1 - r0, h1 - h0
            length = math.hypot(dr, dh)
            if length < 1e-9:
                continue
            nr, nh = dh / length, -dr / length       # outward normal of the profile edge
            for seg in range(segments):
                a0 = 2.0 * math.pi * seg / segments
                a1 = 2.0 * math.pi * (seg + 1) / segments
                u0 = a0 * max(r0, r1, 1.0) / 100.0
                u1 = a1 * max(r0, r1, 1.0) / 100.0
                va, vb = circumference_v / 100.0 * v_scale, (circumference_v + length) / 100.0 * v_scale
                p00, p01 = place(r0, h0, a0), place(r0, h0, a1)
                p10, p11 = place(r1, h1, a0), place(r1, h1, a1)
                n0, n1 = radial(nr, nh, a0), radial(nr, nh, a1)
                verts = [(p00, n0, (u0, vb), col), (p01, n1, (u1, vb), col), (p11, n1, (u1, va), col),
                         (p10, n0, (u0, va), col)]
                if r0 > 1e-6 or r1 > 1e-6:
                    self.triangle_raw(material, [verts[0], verts[1], verts[2]], chart)
                    self.triangle_raw(material, [verts[0], verts[2], verts[3]], chart)
            circumference_v += length
        if caps:
            for (r, h), sign in ((profile[0], -1.0), (profile[-1], 1.0)):
                if r < 1e-6:
                    continue
                pts = [place(r, h, 2.0 * math.pi * k / segments) for k in range(segments)]
                n = radial(0.0, sign, 0.0)
                self.polygon(material, pts if sign > 0 else pts[::-1], normal=n, colours=[col] * len(pts))

    def tube(self, material, path, radius, segments=12, colour=None, closed_ends=True):
        """A pipe along a polyline path (points), radius units, mitred at the corners."""
        col = colour or (1.0, 1.0, 1.0, 1.0)
        chart = object()
        rings = []
        for i, p in enumerate(path):
            if i == 0:
                d = v_norm(v_sub(path[1], path[0]))
            elif i == len(path) - 1:
                d = v_norm(v_sub(path[-1], path[-2]))
            else:
                d = v_norm(v_add(v_norm(v_sub(path[i], path[i - 1])), v_norm(v_sub(path[i + 1], path[i]))))
            rings.append((p, d))
        # A frame carried along the path (parallel transport).
        first_dir = rings[0][1]
        ref = (0.0, 1.0, 0.0) if abs(first_dir[1]) < 0.9 else (1.0, 0.0, 0.0)
        side = v_norm(v_cross(first_dir, ref))
        frames = []
        for p, d in rings:
            side = v_norm(v_sub(side, v_mul(d, v_dot(side, d))))
            up = v_cross(d, side)
            frames.append((p, d, side, up))
        length_v = 0.0
        circ = 2.0 * math.pi * radius / 100.0
        ring_pts = []
        for idx, (p, d, side, up) in enumerate(frames):
            # A corner's ring is widened by 1/cos(half the bend) across the
            # bend, so the pipe keeps its thickness round it (a mitre).
            widen = 1.0
            if 0 < idx < len(frames) - 1:
                incoming = v_norm(v_sub(path[idx], path[idx - 1]))
                widen = 1.0 / max(0.35, v_dot(incoming, d))
                bend = v_sub(incoming, v_mul(d, v_dot(incoming, d)))
                bend = v_norm(bend) if v_len(bend) > 1e-6 else None
            pts = []
            for k in range(segments + 1):
                a = 2.0 * math.pi * k / segments
                off = v_add(v_mul(side, math.cos(a)), v_mul(up, math.sin(a)))
                pos_off = off
                if widen != 1.0 and bend is not None:
                    along = v_dot(off, bend)
                    pos_off = v_add(off, v_mul(bend, along * (widen - 1.0)))
                pts.append((v_add(p, v_mul(pos_off, radius)), off))
            ring_pts.append(pts)
        for i in range(len(ring_pts) - 1):
            seg_len = v_len(v_sub(path[i + 1], path[i])) / 100.0
            for k in range(segments):
                a, b = ring_pts[i][k], ring_pts[i][k + 1]
                c, e = ring_pts[i + 1][k + 1], ring_pts[i + 1][k]
                ua, ub = k / segments * circ, (k + 1) / segments * circ
                va, vb = length_v, length_v + seg_len
                verts = [(a[0], a[1], (ua, va), col), (e[0], e[1], (ua, vb), col), (c[0], c[1], (ub, vb), col),
                         (b[0], b[1], (ub, va), col)]
                self.triangle_raw(material, [verts[0], verts[1], verts[2]], chart)
                self.triangle_raw(material, [verts[0], verts[2], verts[3]], chart)
            length_v += seg_len
        if closed_ends:
            for idx, sign in ((0, -1.0), (len(ring_pts) - 1, 1.0)):
                pts = [q[0] for q in ring_pts[idx][:-1]]
                n = v_mul(frames[idx][1], sign)
                self.polygon(material, pts, normal=n, colours=[col] * len(pts))

    # ------------------------------------------------------------------------------------------

    def merge(self, other, offset=(0.0, 0.0, 0.0)):
        for material in other.order:
            tris = self._tris(material)
            for tri in other.sections[material]:
                tris.append(tuple((v_add(v[0], offset),) + v[1:] for v in tri))

    def triangle_count(self):
        return sum(len(t) for t in self.sections.values())

    def _lightmap_charts(self):
        """UV1: each chart's triangles projected on its own plane, shelf-packed."""
        charts = {}
        for material in self.order:
            for t_index, tri in enumerate(self.sections[material]):
                charts.setdefault(id(tri[0][4]), []).append((material, t_index))
        boxes = []
        for key, members in charts.items():
            pts = []
            n_acc = (0.0, 0.0, 0.0)
            for material, t_index in members:
                tri = self.sections[material][t_index]
                n_acc = v_add(n_acc, v_cross(v_sub(tri[1][0], tri[0][0]), v_sub(tri[2][0], tri[0][0])))
            normal = v_norm(n_acc)
            u_axis, v_axis = plane_axes(normal)
            for material, t_index in members:
                for v in self.sections[material][t_index]:
                    pts.append((v_dot(v[0], u_axis), v_dot(v[0], v_axis)))
            us, vs = [p[0] for p in pts], [p[1] for p in pts]
            boxes.append((key, members, u_axis, v_axis, min(us), min(vs), max(us) - min(us), max(vs) - min(vs)))
        return boxes

    def build(self, lightmap=True):
        positions, normals, uvs, colours, uv1s, indices, sections = [], [], [], [], [], [], []
        uv1_of = {}
        if lightmap:
            boxes = self._lightmap_charts()
            # Charts with no area (degenerate) are skipped; the rest sorted tall first.
            area = sum(max(b[6], 1.0) * max(b[7], 1.0) for b in boxes)
            side = math.sqrt(area) * 1.15
            pad_frac = 0.004
            for attempt in range(40):
                padding = side * pad_frac + 2.0
                x = y = shelf = 0.0
                placed = {}
                ok = True
                for b in sorted(boxes, key=lambda b: -b[7]):
                    w, h = b[6] + padding, b[7] + padding
                    if x + w > side:
                        x, y, shelf = 0.0, y + shelf, 0.0
                    if w > side:
                        ok = False
                        break
                    placed[b[0]] = (x + padding * 0.5, y + padding * 0.5)
                    x += w
                    shelf = max(shelf, h)
                if ok and y + shelf <= side:
                    break
                side *= 1.08
            for b in boxes:
                key, members, u_axis, v_axis, u0, v0, _, _ = b
                ox, oy = placed[key]
                for material, t_index in members:
                    tri = self.sections[material][t_index]
                    for k, v in enumerate(tri):
                        uv1_of[(material, t_index, k)] = ((v_dot(v[0], u_axis) - u0 + ox) / side,
                                                          (v_dot(v[0], v_axis) - v0 + oy) / side)
        for material in self.order:
            first = len(indices)
            shared = {}      # identical vertices within a section are written once
            for t_index, tri in enumerate(self.sections[material]):
                for k, v in enumerate(tri):
                    uv1 = uv1_of.get((material, t_index, k), (0.0, 0.0))
                    key = (tuple(round(c, 3) for c in v[0]), tuple(round(c, 4) for c in v[1]),
                           tuple(round(c, 5) for c in v[2]), tuple(round(c, 3) for c in v[3]),
                           tuple(round(c, 6) for c in uv1))
                    at = shared.get(key)
                    if at is None:
                        positions.append(v[0])
                        normals.append(v[1])
                        uvs.append(v[2])
                        colours.append(v[3])
                        uv1s.append(uv1)
                        at = shared[key] = len(positions) - 1
                    indices.append(at)
            sections.append((material.split("/")[-1].split(".")[0], material, first, len(indices) - first))
        return positions, normals, uvs, colours, uv1s, indices, sections

    def write(self, path, lightmap=True):
        positions, normals, uvs, colours, uv1s, indices, sections = self.build(lightmap)
        lo = tuple(min(p[a] for p in positions) for a in range(3))
        hi = tuple(max(p[a] for p in positions) for a in range(3))
        header = struct.pack("<III", len(positions), len(indices), len(sections)) + struct.pack("<6f", *lo, *hi)
        floats = [c for p in positions for c in p] + [c for n in normals for c in n] + [c for t in uvs for c in t]
        payload = (chunk("MHDR", header)
                   + chunk("MVTX", struct.pack(f"<{len(floats)}f", *floats))
                   + chunk("MIDX", struct.pack(f"<{len(indices)}I", *indices))
                   + chunk("MSEC", b"".join(string(n) + string(m) + struct.pack("<II", f, c) for n, m, f, c in sections)))
        cols = [c for col in colours for c in (tuple(col) + (1.0,) * (4 - len(col)))[:4]]
        assert len(cols) == len(positions) * 4
        payload += chunk("MCOL", struct.pack(f"<{len(cols)}f", *cols))
        if lightmap:
            flat = [min(max(c, 0.0), 1.0) for t in uv1s for c in t]
            payload += chunk("MUV1", struct.pack(f"<{len(flat)}f", *flat))
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(chunk("NMSH", payload))
        return lo, hi


def plane_axes(normal):
    """Two axes across a plane facing normal: u horizontal (right as you face
    the surface), v down the surface (UV0's v runs down the image)."""
    n = v_norm(normal)
    if abs(n[1]) > 0.7:                       # floor or ceiling: u along +X, v along +Z / -Z
        return (1.0, 0.0, 0.0), (0.0, 0.0, -1.0 if n[1] > 0 else 1.0)
    # A wall: v straight down, u to the right seen from in front (left-handed: normal x up).
    up = (0.0, 1.0, 0.0)
    u = v_norm(v_cross(n, up))
    return u, (0.0, -1.0, 0.0)


# ==============================================================================================
# Text worlds
# ==============================================================================================

def num(value):
    """As the editor writes floats: shortest that round-trips, no trailing .0."""
    value = float(value)
    if abs(value) < 1e-9:
        value = 0.0
    text = repr(round(value, 4))
    return text[:-2] if text.endswith(".0") else text


def colour_attr(rgb):
    return f"R=\"{int(rgb[0])}\" G=\"{int(rgb[1])}\" B=\"{int(rgb[2])}\" A=\"255\""


def guid(scope, name):
    return hashlib.md5((scope + "/" + name).encode()).hexdigest()


def transform_xml(position, rotation=(0, 0, 0), scale=(1, 1, 1)):
    return ("  <Component Type=\"Transform\" Version=\"1\">\n"
            f"    <Position X=\"{num(position[0])}\" Y=\"{num(position[1])}\" Z=\"{num(position[2])}\" />\n"
            f"    <Rotation X=\"{num(rotation[0])}\" Y=\"{num(rotation[1])}\" Z=\"{num(rotation[2])}\" />\n"
            f"    <Scale X=\"{num(scale[0])}\" Y=\"{num(scale[1])}\" Z=\"{num(scale[2])}\" />\n"
            "  </Component>\n")


def component_xml(type_name, version, fields):
    """fields: [(name, value)] -- value a str/number/bool written as Value=, or
    ('colour', rgb) for an R G B A element."""
    lines = [f"  <Component Type=\"{type_name}\" Version=\"{version}\">\n"]
    for name, value in fields:
        if isinstance(value, tuple) and value and value[0] == "colour":
            lines.append(f"    <{name} {colour_attr(value[1])} />\n")
        elif isinstance(value, bool):
            lines.append(f"    <{name} Value=\"{'true' if value else 'false'}\" />\n")
        elif isinstance(value, (int, float)):
            lines.append(f"    <{name} Value=\"{num(value)}\" />\n")
        else:
            lines.append(f"    <{name} Value=\"{value}\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def entity_xml(entity_guid, name, components, mobility=None, parent=None, detail=None):
    """components: [(type name, xml)] -- written sorted by type name, as SaveWorldPackage does.
    detail: the scalability bucket (GameCore's DetailMode.h), "Medium" / "High" / "Ultra"; None is Low."""
    attrs = f" Guid=\"{entity_guid}\" Name=\"{name}\""
    if parent:
        attrs += f" Parent=\"{parent}\""
    if mobility:
        attrs += f" Mobility=\"{mobility}\""
    if detail:
        attrs += f" DetailMode=\"{detail}\""
    body = "".join(text for _, text in sorted(components, key=lambda c: c[0]))
    return f"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Entity{attrs}>\n{body}</Entity>\n"


# ==============================================================================================
# DemoLab additions: meshes written from arrays (Blender's exporter), with tangents and skins
# ==============================================================================================

def write_mesh_arrays(path, positions, normals, uv0, indices, sections, colours=None, uv1=None, tangents=None,
                      skin=None):
    """Arrays in engine space (numpy float32/uint32): positions N x 3 (units), normals N x 3,
    uv0 N x 2, colours N x 4, uv1 N x 2, tangents N x 4 (MikkTSpace: xyz, w the bitangent's
    sign), indices M (3 per triangle). sections: [(name, material path, first index, count)].
    skin: (skeleton path, inverse binds bytes -- 12 floats a bone, bone count, joints N x 4
    uint16, weights N x 4). Chunk order as MeshAsset.cpp reads them."""
    positions = np.asarray(positions, dtype=np.float32)
    lo, hi = positions.min(axis=0), positions.max(axis=0)
    n_vert = len(positions)
    header = struct.pack("<III", n_vert, len(indices), len(sections)) + struct.pack("<6f", *lo, *hi)
    vtx = (np.asarray(positions, np.float32).tobytes() + np.asarray(normals, np.float32).tobytes()
           + np.asarray(uv0, np.float32).tobytes())
    payload = (chunk("MHDR", header) + chunk("MVTX", vtx)
               + chunk("MIDX", np.asarray(indices, np.uint32).tobytes())
               + chunk("MSEC", b"".join(string(n) + string(m) + struct.pack("<II", f, c) for n, m, f, c in sections)))
    if colours is not None:
        assert len(colours) == n_vert
        payload += chunk("MCOL", np.asarray(colours, np.float32).tobytes())
    if uv1 is not None:
        assert len(uv1) == n_vert
        payload += chunk("MUV1", np.clip(np.asarray(uv1, np.float32), 0, 1).tobytes())
    if skin is not None:
        skeleton, inverse_binds, bone_count, joints, weights = skin
        assert len(joints) == n_vert and len(weights) == n_vert
        payload += chunk("MSKN", string(skeleton) + struct.pack("<I", bone_count) + inverse_binds
                         + np.asarray(joints, np.uint16).tobytes() + np.asarray(weights, np.float32).tobytes())
    if tangents is not None:
        assert len(tangents) == n_vert
        payload += chunk("MTAN", np.asarray(tangents, np.float32).tobytes())
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(chunk("NMSH", payload))
    return lo, hi


def _read_chunks(data, off, end):
    out = {}
    while off < end:
        t, ver, size = struct.unpack_from("<IHI", data, off)
        name = "".join(chr((t >> s) & 255) for s in (24, 16, 8, 0))
        out[name] = (off + 10, size)
        off += 10 + size
    return out


def read_mesh(path):
    """A .nmmesh as numpy arrays: dict with positions, normals, uv0, indices, sections and,
    when present, colours, uv1, tangents, skeleton, bone_count, inverse_binds (bytes),
    joints, weights."""
    d = Path(path).read_bytes()
    top = _read_chunks(d, 0, len(d))
    o, s = top["NMSH"]
    c = _read_chunks(d, o, o + s)
    nv, ni, ns = struct.unpack_from("<III", d, c["MHDR"][0])
    out = {}
    o = c["MVTX"][0]
    out["positions"] = np.frombuffer(d, np.float32, nv * 3, o).reshape(nv, 3)
    out["normals"] = np.frombuffer(d, np.float32, nv * 3, o + nv * 12).reshape(nv, 3)
    out["uv0"] = np.frombuffer(d, np.float32, nv * 2, o + nv * 24).reshape(nv, 2)
    out["indices"] = np.frombuffer(d, np.uint32, ni, c["MIDX"][0])
    o = c["MSEC"][0]
    secs = []
    for _ in range(ns):
        ln = struct.unpack_from("<I", d, o)[0]; name = d[o + 4:o + 4 + ln].decode(); o += 4 + ln
        ln = struct.unpack_from("<I", d, o)[0]; mat = d[o + 4:o + 4 + ln].decode(); o += 4 + ln
        f, cnt = struct.unpack_from("<II", d, o); o += 8
        secs.append((name, mat, f, cnt))
    out["sections"] = secs
    if "MCOL" in c:
        out["colours"] = np.frombuffer(d, np.float32, nv * 4, c["MCOL"][0]).reshape(nv, 4)
    if "MUV1" in c:
        out["uv1"] = np.frombuffer(d, np.float32, nv * 2, c["MUV1"][0]).reshape(nv, 2)
    if "MTAN" in c:
        out["tangents"] = np.frombuffer(d, np.float32, nv * 4, c["MTAN"][0]).reshape(nv, 4)
    if "MSKN" in c:
        o = c["MSKN"][0]
        ln = struct.unpack_from("<I", d, o)[0]
        out["skeleton"] = d[o + 4:o + 4 + ln].decode()
        o += 4 + ln
        bones = struct.unpack_from("<I", d, o)[0]
        o += 4
        out["bone_count"] = bones
        out["inverse_binds"] = d[o:o + bones * 48]
        o += bones * 48
        out["joints"] = np.frombuffer(d, np.uint16, nv * 4, o).reshape(nv, 4)
        out["weights"] = np.frombuffer(d, np.float32, nv * 4, o + nv * 8).reshape(nv, 4)
    return out
