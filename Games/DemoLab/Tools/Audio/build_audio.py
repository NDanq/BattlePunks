#!/usr/bin/env python3
"""
build_audio.py -- builds every Demo Lab sound and music asset as an
engine-ready `.nmsound` under Content/Sounds/ and Content/Music/, and writes
Content/Sounds/README.md (the table the gameplay code wires from).

Most sounds are synthesised here (dsp.py). The rest are made from CC0
recordings that fetch_cc0.py downloads (Kenney packs, OpenGameArt), trimmed,
layered and loudness-normalised by this script; Docs/Credits.md cites each.

    python Games/DemoLab/Tools/Audio/fetch_cc0.py /tmp/demolab_cc0
    python Games/DemoLab/Tools/Audio/build_audio.py --cc0 /tmp/demolab_cc0
    python Games/DemoLab/Tools/Audio/build_audio.py --only Pistol --preview /tmp/wavs

Without --cc0 the CC0-derived assets are left as they are on disk.
Deterministic: each asset seeds its own random stream from its name.

Formats (Engine/Modules/Audio/README.md): one-shots up to 2 s are
Decompressed (16-bit mono PCM at 48 kHz, FLAC source); longer one-shots and
3D loops are Compressed (mono Ogg Vorbis); ambience beds and music Stream
(stereo Ogg Vorbis). Loudness is BS.1770: one-shots by their loudest 400 ms
(LUFS-M), loops, beds and music integrated (LUFS-I); -1 dBFS peak ceiling.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dsp  # noqa: E402
import nmsound  # noqa: E402
from dsp import (BAR, PLATE, TUBE, bp, brown, drive, env_ad, env_lin, fade, glide, hp, loopify, lp, metal,  # noqa: E402
                 modal, n_of, noise, osc, pad_to, pan, pink, place, reso, reverb, scatter, silence, stretch,
                 sweep_bp, sweep_lp, t_axis, to_stereo, warp)

GAME = Path(__file__).resolve().parents[2]
SOUNDS = GAME / "Content" / "Sounds"
MUSIC = GAME / "Content" / "Music"

ASSETS = []
CC0 = None   # Path of fetch_cc0.py's directory, when given


def U(a, b):
    return dsp.R.uniform(a, b)


def asset(name, folder, use, *, loop=False, bus="SFX", spatial=True, target=-16.0, mind=100.0, maxd=3000.0,
          cc0=None, mode=None, quality=None, rate=dsp.RATE, variants=None):
    """Registers a generator. variants=N registers name_01..name_N, calling fn(i)."""
    def deco(fn):
        names = [(f"{name}_{i + 1:02d}", i) for i in range(variants)] if variants else [(name, None)]
        for n, i in names:
            ASSETS.append(dict(name=n, folder=folder, use=use, loop=loop, bus=bus, spatial=spatial, target=target,
                               mind=mind, maxd=maxd, cc0=cc0, mode=mode, quality=quality, rate=rate,
                               fn=(lambda f=fn, i=i: f(i)) if variants else fn))
        return fn
    return deco


def cc0_file(rel):
    return CC0 / rel


def kenney(pack, file, rate=dsp.RATE):
    return dsp.read_audio(cc0_file(f"Kenney{pack}/{file}"), rate=rate, mono=True)


# =============================================================================
# Building blocks
# =============================================================================

def burst(seconds, lo, hi, attack=0.0005, decay=0.02):
    return bp(noise(seconds), lo, hi) * env_ad(n_of(seconds), attack, decay)


def thud(f0=70.0, f1=40.0, seconds=0.4, decay=0.12, noise_amt=0.6):
    t = t_axis(seconds)
    body = osc(glide(seconds, [(0, f0), (seconds, f1)]), seconds) * env_ad(t, 0.001, decay)
    rumble = lp(noise(seconds), 180) * env_ad(t, 0.001, decay * 0.7) * noise_amt * 3
    return body + rumble


def click(seconds=0.03, f=3500.0, decay=0.004):
    return metal(seconds, f, ratios=[1.0, 1.7, 2.9], decay=decay, bright=0.7) + burst(seconds, 2000, 9000, decay=decay * 0.5) * 0.5


def hiss(seconds, lo=1500, hi=9000, attack=0.01, decay=0.3, wobble=0.3):
    t = t_axis(seconds)
    x = bp(noise(seconds), lo, hi) * env_ad(t, attack, decay)
    am = 1 + wobble * lp(noise(seconds), 12) * 4
    return x * np.clip(am, 0.2, 2.0)


def crackle(seconds, density=60.0, lo=1500, hi=9000):
    """Electric sparks: sparse random impulses with tiny ringing."""
    n = n_of(seconds)
    x = np.zeros(n)
    k = int(density * seconds)
    idx = dsp.R.integers(0, n, k)
    x[idx] = dsp.R.standard_normal(k) * (dsp.R.random(k) ** 2)
    x = bp(x, lo, hi)
    return x * 6


def buzz(seconds, f=100.0, harsh=4.0):
    return drive(osc(f, seconds, "saw") + 0.3 * noise(seconds) * 0.2, harsh)


def whoosh(seconds, f_lo=250, f_hi=1400, peak=0.5, q=1.5):
    t = t_axis(seconds)
    e = np.exp(-((t / seconds - peak) ** 2) / 0.03)
    c = lambda tt: f_lo + (f_hi - f_lo) * np.exp(-((tt / seconds - peak) ** 2) / 0.04)
    return sweep_bp(noise(seconds), c, q) * e * 2


def servo(seconds, contour, harsh=2.0, ripple=70.0, bright=5000.0):
    t = t_axis(seconds)
    f = glide(seconds, contour)
    x = osc(f, seconds, "saw") + 0.5 * osc(f * 2.01, seconds, "sine") + 0.25 * osc(f * 3.03, seconds, "sine")
    x *= 1 + 0.35 * np.sin(2 * np.pi * ripple * t)
    x = drive(x, harsh)
    x = bp(x, 200, bright) + 0.08 * bp(noise(seconds), 2000, 8000)
    e = np.minimum(1, t / 0.025) * np.minimum(1, (seconds - t) / 0.06)
    return x * np.clip(e, 0, 1)


def steam_voice(seconds, contour, formants=(900, 2300), q=4.0, breath=0.4, vibrato=(7.0, 0.02), rough=0.0):
    """A steam whistle that talks: a pitched tone through vowel-like resonances, plus breath."""
    t = t_axis(seconds)
    f = glide(seconds, contour) * (1 + vibrato[1] * np.sin(2 * np.pi * vibrato[0] * t))
    tone = osc(f, seconds, "tri") + 0.35 * osc(f, seconds, "saw")
    if rough:
        tone *= 1 + rough * np.tanh(3 * np.sin(np.pi * np.cumsum(f) / dsp.RATE))   # subharmonic growl
    voiced = sum(reso(tone, fm, q) for fm in formants) + 0.25 * tone
    air = bp(noise(seconds), 1200, 7000) * breath * 0.1
    e = np.minimum(1, t / 0.02) * np.clip((seconds - t) / 0.08, 0, 1)
    return (voiced + air) * e


def gull_call(rng_pitch=1.0):
    """A herring gull's 'kee-ow' and laughing 'ha-ha' (synthetic, heard from a distance)."""
    calls = []
    kind = dsp.R.integers(0, 3)
    count = [1, 3, 5][kind]
    out = silence(0.4 * count + 0.6)
    at = 0.0
    for k in range(count):
        d = U(0.18, 0.32) if count > 1 else U(0.45, 0.6)
        top = U(2000, 2700) * rng_pitch
        contour = [(0, top * 0.55), (d * 0.25, top), (d, top * U(0.45, 0.6))]
        v = steam_voice(d, contour, formants=(1500, 2900, 4200), q=2.5, breath=0.15, vibrato=(28, 0.03), rough=0.25)
        place(out, v, at, 1.0 - 0.1 * k)
        at += d + U(0.04, 0.12)
    return out


def room(x, rt60=0.6, wet=0.18, bright=7000):
    return reverb(x, rt60, wet=wet, bright=bright)


# =============================================================================
# Footsteps
# =============================================================================

def kenney_step(surface, i):
    x = kenney("Impact", f"footstep_{surface}_{i:03d}.ogg")
    x = hp(x, 50)
    return fade(pad_to(x, len(x) / dsp.RATE + 0.02), 0, 0.01)


@asset("Step_Stone", "Footsteps", "Footstep on stone, cobbles, concrete (L1 dock road, L2/L3 floors)", target=-22,
       mind=100, maxd=1500, variants=4, cc0="KenneyImpact:footstep_concrete_000..003")
def step_stone(i):
    return kenney_step("concrete", i)


@asset("Step_Wood", "Footsteps", "Footstep on wood (L1 pier, L2 archive floor)", target=-22, mind=100, maxd=1500,
       variants=4, cc0="KenneyImpact:footstep_wood_000..003")
