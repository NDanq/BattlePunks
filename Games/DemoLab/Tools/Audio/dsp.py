"""
dsp.py -- the small signal toolkit build_audio.py synthesises with: numpy only
(no scipy), 48 kHz float signals, mono as 1-D arrays and stereo as (N, 2).

Filters are applied in the frequency domain (zero phase, Butterworth-shaped
magnitudes), which is cheap and exact enough for offline sound design.
Loudness is ITU-R BS.1770 (K-weighted, gated) so every asset can be brought to
a target in LUFS.
"""

import subprocess
import zlib

import numpy as np

RATE = 48000
R = np.random.default_rng(1)


def seed(name):
    """Every asset draws from its own stream, so one changing leaves the rest alone."""
    global R
    R = np.random.default_rng(zlib.crc32(name.encode()))
    return R


def n_of(seconds):
    return int(round(seconds * RATE))


def t_axis(seconds):
    return np.arange(n_of(seconds)) / RATE


def silence(seconds, stereo=False):
    return np.zeros((n_of(seconds), 2) if stereo else n_of(seconds))


def noise(seconds):
    return R.standard_normal(n_of(seconds))


def _shape(x, gain_fn):
    """Applies |H(f)| = gain_fn(f) to each channel, zero phase."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 2:
        return np.stack([_shape(x[:, c], gain_fn) for c in range(x.shape[1])], axis=1)
    n = len(x)
    m = 1 << int(np.ceil(np.log2(max(n, 2) * 1.25)))
    f = np.fft.rfftfreq(m, 1 / RATE)
    f[0] = 1e-3
    return np.fft.irfft(np.fft.rfft(x, m) * gain_fn(f), m)[:n]


def lp(x, fc, order=2):
    return _shape(x, lambda f: 1 / np.sqrt(1 + (f / fc) ** (2 * order)))


def hp(x, fc, order=2):
    return _shape(x, lambda f: 1 / np.sqrt(1 + (fc / f) ** (2 * order)))


def bp(x, lo, hi, order=2):
    return _shape(x, lambda f: 1 / np.sqrt(1 + (f / hi) ** (2 * order)) / np.sqrt(1 + (lo / f) ** (2 * order)))


def reso(x, f0, q):
    """A resonant band-pass (one peak, unity at f0)."""
    return _shape(x, lambda f: 1 / np.sqrt(1 + q * q * (f / f0 - f0 / f) ** 2))


def pink(seconds):
    return _shape(noise(seconds), lambda f: 1 / np.sqrt(np.maximum(f, 20) / 20)) * 0.3


def brown(seconds):
    return _shape(noise(seconds), lambda f: 20 / np.maximum(f, 10)) * 0.2


def tv_filter(x, gain_at, frame=2048, hop=512):
    """A time-varying filter: gain_at(t_seconds, freqs) -> gains, by STFT overlap-add."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 2:
        return np.stack([tv_filter(x[:, c], gain_at, frame, hop) for c in range(x.shape[1])], axis=1)
    n = len(x)
    pad = np.concatenate([np.zeros(frame), x, np.zeros(frame)])
    win = np.hanning(frame + 1)[:-1]
    out = np.zeros_like(pad)
    norm = np.zeros_like(pad)
    f = np.fft.rfftfreq(frame, 1 / RATE)
    f[0] = 1e-3
    for start in range(0, len(pad) - frame, hop):
        seg = pad[start:start + frame] * win
        t = (start + frame / 2 - frame) / RATE
        y = np.fft.irfft(np.fft.rfft(seg) * gain_at(t, f), frame) * win
        out[start:start + frame] += y
        norm[start:start + frame] += win * win
    return (out / np.maximum(norm, 1e-6))[frame:frame + n]


def sweep_lp(x, fc_of_t, order=2):
    return tv_filter(x, lambda t, f: 1 / np.sqrt(1 + (f / max(fc_of_t(t), 20)) ** (2 * order)))


def sweep_bp(x, f0_of_t, q):
    return tv_filter(x, lambda t, f: 1 / np.sqrt(1 + q * q * (f / f0_of_t(t) - f0_of_t(t) / f) ** 2))


def env_ad(n_or_t, attack, decay):
    t = n_or_t if isinstance(n_or_t, np.ndarray) else np.arange(n_or_t) / RATE
    return np.minimum(1.0, t / max(attack, 1e-5)) * np.exp(-t / decay)


