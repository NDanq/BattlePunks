#!/usr/bin/env python3
"""
make_inset_lattice_mesh.py -- writes Content/Meshes/InsetLattice.nmmesh,
inset shadows' demo caster (the BakedLighting level's InsetLattice): a cage
whose shadow is all fine detail, so a shadow map's resolution shows at once.

    Lattice   a 1-unit cube's frame, each face a 4 x 4 grid of square holes:
              48 square bars, BAR thick (unique -- a bar on an edge belongs
              to two faces but is made once). Section 0.
    Core      a sphere, CORE_RADIUS, at the centre: the lattice shadows it,
              so self-shadowing shows too. Section 1.

Scaled 90 in the level, a bar is about 3 units thick and a hole 19 wide: a
cascade there spends 4+ units a texel and blurs the grid to grey, while a
per-object (inset) map fitted to the cage's ~80-unit sphere spends well
under one.

Centred on the origin, made as Engine/Scripts/generate_primitive_meshes.py
makes its shapes (its helpers, glTF's frame then converted to the engine's).
No UV1: the lattice is Dynamic, lit by the probes; mark it Static and it is
still in the bake (it shadows), lit by the probes. Usage:

    python Games/LightLab/Tools/make_inset_lattice_mesh.py
"""

import struct
import sys
from itertools import product
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(GAME_DIR.parent.parent / "Engine" / "Scripts"))
import generate_primitive_meshes as prims   # noqa: E402

OUTPUT      = GAME_DIR / "Content" / "Meshes" / "InsetLattice.nmmesh"
CELLS       = 4          # holes along each face's side
BAR         = 0.035      # a bar's thickness
CORE_RADIUS = 0.2


def box(mesh, lo, hi):
    """An axis-aligned box, each face its own 4 vertices (as Box.nmmesh)."""
    centre = tuple((lo[a] + hi[a]) * 0.5 for a in range(3))
    size = tuple(hi[a] - lo[a] for a in range(3))
    for axis in range(3):
        u_axis, v_axis = (axis + 1) % 3, (axis + 2) % 3
        for sign in (1.0, -1.0):
            normal = tuple(sign if a == axis else 0.0 for a in range(3))
            face = tuple(centre[a] + (sign * size[a] * 0.5 if a == axis else 0.0) for a in range(3))
            right = tuple(size[a] if a == u_axis else 0.0 for a in range(3))
            down = tuple(size[a] if a == v_axis else 0.0 for a in range(3))
            # quad() winds down x right along the normal: flip one edge if not.
            if prims.dot(prims.cross(down, right), normal) < 0.0:
                right = prims.scale(right, -1.0)
            prims.quad(mesh, face, right, down, normal)


def make_lattice():
    mesh = prims.Mesh("Lattice")
    grid = [-0.5 + i / CELLS for i in range(CELLS + 1)]
    half = 0.5 + BAR * 0.5
    for axis in range(3):
        others = [a for a in range(3) if a != axis]
        for b, c in product(grid, grid):
            if abs(b) < 0.5 and abs(c) < 0.5:
                continue   # inside the cube: no face holds it
            lo, hi = [0.0] * 3, [0.0] * 3
            lo[axis], hi[axis] = -half, half
            for other, at in zip(others, (b, c)):
                lo[other], hi[other] = at - BAR * 0.5, at + BAR * 0.5
            box(mesh, lo, hi)
    return mesh


def serialize(sections):
    """NMSH { MHDR, MVTX, MIDX, MSEC }: the sections' vertices one after
    another, each section its own index range. No MUV1, no MCOL."""
    positions, normals, uvs, indices, ranges = [], [], [], [], []
    for mesh in sections:
        base = len(positions)
        ranges.append((mesh.name, len(indices), len(mesh.indices)))
        positions += mesh.positions
        normals += mesh.normals
        uvs += mesh.uvs
        indices += [base + i for i in mesh.indices]
    lo = tuple(min(p[a] for p in positions) for a in range(3))
    hi = tuple(max(p[a] for p in positions) for a in range(3))
    header = struct.pack("<III", len(positions), len(indices), len(sections)) + struct.pack("<6f", *lo, *hi)
    floats = [c for p in positions for c in p] + [c for n in normals for c in n] + [c for t in uvs for c in t]
    vertices = struct.pack(f"<{len(floats)}f", *floats)
    index_bytes = struct.pack(f"<{len(indices)}I", *indices)
    section_bytes = b"".join(prims.string(name) + prims.string("") + struct.pack("<II", first, count)
                             for name, first, count in ranges)
    payload = (prims.chunk("MHDR", header) + prims.chunk("MVTX", vertices) + prims.chunk("MIDX", index_bytes)
               + prims.chunk("MSEC", section_bytes))
    return prims.chunk("NMSH", payload), len(positions), len(indices) // 3, lo, hi


def main():
    lattice = make_lattice()
    core = prims.make_sphere(radius=CORE_RADIUS)
    core.name = "Core"
    for mesh in (lattice, core):
        mesh.check()
        prims.to_engine(mesh)
        mesh.check()

    data, vertex_count, triangle_count, lo, hi = serialize([lattice, core])
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(data)
    print(f"{OUTPUT}: {vertex_count} vertices, {triangle_count} triangles, "
          f"bounds {tuple(round(v, 3) for v in lo)} - {tuple(round(v, 3) for v in hi)}")


if __name__ == "__main__":
    main()