def step_wood(i):
    return kenney_step("wood", i)


@asset("Step_Grass", "Footsteps", "Footstep on grass/earth (L1 meadow, forest edge, beach path)", target=-23,
       mind=100, maxd=1200, variants=4, cc0="KenneyImpact:footstep_grass_000..003")
def step_grass(i):
    return kenney_step("grass", i)


@asset("Step_MetalGrate", "Footsteps", "Footstep on a steel grate or catwalk (L3 ring gallery, gantries)",
       target=-22, mind=100, maxd=1800, variants=4)
def step_grate(i):
    d = 0.5
    out = silence(d)
    heel = lp(noise(0.03), 900) * env_ad(n_of(0.03), 0.0005, 0.006) * 1.5
    place(out, heel, 0.0)
    f0 = U(330, 520)
    place(out, metal(0.35, f0, PLATE, decay=0.09, bright=0.75), 0.0, 0.5)
    place(out, metal(0.2, f0 * U(1.9, 2.4), BAR, decay=0.05, bright=0.6), U(0.012, 0.02), 0.25)   # bar rattle
    place(out, lp(noise(0.03), 1500) * env_ad(n_of(0.03), 0.0005, 0.008), U(0.06, 0.09), 0.5)   # toe
    place(out, metal(0.25, f0 * U(1.1, 1.3), PLATE, decay=0.06, bright=0.7), U(0.06, 0.09), 0.25)
    return fade(room(out, 0.5, 0.12)[:n_of(0.6)], 0, 0.05)


@asset("Step_Tile", "Footsteps", "Footstep on marble or tile (L2 atrium, foyer and halls)", target=-22, mind=100,
       maxd=1600, variants=4)
def step_tile(i):
    """A hard heel click and the tile's short, bright ring, then the toe; a hall's tail."""
    d = 0.45
    out = silence(d)
    heel = bp(noise(0.012), 1800, 9000) * env_ad(n_of(0.012), 0.0003, 0.0025) * 1.6
    place(out, heel, 0.0)
    place(out, lp(noise(0.02), 700) * env_ad(n_of(0.02), 0.0005, 0.005), 0.0, 0.8)   # the weight landing
    f0 = U(1700, 2500)
    place(out, modal(0.12, [(f0, 0.018, 1.0), (f0 * U(1.45, 1.6), 0.012, 0.5), (f0 * U(2.3, 2.6), 0.008, 0.3)]),
          0.0, 0.35)
    toe = U(0.07, 0.1)
    place(out, bp(noise(0.01), 2500, 10000) * env_ad(n_of(0.01), 0.0003, 0.002), toe, 0.7)
    place(out, modal(0.08, [(f0 * U(1.05, 1.15), 0.012, 1.0)]), toe, 0.2)
    return fade(room(out, 0.9, 0.16, bright=9000)[:n_of(0.6)], 0, 0.06)


@asset("Step_Water", "Footsteps", "Footstep in a puddle or on a flooded floor (L2 sprinklers, shallows)",
       target=-22, mind=100, maxd=1500, variants=4)
def step_water(i):
    """A slap into shallow water, the splash, and a few drops and bubbles after."""
    d = 0.6
    out = silence(d)
    place(out, lp(noise(0.05), 450) * env_ad(n_of(0.05), 0.001, 0.012) * 1.2, 0.0)                    # the foot's slap
    splash = bp(noise(0.25), 600, 5500) * env_ad(n_of(0.25), 0.004, U(0.05, 0.08))
    place(out, splash, 0.003, 0.9)
    place(out, hp(noise(0.18), 3500) * env_ad(n_of(0.18), 0.01, 0.04), U(0.03, 0.05), 0.35)          # spray
    for _ in range(int(U(4, 8))):                                                                     # drops and bubbles
        at, dur = U(0.04, 0.4), U(0.015, 0.04)
        f = U(900, 2600)
        bubble = osc(glide(dur, [(0, f), (dur, f * U(1.3, 1.8))]), dur) * env_ad(n_of(dur), 0.001, dur * 0.35)
        place(out, bubble, at, U(0.08, 0.2))
    return fade(room(out, 0.4, 0.1)[:n_of(d)], 0, 0.08)


# =============================================================================
# Pipe pistol
# =============================================================================

@asset("Pistol_Shot", "Weapons", "Pipe pistol shot (player and Watchmen)", target=-10, mind=300, maxd=8000)
def pistol_shot(_=None):
    d = 1.6
    out = silence(d)
    crack = drive(hp(noise(0.006), 1200) * env_ad(n_of(0.006), 0.0001, 0.0015), 3) * 1.2
    place(out, crack, 0.0)
    t = t_axis(0.25)
    body = osc(glide(0.25, [(0, 190), (0.06, 70), (0.25, 48)]), 0.25) * env_ad(t, 0.0005, 0.045) * 1.6
    place(out, body, 0.0005)
    place(out, lp(noise(0.25), 1200) * env_ad(t, 0.0005, 0.035) * 1.6, 0.0)
    place(out, bp(noise(0.12), 600, 5000) * env_ad(n_of(0.12), 0.0005, 0.02), 0.0, 0.9)
    place(out, metal(0.4, 820, TUBE, decay=0.12, bright=0.6), 0.001, 0.18)        # the pipe rings
    place(out, hiss(0.35, 2500, 10000, 0.002, 0.08, 0.1), 0.004, 0.25)           # gas vents from the breech
    place(out, click(0.04, 2400, 0.006), 0.055, 0.25)                            # hammer resets
    out = drive(out, 1.6)
    return fade(reverb(out, 1.1, wet=0.28, bright=5500)[:n_of(d)], 0, 0.2)


@asset("Pistol_Reload", "Weapons", "Pipe pistol reload: breech opens, cartridge in, breech closes, pressure hiss",
       target=-17, mind=100, maxd=1500, cc0="KenneyRpg:metalLatch,metalClick")
def pistol_reload(_=None):
    d = 1.5
    out = silence(d)
    place(out, kenney("Rpg", "metalLatch.ogg"), 0.0, 0.9)
    t = t_axis(0.18)
    slide = bp(noise(0.18), 1500, 6000) * np.sin(np.pi * t / 0.18) ** 2 * 0.3
    place(out, slide, 0.42)
    place(out, click(0.05, 2900, 0.01), 0.6, 0.7)                                # cartridge seats
    place(out, kenney("Rpg", "metalClick.ogg"), 0.9, 1.0)                        # breech shuts
    place(out, thud(160, 90, 0.1, 0.02, 0.3), 0.9, 0.3)
    place(out, hiss(0.4, 3000, 11000, 0.02, 0.12, 0.2), 1.02, 0.18)              # pressure recharges
    return fade(room(out, 0.4, 0.1)[:n_of(d)], 0, 0.05)


@asset("Pistol_DryFire", "Weapons", "Pipe pistol trigger on an empty chamber", target=-22, mind=100, maxd=800,
       cc0="KenneyRpg:metalClick")
def pistol_dry(_=None):
    out = silence(0.35)
    place(out, click(0.05, 3200, 0.004), 0.0, 0.8)
    place(out, kenney("Rpg", "metalClick.ogg") * 0.6, 0.012)
    place(out, hiss(0.12, 3000, 9000, 0.002, 0.03, 0.0), 0.02, 0.06)
    return fade(out, 0, 0.03)


# =============================================================================
# Bullet impacts
# =============================================================================

@asset("Impact_Metal", "Impacts", "Bullet hits metal (pipes, plates, machinery)", target=-15, mind=200, maxd=3000,
       variants=3, cc0="KenneyImpact:impactMetal_light_000..002")
def impact_metal(i):
    d = 0.7
    out = silence(d)
    place(out, kenney("Impact", f"impactMetal_light_{i:03d}.ogg"), 0.0, 0.8)
    place(out, burst(0.01, 2000, 12000, decay=0.002) * 1.2, 0.0)
    f = U(2600, 3600)
    t = t_axis(0.4)
    ping = osc(glide(0.4, [(0, f), (0.4, f * U(0.7, 0.8))]), 0.4) * env_ad(t, 0.001, 0.09) * 0.3
    place(out, ping, U(0.0, 0.01) if i != 1 else 0.0, 1.0 if i != 2 else 0.3)
    place(out, metal(0.5, U(700, 1200), PLATE, decay=0.15, bright=0.7), 0.0, 0.3)
    return fade(room(out, 0.5, 0.15)[:n_of(d)], 0, 0.05)


@asset("Impact_Stone", "Impacts", "Bullet hits stone, brick or concrete: smack and falling grit", target=-16,
       mind=200, maxd=3000, variants=3, cc0="KenneyImpact:impactMining_000..002")
