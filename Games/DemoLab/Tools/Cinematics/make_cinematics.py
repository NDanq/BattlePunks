#!/usr/bin/env python3
"""
Demo Lab's six cinematics (Docs/Design.md, "Cinematics"), written as Content/Cinematics/C*.nmseq,
and the cinematic-only entities they need, added to the levels as they are generated.

    python3 Tools/Cinematics/make_cinematics.py        (from Games/DemoLab; runs Tools/Levels/make_levels.py)

make_levels.py builds each level and calls build(lv) just before writing it: that adds the shot
cameras (CineCam_<Cn>_<k>), the player's marks (CineMark_*), the C4 shaft under the elevator cage
and C6's airship, then writes that level's sequences. The Sequence Players, the gameplay that starts
them and the Event keys the flow listens for stay make_levels.py's; the keys keep their meaning and
order (the light cascade's signals come from dress_l2_laboratory.CASCADE).

The player's character is the binding "player": the game binds it when a cutscene starts
(LabGameModeBase::OnSequenceStarted); "Lab.PlacePlayer" (arg0 a CineMark) stands the character there
for the hand-over (LabStoryMode). Watch one: Tools/Cinematics/record.sh C3 <folder>.
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(GAME, 'Tools', 'Levels'))
import layout as L  # noqa: E402
from seq import Seq, look, sample, MUSIC  # noqa: E402

CINE = os.path.join(GAME, 'Content', 'Cinematics')
BOX = 'Engine/Content/Meshes/Box.nmmesh'
CYL = 'Engine/Content/Meshes/Cylinder.nmmesh'
MAT = 'Content/Materials/Kit/{}.material.nmgraph'
AIRSHIP = 'Content/Meshes/Airship/'
PART = 'Content/Particles/{}.nmparticle'


# =============================================================================
# Level entities
# =============================================================================
def name_of(text):
    return re.search(r' Name="([^"]+)"', text).group(1)


def drop(lv, pattern):
    rx = re.compile(pattern)
    lv.entities[:] = [(g, t) for g, t in lv.entities if not rx.fullmatch(name_of(t))]


def add_camera(lv, name, pos, target, fov=50):
    p, y = look(pos, target)
    lv.entity(name, [lv.fields('Camera', ZNear=5, ZFar=80000, Active=False, Projection=1, FovY=fov),
                     lv.transform(pos, (p, y, 0))])
    return name


def add_mark(lv, name, pos, yaw):
    """Where Lab.PlacePlayer (and -DemoLab.Cinematic) stands the player, facing yaw."""
    lv.entity(name, [lv.transform(pos, (0, yaw, 0))])


def mesh(lv, name, path, pos, rot=(0, 0, 0), scale=(1, 1, 1), parent=None, material=None, shadows=True, visible=True,
         extra=(), params=()):
    """params: (name, slot, value tuple) material parameter overrides on the Render."""
    r = lv.render(path, visible, shadows, material)
    if params:
        lines = [f'    <ParameterCount Value="{len(params)}" />']
        for i, (pname, slot, v) in enumerate(params):
            v = tuple(v) + (0,) * (4 - len(v))
            lines += [f'    <Parameter{i}Name Value="{pname}" />', f'    <Parameter{i}Slot Value="{slot}" />',
                      f'    <Parameter{i}X Value="{v[0]}" />', f'    <Parameter{i}Y Value="{v[1]}" />',
                      f'    <Parameter{i}Z Value="{v[2]}" />', f'    <Parameter{i}W Value="{v[3]}" />']
        r = [x for x in r if 'ParameterCount' not in x]
        r = r[:-2] + lines + r[-2:]
    return lv.entity(name, [r, lv.transform(pos, rot, scale)] + list(extra), parent)


def point(lv, name, pos, color, intensity, radius, parent=None, source=10, toggle=None, show_when_on=True):
    comps = [lv.fields('Point Light', version=2, Color=color, Intensity=intensity, Radius=radius, Active=True,
                       CastShadows=False, ShadowTechnique=0, SourceRadius=source), lv.transform(pos)]
    if toggle:
        comps.insert(1, lv.fields('LabSignalToggle', Channel=toggle, ShowWhenOn=show_when_on))
    return lv.entity(name, comps, parent)


class Shots:
    """A sequence's cameras: each shot its own camera entity in the level and binding in the sequence."""

    def __init__(self, lv, s, cid, prefix=''):
        self.lv, self.s, self.cid, self.prefix, self.n = lv, s, cid, prefix, 0

    def shot(self, start, end, keys, fov=50, blend=0, fov_keys=None):
        """keys: (frame, position, target[, interp]); the camera is cut to from start to end."""
        self.n += 1
        name = f'{self.prefix}CineCam_{self.cid}_{self.n}'
        add_camera(self.lv, name, keys[0][1], keys[0][2], fov)
        bid = f'cam{self.n}'
        self.s.camera(bid, self.lv.guid(name), name, keys, fov_keys)
        self.s.cut(start, end, bid, blend)
        return bid


def player(s, lv):
    """The player's binding: overridden at runtime with the local character."""
    return s.bind('player', lv.guid('CinePlayer'), 'CinePlayer')


def facing(yaw):
    """The character's Transform yaw for a facing yaw (the mover's MeshYaw is 180)."""
    return yaw + 180.0


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


