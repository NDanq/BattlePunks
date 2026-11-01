#!/usr/bin/env python3
"""
Writes Demo Lab's grey-box worlds -- Content/Levels/{MainMenu,L1_Foothills,
L2_Laboratory,L3_Core}.nmworld, each with its folder of .nmentity files -- and
the placeholder cinematics, Content/Cinematics/*.nmseq.

    python3 Tools/Levels/make_levels.py        (from Games/DemoLab)

The layout (rooms, spawns, interactable spots, the AutoPlay routes) is in
layout.py; this file turns it into boxes and cylinders with the engine's
default material and wires the gameplay: interactables, gates, triggers,
objectives, encounters, timers and travel points (Source/World/*.h), by
signal name. Rerunning overwrites the levels.

Level 2 is one world with two states: entities named "Calm_*" exist only
while the travel state's Alarm flag is off, "Alarm_*" only while it is on.
"""
import hashlib, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout as L  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEVELS = os.path.join(ROOT, 'Content', 'Levels')
CINE = os.path.join(ROOT, 'Content', 'Cinematics')
BOX = 'Engine/Content/Meshes/Box.nmmesh'
CYL = 'Engine/Content/Meshes/Cylinder.nmmesh'
SPH = 'Engine/Content/Meshes/Sphere.nmmesh'
SND = 'Content/Sounds/'
MUSIC = 'Content/Music/'
T = L.WALL_THICKNESS
# DEMOLAB_REVIEW=1: no ceilings, worlds written as Review_<name> (overhead screenshots only; not staged or committed)
REVIEW = os.environ.get('DEMOLAB_REVIEW') == '1'


def f(x):
    return f'{x:g}'


def vec(tag, v):
    return f'    <{tag} X="{f(v[0])}" Y="{f(v[1])}" Z="{f(v[2])}" />'


def look(frm, to):
    """Euler (pitch down-positive, yaw) for a camera at frm looking at to."""
    d = [to[i] - frm[i] for i in range(3)]
    length = math.sqrt(sum(x * x for x in d))
    return (math.degrees(-math.asin(d[1] / length)), math.degrees(math.atan2(d[0], d[2])), 0.0)