def impact_stone(i):
    d = 0.8
    out = silence(d)
    place(out, drive(hp(noise(0.004), 2000) * env_ad(n_of(0.004), 0.0001, 0.001), 2), 0.0, 1.2)
    place(out, lp(noise(0.06), 2500) * env_ad(n_of(0.06), 0.0003, 0.012) * 1.2, 0.0)
    place(out, kenney("Impact", f"impactMining_{i:03d}.ogg"), 0.0, 0.5)
    t = t_axis(0.6)
    grit = crackle(0.6, 160, 1500, 7000) * np.exp(-t / 0.15) * 0.25
    place(out, grit, 0.03)
    return fade(room(out, 0.5, 0.15)[:n_of(d)], 0, 0.05)


@asset("Impact_Robot", "Impacts", "Bullet hits a robot (Watchman enamel, Brute plates): hollow clank and sparks",
       target=-15, mind=200, maxd=3000, variants=3, cc0="KenneyImpact:impactPlate_medium_000..002")
def impact_robot(i):
    d = 0.7
    out = silence(d)
    place(out, kenney("Impact", f"impactPlate_medium_{i:03d}.ogg"), 0.0, 0.8)
    place(out, metal(0.6, U(180, 260), PLATE, decay=0.18, bright=0.55), 0.0, 0.5)     # hollow body
    place(out, burst(0.012, 1500, 10000, decay=0.003), 0.0, 1.0)
    t = t_axis(0.25)
    place(out, crackle(0.25, 200, 2500, 10000) * np.exp(-t / 0.06) * 0.35, 0.005)    # sparks
    return fade(room(out, 0.4, 0.12)[:n_of(d)], 0, 0.05)


# =============================================================================
# Robots: Watchman and Brute
# =============================================================================

@asset("Watchman_Servo", "Robots", "Watchman joint servo (turn, aim, step accent)", target=-21, mind=100,
       maxd=1500, variants=3)
def watchman_servo(i):
    d = [0.35, 0.5, 0.42][i]
    a, b, c = U(700, 900), U(1000, 1400), U(800, 1000)
    x = servo(d, [(0, a), (d * 0.4, b), (d, c)], harsh=2.2, ripple=U(60, 90))
    out = pad_to(x, d + 0.1)
    place(out, click(0.03, 4200, 0.004), d - 0.02, 0.3)
    return out


@asset("Brute_Servo", "Robots", "Brute hydraulic servo (heavy turn, wind-up)", target=-18, mind=150, maxd=2000,
       variants=3)
def brute_servo(i):
    d = [0.6, 0.8, 0.7][i]
    x = servo(d, [(0, U(120, 150)), (d * 0.6, U(200, 240)), (d, U(170, 200))], harsh=3.0, ripple=U(25, 35),
              bright=2500)
    x += hiss(d, 900, 3500, 0.05, d, 0.4) * 0.25 * np.clip(np.sin(np.pi * t_axis(d) / d), 0, 1)
    out = pad_to(x, d + 0.3)
    place(out, thud(90, 50, 0.3, 0.06, 0.5), d - 0.03, 0.6)
    place(out, metal(0.3, U(240, 320), PLATE, decay=0.08), d - 0.03, 0.2)
    return out


def chirp(contour, d, formants, **kw):
    out = pad_to(steam_voice(d, contour, formants, **kw), d + 0.4)
    return reverb(out, 0.5, wet=0.1)[:len(out)]


@asset("Watchman_Alert", "Robots", "Watchman spots the player: sharp rising steam chirp", target=-14, mind=200,
       maxd=4000)
def watchman_alert(_=None):
    d = 0.75
    a = steam_voice(0.28, [(0, 900), (0.12, 1700), (0.28, 1500)], (1100, 2600), q=3.5, breath=0.3, vibrato=(11, 0.03))
    b = steam_voice(0.4, [(0, 1400), (0.15, 2300), (0.4, 2100)], (1300, 2900), q=3.5, breath=0.3, vibrato=(13, 0.04))
    out = silence(d + 0.3)
    place(out, a, 0.0)
    place(out, b, 0.32, 1.1)
    place(out, hiss(0.2, 2500, 9000, 0.005, 0.06), 0.0, 0.1)
    return reverb(out, 0.5, wet=0.1)[:len(out)]


@asset("Watchman_Idle", "Robots", "Watchman idle / patrol warble (sparse, low)", target=-20, mind=150, maxd=2500,
       variants=2)
def watchman_idle(i):
    d = [0.8, 1.0][i]
    pts = [(0, U(550, 650)), (d * 0.3, U(750, 850)), (d * 0.55, U(600, 700)), (d * 0.8, U(800, 900)), (d, U(650, 700))]
    return chirp(pts, d, (700, 1900), q=3.0, breath=0.35, vibrato=(9, 0.025))


@asset("Watchman_Investigate", "Robots", "Watchman hears a noise: questioning chirp (rises at the end)",
       target=-18, mind=150, maxd=3000)
def watchman_investigate(_=None):
    d = 0.7
    return chirp([(0, 700), (0.35, 650), (0.5, 800), (0.7, 1500)], d, (800, 2200), q=3.0, breath=0.4,
                 vibrato=(10, 0.02))


@asset("Watchman_Hurt", "Robots", "Watchman takes damage: falling squeal with a steam burst", target=-15, mind=150,
       maxd=3000)
def watchman_hurt(_=None):
    d = 0.5
    out = pad_to(steam_voice(d, [(0, 2300), (0.1, 1900), (d, 700)], (1200, 2700), q=3.0, breath=0.3,
                             vibrato=(19, 0.05), rough=0.3), d + 0.3)
    place(out, hiss(0.3, 2000, 9000, 0.002, 0.08), 0.0, 0.35)
    place(out, crackle(0.2, 150), 0.0, 0.2)
    return out


@asset("Brute_Alert", "Robots", "Brute spots the player: deep steam-horn growl", target=-13, mind=300, maxd=5000)
def brute_alert(_=None):
    d = 1.2
    out = pad_to(steam_voice(d, [(0, 95), (0.25, 150), (0.9, 140), (d, 105)], (420, 850, 1700), q=2.5,
                             breath=0.04, vibrato=(5, 0.02), rough=0.6), d + 0.6)
    out = drive(out * 1.5, 2.0)
    place(out, hiss(0.9, 900, 6000, 0.03, 0.4, 0.4), 0.05, 0.06)
    return reverb(out, 0.8, wet=0.15)[:len(out)]


@asset("Brute_Hurt", "Robots", "Brute takes damage / armour plate breaks: grunting horn", target=-14, mind=300,
       maxd=4000)
def brute_hurt(_=None):
    d = 0.6
    out = pad_to(drive(steam_voice(d, [(0, 170), (0.15, 150), (d, 85)], (380, 800, 1600), q=2.5, breath=0.2,
                                   vibrato=(6, 0.03), rough=0.7) * 1.5, 2.0), d + 0.4)
    place(out, hiss(0.3, 1000, 6000, 0.002, 0.1), 0.0, 0.15)
    return out


@asset("Brute_Swing", "Robots", "Brute wrench swing (whoosh)", target=-17, mind=150, maxd=2000, variants=2)
def brute_swing(i):
    d = [0.5, 0.6][i]
    x = whoosh(d, U(180, 240), U(900, 1300), peak=U(0.45, 0.6), q=1.2)
    t = t_axis(d)
    x += osc(glide(d, [(0, 60), (d, 90)]), d) * np.exp(-((t / d - 0.55) ** 2) / 0.03) * 0.3
    return pad_to(x, d + 0.05)


@asset("Brute_Hit", "Robots", "Brute wrench connects (player or cover): heavy thud and clank", target=-11,
       mind=200, maxd=3000)
def brute_hit(_=None):
    d = 1.1
    out = silence(d)
    place(out, thud(85, 40, 0.5, 0.11, 0.9) * 1.3, 0.0)
    place(out, metal(0.8, 310, PLATE, decay=0.3, bright=0.6), 0.0, 0.45)
    place(out, burst(0.05, 800, 6000, decay=0.01), 0.0, 0.9)
    place(out, crackle(0.15, 300, 1000, 5000) * np.exp(-t_axis(0.15) / 0.04), 0.002, 0.3)
    return fade(drive(room(out, 0.6, 0.15)[:n_of(d)], 1.5), 0, 0.1)


@asset("Robot_Death", "Robots", "Watchman or Brute destroyed: power-down whine, steam release, clank cascade",
       target=-12, mind=200, maxd=4000)