# =============================================================================
# C1 Arrival (L1 start): the airship in over the sea at golden hour, the cliffs and the lab, the dock
# =============================================================================
D1 = L.L1
P0 = D1['pier']
SHIP_HOME = (P0[0] - 1400, 0, (P0[1] + P0[3]) / 2 - 300)       # dress_l1.airship: the rig's home
SHIP_HULL_Y = 1500                                              # the hull over the rig (the ambient bob's)
SHIP_C1 = {   # the rig's flight: in from the south-west over the sea, easing to its mooring
    'Position.X': [(0, SHIP_HOME[0] - 12500, 'Linear'), (420, SHIP_HOME[0] - 2600, 'CubicOut'), (560, SHIP_HOME[0])],
    'Position.Y': [(0, 900, 'Linear'), (420, 160, 'CubicOut'), (560, 0)],
    'Position.Z': [(0, SHIP_HOME[2] - 3600, 'Linear'), (420, SHIP_HOME[2] - 500, 'CubicOut'), (560, SHIP_HOME[2])],
    'Rotation.Y': [(0, -17, 'Linear'), (420, -6, 'CubicOut'), (560, 0)],
}


def ship_c1(f, up=SHIP_HULL_Y + 450):
    """Where C1's airship is at frame f (the gondola's middle, up from the rig)."""
    return (sample(SHIP_C1['Position.X'], f), sample(SHIP_C1['Position.Y'], f) + up, sample(SHIP_C1['Position.Z'], f))


def c1(lv):
    s = Seq('C1_Arrival', 600)
    sh = Shots(lv, s, 'C1')
    rig = s.bind('ship', lv.guid('Airship_Flight'), 'Airship_Flight')
    s.transform(rig, SHIP_C1)
    s.sound_at(rig, [(0, 600, 'World/Airship_Engine_Loop.nmsound', {'volume': 1.0, 'spatial': '3D'})])

    # 1: low over the water, into the low sun: the airship comes on, a silhouette, and passes overhead.
    c = (SHIP_HOME[0] - 8200, 300, SHIP_HOME[2] + 1400)
    sh.shot(0, 140, [(0, c, ship_c1(0)), (139, add(c, (350, 40, 0)), ship_c1(139))], fov=36)
    # 2: alongside it, the island beyond (the sun behind the camera)
    sh.shot(140, 260, [(140, add(ship_c1(140), (-1700, -500, -2700)), add(ship_c1(140), (400, 0, 0))),
                       (259, add(ship_c1(259), (-1100, -200, -2600)), add(ship_c1(259), (900, -100, 0)))], fov=45)
    # 3: the island: a sweep along the cliff past the lab's facade and the blast door
    bx, by, bz, _ = D1['points']['BlastDoor']
    sh.shot(260, 380, [(260, (bx + 2600, by + 1500, bz - 2600), (bx + 300, by + 700, bz)),
                       (379, (bx - 1900, by + 1100, bz - 2400), (bx - 100, by + 500, bz))], fov=48, blend=0)
    # 4: from the beach, the airship easing in to the dock against the sun
    sh.shot(380, 470, [(380, (SHIP_HOME[0] + 900, 450, SHIP_HOME[2] + 3600), ship_c1(380)),
                       (469, (SHIP_HOME[0] + 700, 420, SHIP_HOME[2] + 3300), ship_c1(469))], fov=45)
    # 5: on the pier: the player walks off it toward us, the airship moored behind
    sh.shot(470, 540, [(470, (-1900, 140, -1450), (-3300, 250, -1550)), (539, (-2050, 130, -1430), (-3000, 210, -1520))],
            fov=42)
    # 6: behind the player, settling where the game's camera will be (Blend Out into it)
    sh.shot(540, 600, [(540, (-3550, 260, -1340), (-2600, 150, -1500)), (599, (-3020, 205, -1470), (-2300, 150, -1500))],
            fov=50)

    me = player(s, lv)
    ex, _, ez, _ = D1['points']['Entry_Dock']
    s.transform(me, {'Position.X': [(0, ex - 900), (440, ex - 900, 'Linear'), (585, ex, 'QuadOut')],
                     'Position.Z': [(0, ez), (585, ez)]})
    s.anim(me, [(0, 445, 'FB_Idle', {'loop': True}), (440, 590, 'FB_Walk', {'loop': True, 'blendIn': 8}),
                (582, 600, 'FB_Idle', {'loop': True, 'blendIn': 10})])

    s.fade([(0, 1), (40, 0)])
    s.audio(0, 600, MUSIC + 'Music_Menu.nmsound', volume=0.85, blendOut=90)
    music = s.bind('music', lv.guid('Music_Explore'), 'Music_Explore')       # the level's bed waits for the theme
    s.float(music, 'Audio Source', 'Volume', [(0, 0), (540, 0), (600, 0.5)])
    s.bus('Ambient', [(0, 0.6), (420, 0.6), (600, 1)])
    s.subtitle(45, 135, 'There is the island. I will set you down at the old dock.')
    s.subtitle(275, 375, 'The road climbs to the lab. Signal when you have what we came for.')
    s.event(586, 'Lab.PlacePlayer', 'CineMark_C1_End')
    s.event(590, 'Story.Banner', 'FOOTHILLS APPROACH', 3)
    return s


def dress_l1(lv):
    drop(lv, r'CineCam_C[12]_\w+_[AB]')
    ex, ey, ez, eyaw = D1['points']['Entry_Dock']
    add_mark(lv, 'CineMark_C1_End', (ex, ey, ez), eyaw)
    bx, by, bz, _ = D1['points']['BlastDoor']
    add_mark(lv, 'CineMark_C2', (bx, by + 5, bz - 650), 0)