class Level:
    """One world being written: entities, then the manifest."""

    def __init__(self, name, mode):
        self.name, self.mode = ('Review_' + name if REVIEW else name), mode
        self.entities = []
        self.names = set()
        self.counter = 0

    def guid(self, name):
        return hashlib.md5(f'{self.name}/{name}'.encode()).hexdigest()

    def unique(self, name):
        if name not in self.names:
            return name
        self.counter += 1
        return f'{name}_{self.counter}'

    # --- components ---------------------------------------------------------
    @staticmethod
    def transform(pos, rot=(0, 0, 0), scale=(1, 1, 1)):
        return ['  <Component Type="Transform" Version="1">', '  ' + vec('Position', pos), '  ' + vec('Rotation', rot),
                '  ' + vec('Scale', scale), '  </Component>']

    @staticmethod
    def render(mesh, visible=True, shadows=True, material=None):
        out = ['  <Component Type="Render" Version="7">', f'    <Visible Value="{"true" if visible else "false"}" />',
               f'    <Mesh Value="{mesh}" />']
        if material:
            out.append(f'    <Material0 Value="{material}" />')
        return out + [f'    <CastShadows Value="{"true" if shadows else "false"}" />', '  </Component>']

    @staticmethod
    def body(motion):   # 1 Kinematic, 2 Static
        return ['  <Component Type="Rigid Body" Version="1">', '    <Enabled Value="true" />', f'    <Motion Value="{motion}" />',
                '    <Shape Value="0" />', '    <ShapeSource Value="0" />', '    <Friction Value="0.6" />',
                '    <Layer Value="1" />', '    <Mask Value="-1" />', '  </Component>']

    @staticmethod
    def fields(ctype, version=1, **values):
        out = [f'  <Component Type="{ctype}" Version="{version}">']
        for k, v in values.items():
            if isinstance(v, tuple) and len(v) == 3 and k.endswith('Color'):
                out.append(f'    <{k} R="{v[0]}" G="{v[1]}" B="{v[2]}" A="255" />')
            elif isinstance(v, tuple):
                out.append('  ' + vec(k, v))
            elif isinstance(v, bool):
                out.append(f'    <{k} Value="{"true" if v else "false"}" />')
            else:
                out.append(f'    <{k} Value="{v}" />')
        return out + ['  </Component>']

    # --- entities -----------------------------------------------------------
    def entity(self, name, comps, parent=None, static=False):
        name = self.unique(name)
        self.names.add(name)
        g = self.guid(name)
        head = (f'<Entity Guid="{g}" Name="{name}"' + (f' Parent="{self.guid(parent)}"' if parent else '')
                + (' Mobility="Static"' if static else '') + '>')
        body = [x for c in comps for x in c]
        self.entities.append((g, '\n'.join(['<?xml version="1.0" encoding="UTF-8"?>', head] + body + ['</Entity>']) + '\n'))
        return name

    def box(self, name, centre, size, yaw=0.0, collide=True, visible=True, mesh=BOX, motion=2, extra=(), parent=None):
        comps = [self.render(mesh, visible), self.transform(centre, (0, yaw, 0), size)]
        if collide:
            comps.insert(1, self.body(motion))
        comps += list(extra)
        return self.entity(name, comps, parent, static=collide and motion == 2)

    def cylinder(self, name, centre, radius, height, collide=True):
        return self.box(name, centre, (radius * 2, height, radius * 2), mesh=CYL, collide=collide)

    def wall(self, name, cx, cz, length, yaw, height, gaps=(), y0=0.0, thick=T):
        """A wall along its local X (yaw 0: east-west), centred at (cx, cz);
        gaps are (offset along local X, width) doorways, DOOR_HEIGHT high with
        a lintel above."""
        ux, uz = math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))
        cuts = sorted((o - w / 2, o + w / 2) for o, w in gaps)
        start = -length / 2
        pieces = []
        for a, b in cuts + [(length / 2, length / 2)]:
            if a - start > 1:
                pieces.append((start, a, 0, height))
            if b > a:
                if height > L.DOOR_HEIGHT + 1:
                    pieces.append((a, b, L.DOOR_HEIGHT, height))
            start = b
        for i, (a, b, ylo, yhi) in enumerate(pieces):
            mid = (a + b) / 2
            self.box(f'{name}_{i}', (cx + ux * mid, y0 + (ylo + yhi) / 2, cz + uz * mid), (b - a, yhi - ylo, thick), yaw)

    def rect_room(self, name, x0, z0, x1, z1, height, doors, ceiling=True, prefix=''):
        cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
        n = prefix + name
        self.box(f'{n}_Floor', (cx, -10, cz), (x1 - x0, 20, z1 - z0))
        if ceiling and not REVIEW:
            self.box(f'{n}_Ceiling', (cx, height + 10, cz), (x1 - x0, 20, z1 - z0))
        self.wall(f'{n}_WallN', cx, z1 - T / 2, x1 - x0, 0, height, [(x - cx, w) for x, w in doors.get('N', [])])
        self.wall(f'{n}_WallS', cx, z0 + T / 2, x1 - x0, 0, height, [(x - cx, w) for x, w in doors.get('S', [])])
        # E/W: yaw 90, so local X runs south (-Z): offsets are negated
        self.wall(f'{n}_WallE', x1 - T / 2, cz, z1 - z0 - 2 * T, 90, height, [(-(z - cz), w) for z, w in doors.get('E', [])])
        self.wall(f'{n}_WallW', x0 + T / 2, cz, z1 - z0 - 2 * T, 90, height, [(-(z - cz), w) for z, w in doors.get('W', [])])

    def rotated_room(self, name, cx, cz, w, d, yaw, height, doors):
        def at(lx, lz):
            c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
            return cx + lx * c + lz * s, cz - lx * s + lz * c
        self.box(f'{name}_Floor', (cx, -10, cz), (w, 20, d), yaw)
        if not REVIEW:
            self.box(f'{name}_Ceiling', (cx, height + 10, cz), (w, 20, d), yaw)
        for side, (lx, lz), length, wyaw, sign in (('N', (0, d / 2 - T / 2), w, 0, 1), ('S', (0, -d / 2 + T / 2), w, 0, 1),
                                                   ('E', (w / 2 - T / 2, 0), d - 2 * T, 90, -1),
                                                   ('W', (-w / 2 + T / 2, 0), d - 2 * T, 90, -1)):
            x, z = at(lx, lz)
            self.wall(f'{name}_Wall{side}', x, z, length, yaw + wyaw, height, [(sign * o, gw) for o, gw in doors.get(side, [])])

    COMPASS = {'N': 0, 'NE': 45, 'E': 90, 'SE': 135, 'S': 180, 'SW': 225, 'W': 270, 'NW': 315}

    def octagon(self, name, centre, apothem, height, doors, glass=(), ceiling=True):
        cx, cz = centre
        self.box(f'{name}_Floor', (cx, -11, cz), (apothem * 2 + 2 * T, 20, apothem * 2 + 2 * T))
        if ceiling and not REVIEW:
            self.box(f'{name}_Ceiling', (cx, height + 11, cz), (apothem * 2 + 2 * T, 20, apothem * 2 + 2 * T))
        side = 2 * (apothem + T) * math.tan(math.radians(22.5))
        for label, a in self.COMPASS.items():
            r = apothem + T / 2
            x, z = cx + r * math.sin(math.radians(a)), cz + r * math.cos(math.radians(a))
            if label in glass:
                # a low wall, and an invisible pane over it (the conservatory is seen, not entered)
                self.wall(f'{name}_Sill{label}', x, z, side, a, 100)
                self.box(f'{name}_Glass{label}', (x, 100 + (height - 100) / 2, z), (side, height - 100, 10), a, visible=False)
                continue
            gaps = [(0, doors[label])] if label in doors else []
            self.wall(f'{name}_Wall{label}', x, z, side, a, height, gaps)

    def stairs(self, name, x0, x1, z0, z1, y0, y1, steps, axis):
        """Terraces of solid boxes from y0 to y1 along axis ('z' or 'x'),
        each a rise of (y1-y0)/steps: walked up, never a slope."""
        for i in range(steps):
            top = y0 + (y1 - y0) * (i + 1) / steps
            if axis == 'z':
                a = z0 + (z1 - z0) * i / steps
                b = z0 + (z1 - z0) * (i + 1) / steps
                self.box(f'{name}_{i}', ((x0 + x1) / 2, top / 2, (a + b) / 2), (x1 - x0, top, b - a))
            else:
                a = x0 + (x1 - x0) * i / steps
                b = x0 + (x1 - x0) * (i + 1) / steps
                self.box(f'{name}_{i}', ((a + b) / 2, top / 2, (z0 + z1) / 2), (b - a, top, z1 - z0))

    # --- gameplay -----------------------------------------------------------
    def player_start(self, name, p):
        return self.entity(name, [self.fields('PlayerStart', AvatarPrefab='', PlayerSlot=0, Enabled=True),
                                  self.transform((p[0], p[1] + 5, p[2]), (0, p[3], 0))])

    def interactable(self, name, p, prompt, channel='', enable_on='', sequence='', weapon=0, ammo=0, once=True,
                     hide=False, hold=0.0, toggle=False, radius=170, mesh=BOX, size=(30, 30, 30), handle=False, sound=''):
        comps = [self.fields('LabInteractable', Prompt=prompt, Radius=radius, WeaponID=weapon, Ammo=ammo, Channel=channel,
                             Toggle=toggle, Sequence=sequence, EnableOn=enable_on, Once=once, HideWhenUsed=hide,
                             HoldSeconds=hold, Sound=sound),
                 self.transform(p[:3], (0, p[3], 0), (1, 1, 1) if handle else size)]
        if mesh and not handle:
            comps.insert(1, self.render(mesh))
        name = self.entity(name, comps)
        if handle:
            self.entity('Handle', [self.transform((0, 0, 0))], parent=name)
            self.box(f'{name}_Wheel', (0, 0, -10), (60, 60, 8), collide=False, mesh=CYL, parent='Handle')
            self.box(f'{name}_Spoke', (0, 0, -16), (70, 8, 8), collide=False, parent='Handle')
        return name

    def ammo_crate(self, name, p, enable_on):
        return self.entity(name, [
            self.fields('LabInteractable', Prompt='Grab ammunition', Radius=170, WeaponID=0, Ammo=24, Channel='', Toggle=False,
                        Sequence='', EnableOn=enable_on, Once=False, HideWhenUsed=False, Sound=SND + 'Weapons/Pistol_Reload.nmsound'),
            self.render(BOX), self.body(2), self.transform(p[:3], (0, p[3], 0), (90, 60, 60))], static=True)

    def gate(self, name, centre, size, open_on, invert=False, yaw=0.0, height=None, sound=None):
        if sound is None:
            sound = SND + 'World/Shutter_Slam.nmsound'
        return self.entity(name, [
            self.fields('LabGate', OpenOn=open_on, OpenHeight=height or size[1] + 20, Speed=240, Invert=invert, Open=invert,
                        Sound=sound),
            self.render(BOX), self.body(1), self.transform(centre, (0, yaw, 0), size)])

    def trigger(self, name, p, extent, channel='', sequence='', enable_on='', checkpoint=False):
        return self.entity(name, [
            self.fields('LabTrigger', Extent=extent, Channel=channel, Sequence=sequence, EnableOn=enable_on, Once=True,
                        Checkpoint=checkpoint),
            self.transform(p[:3])])

    def objective(self, name, on_signal, text):
        return self.entity(name, [self.fields('LabObjective', OnSignal=on_signal, Text=text), self.transform((0, 0, 0))])

    def objectives(self, prefix, rows):
        for i, (sig, text) in enumerate(rows):
            self.objective(f'{prefix}Objective_{i}', sig, text)

    def encounter(self, name, id_, start_on, done, waves, spawns, lock='', stop_on='', health=60, delay=3.0):
        self.entity(name, [self.fields('LabEncounter', Id=id_, StartOn=start_on, DoneChannel=done, SpawnPrefab='',
                                       Waves=waves, WaveDelay=delay, LockChannel=lock, StopOn=stop_on, SpawnTeam=2,
                                       EnemyHealth=health),
                           self.transform((0, 0, 0))])
        for i, (x, z) in enumerate(spawns):
            self.entity(f'Spawn_{name}_{i}', [self.transform((x, 0, z), (0, 180, 0))], parent=name)

    def timer(self, name, start_on, seconds, done, label):
        return self.entity(name, [self.fields('LabTimer', StartOn=start_on, Seconds=seconds, DoneChannel=done, Label=label),
                                  self.transform((0, 0, 0))])

    def all_of(self, name, inputs, output):
        return self.entity(name, [self.fields('LabAllOf', Inputs=','.join(inputs), Output=output), self.transform((0, 0, 0))])

    def travel(self, name, on_signal, world, entry, flags, title):
        return self.entity(name, [self.fields('LabTravel', OnSignal=on_signal, World=world, Entry=entry, Flags=flags,
                                              Title=title), self.transform((0, 0, 0))])

    def light(self, name, pos, color, intensity, radius, toggle=None, show_when_on=True):
        comps = [self.fields('Point Light', version=2, Color=color, Intensity=intensity, Radius=radius, Active=True,
                             CastShadows=False, ShadowTechnique=0, SourceRadius=radius / 2),
                 self.transform(pos)]
        if toggle:
            comps.insert(1, self.fields('LabSignalToggle', Channel=toggle, ShowWhenOn=show_when_on))
        return self.entity(name, comps)

    def sound(self, name, path, pos=(0, 0, 0), autoplay=True, spatial=False, volume=1.0, toggle=None, show_when_on=True,
              min_d=300, max_d=5000):
        """An Audio Source: a 2D bed (ambience, music) or a 3D emitter; with
        toggle, a LabSignalToggle plays and stops it by a signal."""
        comps = [self.fields('Audio Source', version=2, Sound=path, Volume=volume, Pitch=1, AutoPlay=autoplay and not toggle,
                             Override=False, Loop=False, Spatial=spatial, Bus=0, MinDistance=min_d, MaxDistance=max_d,
                             Rolloff=1, DopplerFactor=0, ForceBus=0, ForceSpatial=2 if spatial else 1),
                 self.transform(pos)]
        if toggle:
            comps.insert(1, self.fields('LabSignalToggle', Channel=toggle, ShowWhenOn=show_when_on))
        return self.entity(name, comps)

    def weather(self, mist, mist_height, wind, wetness):
        return self.entity('Weather', [self.fields('Weather', Rain=0, RainMist=0.2, RainParticles=False, MistDensity=mist,
                                                   MistHeight=mist_height, MistFalloff=600, Wetness=wetness,
                                                   WetnessFollowsRain=False, WetSeconds=60, DrySeconds=240, WindYaw=60,
                                                   WindStrength=wind),
                                       self.transform((0, 0, 0))])

    def sun(self, hour, intensity=6, time_of_day=True):
        comps = [self.fields('Directional Light', version=3, Color=(255, 220, 180), Intensity=intensity, Active=True,
                             CastShadows=True, ShadowTechnique=2, ShadowBias=0.0012, ShadowNormalBias=2,
                             ShadowDistance=12000, CascadeCount=4, CascadeSplitLambda=0.8, SourceAngle=0.6),
                 self.fields('Sky Light', version=2, SkyColor=(150, 160, 190), GroundColor=(70, 60, 50), Intensity=0.6,
                             Active=True, Environment='', EnvironmentIntensity=1, EnvironmentRotation=0, DrawBackground=True)]
        if time_of_day:
            comps.append(self.fields('Time Of Day', Hour=hour, DayLengthMinutes=20, Running=False, NorthYaw=35, Latitude=35,
                                     SunIntensityScale=4, Preset='TimeOfDay:/Presets/Default.todpreset'))
        comps.append(self.transform((0, 8000, 0), (32, 215, 0)))
        return self.entity('Sun', comps)

    def ambient(self, intensity=0.25):
        return self.entity('Ambient', [
            self.fields('Sky Light', version=2, SkyColor=(120, 130, 150), GroundColor=(60, 55, 50), Intensity=intensity,
                        Active=True, Environment='', EnvironmentIntensity=1, EnvironmentRotation=0, DrawBackground=False),
            self.transform((0, 1000, 0))])

    def nav_volume(self, centre, half):
        return self.entity('NavMeshVolume', [
            self.fields('Nav Mesh Volume', HalfExtents=half, CellSize=20, CellHeight=10, AgentHeight=180, AgentRadius=35,
                        AgentMaxClimb=45, AgentMaxSlope=45, TileSize=48, RegionMinSize=8, RegionMergeSize=20,
                        EdgeMaxLength=1200, EdgeMaxError=1.3, LayerMask=1,
                        NavMesh=f'Content/Levels/{self.name}/{self.name}.nmnav', BuildAtRuntime=True, RebuildOnChange=True),
            self.transform(centre)])

    def routes(self, routes, prefix=''):
        for route, points in routes.items():
            for i, (x, z) in enumerate(points):
                self.entity(f'{prefix}AP_{route}_{i}', [self.transform((x, 0, z))])

    # --- cinematics ---------------------------------------------------------
    def cinematic(self, player, seq_name, shots, length, autoplay=False, subtitles=(), events=(), fade_in=True,
                  fade_out=False, prefix='', blend_out=0.0, audio=()):
        """A placeholder cutscene: two cameras, a cut between them, a fade,
        subtitles and Event keys. shots: [(frame, position, target)], the cut
        at the middle shot."""
        cams = [prefix + f'CineCam_{seq_name}_A', prefix + f'CineCam_{seq_name}_B']
        half = len(shots) // 2
        groups = [shots[:half + 1], shots[half:]]
        for cam, group in zip(cams, groups):
            self.entity(cam, [self.fields('Camera', ZNear=5, ZFar=60000, Active=False, Projection=1, FovY=50),
                              self.transform(group[0][1], look(group[0][1], group[0][2]))])
        self.entity(prefix + player, [
            # Hand Over: a cinematic that gives control back blends its last shot
            # into the player's camera (BlendOut), releasing input as the blend starts.
            self.fields('Sequence Player', version=2, Sequence=f'Content/Cinematics/{seq_name}.nmseq', PlayRate=1,
                        AutoPlay=autoplay, Loop=False, RestoreState=False, DestroyWhenFinished=False, Cinematic=True,
                        Skippable=True, BlendIn=0, BlendOut=blend_out, BlendEase=0,
                        ReleaseControl=1 if blend_out > 0 else 0, ReleaseBeforeEnd=0),
            self.transform((0, 0, 0))])
        body = []
        for i, (cam, group) in enumerate(zip(cams, groups)):
            body += camera_track(f'cam{i}', self.guid(cam), cam, group)
        cut = groups[1][0][0]
        body += ['  <Track type="CameraCut" name="Shots">', f'    <Section start="0" end="{cut}" camera="cam0"/>',
                 f'    <Section start="{cut}" end="{length}" camera="cam1"/>', '  </Track>']
        fade = ['  <Track type="Fade">']
        fade += ['    <Key f="0" v="1"/>', '    <Key f="15" v="0"/>'] if fade_in else ['    <Key f="0" v="0"/>']
        fade += [f'    <Key f="{length - 15}" v="0"/>', f'    <Key f="{length}" v="1"/>'] if fade_out else []
        body += fade + ['  </Track>']
        if subtitles:
            body += ['  <Track type="Subtitle">'] + [f'    <Section start="{a}" end="{b}" speaker="{s}" text="{t}"/>'
                                                     for a, b, s, t in subtitles] + ['  </Track>']
        if audio:
            body.append('  <Track type="Audio">')
            body += [f'    <Section start="{a}" end="{b}" sound="{MUSIC + snd[6:] if snd.startswith("MUSIC:") else SND + snd}" volume="{v}"/>'
                     for a, b, snd, v in audio]
            body.append('  </Track>')
        if events:
            body.append('  <Track type="Event">')
            for fr, ev, a0, a1 in events:
                extra = f' arg1="{a1}"' if a1 is not None else ''
                body.append(f'    <Key f="{fr}" event="{ev}" arg0="{a0}"{extra} fireOnSkip="true"/>')
            body.append('  </Track>')
        write_seq(seq_name, length, body)

    # --- write --------------------------------------------------------------
    def write(self):
        folder = os.path.join(LEVELS, self.name)
        os.makedirs(folder, exist_ok=True)
        for old in os.listdir(folder):
            if old.endswith('.nmentity'):
                os.remove(os.path.join(folder, old))
        for g, text in self.entities:
            with open(os.path.join(folder, g + '.nmentity'), 'w') as out:
                out.write(text)
        manifest = ['<?xml version="1.0" encoding="UTF-8"?>', '<World Format="1">', f'  <Settings GameMode="{self.mode}" />']
        manifest += [f'  <Entity Guid="{g}" />' for g, _ in self.entities] + ['</World>']
        with open(os.path.join(LEVELS, self.name + '.nmworld'), 'w') as out:
            out.write('\n'.join(manifest) + '\n')
        print(f'{self.name}: {len(self.entities)} entities')