def robot_death(_=None):
    d = 2.6
    out = silence(d)
    t = t_axis(1.2)
    whine = osc(glide(1.2, [(0, 1400), (0.3, 900), (1.2, 70)]), 1.2, "saw") * np.exp(-t / 0.6) * 0.35
    place(out, sweep_lp(whine, lambda tt: 6000 * np.exp(-tt * 2.0) + 300), 0.0)
    place(out, crackle(0.35, 250), 0.0, 0.4)
    place(out, hiss(1.8, 1500, 9000, 0.01, 0.7, 0.4), 0.05, 0.2)
    for k, at in enumerate([0.25, 0.55, 0.8, 1.05]):
        place(out, metal(0.8, U(250, 700) / (1 + 0.2 * k), PLATE, decay=0.25, bright=0.65), at, 0.5 - 0.06 * k)
        place(out, burst(0.02, 500, 5000, decay=0.006), at, 0.4)
    place(out, thud(70, 35, 0.6, 0.15, 1.0) * 1.3, 1.15)                            # the body lands
    place(out, metal(1.0, 160, PLATE, decay=0.4, bright=0.55), 1.15, 0.5)
    place(out, metal(0.5, 420, BAR, decay=0.15), 1.4, 0.25)                         # a plate rolls off
    return fade(room(out, 0.9, 0.18)[:n_of(d)], 0, 0.2)


# =============================================================================
# World: doors, machines, power
# =============================================================================

@asset("Shutter_Slam", "World", "Security shutter drops and slams (C5, alarm reroutes)", target=-11, mind=300,
       maxd=5000)
def shutter_slam(_=None):
    d = 2.6
    out = silence(d)
    # the roll: slats rattle faster as it falls
    rate = np.linspace(18, 45, n_of(0.45))
    ph = np.cumsum(rate) / dsp.RATE
    ticks = (np.diff(np.floor(ph), prepend=0) > 0).astype(float)
    place(out, bp(np.convolve(ticks, metal(0.03, 1800, BAR, decay=0.006), "full")[:n_of(0.45)], 700, 6000) * 0.8, 0.0)
    place(out, sweep_bp(noise(0.45), lambda tt: 400 + 2000 * tt, 1.5) * np.linspace(0.2, 0.8, n_of(0.45)), 0.0, 0.5)
    at = 0.45
    place(out, thud(75, 35, 0.8, 0.16, 1.0) * 1.6, at)
    place(out, metal(2.0, 135, PLATE, decay=0.7, bright=0.65), at, 0.6)
    place(out, metal(1.0, 520, PLATE, decay=0.2, bright=0.6), at, 0.25)
    place(out, burst(0.06, 600, 8000, decay=0.015), at, 1.0)
    for k in range(6):                                                             # slats settle
        place(out, metal(0.2, U(900, 1600), BAR, decay=0.05), at + 0.08 + k * U(0.04, 0.07), 0.15 / (k + 1))
    return fade(drive(reverb(out, 1.8, wet=0.3)[:n_of(d)], 1.3), 0, 0.3)


@asset("BlastDoor_Open", "World", "Plaza blast door: latches, steam release, gears and motor, travel, stop (C2)",
       target=-12, mind=500, maxd=8000, mode="Compressed")
def blast_door(_=None):
    d = 8.0
    out = silence(d)
    for k, at in enumerate([0.0, 0.35, 0.7]):                                     # three latches
        place(out, thud(110, 55, 0.4, 0.07, 0.8), at, 0.8)
        place(out, metal(0.8, U(180, 260), PLATE, decay=0.25), at, 0.4)
    place(out, hiss(2.2, 900, 9000, 0.02, 0.9, 0.5), 1.0, 0.9)                     # pressure release
    run0, run1 = 1.6, 6.6
    rd = run1 - run0
    t = t_axis(rd)
    ramp = np.clip(t / 0.8, 0, 1) * np.clip((rd - t) / 0.4, 0, 1)
    motor = (osc(glide(rd, [(0, 30), (0.8, 55), (rd, 52)]), rd, "saw") * 0.5
             + osc(glide(rd, [(0, 60), (0.8, 110), (rd, 104)]), rd, "sine") * 0.3)
    place(out, lp(motor, 700) * ramp, run0, 0.9)
    teeth_rate = np.interp(t, [0, 0.8, rd], [3, 16, 15])
    ticks = (np.diff(np.floor(np.cumsum(teeth_rate) / dsp.RATE), prepend=0) > 0).astype(float)
    gear = np.convolve(ticks, pad_to(thud(200, 120, 0.05, 0.012, 0.4), 0.08) + metal(0.08, 700, PLATE, decay=0.02) * 0.4,
                       "full")[:len(t)]
    place(out, gear * ramp, run0, 0.6)
    scrape = bp(noise(rd), 250, 1200) * (1 + 0.5 * lp(noise(rd), 4) * 6) * ramp
    place(out, scrape, run0, 0.35)
    place(out, brown(rd) * ramp, run0, 1.2)
    place(out, thud(60, 30, 1.0, 0.25, 1.2) * 1.5, run1)                          # open: it stops
    place(out, metal(1.5, 110, PLATE, decay=0.6), run1, 0.5)
    place(out, hiss(1.0, 1500, 8000, 0.01, 0.3), run1 + 0.1, 0.3)
    return fade(reverb(out, 2.2, wet=0.25)[:n_of(d)], 0, 0.4)


@asset("Steam_Vent_Burst", "World", "Steam vent bursts (C2 plaza, C5 vents open, pipes in L2/L3)", target=-14,
       mind=200, maxd=3500)
def steam_burst(_=None):
    d = 2.4
    out = silence(d)
    place(out, thud(140, 80, 0.15, 0.03, 0.5), 0.0, 0.5)
    t = t_axis(2.3)
    e = np.minimum(1, t / 0.03) * np.exp(-np.maximum(t - 0.4, 0) / 0.6)
    x = (bp(noise(2.3), 1200, 9000) + 0.4 * bp(noise(2.3), 300, 1200)) * e
    x *= np.clip(1 + 0.35 * lp(noise(2.3), 15) * 5, 0.3, 2)
    place(out, x, 0.01)
    return fade(out, 0, 0.3)


@asset("Steam_Vent_Loop", "World", "Steady steam leak / vent hiss (emitters on pipes)", target=-22, loop=True,
       mind=150, maxd=2000, mode="Compressed")
def steam_loop(_=None):
    L, xf = 6.0, 1.0
    s = L + xf
    x = bp(noise(s), 1500, 10000) + 0.3 * bp(noise(s), 400, 1500)
    x *= np.clip(1 + 0.25 * lp(noise(s), 6) * 6, 0.4, 1.8)
    x = sweep_bp(x, lambda tt: 4000 + 1200 * np.sin(2 * np.pi * tt / 2.3), 0.6)
    return loopify(x, L, xf)


@asset("Gears_Loop", "World", "Big brass gears turning (C2 plaza, orrery, machinery)", target=-21, loop=True,
       mind=150, maxd=2500, mode="Compressed")
def gears_loop(_=None):
    L = 4.0
    events = []
    for k in range(16):                                                            # big gear: 4 Hz
        at = k * 0.25
        events.append((at, pad_to(thud(150, 90, 0.12, 0.02, 0.4), 0.15) + metal(0.15, 1500 + U(-40, 40), BAR, decay=0.03) * 0.4,
                       0.9 if k % 4 == 0 else 0.6))
    for k in range(48):                                                            # small gears: 12 Hz
        events.append((k / 12 + 0.04, metal(0.08, 3100 + U(-80, 80), BAR, decay=0.012), 0.25))
    for k in range(8):                                                             # ratchet pawl
        events.append((k * 0.5 + 0.11, click(0.03, 2200, 0.005), 0.3))
    x = scatter(L, events).mean(axis=1) * 2
    grind = loopify(bp(noise(L + 0.5), 120, 900) * 0.15, L, 0.5)
    return x + grind


@asset("Breaker_Throw", "World", "Main breaker lever thrown: creak, heavy clack, arc and relays (C3)", target=-12,
       mind=200, maxd=3500)
def breaker_throw(_=None):
    d = 1.8
    out = silence(d)
    t = t_axis(0.35)
    creak = bp(osc(glide(0.35, [(0, 220), (0.35, 330)]), 0.35, "saw") * (1 + 0.6 * np.sign(np.sin(2 * np.pi * 31 * t))),
               300, 2500) * np.sin(np.pi * t / 0.35) * 0.35
    place(out, creak, 0.0)
    at = 0.36
    place(out, thud(120, 60, 0.4, 0.06, 0.8) * 1.4, at)
    place(out, metal(0.9, 380, PLATE, decay=0.3, bright=0.6), at, 0.6)
    place(out, burst(0.03, 800, 9000, decay=0.006), at, 1.0)
    t = t_axis(0.25)
    arc = (crackle(0.25, 600, 1500, 10000) * 0.7 + buzz(0.25, 100, 5) * 0.25) * np.exp(-t / 0.08)
    place(out, arc, at + 0.01)
    for k in range(5):
        place(out, click(0.03, U(2000, 3000), 0.005), at + 0.25 + k * U(0.08, 0.14), 0.25)
    return fade(room(out, 1.0, 0.2)[:n_of(d)], 0, 0.2)


@asset("Power_Up_Sweep", "World", "Lab powers up after the breaker: hum and flywheel whine rising (C3)",
       target=-14, spatial=False, mode="Compressed")
