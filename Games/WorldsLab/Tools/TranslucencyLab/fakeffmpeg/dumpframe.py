"""Stands in for ffmpeg under FrameRecorder: raw RGBA frames on stdin, the
last one written as <out>.bmp (24-bit, bottom-up rows)."""
import struct
import sys

args = sys.argv[1:]
w, h = map(int, args[args.index("-s") + 1].split("x"))
out = args[-1].strip('"')
size = w * h * 4
last = None
while True:
    buf = sys.stdin.buffer.read(size)
    if len(buf) < size:
        break
    last = buf
if last is None:
    sys.exit(1)
pad = (-(w * 3)) % 4
pixels = bytearray()
for y in range(h - 1, -1, -1):
    line = last[y * w * 4:(y + 1) * w * 4]
    row = bytearray(w * 3)
    row[0::3] = line[2::4]
    row[1::3] = line[1::4]
    row[2::3] = line[0::4]
    pixels += row + b"\0" * pad
header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
with open(out.rsplit(".", 1)[0] + ".bmp", "wb") as f:
    f.write(header + info + pixels)
