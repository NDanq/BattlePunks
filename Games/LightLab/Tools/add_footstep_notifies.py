#!/usr/bin/env python3
"""
add_footstep_notifies.py -- gives an animation clip a footstep notify each
time a paw lands: the fox clips' (Content/Animation/Fox_Walk.nmanim,
Fox_Run.nmanim), so its footsteps play in step (the Audio module plays a
notify's Sound at its bone).

For each paw bone it poses the skeleton at every frame (the clip's tracks
over the skeleton's reference pose, parent before child, as RenderCore's
AnimPose does), follows the paw's height in model space, and takes a landing
where the paw comes down into the lowest part of its swing: below 15% of its
range above its lowest point, having been above 35% since the last one. Each
landing becomes a notify named "Footstep" at that time, at that bone, playing
the sound given. The clip's other notifies are kept; earlier "Footstep" ones
are replaced, so running it again is safe.

Writes the clip as RenderCore's AnimClipAsset.cpp reads one: every chunk
copied as it was but AHDR's notify count and ANTF, written as v3 (each
notify's name, time, particle system and bone, then each one's sound).
Needs numpy. Usage:

    python Games/LightLab/Tools/add_footstep_notifies.py
        [--skeleton Content/Animation/Fox_Skeleton.nmskel]
        [--sound Content/Sounds/Footstep.nmsound]
        [clip ...]            (default: Fox_Walk and Fox_Run)
"""

import argparse
import struct
from pathlib import Path

import numpy as np

PROJECT = Path(__file__).resolve().parent.parent
PAWS = ["b_LeftHand_011", "b_RightHand_08", "b_LeftFoot02_018", "b_RightFoot02_022"]
LOW, HIGH = 0.15, 0.35
NAME = "Footstep"


# --- EngineCore::Archive ------------------------------------------------------------------------

class Reader:
    def __init__(self, data, at=0):
        self.data, self.at = data, at

    def take(self, fmt):
        values = struct.unpack_from("<" + fmt, self.data, self.at)
        self.at += struct.calcsize("<" + fmt)
        return values if len(values) > 1 else values[0]

    def string(self):
        length = self.take("I")
        text = self.data[self.at:self.at + length].decode("utf-8")
        self.at += length
        return text


def chunks(data, start, end):
    """(tag, version, payload start, payload end) for each chunk in [start, end)."""
    out = []
    at = start
    while at + 10 <= end:
        tag = data[at:at + 4][::-1].decode("ascii")      # little-endian: "NANM" is stored "MNAN"
        version, size = struct.unpack_from("<HI", data, at + 4)
        out.append((tag, version, at + 10, at + 10 + size))
        at += 10 + size
    return out


def chunk_bytes(tag, version, payload):
    return tag[::-1].encode("ascii") + struct.pack("<HI", version, len(payload)) + payload


def string_bytes(text):
    raw = text.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


# --- Reading ------------------------------------------------------------------------------------

def read_skeleton(path):
    data = path.read_bytes()
    (tag, _, start, end), = chunks(data, 0, len(data))[:1]
    assert tag == "NSKL", f"{path}: not a skeleton"
    bones = []
    for tag, _, start, end in chunks(data, start, end):
        if tag != "SBON":
            continue
        r = Reader(data, start)
        for _ in range(r.take("I")):
            name, parent = r.string(), r.take("i")
            values = r.take("10f")
            bones.append((name, parent, np.array(values[0:3]), np.array(values[3:7]), np.array(values[7:10])))
    return bones


def read_clip(path):
    data = path.read_bytes()
    (tag, version, start, end), = chunks(data, 0, len(data))[:1]
    assert tag == "NANM", f"{path}: not a clip"
    clip = {"data": data, "outer": (version, start, end), "chunks": chunks(data, start, end), "tracks": {}, "notifies": []}
    for tag, version, start, end in clip["chunks"]:
        r = Reader(data, start)
        if tag == "AHDR":
            clip["rate"], clip["frames"], _, _ = r.take("fIBB")
            r.string(), r.string()
            clip["track_count"], clip["notify_count"] = r.take("II")
            clip["notify_count_at"] = r.at - 4
        elif tag == "ATRK":
            for _ in range(clip["track_count"]):
                bone = r.string()
                keys = []
                for width in (3, 4, 3):
                    count = r.take("I")
                    keys.append(np.array(r.take(f"{count * width}f")).reshape(count, width))
                clip["tracks"][bone] = keys
        elif tag == "ANTF":
            notifies = []
            for _ in range(clip["notify_count"]):
                name, time = r.string(), r.take("f")
                particles, bone = (r.string(), r.string()) if version >= 2 else ("", "")
                notifies.append([name, time, particles, bone, ""])
            if version >= 3:
                for notify in notifies:
                    notify[4] = r.string()
            clip["notifies"] = notifies
    return clip