def power_up(_=None):
    d = 5.5
    t = t_axis(d)
    rise = np.clip(t / 4.0, 0, 1) ** 1.5
    f_hum = 50 * (0.4 + 0.6 * np.clip(t / 3.5, 0, 1))
    hum = (osc(f_hum, d) + 0.6 * osc(f_hum * 2, d) + 0.4 * osc(f_hum * 3, d) + 0.2 * osc(f_hum * 4, d, "saw")) * rise
    whine = osc(glide(d, [(0, 150), (3.8, 2600), (d, 2500)]), d) * np.clip(t / 3.0, 0, 1) * 0.12
    whine *= np.clip((d - t) / 1.5 + 0.3, 0, 1)
    air = lp(pink(d), 800) * rise * 0.8
    x = hum * 0.5 + whine + air
    events = [(0.5 + k * 0.42 + U(-0.05, 0.05), click(0.04, U(1800, 2800), 0.006), 0.3, U(-0.8, 0.8))
              for k in range(9)]
    out = to_stereo(x, 0.4) + scatter(d, events)
    out = reverb(out, 1.5, wet=0.2, stereo=True)[:n_of(d)]
    return fade(out, 0.05, 0.6)


@asset("Power_Hum_Loop", "World", "Steady powered-machinery hum after power-up (breaker panel, junction boxes)",
       target=-24, loop=True, mind=100, maxd=1500, mode="Compressed")
def power_hum(_=None):
    L = 4.0
    t = t_axis(L)
    hum = osc(50, L) + 0.6 * osc(100, L) + 0.35 * osc(150, L) + 0.15 * osc(200, L, "saw")
    hum *= 1 + 0.1 * np.sin(2 * np.pi * 0.5 * t)
    return hum * 0.5 + loopify(bp(noise(L + 0.5), 2000, 6000) * 0.02, L, 0.5)


@asset("Valve_Squeak", "World", "Valve wheel turned: metal squeak and ratchet (L2 turbine hall valves)",
       target=-17, mind=150, maxd=2000, variants=2)
def valve_squeak(i):
    d = [1.3, 1.0][i]
    out = silence(d + 0.2)
    t = t_axis(d)
    f = glide(d, [(0, U(700, 900)), (d * 0.5, U(950, 1200)), (d, U(800, 1000))])
    f *= 1 + 0.02 * lp(noise(d), 40) * 8
    stick = np.clip(np.sin(2 * np.pi * np.cumsum(np.full(len(t), U(18, 28))) / dsp.RATE), 0, 1) ** 0.5
    tone = osc(f, d, "saw") * (0.5 + 0.5 * stick)
    sq = reso(tone, 1400, 2.0) + 0.4 * reso(tone, 2800, 3.0)
    env = np.clip(np.sin(np.pi * t / d), 0, 1) ** 0.6 * (0.6 + 0.4 * np.abs(np.sin(2 * np.pi * t / (d / 2))))
    place(out, sq * env * 0.5, 0.0)
    for k in range(int(d / 0.18)):
        place(out, click(0.03, 1900, 0.006), 0.05 + k * 0.18, 0.15)
    return room(out, 0.9, 0.18)[:len(out)]


def turbine_state(L, level):
    """The steady turbine: blade pass, shaft whine and air rush."""
    t = t_axis(L)
    bpf = 180.0
    x = sum(a * osc(bpf * k, L) for k, a in [(1, 1.0), (2, 0.5), (3, 0.3), (5, 0.15)])
    x *= 1 + 0.2 * np.sin(2 * np.pi * 15 * t)
    whine = osc(3000, L) * (1 + 0.3 * np.sin(2 * np.pi * 0.25 * t)) * 0.08
    rush = bp(noise(L), 300, 3000) * (1 + 0.3 * np.sin(2 * np.pi * 15 * t)) * 0.5
    return (x * 0.35 + whine + rush) * level


@asset("Turbine_Loop", "World", "Steam turbine running (L2 turbine hall, both states)", target=-20, loop=True,
       mind=300, maxd=4000, mode="Compressed")
def turbine_loop(_=None):
    L, xf = 8.0, 1.0
    return loopify(turbine_state(L + xf, 1.0), L, xf)


@asset("Turbine_SpinUp", "World", "Steam turbine spins up from rest to running; crossfade into Turbine_Loop",
       target=-17, mind=300, maxd=4000, mode="Compressed")
def turbine_spinup(_=None):
    d = 6.0
    t = t_axis(d)
    rate = np.clip(t / 5.0, 0, 1) ** 1.6
    bpf = 180 * np.maximum(rate, 0.03)
    x = sum(a * osc(bpf * k, d) for k, a in [(1, 1.0), (2, 0.5), (3, 0.3), (5, 0.15)])
    whine = osc(3000 * np.maximum(rate, 0.05), d) * 0.08
    rush = bp(noise(d), 300, 3000) * (0.2 + 0.8 * rate) * 0.5
    out = (x * 0.35 + whine + rush) * (0.25 + 0.75 * rate)
    place(out, hiss(1.5, 900, 7000, 0.02, 0.6), 0.0, 0.5)                         # steam admitted
    place(out, thud(90, 50, 0.3, 0.06), 0.0, 0.5)
    return out


def lift_running(L):
    t = t_axis(L)
    motor = osc(60, L, "saw") * 0.25 + osc(120, L) * 0.3 + osc(240, L) * 0.1
    motor = lp(motor, 900)
    cable = osc(1100, L) * (1 + 0.5 * np.sin(2 * np.pi * 0.5 * t)) * 0.03
    rumble = brown(L) * 0.8
    return motor + cable + rumble


@asset("Lift_Run_Loop", "World", "Elevator / freight lift running: motor, cable, rail joints, cage rattle (C4, L3 exit)",
       target=-19, loop=True, mind=300, maxd=4000, mode="Compressed")
def lift_run(_=None):
    L, xf = 6.0, 0.8
    base = loopify(lift_running(L + xf), L, xf)
    events = []
    for k in range(6):                                                             # rail joints, 1 per second
        events.append((k + 0.3, pad_to(thud(130, 70, 0.2, 0.04, 0.6), 0.3) + metal(0.3, 260, PLATE, decay=0.08) * 0.3, 0.6))
    for k in range(30):                                                            # cage rattle
        events.append((U(0, L), metal(0.12, U(600, 1800), BAR, decay=0.02), U(0.05, 0.15)))
    return base + scatter(L, events).mean(axis=1) * 2


@asset("Lift_Start", "World", "Elevator / freight lift starts: brake release, motor winds up", target=-16,
       mind=300, maxd=4000)
def lift_start(_=None):
    d = 2.2
    out = silence(d)
    place(out, click(0.05, 1500, 0.01), 0.0, 0.6)
    place(out, thud(100, 60, 0.4, 0.08), 0.02, 0.8)
    place(out, hiss(0.4, 1500, 8000, 0.005, 0.12), 0.05, 0.3)
    run = lift_running(2.0) * np.clip(t_axis(2.0) / 1.5, 0, 1)
    place(out, run, 0.2)
    return out


@asset("Lift_Stop", "World", "Elevator / freight lift stops: wind-down, brake squeal, clunk, chain settle",
       target=-14, mind=300, maxd=4000)
def lift_stop(_=None):
    d = 2.6
    out = silence(d)
    run = lift_running(1.6)
    down = warp(run, np.linspace(1.0, 0.4, len(run)))      # the motor winds down
    place(out, down * np.linspace(1, 0, len(down)), 0.0)
    t = t_axis(0.5)
    squeal = reso(osc(glide(0.5, [(0, 2400), (0.5, 2000)]), 0.5, "saw"), 2300, 4) * np.sin(np.pi * t / 0.5) * 0.25
    place(out, squeal, 0.4)
    place(out, thud(80, 40, 0.6, 0.14, 1.0) * 1.4, 0.95)
    place(out, metal(1.2, 150, PLATE, decay=0.45), 0.95, 0.5)
    for k in range(10):                                                            # chains settle
        place(out, metal(0.1, U(1500, 3000), BAR, decay=0.02), 1.05 + k * U(0.05, 0.11), 0.2 * (1 - k / 10))
    place(out, hiss(0.8, 1500, 8000, 0.01, 0.3), 1.1, 0.25)
    return fade(room(out, 1.0, 0.2)[:n_of(d)], 0, 0.2)


@asset("Keycard_Accept", "World", "Keycard reader accepts: two-tone beep and lock relay (L2 archive)", target=-18,
       mind=100, maxd=1500)
def keycard_ok(_=None):
    out = silence(0.6)
    for k, f in enumerate([1250, 1870]):
        t = t_axis(0.09)
        place(out, lp(osc(f, 0.09, "square"), 5000) * np.minimum(1, t / 0.003) * np.clip((0.09 - t) / 0.01, 0, 1) * 0.5,
              k * 0.11)
    place(out, click(0.05, 1800, 0.01), 0.26, 0.8)
    place(out, thud(180, 110, 0.15, 0.03, 0.4), 0.26, 0.4)
    return out