def channel_keys(name, keys):
    return ([f'      <Channel name="{name}">'] + [f'        <Key f="{fr}" v="{f(round(v, 3))}" interp="CubicInOut"/>' for fr, v in keys]
            + ['      </Channel>'])


def camera_track(bid, guid, cam, shots):
    out = [f'  <Binding id="{bid}" kind="possess" guid="{guid}" name="{cam}">', '    <Track type="Transform">']
    rots = [look(p, t) for _, p, t in shots]
    for axis, idx in (('X', 0), ('Y', 1), ('Z', 2)):
        out += channel_keys(f'Position.{axis}', [(fr, p[idx]) for fr, p, _ in shots])
    out += channel_keys('Rotation.X', [(fr, r[0]) for (fr, _, _), r in zip(shots, rots)])
    out += channel_keys('Rotation.Y', [(fr, r[1]) for (fr, _, _), r in zip(shots, rots)])
    return out + ['    </Track>', '  </Binding>']


def write_seq(name, length, body):
    if REVIEW:
        return   # the review worlds are for screenshots: the real cinematics stay as they are
    os.makedirs(CINE, exist_ok=True)
    text = ['<?xml version="1.0"?>', f'<Sequence version="1" fps="30" length="{length}">'] + body + ['</Sequence>']
    with open(os.path.join(CINE, name + '.nmseq'), 'w') as out:
        out.write('\n'.join(text) + '\n')


