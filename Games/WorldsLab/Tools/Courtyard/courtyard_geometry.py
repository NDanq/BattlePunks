#!/usr/bin/env python3
"""
courtyard_geometry.py -- the mesh kit make_courtyard_level.py builds the
Courtyard level's building from: a mesh builder with named material
sections, world-space UVs and packed lightmap charts, and the shapes the
building is made of -- walls with arched and square openings cut through
them, boxes, lathed solids, barrel-tile roof slopes and ridges.

Positions are the engine's (left-handed, +X right, +Y up, +Z forward), in
world units (centimetres): every mesh is built where it stands, so its
entity sits at the origin unrotated. Faces wind as the engine's primitives
do -- the cross product of a triangle's edges along its normal
(generate_primitive_meshes.py's Mesh.check) -- whatever order the shapes
list their corners in.

    UV0   metres: one unit a metre, so a material scales it to the size its
          texture covers. Walls, floors and ceilings are mapped in world
          space (planar), so neighbouring faces line up; members (beams,
          frames, rafters) run U along their length, for their grain.
    UV1   a mesh made with lightmap=True gets lightmap UVs: one chart per
          planar face (a whole wall side, holes and all, is one chart),
          shelf-packed into 0-1 at one texel density, a gutter apart.
    MSEC  one section per material, named for it, its Material the graph's
          path -- what an empty Render component slot falls back to.

Not run on its own: imported by make_courtyard_level.py.
"""

import hashlib
import math
import struct

# --- Vectors -------------------------------------------------------------------------------

def add(a, b):     return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b):     return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def mul(a, s):     return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b):     return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b):   return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def length(a):     return math.sqrt(dot(a, a))


def norm(a):
    size = length(a)
    return (a[0] / size, a[1] / size, a[2] / size) if size > 1e-12 else (0.0, 1.0, 0.0)