# =============================================================================
# C2 Blast door (L1 end): the plaza, steam, the cog, the leaves part, into the dark
# =============================================================================
def c2(lv):
    s = Seq('C2_BlastDoor', 480)
    sh = Shots(lv, s, 'C2')
    bx, by, bz, _ = D1['points']['BlastDoor']
    fz = bz - 35                       # dress_l1.facade: the door's plane
    cog = (bx, by + 260, fz - 2)
    # 1: wide from the plaza's foot, the vents steaming, a slow push
    sh.shot(0, 110, [(0, (bx + 150, by + 420, bz - 2700), (bx, by + 500, bz)),
                     (109, (bx + 100, by + 380, bz - 2350), (bx, by + 480, bz))], fov=66)
    # 2: the cog wakes and turns once
    sh.shot(110, 200, [(110, (bx + 420, by + 330, fz - 520), cog), (199, (bx + 300, by + 300, fz - 430), cog)], fov=40)
    # 3: the leaves part (the door's latch: L1.BlastDoor)
    sh.shot(200, 330, [(200, (bx + 900, by + 180, bz - 1500), (bx, by + 350, bz)),
                       (329, (bx + 500, by + 260, bz - 1250), (bx, by + 330, bz))], fov=50)
    # 4: behind the player, pushing into the dark doorway; fade
    sh.shot(330, 480, [(330, (bx + 120, by + 230, bz - 1150), (bx, by + 250, bz + 300)),
                       (479, (bx + 60, by + 200, bz - 420), (bx + 20, by + 180, bz + 600))], fov=50)

    for bid, name, dx in (('cog', 'BlastDoor_Cog', -210), ('leafl', 'BlastDoor_LeafL', -210), ('leafr', 'BlastDoor_LeafR', 210)):
        s.bind(bid, lv.guid(name), name)
        ch = {'Position.X': [(0, bx), (205, bx, 'CubicInOut'), (300, bx + dx)]}
        if bid == 'cog':
            ch['Rotation.Z'] = [(0, 0), (120, 0, 'CubicInOut'), (198, 360)]
        s.transform(bid, ch)

    me = player(s, lv)
    s.transform(me, {'Position.X': [(0, bx), (480, bx)],
                     'Position.Z': [(0, bz - 650), (335, bz - 650, 'Linear'), (470, bz - 230)],
                     'Rotation.Y': [(0, facing(0)), (480, facing(0))]})
    s.anim(me, [(0, 340, 'FB_Idle', {'loop': True}), (335, 480, 'FB_Walk', {'loop': True, 'blendIn': 8})])

    s.fade([(0, 1), (15, 0), (440, 0), (480, 1)])
    s.audio(20, 95, 'World/Steam_Vent_Burst.nmsound', volume=0.9)
    s.audio(110, 300, 'World/Gears_Loop.nmsound', volume=1.0, blendIn=10, blendOut=20)
    s.audio(196, 268, 'World/Steam_Vent_Burst.nmsound', volume=1.0)
    s.bus('Music', [(0, 1), (300, 1), (470, 0.2)])
    s.subtitle(20, 105, 'Steam pressure is up. That door has not opened in years.')
    s.subtitle(345, 430, 'You are in. Keep your head down.')
    s.event(200, 'Lab.Signal', 'L1.BlastDoor', 'true')
    s.event(479, 'Lab.Signal', 'L1.Done', 'true')
    return s


# =============================================================================
# C3 Power-up (L2 foyer): the lever, then the lab lighting up room by room
# =============================================================================
D2 = L.L2


def c3(lv):
    from dress_l2_laboratory import CASCADE
    s = Seq('C3_Breaker', 480)
    sh = Shots(lv, s, 'C3', 'Calm_')
    bx, _, bz, _ = D2['points']['Breaker']
    wx = D2['rooms']['Foyer'][0] + L.WALL_THICKNESS         # the panel's wall
    stand = (wx + 120, bz)
    # 1: the dark foyer, the hand lamp; the player throws the lever
    sh.shot(0, 52, [(0, (wx + 330, 120, bz - 230), (wx + 40, 120, bz + 10)),
                    (51, (wx + 300, 118, bz - 205), (wx + 30, 115, bz + 10))], fov=45)
    # 1b: close on the lever as it goes over
    sh.shot(52, 90, [(52, (wx + 105, 150, bz - 95), (wx + 14, 95, bz)), (89, (wx + 100, 145, bz - 85), (wx + 14, 95, bz))],
            fov=40)
    # 2: the foyer from its north door: the pendants pop on
    sh.shot(90, 140, [(90, (450, 300, -1350), (-300, 170, -2050)), (139, (420, 290, -1380), (-300, 160, -2050))], fov=55)
    # 3: the atrium, low under the orrery: it glows up, the posts come on
    sh.shot(140, 190, [(140, (500, 60, -700), (0, 380, 0)), (189, (430, 60, -760), (0, 390, 0))], fov=55)
    # 4: the workshop's samples and plaques
    sh.shot(190, 230, [(190, (-2250, 140, -40), (-2400, 85, -260)), (229, (-2150, 135, -40), (-2300, 85, -260))], fov=45)
    # 5: the turbine hall, its bars down the hall one after another
    sh.shot(230, 275, [(230, (2500, 320, 2800), (-1500, 300, 3600)), (274, (2450, 300, 2800), (-1500, 290, 3600))], fov=55)
    # 6: the atrium wide from the gantry side: the bounce light arrives (DDGI / SSGI), the rings spin up
    sh.shot(275, 400, [(275, (-720, 520, 640), (150, 260, -300)), (399, (-760, 340, 430), (150, 230, -420))], fov=60)
    # 7: back to the foyer: the player turns from the panel to the open door (Blend Out into the game)
    sh.shot(400, 480, [(400, (wx + 260, 230, bz - 260), (wx + 40, 120, bz + 30)),
                       (479, (stand[0] - 60, 200, stand[1] - 330), (stand[0], 150, stand[1] + 400))], fov=50)

    lever = s.bind('lever', lv.guid('Calm_BreakerLever'), 'Calm_BreakerLever')
    s.transform(lever, {'Rotation.X': [(0, -35), (62, -35, 'BackOut'), (72, 35)]})

    me = player(s, lv)
    s.transform(me, {'Position.X': [(0, stand[0]), (480, stand[0])], 'Position.Z': [(0, stand[1]), (480, stand[1])],
                     'Rotation.Y': [(0, facing(270)), (420, facing(270), 'CubicInOut'), (460, facing(360))]})
    s.anim(me, [(0, 480, 'FB_Idle', {'loop': True})])
    s.event(470, 'Lab.PlacePlayer', 'Calm_CineMark_C3_End')
    s.event(478, 'Lab.Signal', 'L2.Powered', 'true')
    s.event(479, 'Lab.Signal', 'Checkpoint.Powered', 'true')

    # The cascade: the same 13 signals in the same order, over ~6 s, each on its room's shot.
    frames = {'L2.Power.Foyer': 96, 'L2.Pwr.Foyer2': 112, 'L2.Power.Atrium': 146, 'L2.Pwr.Atrium2': 158,
              'L2.Pwr.Atrium3': 170, 'L2.Pwr.Conservatory': 180, 'L2.Power.Workshop': 196, 'L2.Pwr.Archive': 214,
              'L2.Pwr.Turbine1': 236, 'L2.Pwr.Turbine2': 246, 'L2.Pwr.Turbine3': 256, 'L2.Pwr.Elevator': 266,
              'L2.Power.All': 272}
    assert [c for _, c in CASCADE] == list(frames)
    for ch, fr in frames.items():
        s.event(fr, 'Lab.Signal', ch, 'true')

    s.audio(58, 120, 'World/Breaker_Throw.nmsound', volume=1.0)
    s.audio(80, 300, 'World/Power_Up_Sweep.nmsound', volume=1.0, blendOut=30)
    s.audio(230, 420, 'World/Turbine_SpinUp.nmsound', volume=0.6, blendOut=40)
    s.audio(290, 480, 'World/Power_Hum_Loop.nmsound', volume=0.5, blendIn=20, blendOut=30)
    s.subtitle(100, 220, 'Power is back. Look at that place light up.')
    s.subtitle(300, 390, 'The archive is through the workshop. Find a way in.')
    return s


