"""
seq.py -- writing a Sequencer .nmseq (Engine/Modules/Sequencer/README.md) from Python: bindings,
cameras aimed at targets, cuts with blends, fades, audio, bus volumes, subtitles, Event keys,
Animation sections, Float/Bool tracks, post-process settings. Frames at 30 fps.

Keys are (frame, value) or (frame, value, interp); interp defaults to the Sequence's Auto (a smooth
curve through the keys, flat at the ends). sample() evaluates the eases this file uses, so a camera
can be aimed at where a moving thing will be.
"""
import math
import os

FPS = 30
CLIP = 'Engine/Content/Characters/CrashDummy/CrashDummy_{}.nmanim'
SND = 'Content/Sounds/'
MUSIC = 'Content/Music/'


def fmt(x):
    return f'{round(x, 3):g}'


def look(frm, to):
    """(pitch down-positive, yaw) for a camera at frm looking at to; yaw = atan2(x, z)."""
    d = [to[i] - frm[i] for i in range(3)]
    length = math.sqrt(sum(x * x for x in d))
    return math.degrees(-math.asin(d[1] / length)), math.degrees(math.atan2(d[0], d[2]))


def unwrap(values):
    """Angles made continuous (no 179 -> -179 jumps)."""
    out = []
    for v in values:
        if out:
            while v - out[-1] > 180:
                v -= 360
            while v - out[-1] < -180:
                v += 360
        out.append(v)
    return out


EASES = {
    'Linear': lambda t: t,
    'Constant': lambda t: 0.0,
    'QuadIn': lambda t: t * t,
    'QuadOut': lambda t: 1 - (1 - t) * (1 - t),
    'QuadInOut': lambda t: 2 * t * t if t < 0.5 else 1 - (-2 * t + 2) ** 2 / 2,
    'CubicIn': lambda t: t ** 3,
    'CubicOut': lambda t: 1 - (1 - t) ** 3,
    'CubicInOut': lambda t: 4 * t ** 3 if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2,
    'Auto': lambda t: t * t * (3 - 2 * t),     # an estimate only (Auto is a spline)
}


def sample(keys, f):
    """A channel's value at frame f (keys as written: (frame, value[, interp]))."""
    keys = [k if len(k) == 3 else (k[0], k[1], 'Auto') for k in keys]
    if f <= keys[0][0]:
        return keys[0][1]
    for (f0, v0, i0), (f1, v1, _) in zip(keys, keys[1:]):
        if f0 <= f <= f1:
            t = (f - f0) / float(f1 - f0) if f1 > f0 else 1.0
            return v0 + (v1 - v0) * EASES[i0](t)
    return keys[-1][1]


def channel(name, keys):
    out = [f'      <Channel name="{name}">']
    for k in keys:
        interp = k[2] if len(k) > 2 else None
        out.append(f'        <Key f="{int(k[0])}" v="{fmt(k[1])}"' + (f' interp="{interp}"' if interp else '') + '/>')
    return out + ['      </Channel>']


