#!/usr/bin/env python3
"""
make_engine_loop_sound.py -- writes Tools/Audio/EngineLoop.wav, the PhysicsLab
buggy's engine at idle: one second that loops without a seam, for its Audio
Source to pitch up with the revs (Content/Prefabs/EngineSound.script.nmgraph).

An engine's sound is its cylinders firing: a pulse each time one does, ringing
through the block and the exhaust. Here a four-cylinder at 1650 rpm, 55 firings
a second:

    pulses     each firing a decaying burst of low harmonics, slightly uneven
               from one cylinder to the next (a real engine is never perfect)
    rumble     the firing frequency's own harmonics underneath, steady
    exhaust    noise, low-passed, swelling with each pulse

Everything repeats a whole number of times in the second -- 55 firings, the
cylinder pattern every 4, the noise filtered round the loop -- so the last
sample runs into the first. 48 kHz, 16-bit mono WAV. Deterministic: running it
again writes the same bytes. Needs numpy. Usage:

    python Games/LightLab/Tools/make_engine_loop_sound.py

then import it into Content/Sounds (the editor's Content Browser, or its
console: import Tools/Audio/EngineLoop.wav Content/Sounds). The .nmsound keeps
the WAV inside it, so only the .nmsound is committed; run this again to
reimport.
"""

import struct
from pathlib import Path

import numpy as np

RATE = 48000
SECONDS = 1.0
FIRINGS = 55            # a second: whole, so the loop closes
CYLINDERS = 4
OUTPUT = Path(__file__).resolve().parent / "Audio" / "EngineLoop.wav"


def firing_pulses(t):
    """A decaying burst at each firing, each cylinder a little different."""
    rng = np.random.default_rng(1650)
    cylinder_gain = 1.0 + 0.12 * (rng.random(CYLINDERS) - 0.5)
    cylinder_ring = 1.0 + 0.10 * (rng.random(CYLINDERS) - 0.5)

    period = 1.0 / FIRINGS
    index = np.floor(t / period).astype(int)
    phase = t - index * period                      # seconds since this firing
    cylinder = index % CYLINDERS

    gain = cylinder_gain[cylinder]
    ring = cylinder_ring[cylinder]
    envelope = np.exp(-phase / (0.0045 * ring))
    burst = (np.sin(2 * np.pi * 180.0 * ring * phase) * 0.6
             + np.sin(2 * np.pi * 410.0 * ring * phase) * 0.25
             + np.sin(2 * np.pi * 760.0 * phase) * 0.1)
    return gain * envelope * burst, np.exp(-phase / 0.008)


def rumble(t):
    """The firing frequency's harmonics: whole cycles in the second."""
    out = np.zeros_like(t)
    for harmonic, amplitude in ((1, 0.35), (2, 0.22), (3, 0.12), (4, 0.08), (6, 0.04)):
        out += amplitude * np.sin(2 * np.pi * FIRINGS * harmonic * t + harmonic * 0.7)
    return out


def exhaust(count, swell):
    """White noise through a one-pole low-pass, run twice round the loop so
    the filter's state at the end is its state at the start."""
    noise = np.random.default_rng(55).standard_normal(count)
    alpha = 1.0 - np.exp(-2 * np.pi * 900.0 / RATE)
    state = 0.0
    out = np.empty(count)
    for _ in range(2):
        for i in range(count):
            state += alpha * (noise[i] - state)
            out[i] = state
    return out * (0.25 + 0.75 * swell)


def main():
    count = int(RATE * SECONDS)
    t = np.arange(count) / RATE
    pulses, swell = firing_pulses(t)
    signal = 0.55 * pulses + 0.45 * rumble(t) + 0.12 * exhaust(count, swell)
    signal *= 0.8 / np.max(np.abs(signal))         # -2 dB peak

    samples = np.round(signal * 32767.0).astype("<i2")
    data = samples.tobytes()
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)
    print(f"wrote {OUTPUT} ({len(data) // 2} samples, {SECONDS:.1f} s at {RATE} Hz)")


if __name__ == "__main__":
    main()