# --- Posing -------------------------------------------------------------------------------------

def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw,
                     aw * bw - ax * bx - ay * by - az * bz])


def quat_rotate(q, v):
    u, w = q[:3], q[3]
    return v + 2.0 * np.cross(u, np.cross(u, v) + w * v)


def paw_heights(skeleton, clip, paws):
    index = {bone[0]: i for i, bone in enumerate(skeleton)}
    heights = {paw: np.zeros(clip["frames"]) for paw in paws}
    for frame in range(clip["frames"]):
        model = []
        for name, parent, t, r, s in skeleton:
            if name in clip["tracks"]:
                keys = clip["tracks"][name]
                t, r, s = (k[min(frame, len(k) - 1)] for k in keys)
                r = r / np.linalg.norm(r)
            if parent < 0:
                model.append((t, r, s))
                continue
            pt, pr, ps = model[parent]
            model.append((pt + quat_rotate(pr, ps * t), quat_mul(pr, r), ps * s))
        for paw in paws:
            heights[paw][frame] = model[index[paw]][0][1]
    return heights


def landings(height):
    lowest, highest = height.min(), height.max()
    span = max(highest - lowest, 1e-6)
    level = (height - lowest) / span
    # A loop: the end leads into the start, so the paw starts as it last was
    # -- up (above HIGH) or down (below LOW) -- before the wrap.
    raised = False
    for value in level[::-1]:
        if value > HIGH or value < LOW:
            raised = value > HIGH
            break
    found = []
    for frame, value in enumerate(level):
        if value > HIGH:
            raised = True
        elif value < LOW and raised:
            found.append(frame)
            raised = False
    return found


# --- Writing ------------------------------------------------------------------------------------

def notifies_chunk(notifies):
    payload = b""
    for name, time, particles, bone, _ in notifies:
        payload += string_bytes(name) + struct.pack("<f", time) + string_bytes(particles) + string_bytes(bone)
    for *_, sound in notifies:
        payload += string_bytes(sound)
    return chunk_bytes("ANTF", 3, payload)


def write_clip(path, clip, notifies):
    data = clip["data"]
    body = b""
    for tag, version, start, end in clip["chunks"]:
        if tag == "ANTF":
            body += notifies_chunk(notifies)
            continue
        payload = bytearray(data[start:end])
        if tag == "AHDR":
            struct.pack_into("<I", payload, clip["notify_count_at"] - start, len(notifies))
        body += chunk_bytes(tag, version, bytes(payload))
    outer_version = clip["outer"][0]
    path.write_bytes(chunk_bytes("NANM", outer_version, body))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--skeleton", default="Content/Animation/Fox_Skeleton.nmskel")
    parser.add_argument("--sound", default="Content/Sounds/Footstep.nmsound")
    parser.add_argument("clips", nargs="*", default=["Content/Animation/Fox_Walk.nmanim", "Content/Animation/Fox_Run.nmanim"])
    args = parser.parse_args()

    skeleton = read_skeleton(PROJECT / args.skeleton)
    for clip_path in args.clips:
        path = PROJECT / clip_path
        clip = read_clip(path)
        heights = paw_heights(skeleton, clip, PAWS)
        notifies = [n for n in clip["notifies"] if n[0] != NAME]
        for paw in PAWS:
            for frame in landings(heights[paw]):
                notifies.append([NAME, frame / clip["rate"], "", paw, args.sound])
        notifies.sort(key=lambda n: n[1])
        write_clip(path, clip, notifies)
        duration = (clip["frames"] - 1) / clip["rate"]
        steps = ", ".join(f"{n[1]:.2f} s {n[3].split('_')[1]}" for n in notifies if n[0] == NAME)
        print(f"{clip_path}: {duration:.2f} s, {sum(n[0] == NAME for n in notifies)} footsteps -- {steps}")


if __name__ == "__main__":
    main()
