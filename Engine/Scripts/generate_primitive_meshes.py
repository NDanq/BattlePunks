#!/usr/bin/env python3
"""
generate_primitive_meshes.py -- the engine's primitive meshes, as .nmmesh files
in Engine/Content/Meshes (staged beside the executable at the same path, so a
RenderComponent names one as "Engine/Content/Meshes/Box.nmmesh").

    Box.nmmesh      a 1-unit cube: 6 faces of 2 triangles, each face its own 4 vertices
    Sphere.nmmesh   a UV sphere, diameter 1: 32 segments around, 16 rings
    Plane.nmmesh    a 1 x 1 square in X and Z, facing up (+Y), in 16 x 16 cells
    Cylinder.nmmesh diameter 1, height 1, along Y: 32 segments around, a flat cap
                    at each end (the wall smooth, its edges hard)

All four are centred on the origin and are made the way the importer makes a
glTF mesh, so a material treats them alike: each shape is built in glTF's
frame -- right-handed, +Y up, +Z toward the viewer -- then converted to the
engine's exactly as GltfImporter.cpp converts one (z flips, and the winding
with it):

  - engine axes: left-handed, +X right, +Y up, +Z away from the viewer
    (RenderCore's RenderView.h);
  - the cross product of a triangle's edges points along its normal (true in
    both frames: the mirror and the winding swap cancel);
  - UV0 with v = 0 at the image's top row; a box face and the plane show an
    image upright seen from outside, up being +Y (or away from the viewer, +Z,
    for the top, and toward it for the bottom);
  - one section, named for the mesh, with no material: drawn with DefaultLit
    unless a RenderComponent slot names one; no vertex colours (white).

The .nmmesh layout is RenderCore's MeshAsset.cpp: EngineCore::Archive chunks
(tag, version, size, payload; little-endian), no file header:

    NMSH v1 { MHDR v1, MVTX v1, MIDX v1, MSEC v1 }

Usage:
    python Engine/Scripts/generate_primitive_meshes.py [-out <dir>]
"""

import argparse
import math
import struct
from pathlib import Path

ENGINE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_OUT = ENGINE_DIR / "Content" / "Meshes"


# --- Small vector helpers --------------------------------------------------------------

def add(a, b):     return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b):     return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def scale(a, s):   return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b):     return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b):   return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


class Mesh:
    """MeshData's arrays: per-vertex position, normal and UV; a triangle list."""

    def __init__(self, name):
        self.name = name
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []

    def vertex(self, position, normal, uv):
        self.positions.append(position)
        self.normals.append(normal)
        self.uvs.append(uv)
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        self.indices += [a, b, c]

    def check(self):
        """Every triangle faces the way its vertices' normals do; every index is in range."""
        assert len(self.indices) % 3 == 0
        for at in range(0, len(self.indices), 3):
            a, b, c = self.indices[at:at + 3]
            assert max(a, b, c) < len(self.positions)
            face = cross(sub(self.positions[b], self.positions[a]), sub(self.positions[c], self.positions[a]))
            normal = add(add(self.normals[a], self.normals[b]), self.normals[c])
            if dot(face, face) > 1e-12:   # a pole's sliver triangle has no area
                assert dot(face, normal) > 0.0, f"{self.name}: triangle {at // 3} is wound backwards"

    def bounds(self):
        lo = tuple(min(p[axis] for p in self.positions) for axis in range(3))
        hi = tuple(max(p[axis] for p in self.positions) for axis in range(3))
        return lo, hi


# --- The shapes, in glTF's frame (right-handed, +Y up, +Z toward the viewer) --------------

UP   = (0.0, 1.0, 0.0)
DOWN = (0.0, -1.0, 0.0)


def quad(mesh, centre, right, down, normal, cells=1):
    """A square of side |right| = |down| = 1 at centre, seen from outside as an
    image: u along right, v along down. Wound so each triangle faces normal."""
    first = len(mesh.positions)
    for row in range(cells + 1):
        for col in range(cells + 1):
            u, v = col / cells, row / cells
            position = add(centre, add(scale(right, u - 0.5), scale(down, v - 0.5)))
            mesh.vertex(position, normal, (u, v))
    stride = cells + 1
    for row in range(cells):
        for col in range(cells):
            top_left = first + row * stride + col
            top_right, bottom_left, bottom_right = top_left + 1, top_left + stride, top_left + stride + 1
            # (TL, BL, BR) and (TL, BR, TR): down x right is the normal.
            mesh.triangle(top_left, bottom_left, bottom_right)
            mesh.triangle(top_left, bottom_right, top_right)


def make_box():
    mesh = Mesh("Box")
    # Each face: its outward normal, and the direction "up" on its image.
    faces = [
        ((1.0, 0.0, 0.0), UP), ((-1.0, 0.0, 0.0), UP),
        ((0.0, 0.0, 1.0), UP), ((0.0, 0.0, -1.0), UP),
        ((0.0, 1.0, 0.0), (0.0, 0.0, -1.0)),   # top: away from the viewer up the image
        ((0.0, -1.0, 0.0), (0.0, 0.0, 1.0)),   # bottom
    ]
    for normal, image_up in faces:
        right = cross(image_up, normal)        # right x up = normal, right-handed
        quad(mesh, scale(normal, 0.5), right, scale(image_up, -1.0), normal)
    return mesh


def make_plane(cells=16):
    mesh = Mesh("Plane")
    normal = UP
    image_up = (0.0, 0.0, -1.0)                # away from the viewer is the image's top
    quad(mesh, (0.0, 0.0, 0.0), cross(image_up, normal), scale(image_up, -1.0), normal, cells)
    return mesh


