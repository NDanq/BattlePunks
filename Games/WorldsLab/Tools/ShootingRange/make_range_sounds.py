#!/usr/bin/env python3
"""
make_range_sounds.py -- synthesises the Shooting Range's sounds as WAVs in
Tools/ShootingRange/Audio/ (48 kHz, 16-bit mono), for the editor to import
into Content/Sounds/ShootingRange/ (only the .nmsound files are kept):

    Gunshot    a 9 mm pistol indoors: the crack, the chamber's thump, and the
               warehouse answering -- early reflections off the slab and the
               walls, then a long metallic tail off the siding
    SteelHit   a round on an AR500 plate: a hard tick and the plate ringing
    PaperHit   through card into the backer: a dull slap
    Impact     a round into concrete or block: a smack and grit falling
    DryFire    the striker on an empty chamber: two small clicks
    Reload     magazine out, a new one seated, the slide released

Deterministic (seeded). Then, from the WorldsLab binaries folder:

    python Games/WorldsLab/Tools/ShootingRange/make_range_sounds.py
    QT_QPA_PLATFORM=offscreen ./NimbleEditor --rhi 0 \\
        --command "import Tools/ShootingRange/Audio/Gunshot.wav Content/Sounds/ShootingRange" ... --command quit
"""

import wave
from pathlib import Path

import numpy as np

RATE = 48000
OUT = Path(__file__).resolve().parent / "Audio"
RNG = np.random.default_rng(4242)


def t_axis(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def noise(seconds):
    return RNG.standard_normal(int(seconds * RATE))


def lowpass(x, cutoff):
    """One-pole low-pass, run twice."""
    a = np.exp(-2 * np.pi * cutoff / RATE)
    for _ in range(2):
        y = np.empty_like(x)
        acc = 0.0
        for i, v in enumerate(x):
            acc = (1 - a) * v + a * acc
            y[i] = acc
        x = y
    return x


def highpass(x, cutoff):
    return x - lowpass(x, cutoff)


def env(t, attack, decay):
    return np.minimum(1.0, t / max(attack, 1e-5)) * np.exp(-t / decay)


def place(dst, src, at_seconds, gain=1.0):
    i = int(at_seconds * RATE)
    n = min(len(src), len(dst) - i)
    if n > 0:
        dst[i:i + n] += src[:n] * gain


def write(name, x, peak=0.89):
    x = x / max(np.max(np.abs(x)), 1e-9) * peak
    fade = min(len(x), int(0.01 * RATE))
    x[-fade:] *= np.linspace(1, 0, fade)
    OUT.mkdir(parents=True, exist_ok=True)
    with wave.open(str(OUT / f"{name}.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    print(f"  {name}.wav  {len(x) / RATE:.2f} s")


def gunshot():
    total = 1.6
    out = np.zeros(int(total * RATE))
    t = t_axis(0.25)
    crack = highpass(noise(0.25), 900) * env(t, 0.0002, 0.006)
    body = lowpass(noise(0.25), 2200) * env(t, 0.0005, 0.03)
    thump = np.sin(2 * np.pi * (70 + 40 * np.exp(-t / 0.02)) * t) * env(t, 0.001, 0.07)
    shot = crack * 1.0 + body * 0.7 + thump * 1.4
    place(out, shot, 0.0)
    # Early reflections: slab, walls, the roof, each duller than the last.
    for delay, gain, cut in ((0.009, 0.55, 5000), (0.031, 0.42, 3500), (0.047, 0.38, 3000), (0.068, 0.3, 2500),
                             (0.094, 0.24, 2000), (0.121, 0.18, 1800)):
        place(out, lowpass(shot, cut), delay, gain)
    # The tail: noise ringing off corrugated steel, decaying over a second.
    tt = t_axis(total)
    tail = lowpass(noise(total), 1600) * env(tt, 0.02, 0.38) * 0.35
    ring = sum(np.sin(2 * np.pi * f * tt + RNG.random() * 6) * np.exp(-tt / d)
               for f, d in ((143, 0.5), (311, 0.4), (587, 0.3), (1213, 0.2)))
    tail += ring * env(tt, 0.03, 0.6) * 0.02
    out += tail
    write("Gunshot", out)


def steel_hit():
    total = 1.4
    t = t_axis(total)
    tick = highpass(noise(total), 2000) * env(t, 0.0001, 0.003)
    ring = np.zeros_like(t)
    for f, amp, d in ((812, 1.0, 0.7), (1957, 0.6, 0.5), (3140, 0.45, 0.32), (4415, 0.3, 0.22), (5870, 0.2, 0.15)):
        ring += amp * np.sin(2 * np.pi * f * t * (1 + 0.0008 * np.sin(2 * np.pi * 5 * t))) * np.exp(-t / d)
    write("SteelHit", tick * 0.9 + ring * env(t, 0.0005, 2.0) * 0.6)


def paper_hit():
    t = t_axis(0.35)
    slap = lowpass(noise(0.35), 1400) * env(t, 0.0005, 0.025)
    thud = np.sin(2 * np.pi * 115 * t) * env(t, 0.001, 0.05)
    write("PaperHit", slap + thud * 0.6, peak=0.7)


def impact():
    total = 0.9
    t = t_axis(total)
    smack = lowpass(highpass(noise(total), 500), 4500) * env(t, 0.0002, 0.012)
    grit = np.zeros_like(t)
    for _ in range(40):
        at = 0.01 + RNG.random() * 0.5
        g = highpass(noise(0.01), 3000) * env(t_axis(0.01), 0.0002, 0.002) * (0.2 + 0.4 * RNG.random()) * np.exp(-at / 0.3)
        place(grit, g, at)
    write("Impact", smack + grit, peak=0.75)


def dry_fire():
    out = np.zeros(int(0.2 * RATE))
    t = t_axis(0.02)
    click = highpass(noise(0.02), 2500) * env(t, 0.0001, 0.0015)
    place(out, click, 0.0, 1.0)
    place(out, click, 0.011, 0.6)
    write("DryFire", out, peak=0.6)


def reload_sound():
    total = 1.7
    out = np.zeros(int(total * RATE))

    def clack(cut_lo, decay, tone):
        t = t_axis(0.12)
        c = highpass(noise(0.12), cut_lo) * env(t, 0.0002, decay)
        return c + np.sin(2 * np.pi * tone * t) * env(t, 0.0005, decay * 2) * 0.3

    place(out, clack(1800, 0.004, 1900), 0.10, 0.7)                  # release button
    t = t_axis(0.18)
    place(out, lowpass(noise(0.18), 2500) * env(t, 0.01, 0.06) * 0.25, 0.14)   # magazine sliding out
    place(out, clack(900, 0.008, 700), 0.30, 0.6)                    # it drops into the hand
    place(out, lowpass(noise(0.15), 3000) * env(t_axis(0.15), 0.01, 0.05) * 0.3, 0.85)
    place(out, clack(700, 0.012, 520), 1.00, 1.0)                    # seated
    place(out, clack(1500, 0.006, 1300), 1.38, 0.9)                  # slide forward
    place(out, clack(600, 0.02, 380), 1.40, 0.7)
    write("Reload", out, peak=0.7)


def main():
    print(f"Writing {OUT}")
    gunshot()
    steel_hit()
    paper_hit()
    impact()
    dry_fire()
    reload_sound()


if __name__ == "__main__":
    main()