@asset("Keycard_Deny", "World", "Keycard reader refuses: low double buzz", target=-18, mind=100, maxd=1500)
def keycard_deny(_=None):
    out = silence(0.55)
    for k in range(2):
        t = t_axis(0.16)
        place(out, lp(osc(310, 0.16, "square"), 3000) * np.minimum(1, t / 0.004) * np.clip((0.16 - t) / 0.01, 0, 1) * 0.5,
              k * 0.21)
    return out


def klaxon_cycle(L):
    """One or more wails: a nasal driven horn rising and falling."""
    t = t_axis(L)
    period = 1.5
    ph = (t % period) / period
    f = 380 + 160 * np.sin(np.pi * ph) ** 0.8
    x = osc(f, L, "saw") + 0.6 * osc(f * 1.5, L, "square")
    x = drive(x, 3)
    x = reso(x, 900, 2.0) + 0.6 * reso(x, 2100, 3.0) + 0.1 * x
    e = np.clip(np.sin(np.pi * ph) * 3, 0, 1)
    return x * e


@asset("Alarm_Klaxon_Loop", "World", "Lab alarm klaxon (L3 after C5, L2 alarm state)", target=-14, loop=True,
       mind=500, maxd=6000, mode="Compressed")
def klaxon(_=None):
    return lp(klaxon_cycle(3.0), 4500) * 0.5


@asset("Beacon_Charge", "World", "Extraction beacon charging (W2 pump hall, ~8 s); Beacon_Fire follows",
       target=-14, mind=400, maxd=6000, mode="Compressed")
def beacon_charge(_=None):
    d = 8.0
    t = t_axis(d)
    p = t / d
    hum = osc(glide(d, [(0, 55), (d, 220)]), d, "saw") * (0.2 + 0.8 * p) * 0.3
    whine = osc(glide(d, [(0, 400), (d, 4200)]), d) * p ** 1.2 * 0.15
    spark = crackle(d, 400) * p ** 2 * 0.5
    tick_rate = 2 + 14 * p ** 1.5
    ticks = (np.diff(np.floor(np.cumsum(tick_rate) / dsp.RATE), prepend=0) > 0).astype(float)
    tk = np.convolve(ticks, click(0.03, 2600, 0.004), "full")[:len(t)] * 0.6
    x = lp(hum, 3000) + whine + spark + tk
    return fade(reverb(x, 1.5, wet=0.2)[:len(t)], 0.2, 0.02)


@asset("Beacon_Fire", "World", "Extraction beacon fires: crack, boom and rising flare (end of W2)", target=-9,
       mind=600, maxd=12000, mode="Compressed")
def beacon_fire(_=None):
    d = 4.5
    out = silence(d)
    place(out, drive(hp(noise(0.02), 1500) * env_ad(n_of(0.02), 0.0001, 0.004), 3) * 1.4, 0.0)
    place(out, thud(70, 28, 2.0, 0.5, 1.2) * 1.8, 0.0)
    t = t_axis(2.5)
    flare = sweep_bp(noise(2.5), lambda tt: 300 + 3500 * (tt / 2.5) ** 0.7, 2.0) * np.exp(-t / 0.9) * 1.5
    place(out, flare, 0.02)
    place(out, osc(glide(2.0, [(0, 300), (2.0, 2400)]), 2.0) * np.exp(-t_axis(2.0) / 0.6) * 0.15, 0.02)
    place(out, crackle(2.5, 300) * np.exp(-t / 0.7) * 0.5, 0.05)
    return fade(drive(reverb(out, 3.0, wet=0.35)[:n_of(d)], 1.4), 0, 0.5)


@asset("Glass_Shatter", "World", "Glass breaks (benches, display cases, conservatory panes in the alarm state)",
       target=-12, mind=200, maxd=3500, variants=2, cc0="KenneyImpact:impactGlass_heavy_000..001")
def glass(i):
    d = 1.8
    out = silence(d)
    place(out, kenney("Impact", f"impactGlass_heavy_{i:03d}.ogg"), 0.0, 1.0)
    place(out, burst(0.15, 2000, 12000, decay=0.04), 0.0, 0.8)
    for k in range(40):                                                            # shards falling
        at = 0.02 + (dsp.R.random() ** 1.6) * 1.3
        place(out, modal(0.15, [(U(2500, 9000), U(0.01, 0.04), 1.0), (U(5000, 11000), 0.01, 0.5)]),
              at, 0.25 * (1 - at / 1.5))
    return fade(room(out, 0.8, 0.15)[:n_of(d)], 0, 0.2)


@asset("Sprinklers_Loop", "World", "Fire sprinklers spraying, water on the floor (L2 alarm: archive, workshop)",
       target=-22, loop=True, bus="Ambient", spatial=False, mode="Compressed")
def sprinklers(_=None):
    L, xf = 8.0, 1.0
    s = L + xf
    spray = np.stack([hp(noise(s), 2500), hp(noise(s), 2500)], axis=1) * 0.25
    spray *= np.clip(1 + 0.3 * lp(noise(s), 30)[:, None] * 6, 0.3, 2)
    pat = np.stack([crackle(s, 900, 500, 5000), crackle(s, 900, 500, 5000)], axis=1) * 0.25
    gurgle = to_stereo(bp(noise(s), 200, 700) * np.clip(1 + lp(noise(s), 3) * 8, 0, 2) * 0.15, 7)
    return loopify(spray + pat + gurgle, L, xf)


@asset("Airship_Engine_Loop", "World", "Airship engines: propeller thrum and steam-engine chuff (C1, C6)",
       target=-16, loop=True, mind=1000, maxd=20000, mode="Compressed")
def airship(_=None):
    L, xf = 8.0, 1.0
    s = L + xf
    t = t_axis(s)
    prop = 14.0
    thrum = lp(osc(prop, s, "saw") + 0.5 * osc(prop * 2, s, "saw"), 300) * 0.8
    drone = osc(45, s) * 0.4 + osc(90, s) * 0.2
    chuff_rate = 4.0
    ph = (t * chuff_rate) % 1.0
    chuff = bp(noise(s), 400, 2200) * np.exp(-ph / 0.12) * 0.3
    wind = lp(pink(s), 600) * 0.6
    am = 1 + 0.25 * np.sin(2 * np.pi * prop * t)
    return hp(loopify((thrum + drone) * am + chuff + wind, L, xf), 20)


# =============================================================================
# UI (Kenney Interface Sounds)
# =============================================================================

def kenney_ui(file):
    x = kenney("Interface", file)
    return fade(pad_to(x, len(x) / dsp.RATE + 0.02), 0, 0.01)


@asset("UI_Click", "UI", "Menu button pressed", bus="UI", spatial=False, target=-20,
       cc0="KenneyInterface:click_001")
def ui_click(_=None):
    return kenney_ui("click_001.ogg")


@asset("UI_Hover", "UI", "Menu item highlighted (hover / focus move)", bus="UI", spatial=False, target=-26,
       cc0="KenneyInterface:tick_001")
def ui_hover(_=None):
    return kenney_ui("tick_001.ogg")


@asset("UI_Back", "UI", "Back / close a menu page", bus="UI", spatial=False, target=-20,
       cc0="KenneyInterface:back_001")
def ui_back(_=None):
    return kenney_ui("back_001.ogg")


@asset("UI_Confirm", "UI", "Confirm (New Game, apply settings, objective complete)", bus="UI", spatial=False,
       target=-19, cc0="KenneyInterface:confirmation_001")
def ui_confirm(_=None):
    return kenney_ui("confirmation_001.ogg")


@asset("UI_Error", "UI", "Not allowed (locked chapter, invalid setting)", bus="UI", spatial=False, target=-20,
       cc0="KenneyInterface:error_004")
def ui_error(_=None):
    return kenney_ui("error_004.ogg")


# =============================================================================
# Ambience beds (stereo, streamed, Ambient bus)
# =============================================================================

AMB = dict(loop=True, bus="Ambient", spatial=False, target=-26, mode="Stream", quality=3)


def level(x, lufs):
    """A layer brought to an integrated loudness, before mixing (no limiting)."""
    return x * 10 ** ((lufs - dsp.lufs_integrated(x)) / 20)


def mlevel(x, lufs):
    """An event brought to a momentary (loudest 400 ms) loudness."""
    return x * 10 ** ((lufs - dsp.lufs_momentary_max(x)) / 20)


def park_wind(start, seconds):
    return dsp.read_audio(cc0_file("ParkWind.wav"), start=start, seconds=seconds)


@asset("Amb_L1_Coast", "Ambience", "L1 coast: sea on the rocks, wind, distant gulls (dock, beach, lower road)",
       cc0="Wave01..04 (jasinski), ParkWind (Thimras)", **AMB)