def env_lin(seconds, points):
    """Piecewise-linear envelope from (time, value) points."""
    t = t_axis(seconds)
    ts, vs = zip(*points)
    return np.interp(t, ts, vs)


def osc(freq, seconds, shape="sine", phase=0.0):
    """freq: a number or an array (Hz per sample)."""
    n = n_of(seconds)
    f = np.full(n, float(freq)) if np.isscalar(freq) else np.asarray(freq)[:n]
    ph = 2 * np.pi * np.cumsum(f) / RATE + phase
    if shape == "sine":
        return np.sin(ph)
    if shape == "saw":
        # a band-limited-ish saw from the first harmonics
        out = np.zeros(n)
        for k in range(1, 16):
            out += np.sin(k * ph) / k * (k * f < RATE / 2.2)
        return out * 0.6
    if shape == "square":
        out = np.zeros(n)
        for k in range(1, 16, 2):
            out += np.sin(k * ph) / k * (k * f < RATE / 2.2)
        return out * 0.9
    if shape == "tri":
        out = np.zeros(n)
        for i, k in enumerate(range(1, 12, 2)):
            out += ((-1) ** i) * np.sin(k * ph) / (k * k) * (k * f < RATE / 2.2)
        return out * 0.8
    raise ValueError(shape)


def glide(seconds, points):
    """A frequency contour (Hz per sample) from (time, hz) points, interpolated in log."""
    t = t_axis(seconds)
    ts, hz = zip(*points)
    return np.exp(np.interp(t, ts, np.log(hz)))


def modal(seconds, modes, strike=0.0005):
    """Struck object: modes = [(hz, decay_s, amp)]."""
    t = t_axis(seconds)
    out = np.zeros_like(t)
    for hz, decay, amp in modes:
        if hz < RATE / 2.1:
            out += amp * np.sin(2 * np.pi * hz * t + R.uniform(0, 2 * np.pi)) * np.exp(-t / decay)
    return out * np.minimum(1, t / strike)


PLATE = [1.0, 1.59, 2.14, 2.30, 2.65, 2.92, 3.16, 3.50, 4.15, 4.62, 5.40, 6.20]
BAR = [1.0, 2.76, 5.40, 8.93, 13.34]
TUBE = [1.0, 2.32, 4.25, 6.63, 9.38]


def metal(seconds, f0, ratios=PLATE, decay=0.4, bright=0.6, spread=0.03):
    modes = []
    for i, r in enumerate(ratios):
        hz = f0 * r * (1 + R.uniform(-spread, spread))
        modes.append((hz, decay * (0.4 + 0.6 * R.random()) / (1 + 0.25 * i), bright ** i * R.uniform(0.5, 1)))
    return modal(seconds, modes)


def place(dst, src, at, gain=1.0):
    i = int(round(at * RATE))
    if i < 0:
        src = src[-i:]
        i = 0
    n = min(len(src), len(dst) - i)
    if n > 0:
        dst[i:i + n] += src[:n] * gain
    return dst


def pad_to(x, seconds):
    n = n_of(seconds)
    if len(x) >= n:
        return x[:n]
    return np.concatenate([x, np.zeros((n - len(x),) + x.shape[1:])])


def stretch(x, rate):
    """Resample by `rate` (2 = an octave up, half as long)."""
    n = int(len(x) / rate)
    src = np.arange(n) * rate
    if x.ndim == 2:
        return np.stack([np.interp(src, np.arange(len(x)), x[:, c]) for c in range(x.shape[1])], axis=1)
    return np.interp(src, np.arange(len(x)), x)


def warp(x, rate_curve):
    """Variable-rate playback: rate_curve per output sample (pitch glides, spin-ups)."""
    pos = np.cumsum(rate_curve)
    pos = pos[pos < len(x) - 1]
    return np.interp(pos, np.arange(len(x)), x)


def drive(x, amount):
    return np.tanh(x * amount) / np.tanh(amount)


def fade(x, fin=0.0, fout=0.0):
    x = x.copy()
    a, b = n_of(fin), n_of(fout)
    sh = (slice(None),) + ((None,) if x.ndim == 2 else ())
    if a:
        x[:a] *= np.linspace(0, 1, a)[sh]
    if b:
        x[-b:] *= np.linspace(1, 0, b)[sh]
    return x