# =============================================================================
# MainMenu
# =============================================================================
def make_menu():
    lv = Level('MainMenu', 'LabMenu')
    m = L.MENU
    x0, z0, x1, z1 = m['ground']
    lv.box('Ground', ((x0 + x1) / 2, -30, (z0 + z1) / 2), (x1 - x0, 20, z1 - z0))
    for name, centre, size in m['facade']:
        lv.box(name, centre, size)
    for i, x in enumerate((-1400, -700, 700, 1400)):
        lv.light(f'FacadeLamp_{i}', (x, 400, 2600), (255, 190, 120), 30, 900)
    lv.sun(17.5)
    lv.sound('Music_Menu', MUSIC + 'Music_Menu.nmsound', volume=0.8)
    first = m['dolly'][0]
    lv.entity('MenuCamera', [lv.fields('Camera', ZNear=5, ZFar=60000, Active=True, Projection=1, FovY=50),
                             lv.transform(first[1], look(first[1], first[2]))])
    lv.entity('MenuDolly', [lv.fields('Sequence Player', Sequence='Content/Cinematics/MenuDolly.nmseq', PlayRate=1,
                                      AutoPlay=True, Loop=True, RestoreState=False, DestroyWhenFinished=False,
                                      Cinematic=False, Skippable=False),
                            lv.transform((0, 0, 0))])
    write_seq('MenuDolly', m['dolly'][-1][0], camera_track('cam', lv.guid('MenuCamera'), 'MenuCamera', m['dolly']))
    lv.write()