class Seq:
    def __init__(self, name, length):
        self.name, self.length = name, int(length)
        self.bindings = {}        # id -> [header, [track lines]]
        self.order = []
        self.top = {'CameraCut': [], 'Fade': [], 'Audio': [], 'Subtitle': [], 'Event': [], 'PostProcessSetting': {},
                    'BusVolume': {}}

    # --- bindings -------------------------------------------------------------
    def bind(self, bid, guid, name):
        if bid not in self.bindings:
            self.bindings[bid] = [f'  <Binding id="{bid}" kind="possess" guid="{guid}" name="{name}">', []]
            self.order.append(bid)
        return bid

    def transform(self, bid, channels):
        """channels: {'Position.X': keys, ...}; an unkeyed channel is left to the entity."""
        lines = ['    <Track type="Transform">']
        for name in ('Position.X', 'Position.Y', 'Position.Z', 'Rotation.X', 'Rotation.Y', 'Rotation.Z',
                     'Scale.X', 'Scale.Y', 'Scale.Z'):
            if name in channels:
                lines += channel(name, channels[name])
        self.bindings[bid][1] += lines + ['    </Track>']

    def float(self, bid, component, field, keys):
        lines = [f'    <Track type="Float" component="{component}" field="{field}">']
        lines += [f'      <Key f="{int(k[0])}" v="{fmt(k[1])}"' + (f' interp="{k[2]}"' if len(k) > 2 else '') + '/>'
                  for k in keys]
        self.bindings[bid][1] += lines + ['    </Track>']

    def bool(self, bid, component, field, keys):
        lines = [f'    <Track type="Bool" component="{component}" field="{field}">']
        lines += [f'      <Key f="{int(f)}" v="{"true" if v else "false"}"/>' for f, v in keys]
        self.bindings[bid][1] += lines + ['    </Track>']

    def anim(self, bid, sections):
        """sections: (start, end, clip name, {blendIn, blendOut, loop, rate, offset})."""
        lines = ['    <Track type="Animation">']
        for start, end, clip, *opt in sections:
            extra = ''.join(f' {k}="{str(v).lower() if isinstance(v, bool) else v}"' for k, v in (opt[0] if opt else {}).items())
            lines.append(f'      <Section start="{int(start)}" end="{int(end)}" clip="{CLIP.format(clip)}"{extra}/>')
        self.bindings[bid][1] += lines + ['    </Track>']

    def sound_at(self, bid, sections):
        """Audio on a binding (heard from it): (start, end, sound, {volume, blendIn, ...})."""
        lines = ['    <Track type="Audio">']
        for start, end, snd, *opt in sections:
            lines.append(self._section(start, end, snd, opt[0] if opt else {}))
        self.bindings[bid][1] += lines + ['    </Track>']

    def camera(self, bid, guid, name, keys, fov=None):
        """keys: (frame, position, target[, interp]); fov: Float keys on the camera's FovY."""
        self.bind(bid, guid, name)
        rots = [look(k[1], k[2]) for k in keys]
        yaws = unwrap([r[1] for r in rots])
        interp = [k[3] if len(k) > 3 else None for k in keys]

        def ks(vals):
            return [(k[0], v, i) if i else (k[0], v) for k, v, i in zip(keys, vals, interp)]
        self.transform(bid, {'Position.X': ks([k[1][0] for k in keys]), 'Position.Y': ks([k[1][1] for k in keys]),
                             'Position.Z': ks([k[1][2] for k in keys]), 'Rotation.X': ks([r[0] for r in rots]),
                             'Rotation.Y': ks(yaws)})
        if fov:
            self.float(bid, 'Camera', 'FovY', fov)

    # --- top-level tracks -----------------------------------------------------
    def cut(self, start, end, cam, blend=0):
        self.top['CameraCut'].append(f'    <Section start="{int(start)}" end="{int(end)}" camera="{cam}"'
                                     + (f' blendIn="{int(blend)}"' if blend else '') + '/>')

    def fade(self, keys):
        self.top['Fade'] += [f'    <Key f="{int(f)}" v="{fmt(v)}"/>' for f, v in keys]

    @staticmethod
    def _section(start, end, snd, opt):
        path = snd if snd.startswith('Content/') else SND + snd
        extra = ''.join(f' {k}="{v}"' for k, v in opt.items())
        return f'      <Section start="{int(start)}" end="{int(end)}" sound="{path}"{extra}/>'

    def audio(self, start, end, snd, **opt):
        self.top['Audio'].append(self._section(start, end, snd, opt))

    def bus(self, bus, keys):
        self.top['BusVolume'].setdefault(bus, []).extend(keys)

    def subtitle(self, start, end, text, speaker='Pilot'):
        self.top['Subtitle'].append(f'    <Section start="{int(start)}" end="{int(end)}" speaker="{speaker}" text="{text}"/>')

    def event(self, f, ev, *args):
        a = ''.join(f' arg{i}="{v}"' for i, v in enumerate(args))
        self.top['Event'].append((int(f), f'    <Key f="{int(f)}" event="{ev}"{a} fireOnSkip="true"/>'))

    def pp(self, setting, keys):
        self.top['PostProcessSetting'].setdefault(setting, []).extend(keys)

    # --- out ------------------------------------------------------------------
    def text(self):
        out = ['<?xml version="1.0"?>', f'<Sequence version="1" fps="{FPS}" length="{self.length}">']
        for bid in self.order:
            head, lines = self.bindings[bid]
            out += [head] + lines + ['  </Binding>']
        t = self.top
        out += ['  <Track type="CameraCut" name="Shots">'] + t['CameraCut'] + ['  </Track>']
        if t['Fade']:
            out += ['  <Track type="Fade">'] + t['Fade'] + ['  </Track>']
        for s, keys in t['PostProcessSetting'].items():
            out += [f'  <Track type="PostProcessSetting" setting="{s}">']
            out += [f'    <Key f="{int(k[0])}" v="{fmt(k[1])}"' + (f' interp="{k[2]}"' if len(k) > 2 else '') + '/>' for k in keys]
            out += ['  </Track>']
        if t['Audio']:
            out += ['  <Track type="Audio">'] + [x[2:] for x in t['Audio']] + ['  </Track>']
        for bus, keys in t['BusVolume'].items():
            out += [f'  <Track type="BusVolume" bus="{bus}">'] + [f'    <Key f="{int(f)}" v="{fmt(v)}"/>' for f, v in keys]
            out += ['  </Track>']
        if t['Subtitle']:
            out += ['  <Track type="Subtitle">'] + t['Subtitle'] + ['  </Track>']
        if t['Event']:
            out += ['  <Track type="Event">'] + [x for _, x in sorted(t['Event'], key=lambda e: e[0])] + ['  </Track>']
        return '\n'.join(out + ['</Sequence>']) + '\n'

    def write(self, folder):
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, self.name + '.nmseq'), 'w') as f:
            f.write(self.text())