def dress_l2(lv):
    drop(lv, r'(Calm|Alarm)_CineCam_C[346]_\w+_[AB]')
    bx, _, bz, _ = D2['points']['Breaker']
    add_mark(lv, 'Calm_CineMark_C3', (D2['rooms']['Foyer'][0] + L.WALL_THICKNESS + 120, 0, bz), 270)
    add_mark(lv, 'Calm_CineMark_C3_End', (D2['rooms']['Foyer'][0] + L.WALL_THICKNESS + 120, 0, bz), 0)
    add_mark(lv, 'Calm_CineMark_C4', (1950, 0, 1300), 90)
    add_mark(lv, 'Alarm_CineMark_C6', (450, L.L2['gantry']['steps'] * L.L2['gantry']['rise'], 600), 90)
    shaft(lv)
    airship_l2(lv)
    backdrop_l2(lv)


# Where L2 stands on L1's island: L2's origin is L1's (3700, 4500, 11000) -- the highland behind
# the cliff the blast door is cut into, the lab's floor just over its highest ground.
L2_ON_L1 = (3700, 4500, 11000)


def backdrop_l2(lv):
    """The island and the sea round the lab, for C6 on the roof (the alarm state only): L1's own
    terrain (its heightfield and Island material, no collision, no shadows) and its far sea,
    moved so the lab sits on the highland: south over the cliff to the plaza and the road, the
    hills north-east, the sea all round. Drawn, not walked."""
    import dress_l1
    ox, oy, oz = L2_ON_L1
    lv.entity('Alarm_CineBackdrop_Island', [
        lv.fields('Terrain', version=3, Terrain=dress_l1.TERRAIN_FILE, Material=dress_l1.ISLAND, Visible=True,
                  CastShadows=False, Occluder=False, Walkable=False, UVTileSize=100, LODDistance=0, Foliage='',
                  FoliageVisible=False),
        lv.transform((dress_l1.OX - ox, -oy, dress_l1.OZ - oz))], static=True)
    mesh(lv, 'Alarm_CineBackdrop_Sea', 'Content/Meshes/L1/Sea_Far.nmmesh', (-ox, dress_l1.SEA - oy, -oz), shadows=False)
    for i in (-1, 0, 1):
        for j in (-1, 0, 1):
            mesh(lv, f'Alarm_CineBackdrop_Sea_{i + 1}{j + 1}', 'Content/Meshes/L1/Sea_Plane.nmmesh',
                 (-6000 + i * 20000 - ox, dress_l1.SEA - oy, -2000 + j * 20000 - oz), shadows=False)


# =============================================================================
# C4 Elevator (L2 end): into the cage, down the shaft toward the core's glow
# =============================================================================
CAGE = (2450, 0, 1800)
SHAFT_DEPTH = 1700
CAGE_KEYS = [(0, 0), (130, 0, 'CubicIn'), (190, -150, 'Linear'), (430, -1550)]


