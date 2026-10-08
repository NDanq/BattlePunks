#!/usr/bin/env python3
"""
make_footstep_sound.py -- writes Tools/Audio/Footstep.wav, a fox's paw on
soft ground: what the fox clips' footstep notifies play
(Tools/add_footstep_notifies.py puts them on the paws).

A short, soft step, about 120 ms:

    thump    a low body, 70 Hz falling to 45, gone in 60 ms -- the weight
    scuff    noise through a band-pass round 1.8 kHz, a fast attack and a
             slightly longer tail -- the paw on grit

Quiet on purpose (-12 dB peak): four paws a stride, under the engine and the
world. 48 kHz, 16-bit mono WAV. Deterministic: running it again writes the
same bytes. Needs numpy. Usage:

    python Games/LightLab/Tools/make_footstep_sound.py

then import it into Content/Sounds (the editor's console: import
Tools/Audio/Footstep.wav Content/Sounds). Only the .nmsound is committed.
"""

import struct
from pathlib import Path

import numpy as np

RATE = 48000
SECONDS = 0.12
OUTPUT = Path(__file__).resolve().parent / "Audio" / "Footstep.wav"


def thump(t):
    frequency = 45.0 + 25.0 * np.exp(-t / 0.02)
    phase = 2 * np.pi * np.cumsum(frequency) / RATE
    return np.sin(phase) * np.exp(-t / 0.018) * (1.0 - np.exp(-t / 0.002))


def scuff(count, t):
    noise = np.random.default_rng(17).standard_normal(count)
    # A two-pole band-pass (RBJ cookbook), centre 1.8 kHz, Q 1.2.
    w0 = 2 * np.pi * 1800.0 / RATE
    alpha = np.sin(w0) / (2 * 1.2)
    b0, b2 = alpha, -alpha
    a0, a1, a2 = 1 + alpha, -2 * np.cos(w0), 1 - alpha
    out = np.zeros(count)
    x1 = x2 = y1 = y2 = 0.0
    for i in range(count):
        y = (b0 * noise[i] + b2 * x2 - a1 * y1 - a2 * y2) / a0
        x2, x1 = x1, noise[i]
        y2, y1 = y1, y
        out[i] = y
    return out * np.exp(-t / 0.03) * (1.0 - np.exp(-t / 0.001))


def main():
    count = int(RATE * SECONDS)
    t = np.arange(count) / RATE
    signal = 0.7 * thump(t) + 0.45 * scuff(count, t)
    fade = np.minimum(1.0, (count - np.arange(count)) / (0.01 * RATE))   # no click at the end
    signal *= fade
    signal *= 0.25 / np.max(np.abs(signal))         # -12 dB peak

    samples = np.round(signal * 32767.0).astype("<i2")
    data = samples.tobytes()
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)
    print(f"wrote {OUTPUT} ({len(data) // 2} samples, {SECONDS * 1000:.0f} ms at {RATE} Hz)")


if __name__ == "__main__":
    main()