def to_stereo(x, delay_ms=0.0):
    """Mono to two channels; delay_ms offsets the right one (a little width)."""
    if x.ndim == 2:
        return x
    d = n_of(delay_ms / 1000)
    r = np.concatenate([np.zeros(d), x[:len(x) - d]]) if d else x
    return np.stack([x, r], axis=1)


def pan(x, p):
    """p in -1..1, equal power; x mono."""
    a = (p + 1) * np.pi / 4
    return np.stack([x * np.cos(a), x * np.sin(a)], axis=1)


def conv(x, ir):
    n = len(x) + len(ir) - 1
    m = 1 << int(np.ceil(np.log2(n)))
    return np.fft.irfft(np.fft.rfft(x, m) * np.fft.rfft(ir, m), m)[:n]


def make_ir(rt60, seconds=None, bright=6000.0, predelay=0.01, density_hz=None):
    """A diffuse reverb tail: noise, decaying faster up high than down low."""
    seconds = seconds or min(rt60 * 1.1, 6.0)
    t = t_axis(seconds)
    lo = lp(noise(seconds), 800) * np.exp(-6.9 * t / rt60)
    mid = bp(noise(seconds), 800, 4000) * np.exp(-6.9 * t / (rt60 * 0.7))
    hi = hp(noise(seconds), 4000) * np.exp(-6.9 * t / (rt60 * 0.35))
    ir = lo + mid + 0.6 * hi
    ir = lp(ir, bright)
    ir *= np.minimum(1, t / 0.004)
    ir = np.concatenate([np.zeros(n_of(predelay)), ir])
    return ir / np.sqrt(np.sum(ir ** 2))


def reverb(x, rt60, wet=0.3, bright=6000.0, predelay=0.01, stereo=False, tail=True):
    """Dry plus a convolved tail; stereo output uses two decorrelated IRs."""
    irs = [make_ir(rt60, bright=bright, predelay=predelay) for _ in range(2 if stereo else 1)]
    mono = x if x.ndim == 1 else x.mean(axis=1)
    extra = len(irs[0]) - 1 if tail else 0
    if stereo:
        dry = to_stereo(x) if x.ndim == 1 else x
        dry = np.concatenate([dry, np.zeros((extra, 2))])
        w = np.stack([conv(mono, ir)[:len(dry)] for ir in irs], axis=1)
        return dry * (1 - wet * 0.5) + w * wet
    dry = np.concatenate([x, np.zeros(extra)])
    return dry * (1 - wet * 0.5) + conv(x, irs[0])[:len(dry)] * wet


def loopify(x, seconds, xfade):
    """Exactly `seconds` long and seamless: the material past the end is
    crossfaded (equal power) over the start. x must be seconds + xfade long."""
    n, k = n_of(seconds), n_of(xfade)
    assert len(x) >= n + k, (len(x), n + k)
    out = x[:n].copy()
    a = np.linspace(0, np.pi / 2, k)
    sh = (slice(None),) + ((None,) if x.ndim == 2 else ())
    out[:k] = x[:k] * np.sin(a)[sh] + x[n:n + k] * np.cos(a)[sh]
    return out


def scatter(total, events, rng=None):
    """Places (time, signal, gain[, pan]) events into a buffer, mono or stereo by pan."""
    out = silence(total, stereo=True)
    for ev in events:
        at, sig, g = ev[:3]
        p = ev[3] if len(ev) > 3 else 0.0
        s = pan(sig, p) if sig.ndim == 1 else sig
        # wrap past the end, so a loop stays continuous
        i = n_of(at) % len(out)
        n1 = min(len(s), len(out) - i)
        out[i:i + n1] += s[:n1] * g
        if n1 < len(s):
            rest = s[n1:]
            out[:len(rest)] += rest[:len(out)] * g
    return out


# === Loudness (ITU-R BS.1770-4) ==============================================

def _biquad_mag(b, a, f):
    w = 2 * np.pi * f / RATE
    z = np.exp(-1j * w)
    return np.abs((b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z))


def _kweight(x):
    b1, a1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1, -1.69065929318241, 0.73248077421585]
    b2, a2 = [1.0, -2.0, 1.0], [1, -1.99004745483398, 0.99007225036621]
    return _shape(x, lambda f: _biquad_mag(b1, a1, f) * _biquad_mag(b2, a2, f))