def shaft(lv):
    """The well the cage drops into: the floor opened round the cage (the elevator room's four middle
    east floor tiles give way to a rim), dark riveted walls down, lamp rings, the core's blue below."""
    drop(lv, r'K_Elevator_Floor_[23]_[12]')
    cx, _, cz = CAGE
    hole = (cx - 215, cz - 215, cx + 215, cz + 215)
    gx0, gz0, gx1, gz1 = 2000, 1415, 2770, 2185        # the dropped tiles' square
    floor = 'Content/Meshes/L2/Floor.nmmesh'
    for k, (x0, z0, x1, z1) in enumerate(((gx0, gz0, hole[0], gz1), (hole[2], gz0, gx1, gz1),
                                          (hole[0], gz0, hole[2], hole[1]), (hole[0], hole[3], hole[2], gz1))):
        mesh(lv, f'Calm_CineShaft_Rim_{k}', floor, ((x0 + x1) / 2, 0, (z0 + z1) / 2), scale=((x1 - x0) / 400, 1, (z1 - z0) / 400),
             shadows=False)
        mesh(lv, f'Alarm_CineShaft_Rim_{k}', floor, ((x0 + x1) / 2, 0, (z0 + z1) / 2), scale=((x1 - x0) / 400, 1, (z1 - z0) / 400),
             shadows=False)
    w = hole[2] - hole[0]
    for k, (x, z, sx, sz) in enumerate(((hole[0] - 10, cz, 20, w + 40), (hole[2] + 10, cz, 20, w + 40),
                                        (cx, hole[1] - 10, w, 20), (cx, hole[3] + 10, w, 20))):
        mesh(lv, f'CineShaft_Wall_{k}', BOX, (x, -SHAFT_DEPTH / 2 - 2, z), scale=(sx, SHAFT_DEPTH, sz),
             material=MAT.format('SteelDark'))
        mesh(lv, f'CineShaft_Trim_{k}', BOX, (x, -2, z), scale=(sx + 6, 6, sz + 6), material=MAT.format('BrassAged'))
        for j, y in enumerate(range(-250, -SHAFT_DEPTH, -300)):
            mesh(lv, f'CineShaft_Band_{k}_{j}', BOX, (x, y, z), scale=(sx + 4, 14, sz + 4), material=MAT.format('BrassAged'),
                 shadows=False)
    # Lamp bands: a caged bulkhead on the west wall each 3 m, its light out from the wall (soft, dim:
    # C4's last shot looks down past them, and close lamps blow the walls out)
    for j, y in enumerate(range(-250, -SHAFT_DEPTH, -300)):
        z = cz - 120 if j % 2 else cz + 120
        mesh(lv, f'CineShaft_LampFixture_{j}', 'Content/Meshes/L3_Core/L3_Core_Bulkhead.nmmesh', (hole[0], y + 40, z),
             rot=(0, 270, 0), shadows=False)
        point(lv, f'CineShaft_Lamp_{j}', (hole[0] + 45, y + 40, z), (255, 196, 128), 4, 520, source=25)
    # the core's glow under the well: blue and held low (at the kit's 12 it filled C4's last shot)
    mesh(lv, 'CineShaft_Glow', BOX, (cx, -SHAFT_DEPTH - 5, cz), scale=(w, 10, w), material=MAT.format('LampGlow'), shadows=False,
         params=(('EmissiveColor', 0, (0.35, 0.7, 1.0)), ('EmissiveIntensity', 0, (0.6,))))
    point(lv, 'CineShaft_Core', (cx, -SHAFT_DEPTH + 150, cz), (120, 190, 255), 9, 1400, source=60)
    point(lv, 'Calm_CineCage_Under', (cx, -40, cz), (255, 190, 120), 8, 500, source=20)


def c4(lv):
    s = Seq('C4_Descent', 450)
    sh = Shots(lv, s, 'C4', 'Calm_')
    cx, _, cz = CAGE

    def cage_y(f):
        return sample(CAGE_KEYS, f)
    # 1: the player crosses the room to the cage
    sh.shot(0, 100, [(0, (1500, 250, 1150), (2000, 120, 1500)), (99, (1550, 260, 1250), (2350, 120, 1800))], fov=50)
    # 2: in the cage, the lamp over him, the cage lurches off
    sh.shot(100, 200, [(100, (cx - 520, 230, cz - 330), (cx, 120, cz)), (199, (cx - 470, 200, cz - 300), (cx, cage_y(199) + 90, cz))],
            fov=50)
    # 3: in the shaft, looking up as the cage comes down past the lamps
    sh.shot(200, 330, [(f, (cx + 190, -720 - (f - 200) * 0.6, cz + 190), (cx, cage_y(f) + 120, cz))
                       for f in range(200, 330, 32)] + [(329, (cx + 190, -797, cz + 190), (cx, cage_y(329) + 120, cz))], fov=70)
    # 4: down over the cage's roof into the blue glow; fade
    sh.shot(330, 450, [(330, (cx + 175, cage_y(330) + 560, cz + 175), (cx - 140, cage_y(330) - 900, cz - 140)),
                       (449, (cx + 170, cage_y(449) + 520, cz + 170), (cx - 140, cage_y(449) - 900, cz - 140))], fov=65)

    cage = s.bind('cage', lv.guid('Calm_ElevatorCage'), 'Calm_ElevatorCage')
    s.transform(cage, {'Position.Y': CAGE_KEYS})
    under = s.bind('under', lv.guid('Calm_CineCage_Under'), 'Calm_CineCage_Under')
    s.transform(under, {'Position.Y': [(f, v - 40, *i) for f, v, *i in CAGE_KEYS]})
    lamp = s.bind('lamp', lv.guid('Calm_ElevatorCage_Lamp'), 'Calm_ElevatorCage_Lamp')
    s.transform(lamp, {'Position.Y': [(f, v + 300, *i) for f, v, *i in CAGE_KEYS]})
    me = player(s, lv)
    s.transform(me, {'Position.X': [(0, 1950), (12, 1950, 'Linear'), (70, 2250, 'QuadOut'), (95, cx - 10)],
                     'Position.Z': [(0, 1300), (12, 1300, 'Linear'), (70, cz)],
                     'Position.Y': [(0, 0)] + [(f, v, *i) for f, v, *i in CAGE_KEYS[1:]],
                     'Rotation.Y': [(0, facing(32)), (60, facing(60)), (95, facing(90), 'CubicInOut'), (130, facing(270))]})
    s.anim(me, [(0, 100, 'FB_Walk', {'loop': True}), (92, 450, 'FB_Idle', {'loop': True, 'blendIn': 10})])

    s.fade([(0, 0), (405, 0), (450, 1)])
    # the last shot looks down past the lamp bands into the glow: the stack's -1.5 held lower there
    s.pp('Exposure', [(0, -1.5, 'Constant'), (330, -2.3, 'Constant')])
    s.audio(100, 170, 'World/Lift_Start.nmsound', volume=1.0)
    s.audio(130, 450, 'World/Lift_Run_Loop.nmsound', volume=1.0, blendIn=20)
    s.audio(250, 450, 'Ambience/Amb_L3_Reactor.nmsound', volume=0.8, blendIn=90)
    s.bus('Music', [(0, 1), (100, 1), (300, 0.3)])
    s.subtitle(15, 120, 'Going down. The core is under the turbine hall.')
    s.subtitle(300, 400, 'Hear that hum? Whatever they keep down there is running hot.')
    return s