def lerp3(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def combo(origin, *terms):
    """origin + sum of axis * amount."""
    x, y, z = origin
    for axis, amount in terms:
        x += axis[0] * amount
        y += axis[1] * amount
        z += axis[2] * amount
    return (x, y, z)


X_AXIS = (1.0, 0.0, 0.0)
Y_AXIS = (0.0, 1.0, 0.0)
Z_AXIS = (0.0, 0.0, 1.0)


def rand(key, low=0.0, high=1.0):
    """A number in [low, high) from key: the same every run."""
    value = int(hashlib.md5(("Courtyard/" + key).encode()).hexdigest()[:8], 16) / 0xFFFFFFFF
    return low + (high - low) * value


def planar_uv(p, n):
    """World-space UV in metres: floors and ceilings by X and Z, everything
    else by its horizontal tangent and height (v down the image, up the world)."""
    if abs(n[1]) >= 0.7:
        return (p[0] / 100.0, -p[2] / 100.0)
    t = norm((n[2], 0.0, -n[0]))
    return (dot(p, t) / 100.0, -p[1] / 100.0)


# --- The builder ---------------------------------------------------------------------------

class Section:
    def __init__(self, name, material):
        self.name, self.material = name, material
        self.pos, self.nrm, self.uv, self.uv1, self.idx = [], [], [], [], []


class Chart:
    """A lightmap chart: vertices and their 2D place on it, in world units."""

    def __init__(self, origin=None, ux=None, uy=None):
        self.origin, self.ux, self.uy = origin, ux, uy
        self.items = []    # (section, vertex index, (x, y))

    def coord(self, p):
        d = sub(p, self.origin)
        return (dot(d, self.ux), dot(d, self.uy))


class Builder:
    """A mesh: its sections, in first-use order; and, with lightmap, its charts."""

    def __init__(self, name, material_path, lightmap=False):
        self.name = name
        self.material_path = material_path    # section name -> graph path
        self.lightmap = lightmap
        self.sections = {}
        self.charts = []

    def section(self, name):
        if name not in self.sections:
            self.sections[name] = Section(name, self.material_path(name))
        return self.sections[name]

    def chart_for(self, n, origin):
        """A new chart in the plane facing n."""
        helper = Y_AXIS if abs(n[1]) < 0.9 else X_AXIS
        ux = norm(cross(helper, n))
        uy = cross(n, ux)
        chart = Chart(origin, ux, uy)
        self.charts.append(chart)
        return chart

    def new_chart(self):
        chart = Chart()
        self.charts.append(chart)
        return chart

    def face(self, sec, pts, n, uv=None, chart=None, chart_xy=None, normals=None):
        """A convex polygon facing n (pts in either order), fanned from its first
        corner. uv: None (planar world), a callable p -> uv, or a list.
        chart: None (a new one in its plane), or a Chart to join, at chart_xy
        if given, else at its projection on the chart's plane."""
        if len(pts) < 3:
            return
        area = (0.0, 0.0, 0.0)
        for i in range(1, len(pts) - 1):
            area = add(area, cross(sub(pts[i], pts[0]), sub(pts[i + 1], pts[0])))
        if length(area) < 1e-6:
            return
        if dot(area, n) < 0.0:
            # Reversed but still fanned from the first corner (a concave outline,
            # a corbel's side, is drawn from a corner that sees all of it).
            flip = lambda seq: [seq[0]] + list(reversed(seq[1:]))
            pts = flip(pts)
            if isinstance(uv, list):
                uv = flip(uv)
            if normals is not None:
                normals = flip(normals)
            if chart_xy is not None:
                chart_xy = flip(chart_xy)
        s = self.section(sec)
        if self.lightmap and chart is None:
            chart = self.chart_for(n, pts[0])
        first = len(s.pos)
        for i, p in enumerate(pts):
            s.pos.append(p)
            s.nrm.append(normals[i] if normals is not None else n)
            if uv is None:
                s.uv.append(planar_uv(p, n))
            elif callable(uv):
                s.uv.append(uv(p))
            else:
                s.uv.append(uv[i])
            s.uv1.append((0.0, 0.0))
            if self.lightmap:
                xy = chart_xy[i] if chart_xy is not None else chart.coord(p)
                chart.items.append((s, first + i, xy))
        for i in range(1, len(pts) - 1):
            s.idx += [first, first + i, first + i + 1]

    def strip(self, sec, rows, normals, uvs):
        """A grid of quads, rows[r][c] (3D) with per-vertex normals and UVs,
        each quad wound to face its vertices' normals. No lightmap."""
        s = self.section(sec)
        cols = len(rows[0])
        first = len(s.pos)
        for r, row in enumerate(rows):
            for c, p in enumerate(row):
                s.pos.append(p)
                s.nrm.append(normals[r][c])
                s.uv.append(uvs[r][c])
                s.uv1.append((0.0, 0.0))
        for r in range(len(rows) - 1):
            for c in range(cols - 1):
                a = first + r * cols + c
                b, d, e = a + 1, a + cols, a + cols + 1
                for tri in ((a, d, e), (a, e, b)):
                    p0, p1, p2 = (s.pos[i] for i in tri)
                    facing = cross(sub(p1, p0), sub(p2, p0))
                    want = add(add(s.nrm[tri[0]], s.nrm[tri[1]]), s.nrm[tri[2]])
                    if dot(facing, want) < 0.0:
                        tri = (tri[0], tri[2], tri[1])
                    if length(facing) > 1e-9:
                        s.idx += list(tri)

    # --- Lightmap charts, packed ---------------------------------------------------------

    def pack_charts(self):
        """Every chart into 0-1 at one density, shelf by shelf, tallest first."""
        if not self.lightmap or not self.charts:
            return
        boxes = []
        total = 0.0
        for chart in self.charts:
            if not chart.items:
                continue
            xs = [xy[0] for _, _, xy in chart.items]
            ys = [xy[1] for _, _, xy in chart.items]
            lo = (min(xs), min(ys))
            size = (max(xs) - lo[0], max(ys) - lo[1])
            boxes.append((chart, lo, size))
            total += max(size[0], 1.0) * max(size[1], 1.0)
        side = math.sqrt(total) * 1.15
        gutter = side / 256.0 * 2.0      # two texels of a 256 map, in world units
        boxes.sort(key=lambda b: -b[2][1])
        x = y = shelf = 0.0
        width = 0.0
        placed = []
        for chart, lo, size in boxes:
            w, h = size[0] + gutter * 2.0, size[1] + gutter * 2.0
            if x + w > side and x > 0.0:
                x, y, shelf = 0.0, y + shelf, 0.0
            placed.append((chart, lo, (x + gutter, y + gutter)))
            x += w
            shelf = max(shelf, h)
            width = max(width, x)
        extent = max(width, y + shelf)
        for chart, lo, at in placed:
            for s, vi, xy in chart.items:
                s.uv1[vi] = ((at[0] + xy[0] - lo[0]) / extent, (at[1] + xy[1] - lo[1]) / extent)

    # --- .nmmesh ---------------------------------------------------------------------------

    def serialize(self):
        self.pack_charts()
        positions, normals, uvs, uv1s, indices, sections = [], [], [], [], [], []
        for s in self.sections.values():
            if not s.idx:
                continue
            base = len(positions)
            sections.append((s.name, s.material, len(indices), len(s.idx)))
            positions += s.pos
            normals += s.nrm
            uvs += s.uv
            uv1s += s.uv1
            indices += [base + i for i in s.idx]
        lo = tuple(min(p[a] for p in positions) for a in range(3))
        hi = tuple(max(p[a] for p in positions) for a in range(3))
        header = struct.pack("<III", len(positions), len(indices), len(sections)) + struct.pack("<6f", *lo, *hi)
        floats = [c for p in positions for c in p] + [c for n in normals for c in n] + [c for t in uvs for c in t]
        payload = (chunk("MHDR", header) + chunk("MVTX", struct.pack(f"<{len(floats)}f", *floats))
                   + chunk("MIDX", struct.pack(f"<{len(indices)}I", *indices))
                   + chunk("MSEC", b"".join(string(n) + string(m) + struct.pack("<II", f, c) for n, m, f, c in sections)))
        if self.lightmap:
            flat = [c for t in uv1s for c in t]
            payload += chunk("MUV1", struct.pack(f"<{len(flat)}f", *flat))
        return chunk("NMSH", payload), len(positions), len(indices) // 3, [s[0] for s in sections]

    def check(self):
        """Every triangle faces the way its vertices' normals do."""
        for s in self.sections.values():
            for at in range(0, len(s.idx), 3):
                a, b, c = s.idx[at:at + 3]
                facing = cross(sub(s.pos[b], s.pos[a]), sub(s.pos[c], s.pos[a]))
                want = add(add(s.nrm[a], s.nrm[b]), s.nrm[c])
                if length(facing) > 1e-6 and length(want) > 1e-6:
                    assert dot(facing, want) > 0.0, f"{self.name}/{s.name}: triangle {at // 3} wound backwards"


def tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


def chunk(name, payload, version=1):
    return struct.pack("<IHI", tag(name), version, len(payload)) + payload


def string(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


# --- Boxes -------------------------------------------------------------------------------

def box(b, sec, centre, size, axes=(X_AXIS, Y_AXIS, Z_AXIS), faces="xXyYzZ", uv_seed=None, chart=None):
    """An oriented box: centre, size along each of axes. UV U runs along the
    box's longest side on the faces that have it (grain), from a random
    offset per box so neighbours differ. faces: which to make, lower case the
    negative side (x: -axes[0])."""
    half = [s * 0.5 for s in size]
    longest = max(range(3), key=lambda i: size[i])
    seed = uv_seed if uv_seed is not None else f"{centre[0]:.1f},{centre[1]:.1f},{centre[2]:.1f}"
    offset = (rand(seed + "/u", 0.0, 7.0), rand(seed + "/v", 0.0, 7.0))
    corner = combo(centre, (axes[0], -half[0]), (axes[1], -half[1]), (axes[2], -half[2]))
    for i, letters in enumerate(("xX", "yY", "zZ")):
        for sign, letter in ((-1.0, letters[0]), (1.0, letters[1])):
            if letter not in faces:
                continue
            j, k = (i + 1) % 3, (i + 2) % 3
            n = mul(axes[i], sign)
            fc = combo(centre, (axes[i], sign * half[i]))
            pts = [combo(fc, (axes[j], sj * half[j]), (axes[k], sk * half[k]))
                   for sj, sk in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            if longest == j:
                ua, va = j, k
            elif longest == k:
                ua, va = k, j
            else:
                ua, va = j, k
            uvs = [(dot(sub(p, corner), axes[ua]) / 100.0 + offset[0], dot(sub(p, corner), axes[va]) / 100.0 + offset[1])
                   for p in pts]
            b.face(sec, pts, n, uvs, chart=chart)


def aabb(b, sec, lo, hi, faces="xXyYzZ", **kw):
    """box() from two corners, world axes."""
    centre = tuple((lo[a] + hi[a]) * 0.5 for a in range(3))
    size = tuple(hi[a] - lo[a] for a in range(3))
    box(b, sec, centre, size, faces=faces, **kw)


def beam(b, sec, start, end, width, depth, up=Y_AXIS, faces="xXyYzZ"):
    """A box from start to end (its length along axes[0]), width across and
    depth along up (made square to the length)."""
    along = norm(sub(end, start))
    side = norm(cross(up, along))
    upward = cross(along, side)
    box(b, sec, lerp3(start, end, 0.5), (length(sub(end, start)), depth, width), axes=(along, upward, side), faces=faces)


# --- Lathed solids -------------------------------------------------------------------------

def lathe(b, sec, centre, profile, segments=24, hard_angle=40.0, phase=0.0):
    """profile: (radius, height) from bottom to top, the outside first. Each
    segment its own ring of quads; a corner sharper than hard_angle splits
    the normals there. UV: U round (metres at the largest radius), V along
    the profile."""
    seg_normals = []
    for i in range(len(profile) - 1):
        (r0, y0), (r1, y1) = profile[i], profile[i + 1]
        dr, dy = r1 - r0, y1 - y0
        size = math.hypot(dr, dy) or 1.0
        seg_normals.append((dy / size, -dr / size))   # (radial, up)
    rmax = max(r for r, _ in profile) or 1.0
    cos_hard = math.cos(math.radians(hard_angle))
    run = 0.0
    for i in range(len(profile) - 1):
        (r0, y0), (r1, y1) = profile[i], profile[i + 1]
        seglen = math.hypot(r1 - r0, y1 - y0)
        if seglen < 1e-6:
            continue
        here = seg_normals[i]

        def blended(other):
            if other is None:
                return here
            d = here[0] * other[0] + here[1] * other[1]
            if d < cos_hard:
                return here
            m = (here[0] + other[0], here[1] + other[1])
            s = math.hypot(*m) or 1.0
            return (m[0] / s, m[1] / s)

        n0 = blended(seg_normals[i - 1] if i > 0 else None)
        n1 = blended(seg_normals[i + 1] if i + 1 < len(seg_normals) else None)
        rows, nrows, uvrows = [], [], []
        for (r, y, n2, v) in ((r0, y0, n0, run), (r1, y1, n1, run + seglen)):
            row, nrow, uvrow = [], [], []
            for k in range(segments + 1):
                a = phase + 2.0 * math.pi * k / segments
                ca, sa = math.cos(a), math.sin(a)
                row.append((centre[0] + r * ca, centre[1] + y, centre[2] + r * sa))
                nrow.append(norm((n2[0] * ca, n2[1], n2[0] * sa)))
                uvrow.append((a * rmax / 100.0, v / 100.0))
            rows.append(row)
            nrows.append(nrow)
            uvrows.append(uvrow)
        b.strip(sec, rows, nrows, uvrows)
        run += seglen


def polygon_prism(b, sec, centre, radius, sides, y0, y1, phase=0.0, top=True, bottom=False, inner=False, uv_scale=1.0):
    """A regular prism, flat sides; inner: facing in (a basin's inside)."""
    corners = [(centre[0] + radius * math.cos(phase + 2 * math.pi * k / sides), centre[2] + radius * math.sin(phase + 2 * math.pi * k / sides))
               for k in range(sides)]
    run = 0.0
    for k in range(sides):
        a, c = corners[k], corners[(k + 1) % sides]
        mid = ((a[0] + c[0]) * 0.5 - centre[0], (a[1] + c[1]) * 0.5 - centre[2])
        n = norm((mid[0], 0.0, mid[1]))
        if inner:
            n = mul(n, -1.0)
        w = math.hypot(c[0] - a[0], c[1] - a[1])
        pts = [(a[0], y0, a[1]), (c[0], y0, c[1]), (c[0], y1, c[1]), (a[0], y1, a[1])]
        uvs = [(run / 100 * uv_scale, -y0 / 100 * uv_scale), ((run + w) / 100 * uv_scale, -y0 / 100 * uv_scale),
               ((run + w) / 100 * uv_scale, -y1 / 100 * uv_scale), (run / 100 * uv_scale, -y1 / 100 * uv_scale)]
        b.face(sec, pts, n, uvs)
        run += w
    if top:
        b.face(sec, [(x, y1, z) for x, z in corners], Y_AXIS)
    if bottom:
        b.face(sec, [(x, y0, z) for x, z in corners], mul(Y_AXIS, -1.0))
    return corners


# --- Walls with openings -------------------------------------------------------------------

class Opening:
    """A hole through a wall, from u0 to u1 along it, from its sill up to its
    head: square (top) or a round arch (springing at spring, a semicircle),
    or a segmental arch (spring, rise)."""

    def __init__(self, u0, u1, sill, top=None, spring=None, rise=None, segments=None, name=""):
        self.u0, self.u1, self.sill, self.name = u0, u1, sill, name
        self.kind = "rect" if spring is None else ("arch" if rise is None else "segment")
        self.top, self.spring, self.rise = top, spring, rise
        width = u1 - u0
        if self.kind == "rect":
            self.profile = [(u0, top), (u1, top)]
            self.head = top
        else:
            half = width * 0.5
            centre = u0 + half
            n = segments or (20 if half > 100 else 14)
            if self.kind == "arch":
                self.profile = [(centre - half * math.cos(math.pi * i / n), spring + half * math.sin(math.pi * i / n)) for i in range(n + 1)]
                self.head = spring + half
            else:
                radius = (half * half + rise * rise) / (2.0 * rise)
                drop = radius - rise
                sweep = math.asin(half / radius)
                self.profile = [(centre + radius * math.sin(-sweep + 2 * sweep * i / n),
                                 spring - drop + radius * math.cos(-sweep + 2 * sweep * i / n)) for i in range(n + 1)]
                self.head = spring + rise

    @property
    def width(self):
        return self.u1 - self.u0

    @property
    def centre(self):
        return (self.u0 + self.u1) * 0.5

    def head_at(self, u):
        return polyline_at(self.profile, u)

    def outline(self, inset=0.0):
        """The opening's outline in (u, y), counter-clockwise from the
        bottom left, shrunk by inset (a frame's inside)."""
        bottom = self.sill + inset
        if self.kind == "rect":
            return [(self.u0 + inset, bottom), (self.u1 - inset, bottom), (self.u1 - inset, self.top - inset), (self.u0 + inset, self.top - inset)]
        pts = [(self.u0 + inset, bottom), (self.u1 - inset, bottom)]
        if self.kind == "arch":
            half = self.width * 0.5 - inset
            n = len(self.profile) - 1
            pts += [(self.centre + half * math.cos(math.pi * i / n), self.spring + half * math.sin(math.pi * i / n)) for i in range(n + 1)]
        else:
            for u, y in reversed(self.profile):
                pts.append((min(max(u, self.u0 + inset), self.u1 - inset), y - inset))
        return pts


def polyline_at(points, u):
    if u <= points[0][0]:
        return points[0][1]
    for (ua, ya), (ub, yb) in zip(points, points[1:]):
        if u <= ub:
            return ya + (yb - ya) * ((u - ua) / (ub - ua) if ub > ua else 0.0)
    return points[-1][1]


class Wall:
    """A straight wall: along X (at z = c) or Z (at x = c), from a0 to a1,
    T thick, from y0 up to top -- a height or (u, y) points for a gable. Its
    + side faces +Z (or +X)."""

    def __init__(self, axis, c, a0, a1, T, y0, top, openings=(), plus_section="Stucco", minus_section="Stucco",
                 ends=(False, False), bottom=False, top_cap=True, name=""):
        self.axis, self.c, self.a0, self.a1, self.T, self.y0 = axis, c, a0, a1, T, y0
        self.top = top if isinstance(top, list) else [(a0, top), (a1, top)]
        self.openings = sorted(openings, key=lambda o: o.u0)
        self.sections = {1: plus_section, -1: minus_section}
        self.ends, self.bottom, self.top_cap, self.name = ends, bottom, top_cap, name

    def along(self):
        return X_AXIS if self.axis == "x" else Z_AXIS

    def perp(self):
        return Z_AXIS if self.axis == "x" else X_AXIS

    def point(self, u, y, depth):
        """depth: across the wall from its centre line, + toward its + side."""
        return (u, y, self.c + depth) if self.axis == "x" else (self.c + depth, y, u)

    def face_point(self, u, y, side, out=0.0):
        """On side's face (side +1 or -1), out further along its normal."""
        return self.point(u, y, side * (self.T * 0.5 + out))


def build_wall(b, w):
    half = w.T * 0.5
    along, perp = w.along(), w.perp()
    breaks = {w.a0, w.a1} | {u for u, _ in w.top if w.a0 < u < w.a1}
    for o in w.openings:
        breaks |= {u for u, _ in o.profile}
    us = sorted(breaks)
    us = [u for i, u in enumerate(us) if i == 0 or u - us[i - 1] > 1e-4]
    top_at = lambda u: polyline_at(w.top, u)

    side_charts = {}
    if b.lightmap:
        for side in (1, -1):
            chart = b.new_chart()
            side_charts[side] = chart
    top_chart = b.new_chart() if b.lightmap else None
    bottom_chart = b.new_chart() if b.lightmap else None

    def side_quad(side, ua, ub, ya0, yb0, ya1, yb1):
        if (ya1 - ya0) < 1e-3 and (yb1 - yb0) < 1e-3:
            return
        pts = [w.point(ua, ya0, side * half), w.point(ub, yb0, side * half), w.point(ub, yb1, side * half), w.point(ua, ya1, side * half)]
        xy = [(ua * side, ya0), (ub * side, yb0), (ub * side, yb1), (ua * side, ya1)]
        b.face(w.sections[side], pts, mul(perp, side), chart=side_charts.get(side), chart_xy=xy if b.lightmap else None)

    for ua, ub in zip(us, us[1:]):
        mid = (ua + ub) * 0.5
        opening = next((o for o in w.openings if o.u0 < mid < o.u1), None)
        ta, tb = top_at(ua), top_at(ub)
        for side in (1, -1):
            if opening is None:
                side_quad(side, ua, ub, w.y0, w.y0, ta, tb)
            else:
                ha, hb = opening.head_at(ua), opening.head_at(ub)
                if ta > ha + 1e-3 or tb > hb + 1e-3:
                    side_quad(side, ua, ub, ha, hb, max(ta, ha), max(tb, hb))
                if opening.sill > w.y0 + 1e-3:
                    side_quad(side, ua, ub, w.y0, w.y0, opening.sill, opening.sill)
        if w.top_cap:
            dn = norm(combo((0, 0, 0), (along, -(tb - ta)), (Y_AXIS, ub - ua)))
            pts = [w.point(ua, ta, -half), w.point(ub, tb, -half), w.point(ub, tb, half), w.point(ua, ta, half)]
            b.face(w.sections[1], pts, dn, chart=top_chart,
                   chart_xy=[(ua, -half), (ub, -half), (ub, half), (ua, half)] if b.lightmap else None)
        if w.bottom and opening is None:
            pts = [w.point(ua, w.y0, -half), w.point(ub, w.y0, -half), w.point(ub, w.y0, half), w.point(ua, w.y0, half)]
            b.face(w.sections[1], pts, mul(Y_AXIS, -1.0), chart=bottom_chart,
                   chart_xy=[(ua, -half), (ub, -half), (ub, half), (ua, half)] if b.lightmap else None)

    # The openings' reveals: jambs, head (or intrados), sill.
    for o in w.openings:
        reveal = w.sections[1] if w.sections[1] == w.sections[-1] else "Stucco"
        for u, sign in ((o.u0, 1.0), (o.u1, -1.0)):
            h = o.head_at(u)
            if h - o.sill > 1e-3:
                pts = [w.point(u, o.sill, -half), w.point(u, o.sill, half), w.point(u, h, half), w.point(u, h, -half)]
                b.face(reveal, pts, mul(along, sign), chart=b.new_chart() if b.lightmap else None,
                       chart_xy=[(o.sill, -half), (o.sill, half), (h, half), (h, -half)] if b.lightmap else None)
        chart = b.new_chart() if b.lightmap else None
        run = 0.0
        for (ua, ya), (ub, yb) in zip(o.profile, o.profile[1:]):
            du, dy = ub - ua, yb - ya
            seglen = math.hypot(du, dy)
            if seglen < 1e-6:
                continue
            n2 = (dy / seglen, -du / seglen)
            n = norm(combo((0, 0, 0), (along, n2[0]), (Y_AXIS, n2[1])))
            pts = [w.point(ua, ya, -half), w.point(ub, yb, -half), w.point(ub, yb, half), w.point(ua, ya, half)]
            b.face(reveal, pts, n, chart=chart,
                   chart_xy=[(run, -half), (run + seglen, -half), (run + seglen, half), (run, half)] if b.lightmap else None)
            run += seglen
        if o.sill > w.y0 + 1e-3:
            pts = [w.point(o.u0, o.sill, -half), w.point(o.u1, o.sill, -half), w.point(o.u1, o.sill, half), w.point(o.u0, o.sill, half)]
            b.face(reveal, pts, Y_AXIS, chart=b.new_chart() if b.lightmap else None,
                   chart_xy=[(o.u0, -half), (o.u1, -half), (o.u1, half), (o.u0, half)] if b.lightmap else None)

    for at, sign, flag in ((w.a0, -1.0, w.ends[0]), (w.a1, 1.0, w.ends[1])):
        if flag:
            t = top_at(at)
            pts = [w.point(at, w.y0, -half), w.point(at, w.y0, half), w.point(at, t, half), w.point(at, t, -half)]
            b.face(w.sections[1], pts, mul(along, sign))


# --- Barrel-tile roofs -----------------------------------------------------------------------

TILE_PERIOD = 27.0     # a pan and a cap, across the slope
TILE_PITCH = 36.0      # each course's exposure up the slope
TILE_LENGTH = 46.0     # a tile's length: TILE_LENGTH - TILE_PITCH under the next course
TILE_TILT = 2.2        # how much higher a tile's lower end sits than its upper
TILE_THICK = 1.8
CAP_HEIGHT = 8.5
PAN_DEPTH = 2.5

# One period's profile across the slope, x in 0-1 of the period: the cap's
# half-round over the first half, then the pan's shallow trough.
CAP_FRACTIONS = (0.0, 0.11, 0.5, 0.89, 1.0)
PAN_FRACTIONS = (0.0, 0.5, 1.0)


def cap_profile(f):
    """Height and slope (dh/dx in half-period units) at f in 0-1 of the cap."""
    c = 2.0 * f - 1.0
    root = math.sqrt(max(0.0, 1.0 - c * c))
    h = CAP_HEIGHT * root
    slope = -CAP_HEIGHT * c / max(root, 0.25) * 2.0
    return h, slope


def pan_profile(f):
    h = -PAN_DEPTH * math.sin(math.pi * f)
    slope = -PAN_DEPTH * math.pi * math.cos(math.pi * f)
    return h, slope


class RoofSlope:
    """A roof plane: its eave edge from E0 along A for La, rising toward H (a
    horizontal unit) at pitch degrees for Ls along the slope. Points are
    (t along the eave, s up the slope, h off the plane)."""

    def __init__(self, E0, A, H, pitch, La, Ls):
        self.E0, self.A, self.La, self.Ls = E0, A, La, Ls
        p = math.radians(pitch)
        self.S = norm(combo((0, 0, 0), (H, math.cos(p)), (Y_AXIS, math.sin(p))))
        self.N = norm(combo((0, 0, 0), (H, -math.sin(p)), (Y_AXIS, math.cos(p))))
        self.H, self.pitch = H, pitch

    def at(self, t, s, h=0.0):
        return combo(self.E0, (self.A, t), (self.S, s), (self.N, h))


def tile_slope(b, sec, slope, start_course=0, stop_at=None, seed=""):
    """Courses of barrel tiles over slope, eave to top; each course a strip
    tilted so its foot rests on the course below, with a lip (its thickness)
    at the foot. The eave course's lip runs down to the deck: the mortar
    that closes the caps."""
    count = max(1, round(slope.La / TILE_PERIOD))
    period = slope.La / count
    stop = slope.Ls if stop_at is None else stop_at
    # The across-slope samples: (t, h, dh/dt), cap and pan their own vertices.
    across = []
    for k in range(count):
        t0 = k * period
        cap = []
        for f in CAP_FRACTIONS:
            h, d = cap_profile(f)
            cap.append((t0 + f * period * 0.5, h, d / (period * 0.5)))
        pan = []
        for f in PAN_FRACTIONS:
            h, d = pan_profile(f)
            pan.append((t0 + period * 0.5 + f * period * 0.5, h, d / (period * 0.5)))
        across.append(cap)
        across.append(pan)

    course = 0
    s0 = 0.0
    while s0 < stop - 1.0:
        s1 = min(s0 + TILE_LENGTH, stop)
        lift0, lift1 = TILE_TILT, TILE_TILT * (1.0 - (s1 - s0) / TILE_LENGTH)
        shift = rand(f"{seed}/{course}", 0.0, 3.0)
        for piece in across:
            rows, nrows, uvrows = [[], []], [[], []], [[], []]
            for t, h, d in piece:
                n = norm(combo((0, 0, 0), (slope.A, -d), (slope.N, 1.0)))
                for r, (s, lift) in enumerate(((s0, lift0), (s1, lift1))):
                    rows[r].append(slope.at(t, s, h + lift))
                    nrows[r].append(n)
                    uvrows[r].append((s / 100.0 + shift, t / 100.0))
            b.strip(sec, rows, nrows, uvrows)
            # The foot's lip, facing down the slope.
            down = mul(slope.S, -1.0)
            lips, lipn, lipuv = [[], []], [[], []], [[], []]
            for t, h, d in piece:
                foot = (min(h, 0.0) - 2.0) if course == start_course else (h + lift0 - TILE_THICK)
                lips[0].append(slope.at(t, s0, foot))
                lips[1].append(slope.at(t, s0, h + lift0))
                for r in range(2):
                    lipn[r].append(down)
                lipuv[0].append((s0 / 100.0 + shift, t / 100.0))
                lipuv[1].append((s0 / 100.0 + shift + 0.02, t / 100.0))
            b.strip(sec, lips, lipn, lipuv)
        course += 1
        s0 += TILE_PITCH


def tile_ridge(b, sec, start, end, across, radius=13.0, seed=""):
    """Ridge caps from start to end along the ridge line (on the roof's tile
    seat), each a half-round over both slopes; across is horizontal and
    square to the ridge."""
    along = norm(sub(end, start))
    total = length(sub(end, start))
    count = max(1, math.ceil(total / 34.0))
    pitch = total / count
    segs = 8
    for k in range(count):
        a0 = k * pitch
        a1 = min(a0 + 42.0, total)
        lift0, lift1 = 2.0, 0.4
        rows, nrows, uvrows = [[], []], [[], []], [[], []]
        for i in range(segs + 1):
            phi = math.pi * i / segs
            c, s = math.cos(phi), math.sin(phi)
            n = norm(combo((0, 0, 0), (across, c), (Y_AXIS, s)))
            for r, (a, lift) in enumerate(((a0, lift0), (a1, lift1))):
                p = combo(start, (along, a), (across, radius * c), (Y_AXIS, radius * 0.75 * s + lift + 4.0))
                rows[r].append(p)
                nrows[r].append(n)
                uvrows[r].append((a / 100.0, phi * radius / 100.0 + rand(f"{seed}/{k}", 0, 2)))
        b.strip(sec, rows, nrows, uvrows)
        # The foot's lip, and at the ridge's start the hollow closed with mortar.
        ring = [combo(start, (along, a0), (across, radius * math.cos(math.pi * i / segs)),
                      (Y_AXIS, radius * 0.75 * math.sin(math.pi * i / segs) + lift0 + 4.0)) for i in range(segs + 1)]
        base = [combo(start, (along, a0), (across, radius * math.cos(math.pi * i / segs)), (Y_AXIS, -6.0)) for i in range(segs + 1)]
        inner = [combo(start, (along, a0), (across, (radius - TILE_THICK) * math.cos(math.pi * i / segs)),
                       (Y_AXIS, (radius - TILE_THICK) * 0.75 * math.sin(math.pi * i / segs) + lift0 + 4.0)) for i in range(segs + 1)]
        lower = base if k == 0 else inner
        back = mul(along, -1.0)
        b.strip(sec, [lower, ring], [[back] * (segs + 1)] * 2,
                [[(0.0, i * 0.03) for i in range(segs + 1)], [(0.02, i * 0.03) for i in range(segs + 1)]])
    # The far end closed too.
    ring = [combo(end, (across, radius * math.cos(math.pi * i / segs)), (Y_AXIS, radius * 0.75 * math.sin(math.pi * i / segs) + 4.4))
            for i in range(segs + 1)]
    base = [combo(end, (across, radius * math.cos(math.pi * i / segs)), (Y_AXIS, -6.0)) for i in range(segs + 1)]
    b.strip(sec, [base, ring], [[along] * (segs + 1)] * 2,
            [[(0.0, i * 0.03) for i in range(segs + 1)], [(0.02, i * 0.03) for i in range(segs + 1)]])


def roof_deck(b, sec, slope, thickness=3.0, edge=True, rake=(True, True)):
    """The plank deck under the tiles: its underside (seen at the eaves and
    from a gallery), its edge along the eave and barge boards at the ends."""
    under = [slope.at(0, 0, -thickness), slope.at(slope.La, 0, -thickness), slope.at(slope.La, slope.Ls, -thickness), slope.at(0, slope.Ls, -thickness)]
    uv = lambda p: (dot(sub(p, slope.E0), slope.A) / 100.0, dot(sub(p, slope.E0), slope.S) / 100.0)
    b.face(sec, under, mul(slope.N, -1.0), uv)
    if edge:
        pts = [slope.at(0, 0, 0.0), slope.at(slope.La, 0, 0.0), slope.at(slope.La, 0, -thickness), slope.at(0, 0, -thickness)]
        b.face(sec, pts, mul(slope.S, -1.0), lambda p: (dot(sub(p, slope.E0), slope.A) / 100.0, p[1] / 100.0))
    for t, flag, sign in ((0.0, rake[0], -1.0), (slope.La, rake[1], 1.0)):
        if not flag:
            continue
        outward = mul(slope.A, sign)
        pts = [slope.at(t, 0, 4.0), slope.at(t, slope.Ls, 4.0), slope.at(t, slope.Ls, -20.0), slope.at(t, 0, -20.0)]
        b.face(sec, pts, outward, lambda p: (dot(sub(p, slope.E0), slope.S) / 100.0, p[1] / 100.0))
        inset = mul(slope.A, -sign * 3.0)
        under = [add(slope.at(t, 0, -20.0), (0, 0, 0)), slope.at(t, slope.Ls, -20.0), add(slope.at(t, slope.Ls, -20.0), inset), add(slope.at(t, 0, -20.0), inset)]
        b.face(sec, under, mul(slope.N, -1.0), uv)


def rafters(b, sec, slope, s_from, s_to, spacing=55.0, width=8.0, depth=14.0, margin=20.0, t_range=None):
    """Rafters under the deck, from s_from to s_to up the slope, every
    spacing along it (within t_range, default the whole eave)."""
    lo, hi = t_range if t_range else (0.0, slope.La)
    count = max(1, int((hi - lo - 2 * margin) / spacing))
    gap = (hi - lo - 2 * margin) / count
    for k in range(count + 1):
        t = lo + margin + k * gap
        start = slope.at(t, s_from, -3.0 - depth * 0.5)
        end = slope.at(t, s_to, -3.0 - depth * 0.5)
        beam(b, sec, start, end, width, depth, up=slope.N)


def tube(b, sec, start, end, radius, segments=10, caps=True, radius_end=None):
    """A round bar from start to end, smooth, its ends capped."""
    along = norm(sub(end, start))
    side = norm(cross(Y_AXIS if abs(along[1]) < 0.95 else X_AXIS, along))
    other = cross(along, side)
    r1 = radius if radius_end is None else radius_end
    total = length(sub(end, start))
    rows, nrows, uvrows = [[], []], [[], []], [[], []]
    for k in range(segments + 1):
        a = 2.0 * math.pi * k / segments
        d = combo((0, 0, 0), (side, math.cos(a)), (other, math.sin(a)))
        for r, (p, rad) in enumerate(((start, radius), (end, r1))):
            rows[r].append(combo(p, (d, rad)))
            nrows[r].append(d)
            uvrows[r].append((r * total / 100.0, a * radius / 100.0))
    b.strip(sec, rows, nrows, uvrows)
    if caps:
        for p, rad, n in ((start, radius, mul(along, -1.0)), (end, r1, along)):
            ring = [combo(p, (side, rad * math.cos(2 * math.pi * k / segments)), (other, rad * math.sin(2 * math.pi * k / segments)))
                    for k in range(segments)]
            b.face(sec, ring, n)