def amb_coast(_=None):
    L, xf = 60.0, 3.0
    s = L + xf
    wind = level(hp(lp(park_wind(95.0, s), 3500), 90), -30)
    wash = np.stack([lp(pink(s), 900), lp(pink(s), 900)], axis=1)
    wash = level(wash * (0.6 + 0.4 * np.sin(2 * np.pi * t_axis(s) / 9.0))[:, None], -31)
    bed = loopify(wind + wash, L, xf)
    waves = [mlevel(lp(dsp.read_audio(cc0_file(f"Wave0{k}.flac")), 7000), -27) for k in range(1, 5)]
    events = []
    at = 0.0
    while at < L:
        events.append((at, waves[dsp.R.integers(0, 4)], U(0.5, 1.0)))
        at += U(2.5, 5.0)
    for _k in range(9):                                                            # distant gulls
        g = lp(reverb(gull_call(U(0.9, 1.1)), 1.5, wet=0.4), 6000)
        events.append((U(0, L), mlevel(g, -34), U(0.6, 1.0), U(-0.9, 0.9)))
    return bed + scatter(L, events)


@asset("Amb_L1_Forest", "Ambience", "L1 spruce forest: wind in the trees, birds, creaking trunks (forest, overlook)",
       cc0="ParkWind, ParkBirds (Thimras), TreeCreak (Department64)", **AMB)
def amb_forest(_=None):
    L, xf = 60.0, 3.0
    s = L + xf
    rustle = level(bp(park_wind(135.0, s), 250, 6000), -29)
    birds = level(hp(dsp.read_audio(cc0_file("ParkBirds.wav"), start=200.0, seconds=s), 1500), -34)
    bed = loopify(rustle + birds, L, xf)
    creak = dsp.read_audio(cc0_file("TreeCreak.flac"), mono=True)
    events = [(at, mlevel(lp(stretch(creak, U(0.85, 1.1)), 4000), -33), 1.0, U(-0.8, 0.8)) for at in (7.0, 26.0, 44.0)]
    return bed + scatter(L, events)


@asset("Amb_L2_Calm", "Ambience", "L2 laboratory, calm: electrical hum, ventilation, clock ticks, distant pipes",
       **AMB)
def amb_lab_calm(_=None):
    L, xf = 60.0, 2.0
    s = L + xf
    t = t_axis(s)
    hum = (osc(50, s) + 0.5 * osc(100, s) + 0.25 * osc(150, s)) * (1 + 0.15 * np.sin(2 * np.pi * 0.07 * t))
    vent = np.stack([lp(pink(s), 500), lp(pink(s), 500)], axis=1)
    bed = loopify(level(to_stereo(hum, 3), -36) + level(vent, -30), L, xf)
    events = []
    tick = [mlevel(reverb(click(0.04, f, 0.006), 1.2, wet=0.4)[:n_of(0.6)], -44) for f in (2600, 2200)]
    for k in range(int(L)):                                                        # a far clock: tick, tock
        events.append((k + 0.0, tick[k % 2], 1.0, 0.5))
    for _k in range(10):                                                           # pipes knock, relays
        sig = metal(0.6, U(200, 500), TUBE, decay=0.2) if dsp.R.random() < 0.6 else click(0.04, U(1500, 2500))
        events.append((U(0, L), mlevel(reverb(sig, 1.5, wet=0.5)[:n_of(1.5)], -38), U(0.6, 1.0), U(-1, 1)))
    for _k in range(4):                                                            # a far steam sigh
        events.append((U(0, L), mlevel(lp(hiss(2.0, 800, 5000, 0.5, 1.0), 4000), -40), 1.0, U(-1, 1)))
    return bed + scatter(L, events)


@asset("Amb_L2_Alarm", "Ambience", "L2 laboratory on alarm: distant klaxon, sparking wiring, stressed metal",
       **{**AMB, "target": -23})
def amb_lab_alarm(_=None):
    L, xf = 60.0, 2.0
    s = L + xf
    hum = osc(50, s) + 0.5 * osc(100, s)
    vent = np.stack([lp(pink(s), 400), lp(pink(s), 400)], axis=1)
    bed = loopify(level(to_stereo(hum, 3), -36) + level(vent, -31), L, xf)
    k = klaxon_cycle(L)                                                            # 40 wails, periodic in 60 s
    far = level(lp(reverb(k, 2.5, wet=0.8, tail=False), 1800)[:n_of(L)], -29)
    events = []
    for _k in range(14):                                                           # sparks
        d = U(0.2, 0.7)
        sp = crackle(d, U(300, 900)) * np.exp(-t_axis(d) / (d / 3)) + buzz(d, 100, 6) * 0.05 * np.exp(-t_axis(d) / 0.1)
        events.append((U(0, L), mlevel(reverb(sp, 1.0, wet=0.3)[:n_of(1.2)], -32), U(0.5, 1.0), U(-1, 1)))
    for _k in range(6):                                                            # metal groans
        d = 1.5
        g = reso(osc(glide(d, [(0, U(70, 110)), (d, U(50, 80))]), d, "saw"), 300, 3) * np.sin(np.pi * t_axis(d) / d)
        events.append((U(0, L), mlevel(reverb(g, 2.0, wet=0.5)[:n_of(3)], -34), 1.0, U(-1, 1)))
    return bed + to_stereo(far, 11) + scatter(L, events)


@asset("Amb_L3_Reactor", "Ambience", "L3 core: reactor drone, rotating rings, lightning arcs in a huge shaft",
       **{**AMB, "target": -22})
def amb_reactor(_=None):
    L, xf = 60.0, 3.0
    s = L + xf
    t = t_axis(s)
    drone = lp((osc(41, s) + 0.7 * osc(41.4, s) + 0.5 * osc(82, s) + 0.25 * osc(123.5, s, "saw")), 500)
    rings = bp(noise(s), 200, 1500) * (0.5 + 0.5 * np.sin(2 * np.pi * t / 6.0) ** 2)
    air = np.stack([lp(pink(s), 300), lp(pink(s), 300)], axis=1)
    bed = loopify(level(to_stereo(drone, 5), -28) + level(to_stereo(rings, 13), -33) + level(air, -32), L, xf)
    events = []
    for _k in range(12):                                                           # arcs
        d = U(0.3, 1.0)
        tt = t_axis(d)
        arc = (crackle(d, U(800, 2000), 1000, 12000) + buzz(d, U(90, 130), 8) * 0.2) * np.exp(-tt / (d / 2.5))
        arc += thud(60, 35, d, 0.2, 0.5) * 0.5
        events.append((U(0, L), mlevel(reverb(arc, 3.5, wet=0.6, bright=8000)[:n_of(4)], -27), U(0.5, 1.0),
                       U(-0.9, 0.9)))
    return bed + scatter(L, events)


# =============================================================================
# Music (CC0, Streamed, Music bus)
# =============================================================================

MUS = dict(bus="Music", spatial=False, target=-18, mode="Stream", quality=4, rate=44100)


def music_from(file, loop_xfade=None):
    x = dsp.read_audio(cc0_file(file), rate=44100)
    x = x[: len(x) - (len(x) % 1)]
    if loop_xfade:
        # seam the loop: the last loop_xfade seconds crossfade over the start
        k = int(loop_xfade * 44100)
        n = len(x) - k
        a = np.linspace(0, np.pi / 2, k)[:, None]
        y = x[:n].copy()
        y[:k] = x[:k] * np.sin(a) + x[n:] * np.cos(a)
        x = y
    return x


@asset("Music_Menu", "Music", "Main menu theme (loops)", loop=True, cc0="MusicMenu: Unexplored Expansion", **MUS)
def music_menu(_=None):
    return music_from("MusicMenu.mp3", loop_xfade=0.05)


@asset("Music_Explore", "Music", "Exploration bed: L1 and L2 calm, under the ambience (loops; keep it low)",
       loop=True, cc0="MusicExplore: Factory ambiance", **{**MUS, "target": -21})
def music_explore(_=None):
    return music_from("MusicExplore.ogg", loop_xfade=0.05)


@asset("Music_Combat", "Music", "Combat loop: waves W1-W4 (loops seamlessly)", loop=True,
       cc0="MusicCombat: Determined Pursuit", **MUS)
def music_combat(_=None):
    return music_from("MusicCombat.wav")


@asset("Music_Extraction", "Music", "Extraction and credits cue: C6 and the credits roll (plays once)",
       cc0="MusicExtraction: Epic Endgame Cinematic", **MUS)
def music_extraction(_=None):
    return fade(music_from("MusicExtraction.wav"), 0, 0.5)


# =============================================================================
# Build
# =============================================================================