# =============================================================================
# C5 Theft (L3 vault): the intel lifted, the alarm, the run to the locker
# =============================================================================
D3 = L.L3
LOCKER_STAND = (-560, 0, -1395)


def c5(lv):
    s = Seq('C5_Theft', 540)
    sh = Shots(lv, s, 'C5')
    # 1: the vault from inside its door, the deposit banks round it; in on the plinth as the intel
    # lifts off its cradle into the player's reach
    sh.shot(0, 70, [(0, (-330, 250, -480), (0, 150, 0)), (69, (-150, 185, -250), (0, 150, -20))], fov=50)
    # 2: the helix's foot from the south landing, square on to its shutter: it drops in its housing
    # (L3.Alarm at 72, down in ~1.3 s) across the way back up, the reactor going red over it
    hsx, hsz = 271, -726
    sh.shot(70, 140, [(70, (-430, 250, -1090), (hsx + 30, 360, hsz + 40)), (139, (-400, 235, -1060), (hsx + 30, 330, hsz + 40))],
            fov=60)
    # 3: the shaft wide from the gallery's south side: the reactor red, klaxons sweeping, the vents blow
    sh.shot(140, 200, [(140, (300, 260, -1650), (0, 1100, 600)), (199, (250, 240, -1600), (0, 1000, 600))], fov=65)
    # 4: a vent beside, steam jetting in the red
    vx, vz = 1800 * math.sin(math.radians(65)), 1800 * math.cos(math.radians(65))
    sh.shot(200, 260, [(200, (vx - 700, 160, vz - 650), (vx, 90, vz)), (259, (vx - 600, 150, vz - 560), (vx, 80, vz))], fov=50)
    # 4: the player runs out of the vault and over the bridge
    sh.shot(260, 400, [(260, (300, 220, -1250), (0, 120, -300)), (399, (-150, 230, -1750), (-520, 110, -1420))], fov=50)
    # 6: over the shoulder at the locker (Blend Out into the game)
    lx, _, lz = LOCKER_STAND
    sh.shot(400, 540, [(400, (lx + 230, 210, lz + 300), (lx, 120, lz - 150)), (539, (lx + 40, 200, lz + 330), (lx, 130, lz - 300))],
            fov=50)

    intel = s.bind('intel', lv.guid('CineIntel'), 'CineIntel')
    s.transform(intel, {'Position.Y': [(0, 120), (20, 120, 'QuadOut'), (60, 175)],
                        'Position.Z': [(0, 0), (20, 0, 'QuadOut'), (60, -45)]})
    s.bool(intel, 'Render', 'Visible', [(0, True), (70, False)])

    me = player(s, lv)
    route = [(0, (0, -105), 0), (270, (0, -105), 0), (300, (0, -450), 180), (330, (0, -820), 180),
             (380, (lx, lz), 223), (400, (lx, lz), 180)]
    s.transform(me, {'Position.X': [(0, 0), (270, 0, 'QuadIn'), (330, 0, 'Linear'), (380, lx, 'QuadOut'), (400, lx)],
                     'Position.Z': [(0, -105), (270, -105, 'QuadIn'), (330, -820, 'Linear'), (380, lz, 'QuadOut'), (400, lz)],
                     'Rotation.Y': [(0, facing(0)), (255, facing(0), 'CubicInOut'), (280, facing(180)), (330, facing(180)),
                                    (345, facing(223)), (380, facing(223), 'CubicInOut'), (400, facing(180))]})
    del route
    s.anim(me, [(0, 275, 'FB_Idle', {'loop': True}), (268, 385, 'FB_Run', {'loop': True, 'blendIn': 6}),
                (378, 540, 'FB_Idle', {'loop': True, 'blendIn': 10})])

    s.audio(0, 40, 'UI/UI_Confirm.nmsound', volume=0.6)
    s.audio(68, 150, 'World/Shutter_Slam.nmsound', volume=1.0)
    s.audio(74, 150, 'World/Steam_Vent_Burst.nmsound', volume=0.9)
    s.audio(200, 270, 'World/Steam_Vent_Burst.nmsound', volume=0.8)
    s.audio(70, 200, 'Robots/Robot_Death.nmsound', volume=0.5, rate=0.6)
    s.subtitle(8, 66, 'You have it. Now get out of there.')
    s.subtitle(180, 255, 'They know. Security is on its way down.')
    s.subtitle(410, 530, 'Alarm! There is a locker by the bridge. Arm yourself.')
    s.event(72, 'Lab.Signal', 'L3.Alarm', 'true')
    s.event(534, 'Lab.PlacePlayer', 'CineMark_C5_End')
    s.event(538, 'Lab.Signal', 'Checkpoint.Theft', 'true')
    return s