# =============================================================================
# L1 Foothills
# =============================================================================
def make_l1():
    lv = Level('L1_Foothills', 'LabStory')
    d = L.L1
    x0, z0, x1, z1 = d['ground']
    lv.box('Ground', ((x0 + x1) / 2, -10, (z0 + z1) / 2), (x1 - x0, 20, z1 - z0))
    x0, z0, x1, z1 = d['sea']
    lv.box('Sea', ((x0 + x1) / 2, -90, (z0 + z1) / 2), (x1 - x0, 20, z1 - z0))
    x0, z0, x1, z1 = d['pier']
    lv.box('Pier', ((x0 + x1) / 2, -100, (z0 + z1) / 2), (x1 - x0, 200, z1 - z0))
    for i, x in enumerate(range(x0 + 100, x1, 400)):
        for side in (z0, z1):
            lv.cylinder(f'PierPost_{i}', (x, -40, side), 20, 200, collide=False)

    leg = d['leg1']
    lv.stairs('Road_Leg1', leg['x0'], leg['x1'], leg['z0'], leg['z1'], leg['y0'], leg['y1'], leg['steps'], 'z')
    lx0, lz0, lx1, lz1, top = d['landing']
    lv.box('Road_Landing', ((lx0 + lx1) / 2, top / 2, (lz0 + lz1) / 2), (lx1 - lx0, top, lz1 - lz0))
    lv.box('Overlook_Rail', (lx0 + 15, top + 50, (lz0 + lz1) / 2), (30, 100, lz1 - lz0))
    leg = d['leg2']
    lv.stairs('Road_Leg2', leg['x0'], leg['x1'], leg['z0'], leg['z1'], leg['y0'], leg['y1'], leg['steps'], 'z')
    px0, pz0, px1, pz1, top = d['plaza']
    lv.box('Plaza', ((px0 + px1) / 2, top / 2, (pz0 + pz1) / 2), (px1 - px0, top, pz1 - pz0))
    cx0, cz0, cx1, cz1, h = d['cliff']
    # the cliff, with the blast door's opening in it
    door_x = d['points']['BlastDoor'][0]
    lv.box('Cliff_W', ((cx0 + door_x - 300) / 2, h / 2, (cz0 + cz1) / 2), (door_x - 300 - cx0, h, cz1 - cz0))
    lv.box('Cliff_E', ((door_x + 300 + cx1) / 2, h / 2, (cz0 + cz1) / 2), (cx1 - door_x - 300, h, cz1 - cz0))
    lv.box('Cliff_Lintel', (door_x, (1100 + h) / 2, (cz0 + cz1) / 2), (600, h - 1100, cz1 - cz0))
    lv.box('Facade', (door_x, 1500, cz0 - 50), (3000, 1800, 100), collide=False)
    lv.box('Tunnel_Back', (door_x, 850, cz1 - 15), (600, 500, 30))
    gx0, gz0, gx1, gz1, gh = d['gatehouse']
    lv.box('Gatehouse', ((gx0 + gx1) / 2, gh / 2, (gz0 + gz1) / 2), (gx1 - gx0, gh, gz1 - gz0))
    # the forest (spruce stand-ins), scree and the signal mast
    fx0, fz0, fx1, fz1 = d['forest']
    for i in range(24):
        x = fx0 + (fx1 - fx0) * ((i * 37) % 24) / 24
        z = fz0 + (fz1 - fz0) * ((i * 11) % 24) / 24
        lv.cylinder(f'Spruce_{i}', (x, 400, z), 60, 800, collide=False)
        lv.box(f'SpruceCrown_{i}', (x, 900, z), (380, 600, 380), mesh=SPH, collide=False)
    for i in range(10):
        lv.box(f'Scree_{i}', (px0 + 150 + i * 230, top + 25, pz1 - 150 - (i % 3) * 60), (90, 50, 70), yaw=i * 23,
               collide=False)
    mx, mz = d['mast']
    lv.cylinder('SignalMast', (mx, 1500, mz), 30, 3000, collide=False)

    p = d['points']
    lv.player_start('Entry_Dock', p['Entry_Dock'])
    # the gatehouse gate and its crank (held)
    gx, gy, gz, _ = p['Gate']
    lv.gate('GatehouseGate', (gx, gy + 37 + 180, gz), (600, 360, 40), 'L1.GateOpen', sound=SND + 'World/Lift_Start.nmsound')
    lv.interactable('Crank_Gatehouse', p['Crank_Gatehouse'], 'Turn the crank', channel='L1.GateOpen', hold=2.5,
                    handle=True, sound=SND + 'World/Valve_Squeak_01.nmsound')
    lv.box('Crank_Post', (p['Crank_Gatehouse'][0], 300 + 45, p['Crank_Gatehouse'][2] + 25), (30, 90, 30))
    bx, by, bz, _ = p['BlastDoor']
    lv.gate('BlastDoor', (bx, by + 250, bz), (600, 500, 60), 'L1.BlastDoor', height=520,
            sound=SND + 'World/BlastDoor_Open.nmsound')
    lv.trigger('Trigger_AtGate', p['Trigger_AtGate'], (500, 200, 300), channel='L1.AtGate')
    lv.trigger('Trigger_Overlook', p['Trigger_Overlook'], (400, 200, 300), channel='L1.Overlook')
    lv.trigger('Trigger_Plaza', p['Trigger_Plaza'], (1200, 200, 300), channel='L1.Plaza')
    lv.trigger('Trigger_BlastDoor', p['Trigger_BlastDoor'], (600, 200, 300), sequence='C2_BlastDoor')
    lv.travel('Travel_L2', 'L1.Done', 'L2_Laboratory', 'Entry_Airlock', '', 'The Laboratory')
    lv.objectives('', [('', 'Walk up from the dock to the road'),
                       ('L1.AtGate', 'Turn the gatehouse crank'),
                       ('L1.GateOpen', 'Climb the road to the plaza'),
                       ('L1.Plaza', 'Approach the blast door')])
    lv.sun(17.5)
    lv.weather(mist=0.3, mist_height=-100, wind=1.5, wetness=0.15)   # sea mist low over the water, a damp coast
    lv.sound('Amb_Coast', SND + 'Ambience/Amb_L1_Coast.nmsound', toggle='L1.AtGate', show_when_on=False)
    lv.sound('Amb_Forest', SND + 'Ambience/Amb_L1_Forest.nmsound', toggle='L1.AtGate')
    lv.sound('Music_Explore', MUSIC + 'Music_Explore.nmsound', volume=0.5)
    lv.sound('Plaza_Gears', SND + 'World/Gears_Loop.nmsound', (bx, 900, bz - 200), spatial=True, min_d=200, max_d=2500)
    lv.nav_volume((3700, 600, 6200), (1400, 300, 900))
    lv.routes(d['routes'])

    lv.cinematic('C1_Arrival', 'C1_Arrival', [(0, (-6000, 1800, -5000), (-1500, 0, -1500)),
                                              (90, (-3500, 700, -2600), (-2400, 0, -1500)),
                                              (91, (2500, 1400, 1000), (3700, 600, 6500)),
                                              (180, (-2000, 300, -2200), (-2600, 120, -1500))],
                 180, autoplay=True, blend_out=1.5, audio=[(0, 180, 'World/Airship_Engine_Loop.nmsound', 0.8)],
                 subtitles=[(15, 85, 'Pilot', 'There is the island. I will set you down at the old dock.'),
                            (95, 170, 'Pilot', 'The road climbs to the lab. Signal when you have what we came for.')],
                 events=[(175, 'Story.Banner', 'FOOTHILLS APPROACH', 3)])
    lv.cinematic('C2_BlastDoor', 'C2_BlastDoor', [(0, (3000, 900, 5600), (3700, 800, 7000)),
                                                  (75, (3300, 800, 6100), (3700, 800, 7000)),
                                                  (76, (4600, 700, 6300), (3700, 700, 7000)),
                                                  (150, (4100, 750, 6600), (3700, 800, 7000))],
                 150, fade_out=True, audio=[(30, 100, 'World/Steam_Vent_Burst.nmsound', 1.0)],
                 subtitles=[(15, 120, 'Pilot', 'Steam pressure is up. That door has not opened in years.')],
                 events=[(60, 'Lab.Signal', 'L1.BlastDoor', 'true'), (149, 'Lab.Signal', 'L1.Done', 'true')])
    lv.write()