def make_sphere(segments=32, rings=16, radius=0.5):
    """Rings from the top pole (v = 0) to the bottom (v = 1); u once around,
    rightwards seen from outside. The seam and the poles repeat vertices so
    every triangle's UVs are continuous."""
    mesh = Mesh("Sphere")
    for ring in range(rings + 1):
        polar = math.pi * ring / rings                    # 0 at the top
        for segment in range(segments + 1):
            azimuth = 2.0 * math.pi * segment / segments
            # Seen from outside, u grows to the right: right = up x normal, so
            # the azimuth turns from -Z towards -X.
            normal = (-math.sin(polar) * math.sin(azimuth), math.cos(polar), -math.sin(polar) * math.cos(azimuth))
            mesh.vertex(scale(normal, radius), normal, (segment / segments, ring / rings))

    stride = segments + 1
    for ring in range(rings):
        for segment in range(segments):
            top_left = ring * stride + segment
            top_right, bottom_left, bottom_right = top_left + 1, top_left + stride, top_left + stride + 1
            if ring != 0:
                mesh.triangle(top_left, bottom_left, bottom_right)
            if ring != rings - 1:
                mesh.triangle(top_left, bottom_right, top_right)
    return mesh


def make_cylinder(segments=32, radius=0.5, height=1.0):
    """The wall as the sphere's equator stretched: u once around, rightwards
    seen from outside, v from the top edge (0) to the bottom (1), the seam
    repeating its column. Each cap has its own vertices, so the edges are hard,
    mapped flat: seen from outside, the top shows an image as the box's top
    does, the bottom as its bottom."""
    mesh = Mesh("Cylinder")
    half = height * 0.5

    def around(segment):
        azimuth = 2.0 * math.pi * segment / segments
        return (-math.sin(azimuth), 0.0, -math.cos(azimuth))   # as the sphere's: u grows rightwards

    first = len(mesh.positions)
    for row, y in ((0, half), (1, -half)):
        for segment in range(segments + 1):
            normal = around(segment)
            mesh.vertex((normal[0] * radius, y, normal[2] * radius), normal, (segment / segments, float(row)))
    stride = segments + 1
    for segment in range(segments):
        top_left = first + segment
        top_right, bottom_left, bottom_right = top_left + 1, top_left + stride, top_left + stride + 1
        mesh.triangle(top_left, bottom_left, bottom_right)
        mesh.triangle(top_left, bottom_right, top_right)

    for normal, y, v_sign in ((UP, half, 1.0), (DOWN, -half, -1.0)):
        # Top: the image's top is away from the viewer (-Z), so v grows with z;
        # bottom: toward the viewer, so v falls with z.
        def cap_vertex(x, z):
            return mesh.vertex((x, y, z), normal, (0.5 + x, 0.5 + v_sign * z))
        centre = cap_vertex(0.0, 0.0)
        ring = []
        for segment in range(segments):
            direction = around(segment)
            ring.append(cap_vertex(direction[0] * radius, direction[2] * radius))
        for segment in range(segments):
            a, b = ring[segment], ring[(segment + 1) % segments]
            edge = cross(sub(mesh.positions[a], mesh.positions[centre]), sub(mesh.positions[b], mesh.positions[centre]))
            if dot(edge, normal) > 0.0:
                mesh.triangle(centre, a, b)
            else:
                mesh.triangle(centre, b, a)
    return mesh


def to_engine(mesh):
    """glTF's frame -> the engine's, as GltfImporter.cpp does: z flips (a
    mirror), so each triangle's winding swaps to keep it facing its normal."""
    mesh.positions = [(p[0], p[1], -p[2]) for p in mesh.positions]
    mesh.normals = [(n[0], n[1], -n[2]) for n in mesh.normals]
    for at in range(0, len(mesh.indices), 3):
        mesh.indices[at + 1], mesh.indices[at + 2] = mesh.indices[at + 2], mesh.indices[at + 1]
    return mesh


# --- .nmmesh -------------------------------------------------------------------------------

def tag(text):
    return (ord(text[0]) << 24) | (ord(text[1]) << 16) | (ord(text[2]) << 8) | ord(text[3])


def chunk(name, payload, version=1):
    return struct.pack("<IHI", tag(name), version, len(payload)) + payload


def string(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


def serialize(mesh):
    lo, hi = mesh.bounds()
    header = struct.pack("<III", len(mesh.positions), len(mesh.indices), 1) + struct.pack("<6f", *lo, *hi)
    floats = [c for p in mesh.positions for c in p] + [c for n in mesh.normals for c in n] + [c for t in mesh.uvs for c in t]
    vertices = struct.pack(f"<{len(floats)}f", *floats)
    indices = struct.pack(f"<{len(mesh.indices)}I", *mesh.indices)
    sections = string(mesh.name) + string("") + struct.pack("<II", 0, len(mesh.indices))
    return chunk("NMSH", chunk("MHDR", header) + chunk("MVTX", vertices) + chunk("MIDX", indices) + chunk("MSEC", sections))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-out", type=Path, default=DEFAULT_OUT, help="where to write the .nmmesh files")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    for mesh in (make_box(), make_sphere(), make_plane(), make_cylinder()):
        mesh.check()
        to_engine(mesh)
        mesh.check()
        path = args.out / f"{mesh.name}.nmmesh"
        path.write_bytes(serialize(mesh))
        lo, hi = mesh.bounds()
        print(f"{path.name}: {len(mesh.positions)} vertices, {len(mesh.indices) // 3} triangles, "
              f"bounds {tuple(round(v, 3) for v in lo)} - {tuple(round(v, 3) for v in hi)}")


if __name__ == "__main__":
    main()