def dress_l3(lv):
    drop(lv, r'CineCam_C5_\w+_[AB]')
    add_mark(lv, 'CineMark_C5', (0, 0, -105), 0)
    add_mark(lv, 'CineMark_C5_End', LOCKER_STAND, 180)
    mesh(lv, 'CineIntel', 'Content/Meshes/L3/Vault_Intel.nmmesh', (0, 120, 0), visible=False)
    point(lv, 'CineIntel_Glow', (0, 40, 0), (90, 255, 140), 6, 300, parent='CineIntel', source=10, toggle='L3.Alarm',
          show_when_on=False)


# =============================================================================
# C6 Extraction (L2 on alarm): the roof hatch, the airship, the winch, away at dusk
# =============================================================================
HATCH = L.L2['points']['RoofHatch']
DECK = L.L2['gantry']['steps'] * L.L2['gantry']['rise']           # the gantry platform's top
LINE = (-450, -300, 0)                                            # the winch line's top, from the hull (assets.json)
HOVER = (HATCH[0] - LINE[0], 2700, HATCH[2])                       # the rig over the hatch: its line hangs straight down
SHIP_C6 = {
    'Position.X': [(0, HOVER[0] - 9000, 'CubicOut'), (170, HOVER[0]), (400, HOVER[0], 'CubicIn'), (660, HOVER[0] + 11000)],
    'Position.Y': [(0, HOVER[1] + 1400, 'CubicOut'), (170, HOVER[1]), (400, HOVER[1], 'QuadIn'), (660, HOVER[1] + 3200)],
    'Position.Z': [(0, HOVER[2] - 2500, 'CubicOut'), (170, HOVER[2]), (400, HOVER[2], 'QuadIn'), (660, HOVER[2] + 4000)],
    'Rotation.Y': [(0, 12, 'CubicOut'), (170, 0), (400, 0, 'QuadIn'), (660, -18)],
    'Rotation.Z': [(0, 4), (170, -3), (230, 0), (400, 0), (520, 4), (660, 2)],
}
LIFT = [(0, DECK), (250, DECK, 'QuadInOut'), (390, HOVER[1] + LINE[1] - 190)]   # the player's feet, winched up


def ship_c6(f, up=450):
    return (sample(SHIP_C6['Position.X'], f), sample(SHIP_C6['Position.Y'], f) + up, sample(SHIP_C6['Position.Z'], f))


def airship_l2(lv):
    """C6's airship: a rig parked out of sight below the world, the hull, envelope and propellers on it,
    and the winch line under the gondola (scaled out by the sequence)."""
    lv.entity('Alarm_CineAirship', [lv.transform((0, -60000, 0))])
    hull = mesh(lv, 'Alarm_CineAirship_Hull', AIRSHIP + 'Airship_Hull.nmmesh', (0, 0, 0), parent='Alarm_CineAirship')
    mesh(lv, 'Alarm_CineAirship_Envelope', AIRSHIP + 'Airship_Envelope.nmmesh', (0, 0, 0), parent=hull)
    for part, off in (('PropLeft', (-195, 220, -400)), ('PropRight', (-195, 220, 400)), ('PropTail', (-1860, 900, 0))):
        mesh(lv, f'Alarm_CineAirship_{part}', AIRSHIP + f'Airship_{part}.nmmesh', off, parent=hull)
    # Its own lamps glow, no more (they lit the whole envelope before, a lantern in the dusk); the
    # dusk does the lighting: a warm rose key from low in the west-south-west (where the cameras
    # stand), a cool sky fill from overhead and an orange rim from the afterglow behind it, all
    # riding on the hull.
    point(lv, 'Alarm_CineAirship_Lamp', (0, -60, 0), (255, 178, 105), 10, 1600, parent=hull, source=40)
    point(lv, 'Alarm_CineAirship_Belly', (300, 280, 0), (255, 196, 140), 6, 1400, parent=hull, source=60)
    point(lv, 'Alarm_CineAirship_Nose', (1300, 900, 0), (255, 170, 110), 4, 1200, parent=hull, source=60)
    # (kept soft: the alarm's volumetric fog draws a spot's cone, and the envelope's canvas takes light easily)
    for name, pos, color, intensity, cone in (('Key', (-2600, -1400, -4200), (255, 176, 140), 14, (28, 42)),
                                              ('Fill', (-600, 5200, -1200), (150, 165, 230), 9, (40, 60)),
                                              ('Rim', (2800, 2200, 4400), (255, 140, 70), 26, (24, 38))):
        p, y = look(pos, (-200, 700, 0))
        lv.entity(f'Alarm_CineAirship_{name}', [
            lv.fields('Spot Light', version=2, Color=color, Intensity=intensity, Range=9000, InnerConeAngle=cone[0],
                      OuterConeAngle=cone[1], Active=True, CastShadows=False, ShadowTechnique=1, ShadowBias=0.002,
                      ShadowNormalBias=1.5, SourceRadius=200),
            lv.transform(pos, (p, y, 0))], hull)
    point(lv, 'Alarm_CineAirship_Winch', (LINE[0], LINE[1] - 40, 0), (255, 200, 150), 6, 600, parent=hull)
    mesh(lv, 'Alarm_CineAirship_Line', CYL, (LINE[0], LINE[1], 0), scale=(3, 1, 3), parent=hull,
         material=MAT.format('Leather'), shadows=False)