# =============================================================================
# L2 The Laboratory (Calm and Alarm)
# =============================================================================
def make_l2():
    lv = Level('L2_Laboratory', 'LabStory')
    d = L.L2
    for name, (x0, z0, x1, z1, h, doors) in d['rooms'].items():
        lv.rect_room(name, x0, z0, x1, z1, h, doors)
    a = d['atrium']
    lv.octagon('Atrium', a['centre'], a['apothem'], a['height'], a['doors'], glass=a['glass'])
    ox, oz, orad, oh = d['orrery']
    lv.cylinder('Orrery_Base', (ox, oh / 2, oz), orad, oh)
    lv.box('Orrery_Ring1', (ox, oh + 150, oz), (900, 10, 900), mesh=CYL, collide=False)
    lv.box('Orrery_Ring2', (ox, oh + 260, oz), (650, 10, 650), mesh=CYL, collide=False)
    lv.box('Orrery_Sun', (ox, oh + 200, oz), (120, 120, 120), mesh=SPH, collide=False)
    g = d['gantry']
    x1 = g['x0'] + g['steps'] * g['run']
    lv.stairs('Gantry_Step', g['x0'], x1, g['z0'], g['z1'], 0, g['steps'] * g['rise'], g['steps'], 'x')
    px0, pz0, px1, pz1 = g['platform']
    top = g['steps'] * g['rise']
    lv.box('Gantry_Platform', ((px0 + px1) / 2, top / 2, (pz0 + pz1) / 2), (px1 - px0, top, pz1 - pz0))
    for i, (x, z) in enumerate(d['turbines']):
        lv.cylinder(f'Turbine_{i}', (x, 250, z), 250, 500)
    for i, (x, z) in enumerate(d['benches']):
        lv.box(f'Bench_{i}', (x, 45, z), (300, 90, 120))
    for i, sample in enumerate(d['samples']):   # the shading-model samples, for the art pass to dress
        bx, bz = d['benches'][i // 2]
        lv.box(f'Sample_{sample}', (bx - 70 + (i % 2) * 140, 120, bz), (50, 50, 50), mesh=SPH, collide=False)
    for i in range(6):
        lv.cylinder(f'Plant_{i}', (1600 + (i % 3) * 500, 150, -500 + (i // 3) * 900), 120, 300, collide=False)
    lv.box('FreightLift_Platform', (-2950, 5, 3400), (400, 10, 600), collide=False)

    p = d['points']
    lv.player_start('Entry_Airlock', p['Entry_Airlock'])
    lv.player_start('Entry_FreightLift', p['Entry_FreightLift'])
    lv.ambient(0.2)

    # --- Calm: power, keycard, valves, the elevator ---------------------------
    lv.interactable('Calm_Breaker', p['Breaker'], 'Throw the breaker', channel='L2.Breaker', sequence='Calm_C3_Breaker',
                    size=(20, 60, 40), sound=SND + 'World/Breaker_Throw.nmsound')
    lv.gate('Calm_FoyerDoor', (0, 165, -1215), (300, 330, 30), 'L2.Powered', sound=SND + 'World/Lift_Stop.nmsound')
    lv.interactable('Calm_Keycard', p['Keycard'], 'Take the keycard', channel='L2.Keycard', hide=True,
                    sound=SND + 'UI/UI_Confirm.nmsound')
    lv.interactable('Calm_KeycardReader', p['KeycardReader'], 'Swipe the keycard', channel='L2.ArchiveOpen',
                    enable_on='L2.Keycard', sound=SND + 'World/Keycard_Accept.nmsound')
    lv.gate('Calm_KeycardDoor', (-2200, 165, 900), (240, 330, 40), 'L2.ArchiveOpen', sound=SND + 'World/Lift_Stop.nmsound')
    lv.interactable('Calm_Valve1', p['Valve1'], 'Open valve 1', channel='L2.Valve1', handle=True,
                    sound=SND + 'World/Valve_Squeak_01.nmsound')
    lv.interactable('Calm_Valve2', p['Valve2'], 'Open valve 2', channel='L2.Valve2', enable_on='L2.Valve1', handle=True,
                    sound=SND + 'World/Valve_Squeak_02.nmsound')
    lv.interactable('Calm_Valve3', p['Valve3'], 'Open valve 3', channel='L2.ValvesDone', enable_on='L2.Valve2',
                    handle=True, sound=SND + 'World/Valve_Squeak_01.nmsound')
    lv.interactable('Calm_ElevatorPanel', p['ElevatorPanel'], 'Call the elevator', channel='L2.Elevator',
                    enable_on='L2.ValvesDone', sequence='Calm_C4_Descent', sound=SND + 'World/Lift_Start.nmsound')
    lv.sound('Calm_Amb', SND + 'Ambience/Amb_L2_Calm.nmsound')
    lv.sound('Calm_Music_Explore', MUSIC + 'Music_Explore.nmsound', volume=0.5, toggle='L2.Powered')
    lv.sound('Calm_PowerHum', SND + 'World/Power_Hum_Loop.nmsound', p['Breaker'][:3], spatial=True, min_d=100,
             max_d=1500, toggle='L2.Powered')
    lv.sound('Calm_Turbine', SND + 'World/Turbine_Loop.nmsound', (0, 300, 3150), spatial=True, min_d=300, max_d=4000,
             toggle='L2.ValvesDone')
    lv.trigger('Calm_Trigger_Atrium', (0, 0, -500), (900, 200, 500), channel='L2.Atrium', enable_on='L2.Powered')
    lv.trigger('Calm_Trigger_Turbine', (-2200, 0, 3100), (500, 200, 300), channel='L2.TurbineHall')
    lv.travel('Calm_Travel_L3', 'L2.Elevator', 'L3_Core', 'Entry_Elevator', '', 'The Core')
    lv.objectives('Calm_', [('', 'Find power: the breaker panel in the foyer'),
                            ('L2.Powered', 'Explore the atrium; the archive is through the workshop'),
                            ('L2.Keycard', 'Open the archive door with the keycard'),
                            ('L2.ArchiveOpen', 'Go through the archive to the turbine hall'),
                            ('L2.TurbineHall', 'Open the three valves in order'),
                            ('L2.ValvesDone', 'Take the elevator down to the core')])
    rooms = {'Foyer': (0, 350, -1800), 'Atrium': (0, 700, 0), 'Workshop': (-2200, 380, 0),
             'Archive': (-2200, 380, 1750), 'Turbine': (-200, 600, 3400), 'Elevator': (2000, 380, 1800),
             'Airlock': (0, 330, -2700)}
    for room, pos in rooms.items():
        channel = {'Foyer': 'L2.Power.Foyer', 'Atrium': 'L2.Power.Atrium', 'Workshop': 'L2.Power.Workshop',
                   'Airlock': 'L2.Power.Foyer'}.get(room, 'L2.Power.All')
        lv.light(f'Calm_Light_{room}', pos, (255, 214, 160), 40, 1600, toggle=channel)
        lv.light(f'Calm_Emergency_{room}', (pos[0], 200, pos[2]), (90, 120, 255), 4, 700, toggle='L2.Power.All',
                 show_when_on=False)
    lv.cinematic('C3_Breaker', 'C3_Breaker', [(0, (-500, 250, -1600), (-850, 150, -1800)),
                                              (60, (-400, 260, -2000), (-850, 150, -1800)),
                                              (61, (0, 700, -1000), (0, 300, 600)),
                                              (180, (500, 800, 400), (-500, 200, -400))],
                 180, fade_in=False, prefix='Calm_', blend_out=1.5,
                 audio=[(20, 180, 'World/Power_Up_Sweep.nmsound', 1.0)],
                 subtitles=[(10, 90, 'Pilot', 'Power is back. Look at that place light up.')],
                 events=[(40, 'Lab.Signal', 'L2.Power.Foyer', 'true'), (90, 'Lab.Signal', 'L2.Power.Atrium', 'true'),
                         (120, 'Lab.Signal', 'L2.Power.Workshop', 'true'), (150, 'Lab.Signal', 'L2.Power.All', 'true'),
                         (178, 'Lab.Signal', 'L2.Powered', 'true'), (179, 'Lab.Signal', 'Checkpoint.Powered', 'true')])
    lv.cinematic('C4_Descent', 'C4_Descent', [(0, (1500, 300, 1500), (1900, 150, 1800)),
                                              (75, (1600, 250, 1300), (1900, 200, 2200)),
                                              (76, (2300, 120, 2100), (1900, -800, 1800)),
                                              (150, (2200, -400, 2000), (1900, -2000, 1800))],
                 150, fade_in=False, fade_out=True, prefix='Calm_',
                 audio=[(0, 150, 'World/Lift_Run_Loop.nmsound', 1.0)],
                 subtitles=[(10, 120, 'Pilot', 'Going down. The core is under the turbine hall.')])

    # --- Alarm: red light, shutters, W3, stragglers, the W4 hold, the roof ----
    lv.box('Alarm_Shutter_Elevator', (1900, 165, 2600), (300, 330, 60), motion=2)
    lv.gate('Alarm_Shutter_Archive', (-2200, 165, 2615), (300, 330, 40), 'L2A.W3Done')
    lv.gate('Alarm_Shutter_Workshop', (-1215, 165, 0), (40, 330, 300), 'L2A.StragglersDone')
    lv.trigger('Alarm_Trigger_W3', (-2900, 0, 3400), (500, 200, 600), channel='L2A.W3')
    lv.trigger('Alarm_Trigger_Archive', (-2200, 0, 1750), (900, 200, 700), channel='L2A.Archive', enable_on='L2A.W3Done')
    lv.trigger('Alarm_Trigger_W4', (-650, 0, 0), (450, 200, 700), channel='L2A.W4', enable_on='L2A.StragglersDone')
    s = d['spawns']
    lv.encounter('Alarm_Enc_W3', 'W3', 'L2A.W3', 'L2A.W3Done', 'Watchman:3|Watchman:2,Brute:1', s['W3'])
    lv.encounter('Alarm_Enc_Stragglers', 'Stragglers', 'L2A.Archive', 'L2A.StragglersDone', 'Watchman:2', s['Stragglers'])
    lv.encounter('Alarm_Enc_W4', 'W4', 'L2A.W4', 'L2A.W4Done', 'Watchman:2|Watchman:1,Brute:1|Watchman:2|Watchman:1,Brute:1|Watchman:2',
                 s['W4'], stop_on='L2A.HoldDone', delay=5.0)
    lv.timer('Alarm_Timer_Hold', 'L2A.W4', 60, 'L2A.HoldDone', 'Airship inbound')
    for name in ('AmmoCrate_Turbine', 'AmmoCrate_Archive', 'AmmoCrate_Atrium'):
        lv.ammo_crate(f'Alarm_{name}', p[name], 'State.Alarm')
    lv.interactable('Alarm_RoofHatch', p['RoofHatch'], 'Open the roof hatch', channel='L2A.Hatch',
                    enable_on='L2A.HoldDone', sequence='Alarm_C6_Extraction', size=(160, 20, 160),
                    sound=SND + 'World/Steam_Vent_Burst.nmsound')
    lv.sound('Alarm_Amb', SND + 'Ambience/Amb_L2_Alarm.nmsound')
    lv.sound('Alarm_Music_Combat', MUSIC + 'Music_Combat.nmsound', volume=0.7, toggle='L2A.HoldDone', show_when_on=False)
    lv.sound('Alarm_Turbine', SND + 'World/Turbine_Loop.nmsound', (0, 300, 3150), spatial=True, min_d=300, max_d=4000)
    lv.sound('Alarm_Sprinklers', SND + 'World/Sprinklers_Loop.nmsound', (-2200, 400, 1000), spatial=True, min_d=400,
             max_d=2500)
    lv.sound('Alarm_Klaxon', SND + 'World/Alarm_Klaxon_Loop.nmsound', (0, 800, 0), spatial=True, min_d=500, max_d=6000,
             volume=0.6)
    lv.objectives('Alarm_', [('', 'Alarm! Hold out in the turbine hall'),
                             ('L2A.W3Done', 'Fall back through the archive'),
                             ('L2A.Archive', 'Clear the stragglers'),
                             ('L2A.StragglersDone', 'Reach the atrium'),
                             ('L2A.W4', 'Hold the atrium until the airship arrives'),
                             ('L2A.HoldDone', 'Climb the orrery gantry to the roof hatch')])
    for room, pos in rooms.items():
        lv.light(f'Alarm_Red_{room}', pos, (255, 40, 25), 30, 1500)
    lv.cinematic('C6_Extraction', 'C6_Extraction', [(0, (900, 650, 1000), (450, 650, 600)),
                                                    (90, (700, 900, 800), (450, 1100, 600)),
                                                    (91, (-3000, 3000, -3000), (0, 1200, 0)),
                                                    (180, (-5000, 4000, -6000), (0, 1500, 0))],
                 180, fade_in=False, fade_out=True, prefix='Alarm_',
                 audio=[(0, 180, 'World/Airship_Engine_Loop.nmsound', 1.0), (60, 180, 'MUSIC:Music_Extraction.nmsound', 1.0)],
                 subtitles=[(10, 90, 'Pilot', 'Line is down. Clip on, we are leaving!'),
                            (100, 170, 'Pilot', 'Intel aboard. Taking us home over the island.')],
                 events=[(179, 'Lab.Signal', 'Game.Complete', 'true')])

    lv.nav_volume((0, 300, 600), (3300, 400, 3700))
    lv.routes({k: v for k, v in d['routes'].items() if not k.startswith('L2A_')}, prefix='Calm_')
    lv.routes({k: v for k, v in d['routes'].items() if k.startswith('L2A_')}, prefix='Alarm_')
    __import__('dress_l2_laboratory').dress(lv, sys.modules[__name__])   # the art pass (Tools/Levels/dress_l2_laboratory.py)
    lv.write()


# =============================================================================
# L3 The Core
# =============================================================================
def make_l3():
    lv = Level('L3_Core', 'LabStory')
    d = L.L3
    r = d['ring']
    lv.octagon('Ring', r['centre'], r['apothem'], r['height'], r['doors'])
    dr = d['drum']
    lv.octagon('Drum', dr['centre'], dr['apothem'], dr['height'], dr['doors'], ceiling=False)
    e = d['elevator']
    ex = e['distance'] * math.sin(math.radians(e['angle']))
    ez = e['distance'] * math.cos(math.radians(e['angle']))
    lv.rotated_room('ElevatorRoom', ex, ez, e['size'][0], e['size'][1], e['angle'], e['height'], e['doors'])
    for name, (x0, z0, x1, z1, h, doors) in d['rooms'].items():
        lv.rect_room(name, x0, z0, x1, z1, h, doors)
    for i, (x, z) in enumerate(d['pumps']):
        lv.cylinder(f'Pump_{i}', (x, 200, z), 300, 400)
    lv.box('Vault_Pedestal', (0, 45, 0), (100, 90, 100))
    lv.box('Reactor_Ring1', (0, 650, 0), (1500, 20, 1500), mesh=CYL, collide=False)
    lv.box('Beacon_Mast', (1400, 300, -5100), (60, 600, 60), mesh=CYL, collide=False)
    lv.box('Locker_Cabinet', (-600, 100, -1560), (120, 200, 60))

    p = d['points']
    lv.player_start('Entry_Elevator', p['Entry_Elevator'])
    lv.ambient(0.2)
    lv.interactable('Intel', p['Intel'], 'Take the intel', channel='L3.Intel', sequence='C5_Theft', hide=True,
                    sound=SND + 'UI/UI_Confirm.nmsound')
    lv.interactable('Locker', p['Locker'], 'Take the pipe pistol', channel='L3.Armed', enable_on='L3.Alarm', weapon=2,
                    sound=SND + 'Weapons/Pistol_Reload.nmsound')
    lv.sound('Amb_Reactor', SND + 'Ambience/Amb_L3_Reactor.nmsound')
    lv.sound('Music_Explore', MUSIC + 'Music_Explore.nmsound', volume=0.4, toggle='L3.Alarm', show_when_on=False)
    lv.sound('Music_Combat', MUSIC + 'Music_Combat.nmsound', volume=0.7, toggle='L3.Alarm')
    lv.sound('Klaxon', SND + 'World/Alarm_Klaxon_Loop.nmsound', (0, 650, 0), spatial=True, min_d=500, max_d=6000,
             volume=0.6, toggle='L3.Alarm')
    lv.ammo_crate('AmmoCrate_Ring', p['AmmoCrate_Ring'], 'L3.Armed')
    lv.ammo_crate('AmmoCrate_Pump', p['AmmoCrate_Pump'], 'L3.Armed')
    # the elevator's door slams when the alarm goes; the tunnel opens after W1
    door_r = r['apothem'] + T / 2
    nwx, nwz = door_r * math.sin(math.radians(315)), door_r * math.cos(math.radians(315))
    lv.gate('Shutter_Elevator', (nwx, 165, nwz), (300, 330, 40), 'L3.Alarm', invert=True, yaw=315)
    lv.gate('TunnelGate', (0, 165, -(r['apothem'] + T / 2)), (300, 330, 40), 'L3.W1Done')
    lv.gate('FreightGate', (3400 - T, 165, -5400), (40, 330, 300), 'L3.BeaconFired')
    lv.trigger('Trigger_PumpHall', p['Trigger_PumpHall'], (300, 200, 200), channel='L3.PumpHall', checkpoint=True)
    lv.interactable('Beacon_Start', p['Beacon_Start'], 'Start charging the beacon', channel='L3.BeaconCharging',
                    enable_on='L3.PumpHall', sound=SND + 'World/Beacon_Charge.nmsound')
    lv.interactable('Beacon_Fire', p['Beacon_Fire'], 'Fire the beacon', channel='L3.BeaconFired',
                    enable_on='L3.BeaconReady', sound=SND + 'World/Beacon_Fire.nmsound')
    lv.interactable('FreightPanel', p['FreightPanel'], 'Ride the freight lift up', channel='L3.FreightLift',
                    enable_on='L3.BeaconFired', sound=SND + 'World/Lift_Start.nmsound')
    s = d['spawns']
    lv.encounter('Enc_W1', 'W1', 'L3.Armed', 'L3.W1Done', 'Watchman:4', s['W1'])
    lv.encounter('Enc_W2', 'W2', 'L3.BeaconCharging', 'L3.W2Done', 'Watchman:3|Watchman:2,Brute:1', s['W2'])
    lv.timer('Timer_Beacon', 'L3.BeaconCharging', 40, 'L3.BeaconCharged', 'Beacon charging')
    lv.all_of('Beacon_Ready', ['L3.W2Done', 'L3.BeaconCharged'], 'L3.BeaconReady')
    lv.travel('Travel_L2Alarm', 'L3.FreightLift', 'L2_Laboratory', 'Entry_FreightLift', 'Alarm=1,Armed=1',
              'The Laboratory: Alarm')
    lv.objectives('', [('', 'Descend the ring gallery to the vault'),
                       ('L3.Alarm', 'Take the sidearm from the locker'),
                       ('L3.Armed', 'Survive the security response'),
                       ('L3.W1Done', 'Take the maintenance tunnel south'),
                       ('L3.PumpHall', 'Start the extraction beacon'),
                       ('L3.BeaconCharging', 'Defend the beacon while it charges'),
                       ('L3.BeaconReady', 'Fire the beacon'),
                       ('L3.BeaconFired', 'Ride the freight lift up')])
    for i, (x, y, z) in enumerate([(1200, 600, 0), (-1200, 600, 0), (0, 600, 1200), (0, 600, -1200), (0, 500, 0),
                                   (0, 350, -3100), (1500, 600, -5400), (3800, 400, -5400)]):
        lv.light(f'Light_{i}', (x, y, z), (220, 230, 255), 35, 1500, toggle='L3.Alarm', show_when_on=False)
        lv.light(f'RedLight_{i}', (x, y - 50, z), (255, 40, 25), 30, 1500, toggle='L3.Alarm', show_when_on=True)
    lv.cinematic('C5_Theft', 'C5_Theft', [(0, (250, 200, -250), (0, 110, 0)),
                                          (70, (150, 160, -150), (0, 110, 0)),
                                          (71, (-1200, 600, -1500), (0, 300, 0)),
                                          (180, (-900, 250, -1900), (-600, 120, -1500))],
                 180, fade_in=False, blend_out=1.5,
                 audio=[(60, 140, 'World/Shutter_Slam.nmsound', 1.0), (66, 140, 'World/Steam_Vent_Burst.nmsound', 0.9)],
                 subtitles=[(10, 70, 'Pilot', 'You have it. Now get out of there.'),
                            (80, 170, 'Pilot', 'Alarm! There is a locker by the bridge. Arm yourself.')],
                 events=[(60, 'Lab.Signal', 'L3.Alarm', 'true'), (179, 'Lab.Signal', 'Checkpoint.Theft', 'true')])
    lv.nav_volume((1200, 300, -2300), (3200, 400, 4300))
    lv.routes(d['routes'])
    lv.write()


if __name__ == '__main__':
    make_menu()
    make_l1()
    make_l2()
    make_l3()