def build_one(a, preview):
    dsp.seed(a["name"])
    x = np.asarray(a["fn"](), dtype=np.float64)
    x = x - x.mean(axis=0)   # no DC offset
    if a["mode"] is None:
        a["mode"] = "Compressed" if a["loop"] or len(x) / a["rate"] > 2.0 else "Decompressed"
    mode = a["mode"]
    if mode in ("Decompressed", "Compressed") and x.ndim == 2 and a["spatial"]:
        x = x.mean(axis=1)
    if mode == "Decompressed" and x.ndim == 2:
        x = x.mean(axis=1)
    kind = "integrated" if (a["loop"] or a["bus"] == "Music") else "momentary"
    rate_save = dsp.RATE
    dsp.RATE = a["rate"]
    try:
        x = dsp.normalize(x, a["target"], kind)
        loud = dsp.lufs_integrated(x) if kind == "integrated" else dsp.lufs_momentary_max(x)
    finally:
        dsp.RATE = rate_save
    settings = dict(LoadMode=mode, Loop=a["loop"], Bus=a["bus"], Spatial=a["spatial"], MinDistance=a["mind"],
                    MaxDistance=a["maxd"], ForceMono=True)
    if mode == "Decompressed":
        pcm = dsp.to_int16(x)
        src = dsp.encode(x, "flac", rate=a["rate"])
        data, secs = nmsound.build(src, settings, pcm)
    else:
        if mode == "Stream" or not a["spatial"]:
            settings["ForceMono"] = True   # irrelevant outside Decompressed; kept at its default
        q = a["quality"] if a["quality"] is not None else 4
        src = dsp.encode(x, "ogg", rate=a["rate"], quality=q)
        data, secs = nmsound.build(src, settings)
    root = MUSIC if a["folder"] == "Music" else SOUNDS / a["folder"]
    root.mkdir(parents=True, exist_ok=True)
    out = root / f"{a['name']}.nmsound"
    out.write_bytes(data)
    if preview:
        Path(preview).mkdir(parents=True, exist_ok=True)
        dsp.write_wav(Path(preview) / f"{a['name']}.wav", x, a["rate"])
    peak = 20 * np.log10(max(np.abs(x).max(), 1e-9))
    print(f"  {a['name']:26s} {mode:12s} {secs:6.2f} s  {loud:6.1f} LUFS  peak {peak:5.1f} dB  {len(data) / 1024:8.0f} KB")
    return dict(path=str(out.relative_to(GAME)), seconds=round(secs, 2), lufs=round(loud, 1), kb=len(data) // 1024)


def write_readme(results):
    groups = {}
    for a in ASSETS:
        groups.setdefault(a["folder"], []).append(a)
    lines = [
        "# Demo Lab sounds",
        "",
        "Built by `Tools/Audio/build_audio.py` (do not edit the `.nmsound` files by hand; change the script and",
        "rebuild). Sources and licences: `Docs/Credits.md`. Paths below are relative to the project root, as",
        "`Audio.PlaySoundAt(\"Content/Sounds/...\")` and an Audio Source's `Sound` take them.",
        "",
        "- **3D**: heard from where it plays (Audio Source or `PlaySoundAt`/`PlaySoundAttached`); distances are",
        "  world units (cm): full volume within Min, inverse rolloff out to Max. **2D**: the same in both ears",
        "  (`PlaySound2D`, or an Audio Source on any entity).",
        "- **Loop** sounds are seamless: set them on an Audio Source with AutoPlay, or `Play`/`FadeTo` from script.",
        "  One-shots: `PlaySoundAt` / `PlaySound2D`. Variants (`_01`..`_04`): pick one at random each time.",
        "- **Load**: Decompressed = PCM in the file (short, frequent); Compressed = Ogg decoded once; Stream = Ogg",
        "  decoded as it plays (beds, music).",
        "- Loudness is normalised (BS.1770): one-shots by their loudest 400 ms, loops/beds/music integrated;",
        "  the LUFS column is the result, so relative levels are already balanced. Bus volumes do the rest.",
        "- **Footsteps** are wired by surface: `Source/Core/LabFootsteps.h` plays a variant each stride by what is",
        "  under the foot (metal, tile, stone, wood, grass/dirt, water).",
        "",
    ]
    order = ["Footsteps", "Weapons", "Impacts", "Robots", "World", "UI", "Ambience", "Music"]
    for g in order:
        if g not in groups:
            continue
        lines += [f"## {g}", "", "| Asset | Use | Bus | 3D (min-max) | Loop | Load | Length | LUFS |",
                  "|---|---|---|---|---|---|---|---|"]
        seen = set()
        for a in groups[g]:
            r = results.get(a["name"])
            if not r:
                continue
            base = a["name"].rsplit("_", 1)[0] if a["name"][-3:-2] == "_" and a["name"][-2:].isdigit() else a["name"]
            path = r["path"]
            if base != a["name"]:
                if base in seen:
                    continue
                seen.add(base)
                count = sum(1 for b in groups[g] if b["name"].startswith(base + "_"))
                path = path.replace(a["name"], f"{base}_01..{count:02d}")
            d3 = f"3D ({a['mind']:.0f}-{a['maxd']:.0f})" if a["spatial"] else "2D"
            lines.append(f"| `{path}` | {a['use']} | {a['bus']} | {d3} | {'loop' if a['loop'] else 'one-shot'} | "
                         f"{a['mode']} | {r['seconds']} s | {r['lufs']} |")
        lines.append("")
    lines += [
        "## Wiring notes",
        "",
        "- **Level 1**: `Amb_L1_Coast` near the dock/beach, crossfade to `Amb_L1_Forest` up the road (two 2D",
        "  Audio Sources and `FadeTo`), `Music_Explore` low under both. C1/C6: `Airship_Engine_Loop` attached to",
        "  the airship. C2: `Steam_Vent_Burst`, `Gears_Loop`, `BlastDoor_Open` on the door.",
        "- **Level 2 calm**: `Amb_L2_Calm`; C3 is `Breaker_Throw` then `Power_Up_Sweep`, then `Power_Hum_Loop`",
        "  on panels; `Turbine_Loop` in the turbine hall (`Turbine_SpinUp` if a valve starts it), `Valve_Squeak`",
        "  per valve, `Keycard_Accept`/`Keycard_Deny` at the archive, `Lift_Start` + `Lift_Run_Loop` + `Lift_Stop`",
        "  for C4.",
        "- **Level 3**: `Amb_L3_Reactor`; C5: `Shutter_Slam`, `Steam_Vent_Burst`, then `Alarm_Klaxon_Loop` and",
        "  `Music_Combat`. W2: `Beacon_Charge` then `Beacon_Fire`. Freight lift: the Lift sounds.",
        "- **Level 2 alarm**: `Amb_L2_Alarm` (it already carries a distant klaxon; place `Alarm_Klaxon_Loop`",
        "  emitters only near the horns), `Sprinklers_Loop` in the archive/workshop, `Glass_Shatter` on benches,",
        "  `Music_Combat`; C6: `Music_Extraction` (one-shot; the credits run under it), then back to `Music_Menu`.",
        "- **Robots**: Watchman `_Idle` every 6-12 s on patrol, `_Investigate` on a noise, `_Alert` on sight,",
        "  `_Hurt` on damage, `Watchman_Servo` on turns; Brute the same with `Brute_*`, `Brute_Swing` at the",
        "  wind-up, `Brute_Hit` on contact; `Robot_Death` for both. `Impact_Robot` when a round hits either.",
        "- **Cinematics**: Sequencer Audio tracks can play any of these (Force Bus/Spatial as needed); duck",
        "  `Music` with a Bus Volume track under C3/C5 stingers.",
        "",
    ]
    (SOUNDS / "README.md").write_text("\n".join(lines))


def main():
    global CC0
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc0", help="fetch_cc0.py's directory (without it, CC0-derived assets are skipped)")
    ap.add_argument("--only", help="build only assets whose name contains this")
    ap.add_argument("--preview", help="also write each asset as a WAV here, for listening")
    ap.add_argument("--readme-only", action="store_true")
    args = ap.parse_args()
    CC0 = Path(args.cc0) if args.cc0 else None
    index_path = Path(__file__).resolve().parent / "build_index.json"
    results = json.loads(index_path.read_text()) if index_path.exists() else {}
    if not args.readme_only:
        for a in ASSETS:
            if args.only and args.only not in a["name"]:
                continue
            if a["cc0"] and CC0 is None:
                print(f"  {a['name']:26s} skipped (needs --cc0)")
                continue
            results[a["name"]] = build_one(a, args.preview)
    for a in ASSETS:   # modes for the README, for assets not rebuilt this run
        if a["mode"] is None and a["name"] in results:
            a["mode"] = "Compressed" if a["loop"] or results[a["name"]]["seconds"] > 2.0 else "Decompressed"
    SOUNDS.mkdir(parents=True, exist_ok=True)
    index_path.write_text(json.dumps(dict(sorted(results.items())), indent=1) + "\n")
    write_readme(results)
    total = sum(r["kb"] for r in results.values())
    print(f"{len(results)} assets, {total / 1024:.1f} MB")


if __name__ == "__main__":
    main()