def c6(lv):
    s = Seq('C6_Extraction', 660)
    sh = Shots(lv, s, 'C6', 'Alarm_')
    hx, hz = HATCH[0], HATCH[2]
    feet = lambda f: sample(LIFT, f)  # noqa: E731
    # 1: on the gantry: the hatch slides open on the evening sky
    sh.shot(0, 90, [(0, (hx - 300, DECK + 160, hz - 240), (hx, DECK + 130, hz)),
                    (89, (hx - 270, DECK + 140, hz - 215), (hx + 30, DECK + 230, hz + 20))], fov=45)
    # 2: up through the dome: the airship sweeps in and stops over the hatch
    sh.shot(90, 185, [(90, (-300, 150, -500), ship_c6(110)), (184, (-260, 140, -450), ship_c6(184))], fov=60)
    # 3: just over the dome: the line drops to the hatch
    sh.shot(185, 255, [(185, (hx - 1500, 1500, hz - 1200), (hx, 1750, hz)),
                       (254, (hx - 1450, 1480, hz - 1150), (hx, 1450, hz))], fov=45)
    # 4: the winch: alongside the player rising to the gondola
    sh.shot(255, 400, [(255, (hx - 1150, 1450, hz - 900), (hx, feet(255) + 250, hz)),
                       (399, (hx - 1000, 2250, hz - 800), (hx + 100, feet(399) + 250, hz))], fov=45)
    # 5: from the dome's crown: the airship turns away, climbing into the dusk
    sh.shot(400, 540, [(400, (-200, 1330, -300), ship_c6(400)), (539, (-150, 1330, -250), ship_c6(539))], fov=55)
    # 6: wide and low: away over the island; the music swells; fade to the credits
    # (high over the lab's west side, looking east over the highland to the sea: L2's island backdrop)
    sh.shot(540, 660, [(540, (-2200, 2700, -2400), add(ship_c6(540), (0, -2300, 0))),
                       (659, (-2000, 2750, -2300), add(ship_c6(659), (0, -2600, 0)))], fov=45)

    rig = s.bind('ship', lv.guid('Alarm_CineAirship'), 'Alarm_CineAirship')
    s.transform(rig, SHIP_C6)
    s.sound_at(rig, [(0, 660, 'World/Airship_Engine_Loop.nmsound', {'volume': 1.0, 'spatial': '3D'})])
    for k, part in enumerate(('PropLeft', 'PropRight', 'PropTail')):
        b = s.bind(f'prop{k}', lv.guid(f'Alarm_CineAirship_{part}'), f'Alarm_CineAirship_{part}')
        s.transform(b, {'Rotation.X': [(0, 0, 'Linear'), (660, 660 / 30 * 8 * 360 * (1 if k != 1 else -1))]})
    # the line: out from the winch to the hatch, then wound in with the player
    top = HOVER[1] + LINE[1]
    length = [(0, 1), (175, 1, 'QuadOut'), (230, top - DECK - 190)] + \
             [(f, top - feet(f) - 190) for f in (260, 290, 320, 350, 380)] + [(392, 1)]
    line = s.bind('line', lv.guid('Alarm_CineAirship_Line'), 'Alarm_CineAirship_Line')
    s.transform(line, {'Scale.Y': length, 'Position.Y': [(k[0], LINE[1] - k[1] / 2) + tuple(k[2:]) for k in length]})
    hatch = s.bind('hatch', lv.guid('Alarm_RoofHatch'), 'Alarm_RoofHatch')
    s.transform(hatch, {'Position.X': [(0, hx), (20, hx, 'CubicInOut'), (55, hx + 175)]})

    me = player(s, lv)
    s.transform(me, {'Position.X': [(0, hx), (660, hx)], 'Position.Z': [(0, hz), (660, hz)], 'Position.Y': LIFT,
                     'Rotation.Y': [(0, facing(90)), (660, facing(90))]})
    s.anim(me, [(0, 235, 'FB_Idle', {'loop': True}), (230, 400, 'FB_Fall', {'loop': True, 'blendIn': 8})])
    s.bool(me, 'Render', 'Visible', [(0, True), (392, False)])

    s.fade([(0, 0), (615, 0), (660, 1)])
    s.pp('Exposure', [(0, -1.2, 'Constant'), (185, 0.4, 'Constant'), (400, 0.2)])     # the stack's, then the sky's
    s.audio(40, 660, MUSIC + 'Music_Extraction.nmsound', volume=1.0, blendIn=30)
    s.audio(0, 70, 'World/Steam_Vent_Burst.nmsound', volume=0.7)
    s.audio(175, 260, 'World/Lift_Start.nmsound', volume=0.8)
    s.audio(250, 400, 'World/Lift_Run_Loop.nmsound', volume=0.7, rate=1.3, blendOut=10)
    s.bus('Ambient', [(0, 1), (200, 0.5), (500, 0.15)])
    s.bus('SFX', [(0, 1), (420, 1), (640, 0.4)])
    s.bus('Music', [(0, 0.9), (420, 1.0), (560, 1.35)])
    s.subtitle(130, 230, 'Line is down. Clip on, we are leaving!')
    s.subtitle(300, 390, 'Hold on. Winching you up.')
    s.subtitle(420, 530, 'Intel aboard. Taking us home over the island.')
    s.event(659, 'Lab.Signal', 'Game.Complete', 'true')
    return s


# =============================================================================
# The hook
# =============================================================================
def build(lv):
    """make_levels.py: before a level is written, its cinematic entities and its sequences."""
    name = lv.name
    if name == 'L1_Foothills':
        dress_l1(lv)
        seqs = [c1(lv), c2(lv)]
    elif name == 'L2_Laboratory':
        dress_l2(lv)
        seqs = [c3(lv), c4(lv), c6(lv)]
    elif name == 'L3_Core':
        dress_l3(lv)
        seqs = [c5(lv)]
    else:
        return
    for s in seqs:
        s.write(CINE)
        print(f'  {s.name}: {s.length / 30.0:.1f} s')


if __name__ == '__main__':
    import make_levels
    make_levels.make_menu()
    make_levels.make_l1()
    make_levels.make_l2()
    make_levels.make_l3()