def _blocks(x):
    """Mean-square power of 400 ms blocks at 75 % overlap, summed over channels."""
    y = _kweight(x)
    y = y[:, None] if y.ndim == 1 else y
    blk, hop = n_of(0.4), n_of(0.1)
    if len(y) < blk:
        y = np.concatenate([y, np.zeros((blk - len(y), y.shape[1]))])
    c = np.concatenate([np.zeros((1, y.shape[1])), np.cumsum(y ** 2, axis=0)])
    starts = np.arange(0, len(y) - blk + 1, hop)
    return ((c[starts + blk] - c[starts]) / blk).sum(axis=1)


def lufs_integrated(x):
    z = _blocks(x)
    l = -0.691 + 10 * np.log10(np.maximum(z, 1e-12))
    z = z[l > -70]
    if not len(z):
        return -70.0
    rel = -0.691 + 10 * np.log10(z.mean()) - 10
    l = -0.691 + 10 * np.log10(z)
    z = z[l > rel]
    return float(-0.691 + 10 * np.log10(z.mean()))


def lufs_momentary_max(x):
    return float(-0.691 + 10 * np.log10(max(_blocks(x).max(), 1e-12)))


def limit(x, ceiling_db=-1.0, window=0.003):
    """Brick-wall-ish limiter: a min-filtered, smoothed gain never above what's needed."""
    ceil = 10 ** (ceiling_db / 20)
    a = np.abs(x) if x.ndim == 1 else np.abs(x).max(axis=1)
    if a.max() <= ceil:
        return x
    g = np.minimum(1.0, ceil / np.maximum(a, 1e-9))
    w = max(3, n_of(window))
    padded = np.concatenate([np.ones(w), g, np.ones(w)])
    gm = np.lib.stride_tricks.sliding_window_view(padded, 2 * w + 1).min(axis=1)
    k = np.ones(w) / w
    gs = np.convolve(gm, k, mode="same")[: len(g)]
    gs = np.minimum(gs, gm[: len(g)])
    y = x * (gs if x.ndim == 1 else gs[:, None])
    return np.clip(y, -ceil, ceil)


def normalize(x, target, kind="integrated", ceiling_db=-1.0, max_squash_db=4.0):
    """To `target` LUFS, then limited to the ceiling -- but never limited by
    more than max_squash_db: a very short, spiky sound (a footstep) stays
    clean and ends up a little under its target instead."""
    meas = lufs_integrated(x) if kind == "integrated" else lufs_momentary_max(x)
    g = target - meas
    peak = 20 * np.log10(max(np.abs(x).max(), 1e-12))
    g = min(g, ceiling_db - peak + max_squash_db)
    return limit(x * 10 ** (g / 20), ceiling_db)


# === Files ===================================================================

def read_audio(path, rate=RATE, mono=False, start=None, seconds=None):
    """Any file ffmpeg reads, as float at `rate` (rate=None keeps the file's)."""
    cmd = ["ffmpeg", "-v", "error"]
    if start is not None:
        cmd += ["-ss", str(start)]
    if seconds is not None:
        cmd += ["-t", str(seconds)]
    cmd += ["-i", str(path)]
    if rate:
        cmd += ["-ar", str(rate)]
    cmd += ["-ac", "1" if mono else "2", "-f", "f32le", "-acodec", "pcm_f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    x = np.frombuffer(raw, dtype="<f4").astype(np.float64)
    return x if mono else x.reshape(-1, 2)


def to_int16(x):
    return np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")


def encode(x, codec, rate=RATE, quality=4):
    """Bytes of a FLAC (16-bit) or Ogg Vorbis file of x."""
    ch = 1 if x.ndim == 1 else x.shape[1]
    pcm = to_int16(x).tobytes()
    cmd = ["ffmpeg", "-v", "error", "-f", "s16le", "-ar", str(rate), "-ac", str(ch), "-i", "pipe:0",
           "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact"]
    if codec == "flac":
        cmd += ["-c:a", "flac", "-compression_level", "8", "-f", "flac", "-"]
    else:
        cmd += ["-c:a", "libvorbis", "-q:a", str(quality), "-f", "ogg", "-"]
    return subprocess.run(cmd, input=pcm, capture_output=True, check=True).stdout


def write_wav(path, x, rate=RATE):
    import wave
    ch = 1 if x.ndim == 1 else x.shape[1]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(to_int16(x).tobytes())
