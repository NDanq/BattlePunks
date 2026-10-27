#!/usr/bin/env python3
"""
compare.py -- TranslucencyLab captures, before against after
(Engine/Modules/RenderCore/LIT_TRANSLUCENCY.md, "Validation").

    py -3 compare.py <dir> <tagA> <tagB> [--views 0 1 2 3] [--diff]

For each view: PSNR of <tagA>_v<view>.bmp against <tagB>_v<view>.bmp over the
whole frame and over each named region of that view (REGIONS, in pixels of a
1280 x 720 capture, scaled to the capture's size), and the mean luminance
change in each region (negative: darker). --diff writes <tagB>_v<view>_diff.bmp,
the absolute difference x 8.
"""
import argparse
import math
import struct
from pathlib import Path

# (x0, y0, x1, y1) as fractions of the frame: where each phase is expected to act.
REGIONS = {
    0: {"glass": (0.05, 0.35, 0.35, 0.75), "water": (0.38, 0.55, 0.62, 0.8), "plume": (0.72, 0.1, 0.98, 0.8)},
    1: {"shadowed": (0.1, 0.25, 0.45, 0.85), "lit": (0.55, 0.25, 0.9, 0.85)},
    2: {"plume": (0.3, 0.05, 0.7, 0.75), "floor": (0.2, 0.75, 0.8, 1.0)},
    3: {"water": (0.1, 0.45, 0.9, 1.0), "wall": (0.1, 0.0, 0.9, 0.4)},
}


def read_bmp(path):
    data = Path(path).read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise SystemExit(f"{path}: {bpp}-bit, want 24")
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        src = (abs(h) - 1 - y) if h > 0 else y
        start = offset + src * stride
        rows.append(data[start:start + w * 3])   # B, G, R
    return w, abs(h), rows


def stats(a, b, box):
    w, h, ra = a
    _, _, rb = b
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
    sq = 0.0
    lum = 0.0
    n = 0
    for y in range(y0, y1):
        la, lb = ra[y], rb[y]
        for i in range(x0 * 3, x1 * 3, 3):
            for c in range(3):
                d = la[i + c] - lb[i + c]
                sq += d * d
            lum += (0.0722 * (lb[i] - la[i]) + 0.7152 * (lb[i + 1] - la[i + 1]) + 0.2126 * (lb[i + 2] - la[i + 2]))
            n += 1
    mse = sq / max(n * 3, 1)
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    return psnr, lum / max(n, 1)


def write_diff(a, b, path):
    w, h, ra = a
    _, _, rb = b
    pad = (-(w * 3)) % 4
    pixels = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytes(min(255, abs(p - q) * 8) for p, q in zip(ra[y], rb[y]))
        pixels += row + b"\0" * pad
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    Path(path).write_bytes(header + info + pixels)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--views", type=int, nargs="*", default=[0, 1, 2, 3])
    ap.add_argument("--diff", action="store_true")
    args = ap.parse_args()
    d = Path(args.dir)
    for view in args.views:
        pa, pb = d / f"{args.a}_v{view}.bmp", d / f"{args.b}_v{view}.bmp"
        if not pa.exists() or not pb.exists():
            print(f"view {view}: missing {pa.name if not pa.exists() else pb.name}")
            continue
        a, b = read_bmp(pa), read_bmp(pb)
        if a[:2] != b[:2]:
            print(f"view {view}: sizes differ {a[:2]} {b[:2]}")
            continue
        whole, _ = stats(a, b, (0, 0, 1, 1))
        parts = "  ".join(f"{name} {p:.1f} dB {l:+.1f}" for name, box in REGIONS.get(view, {}).items()
                          for p, l in [stats(a, b, box)])
        print(f"view {view}: whole {whole:.1f} dB  {parts}")
        if args.diff:
            write_diff(a, b, d / f"{args.b}_v{view}_diff.bmp")


if __name__ == "__main__":
    main()
