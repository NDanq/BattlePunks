"""
L3_Core's dressing (milestone M5), called by make_levels.py's make_l3 just before it writes the
world: the grey box becomes the reactor shaft.

    elevator (high, NW) -> bridge -> helix catwalk spiralling clockwise round the reactor, over
    the pit -> south bridge -> vault (in the drum the reactor stands on) -> gallery floor (W1)
    -> tunnel -> pump hall (W2, beacon) -> freight lift

What it does to the grey box:
  - its walls and floors stay as the collision, hidden; the visible level is the kit
    (Content/Meshes/L3/*) and L3_Core's own meshes (Content/Meshes/L3_Core/*, written by
    Tools/Art/blender/make_l3_core.py from the same layout.py numbers);
  - the ring gallery's floor, walls and ceiling and the elevator room are rebuilt for the
    shaft: 28 m walls, the arrival 8 m up, the pit round the drum (fenced), the helix's
    collider, the bridges;
  - gameplay entities keep their names and fields; some get a kit mesh (the intel, the gates'
    shutters, the ammo boxes) or none (the locker's and the beacon's buttons, behind kit props);
  - lights, the reactor's motion, the alarm's red, klaxons, vents, post process per state, GI.
State follows the level's signals: L3.Alarm (C5), L3.Armed, L3.Intel, L3.BeaconCharging,
L3.BeaconFired, through LabSignalToggle and LabMotion (Source/World/LabMotion.h).
"""
import math
import os
import re

import layout as L

KIT = 'Content/Meshes/L3/'
L2KIT = 'Content/Meshes/L2/'
OWN = 'Content/Meshes/L3_Core/'
PART = 'Content/Particles/'
PP = 'Content/PostProcess/'
BOX = 'Engine/Content/Meshes/Box.nmmesh'
SND = 'Content/Sounds/'

D = L.L3
ARRIVE = D['arrival_y']
SP = D['spiral']
PIT = D['pit']
RING = D['ring']
DRUM_TOP = D['drum']['height'] + 42        # the drum's roof (make_l3_core.py: H + 0.42 m)

CALM = (255, 238, 218)
RED = (255, 52, 34)


def f(x):
    return f'{x:g}'


def compass(a, r):
    return r * math.sin(math.radians(a)), r * math.cos(math.radians(a))


# =============================================================================
# Entity text surgery (make_levels keeps each entity as its file text)
# =============================================================================
NAME_RE = re.compile(r'<Entity Guid="[0-9a-f]+" Name="([^"]+)"')


def name_of(text):
    m = NAME_RE.search(text)
    return m.group(1) if m else ''


def drop(lv, pred):
    keep = []
    for g, text in lv.entities:
        n = name_of(text)
        if pred(n):
            lv.names.discard(n)
        else:
            keep.append((g, text))
    lv.entities = keep


def edit(lv, pred, fn):
    lv.entities = [(g, fn(text) if pred(name_of(text)) else text) for g, text in lv.entities]


def hide_greybox(lv):
    """Every static grey-box primitive (walls, floors, ceilings, pedestals): kept as collision,
    not drawn."""
    def fn(text):
        if ('Engine/Content/Meshes/Box.nmmesh' in text or 'Engine/Content/Meshes/Cylinder.nmmesh' in text) \
                and '<Motion Value="2" />' in text and 'LabInteractable' not in text:
            text = text.replace('<Visible Value="true" />', '<Visible Value="false" />')
            text = text.replace('<CastShadows Value="true" />', '<CastShadows Value="false" />')
        return text
    lv.entities = [(g, fn(t)) for g, t in lv.entities]


def set_pos(text, pos=None, rot=None, scale=None):
    if pos is not None:
        text = re.sub(r'<Position X="[^"]*" Y="[^"]*" Z="[^"]*" />',
                      f'<Position X="{f(pos[0])}" Y="{f(pos[1])}" Z="{f(pos[2])}" />', text, count=1)
    if rot is not None:
        text = re.sub(r'<Rotation X="[^"]*" Y="[^"]*" Z="[^"]*" />',
                      f'<Rotation X="{f(rot[0])}" Y="{f(rot[1])}" Z="{f(rot[2])}" />', text, count=1)
    if scale is not None:
        text = re.sub(r'<Scale X="[^"]*" Y="[^"]*" Z="[^"]*" />',
                      f'<Scale X="{f(scale[0])}" Y="{f(scale[1])}" Z="{f(scale[2])}" />', text, count=1)
    return text


def restyle(lv, name, mesh=None, pos=None, rot=None, scale=(1, 1, 1), hidden=False, box_shape=False):
    """A gameplay entity's look: its Render's mesh (kit, at scale 1), or none drawn."""
    def fn(text):
        if mesh:
            text = re.sub(r'<Mesh Value="[^"]*" />', f'<Mesh Value="{mesh}" />', text, count=1)
        if hidden:
            text = text.replace('<Visible Value="true" />', '<Visible Value="false" />')
        if box_shape:   # a kit mesh's rigid body: a box fitted to the mesh's bounds
            text = text.replace('<Shape Value="0" />\n    <ShapeSource Value="0" />',
                                '<Shape Value="1" />\n    <ShapeSource Value="2" />')
        return set_pos(text, pos, rot, scale if (mesh or scale != (1, 1, 1)) else None)
    edit(lv, lambda n: n == name, fn)


def shift_y(lv, pred, dy):
    def fn(text):
        m = re.search(r'<Position X="([^"]*)" Y="([^"]*)" Z="([^"]*)" />', text)
        return set_pos(text, (float(m.group(1)), float(m.group(2)) + dy, float(m.group(3))))
    edit(lv, pred, fn)


# =============================================================================
# Components
# =============================================================================
def render(mesh, visible=True, shadows=True, params=(), material=None):
    out = ['  <Component Type="Render" Version="7">', f'    <Visible Value="{"true" if visible else "false"}" />']
    if material:
        out.append(f'    <Material0 Value="{material}" />')
    out.append(f'    <Mesh Value="{mesh}" />')
    out.append(f'    <ParameterCount Value="{len(params)}" />')
    for i, (pname, slot, v) in enumerate(params):
        v = tuple(v) + (0,) * (4 - len(v))
        out += [f'    <Parameter{i}Name Value="{pname}" />', f'    <Parameter{i}Slot Value="{slot}" />',
                f'    <Parameter{i}X Value="{f(v[0])}" />', f'    <Parameter{i}Y Value="{f(v[1])}" />',
                f'    <Parameter{i}Z Value="{f(v[2])}" />', f'    <Parameter{i}W Value="{f(v[3])}" />']
    return out + [f'    <CastShadows Value="{"true" if shadows else "false"}" />', '  </Component>']


def body(shape=0, source=0, motion=2):
    return ['  <Component Type="Rigid Body" Version="1">', '    <Enabled Value="true" />', f'    <Motion Value="{motion}" />',
            f'    <Shape Value="{shape}" />', f'    <ShapeSource Value="{source}" />', '    <Friction Value="0.6" />',
            '    <Layer Value="1" />', '    <Mask Value="-1" />', '  </Component>']


def xform(pos, rot=(0, 0, 0), scale=(1, 1, 1)):
    return ['  <Component Type="Transform" Version="1">',
            f'    <Position X="{f(pos[0])}" Y="{f(pos[1])}" Z="{f(pos[2])}" />',
            f'    <Rotation X="{f(rot[0])}" Y="{f(rot[1])}" Z="{f(rot[2])}" />',
            f'    <Scale X="{f(scale[0])}" Y="{f(scale[1])}" Z="{f(scale[2])}" />', '  </Component>']


def toggle(lv, channel, show_when_on=True):
    return lv.fields('LabSignalToggle', Channel=channel, ShowWhenOn=show_when_on)


def motion(lv, spin=(0, 0, 0), spin_on='', alarm_on='', alarm_scale=1.0, flicker=0.0, duty=0.5, ramp_on='',
           ramp_seconds=10.0, ramp_param='', ramp_slot=0, ramp_from=0.0, ramp_to=1.0, ramp_light=0.0):
    return lv.fields('LabMotion', Spin=tuple(float(x) for x in spin), SpinOn=spin_on, AlarmOn=alarm_on,
                     AlarmScale=alarm_scale, Flicker=flicker, FlickerDuty=duty, RampOn=ramp_on,
                     RampSeconds=ramp_seconds, RampParam=ramp_param, RampSlot=ramp_slot, RampFrom=ramp_from,
                     RampTo=ramp_to, RampLight=ramp_light)


def point(lv, color, intensity, radius, shadows=False):
    return lv.fields('Point Light', version=2, Color=color, Intensity=intensity, Radius=radius, Active=True,
                     CastShadows=shadows, ShadowTechnique=0, SourceRadius=min(60, radius / 20))


def spot(lv, color, intensity, rng, inner, outer, shadows=False):
    return lv.fields('Spot Light', version=2, Color=color, Intensity=intensity, Range=rng, InnerConeAngle=inner,
                     OuterConeAngle=outer, Active=True, CastShadows=shadows, ShadowTechnique=1, ShadowBias=0.002,
                     ShadowNormalBias=1.5, SourceRadius=10)


def particles(lv, system, emitting=True, tint=(255, 255, 255), rate=1.0, target=None):
    out = ['  <Component Type="Particle System" Version="3">', f'    <System Value="{system}" />',
           f'    <Emitting Value="{"true" if emitting else "false"}" />',
           f'    <Tint R="{tint[0]}" G="{tint[1]}" B="{tint[2]}" A="255" />', f'    <SpawnRateScale Value="{rate}" />',
           '    <TimeScale Value="1" />', '    <Seed Value="0" />',
           f'    <UseBeamTarget Value="{"true" if target else "false"}" />']
    t = target or (0, 0, 0)
    return out + [f'    <BeamTarget X="{f(t[0])}" Y="{f(t[1])}" Z="{f(t[2])}" />', '  </Component>']


def mesh(lv, name, path, pos, yaw=0.0, rot=None, collide=None, shadows=True, params=(), extra=(), static=True,
         visible=True, scale=(1, 1, 1), parent=None, material=None):
    """A kit mesh. collide: None, 'box' (fitted to its bounds) or 'mesh' (its triangles)."""
    comps = [render(path, visible, shadows, params, material), xform(pos, rot if rot else (0, yaw, 0), scale)]
    if collide == 'box':
        comps.insert(1, body(1, 2))
    elif collide == 'mesh':
        comps.insert(1, body(0, 0))
    comps += list(extra)
    return lv.entity(name, comps, parent, static=static and not extra)


def collider(lv, name, centre, size, yaw=0.0):
    """An invisible static box."""
    return lv.entity(name, [render(BOX, False, False), body(0, 0), xform(centre, (0, yaw, 0), size)], static=True)


def light(lv, name, pos, color, intensity, radius, channel=None, show_when_on=True, flick=None, rot=None,
          spot_cone=None, rng=None, shadows=False, extra=()):
    comps = [spot(lv, color, intensity, rng or radius, spot_cone[0], spot_cone[1], shadows) if spot_cone
             else point(lv, color, intensity, radius, shadows), xform(pos, rot or (0, 0, 0))]
    if channel:
        comps.append(toggle(lv, channel, show_when_on))
    if flick:
        comps.append(motion(lv, flicker=flick[0], duty=flick[1]))
    comps += list(extra)
    return lv.entity(name, comps)


# =============================================================================
# The dressing
# =============================================================================
def dress(lv):
    greybox(lv)
    shell(lv)
    helix(lv)
    reactor(lv)
    vault(lv)
    gallery(lv)
    alarm(lv)
    tunnel_and_pumps(lv)
    lighting(lv)
    state = os.environ.get('DEMOLAB_L3_STATE')
    if state and lv.name.startswith('Review_'):
        review_state(lv, state)


def greybox(lv):
    """Reshape the grey box for the shaft; what stays is hidden collision."""
    drop(lv, lambda n: n.startswith(('Ring_', 'Pump_', 'Light_', 'RedLight_')) or n in (
        'Reactor_Ring1', 'Beacon_Mast', 'Locker_Cabinet', 'Drum_Floor', 'NavMeshVolume', 'Shutter_Elevator', 'Ambient'))
    # the elevator room, up at the arrival
    shift_y(lv, lambda n: n.startswith('ElevatorRoom_'), ARRIVE)
    hide_greybox(lv)

    # the shaft's walls (28 m), the NW door at the arrival, the S door to the tunnel
    A = RING['apothem']
    T = L.WALL_THICKNESS
    side = 2 * (A + T) * math.tan(math.radians(22.5))
    H = RING['height']
    for label, a in lv.COMPASS.items():
        x, z = compass(a, A + T / 2)
        if label == 'NW':
            lv.wall(f'Shaft_Wall{label}_Lo', x, z, side, a, ARRIVE)
            lv.wall(f'Shaft_Wall{label}_Hi', x, z, side, a, H - ARRIVE, [(0, RING['doors']['NW'])], y0=ARRIVE)
        else:
            lv.wall(f'Shaft_Wall{label}', x, z, side, a, H, [(0, RING['doors'][label])] if label in RING['doors'] else [])
    # the gallery floor: eight bands round the pit (their union's inner edge is the pit's octagon)
    Pa = PIT['apothem']
    for k in range(8):
        a = 45 * k
        r0, r1 = Pa, A + T
        x, z = compass(a, (r0 + r1) / 2)
        lv.box(f'Shaft_Floor_{k}', (x, -10, z), (2 * r1 * math.tan(math.radians(22.5)), 20, r1 - r0), yaw=a)
    # the drum's island (four strips make its octagon) and the south bridge and landing over the pit
    Da = D['drum']['apothem'] + T
    for k in range(4):
        lv.box(f'Shaft_DrumFloor_{k}', (0, -10, 0), (2 * Da * math.tan(math.radians(22.5)), 20, 2 * Da), yaw=45 * k)
    bw = PIT['bridge']
    lv.box('Shaft_SouthBridge', (55, -10, -(Pa + Da) / 2), (bw + 110, 20, Pa - Da + 40))
    # the pit's fence, open at the bridge (and the landing east of it)
    fence_h = 130
    for label, a in lv.COMPASS.items():
        r = Pa + 15
        x, z = compass(a, r)
        side_p = 2 * r * math.tan(math.radians(22.5))
        if label == 'S':   # yaw 180: local X runs west, so the gap at x = +55 is at -55
            lv.wall('Shaft_Fence_S', x, z, side_p, a, fence_h, [(-55, bw + 110)], thick=10)
        else:
            lv.wall(f'Shaft_Fence_{label}', x, z, side_p, a, fence_h, thick=10)
    collider(lv, 'Shaft_BridgeRailW', (-bw / 2 - 8, fence_h / 2, -(Pa + Da) / 2), (10, fence_h, Pa - Da))
    collider(lv, 'Shaft_BridgeRailE', (bw / 2 + 118, fence_h / 2, -(Pa + Da) / 2 - 60), (10, fence_h, Pa - Da - 120))
    # the arrival bridge, from the elevator door in to the helix (a landing over the helix's band)
    a = SP['start'] - 6
    r_in, r_out = D['drum']['apothem'] + T + 20, A + T
    x, z = compass(315, (r_in + r_out) / 2)
    collider(lv, 'Shaft_ArrivalBridge', (x, ARRIVE - 10, z), (170, 20, r_out - r_in), yaw=315)
    for side_off, r0 in ((-1, r_in), (1, SP['r_out'])):    # the clockwise rail stops where the helix leaves
        cx, cz = compass(315, (r0 + r_out) / 2)
        ox, oz = math.cos(math.radians(315)) * 88 * side_off, -math.sin(math.radians(315)) * 88 * side_off
        collider(lv, f'Shaft_ArrivalRail_{side_off + 1}', (cx + ox, ARRIVE + fence_h / 2, cz + oz), (10, fence_h, r_out - r0),
                 yaw=315)
    # hide every new collision box
    hide_greybox(lv)
    # the helix's walkable stretch: its own triangle collider
    lv.entity('Shaft_HelixCollider', [render(OWN + 'L3_Core_HelixCollider.nmmesh', False, False), body(0, 0),
                                       xform((0, 0, 0))], static=True)
    lv.nav_volume((1200, 500, -2300), (3200, 700, 4300))


def shell(lv):
    for nm in ('Shaft', 'Pit', 'Drum', 'Tunnel', 'PumpHall', 'FreightShaft', 'ElevatorRoom'):
        floor_tint = (('Tint', 0, (0.5, 0.5, 0.52)),) if nm in ('Pit', 'Tunnel', 'PumpHall', 'FreightShaft') else ()
        mesh(lv, f'Dress_{nm}', OWN + f'L3_Core_{nm}.nmmesh', (0, 0, 0), params=floor_tint)
    # the lift cage the player arrives in (L2's kit), its gate open toward the shaft
    e = D['elevator']
    ex, ez = compass(e['angle'], e['distance'])
    mesh(lv, 'Dress_ElevatorCage', L2KIT + 'ElevatorCage.nmmesh', (ex, ARRIVE, ez), yaw=e['angle'] + 180 - 180)


def helix(lv):
    """The helix of catwalk pieces round the reactor: up from the arrival to the vent stack,
    down from the south landing into the pit; the walkable stretch between (its collider is
    greybox()'s)."""
    drop_ = SP['drop']
    for k in range(-24, SP['walk'] + 18):
        if k in (-1, -2):      # the arrival bridge crosses here
            continue
        y = ARRIVE - drop_ * k
        if y > RING['height'] - 200 or y < -PIT['depth'] + 120:
            continue
        yaw = (SP['start'] + 22.5 * k) % 360
        mesh(lv, f'Dress_Helix_{k + 30}', OWN + 'L3_Core_Helix.nmmesh', (0, y, 0), yaw=yaw)
    # the arrival bridge: kit catwalk and railings on the 315-degree radial
    A = RING['apothem']
    r0, r1 = D['drum']['apothem'] + L.WALL_THICKNESS + 20, A + 10
    yaw = 135      # the kit runs +Z: from the door inward
    sx, sz = compass(315, r1)
    length = r1 - r0
    n = int(math.ceil(length / 400))
    for i in range(n):
        d = min(i * 400, length - 400)
        x, z = sx + math.sin(math.radians(yaw)) * d, sz + math.cos(math.radians(yaw)) * d
        mesh(lv, f'Dress_Bridge_{i}', KIT + 'Catwalk_Straight.nmmesh', (x, ARRIVE, z), yaw=yaw)
        for side in (-1, 1):
            if side == -1 and d + 400 > length - (SP['r_out'] - r0):   # -1: the clockwise side here
                continue
            ox, oz = math.cos(math.radians(yaw)) * 75 * side, -math.sin(math.radians(yaw)) * 75 * side
            mesh(lv, f'Dress_BridgeRail_{i}_{side}', KIT + 'Railing_Straight.nmmesh', (x + ox, ARRIVE, z + oz), yaw=yaw)
    # struts under the bridge down to the gallery floor
    for d in (150, 550):
        x, z = sx + math.sin(math.radians(yaw)) * d, sz + math.cos(math.radians(yaw)) * d
        for side in (-1, 1):
            ox, oz = math.cos(math.radians(yaw)) * 70 * side, -math.sin(math.radians(yaw)) * 70 * side
            mesh(lv, f'Dress_BridgeStrut_{d}_{side}', BOX, (x + ox, ARRIVE / 2 - 15, z + oz), yaw=yaw,
                 scale=(16, ARRIVE - 30, 16), collide='box', material='Content/Materials/Kit/SteelDark.material.nmgraph')
    # the south bridge over the pit (and the helix's landing)
    Pa, Da = PIT['apothem'], D['drum']['apothem'] + L.WALL_THICKNESS
    for x in (-75, 75, 185):
        mesh(lv, f'Dress_SouthBridge_{x}', KIT + 'Catwalk_Straight.nmmesh', (x, 0, -Pa - 10), yaw=0,
             scale=(1, 1, (Pa - Da + 20) / 400))
    mesh(lv, 'Dress_SouthRailW', KIT + 'Railing_Straight.nmmesh', (-152, 0, -Pa), yaw=0, scale=(1, 1, (Pa - Da) / 400))
    mesh(lv, 'Dress_SouthRailE', KIT + 'Railing_Straight.nmmesh', (262, 0, -Pa), yaw=0, scale=(1, 1, 120 / 400))


def reactor(lv):
    y0 = DRUM_TOP
    rings = (('A', 360, 18), ('B', 600, -13), ('C', 840, 24))
    for state, ch, show, color in (('Calm', 'L3.Alarm', False, None), ('Alarm', 'L3.Alarm', True, (1.0, 0.1, 0.04))):
        params_core = (('EmissiveColor', 7, color), ('PulseSpeed', 7, (3.5,)), ('EmissiveIntensity', 7, (26,))) \
            if color else ()
        mesh(lv, f'Dress_Reactor_{state}', KIT + 'ReactorCore.nmmesh', (0, y0, 0), params=params_core,
             extra=[toggle(lv, ch, show)])
        for r, off, speed in rings:
            pr = (('EmissiveColor', 1, color), ('PulseSpeed', 1, (3.5,))) if color else ()
            mesh(lv, f'Dress_ReactorRing{r}_{state}', KIT + f'Reactor_Ring{r}.nmmesh', (0, y0 + off, 0), params=pr,
                 extra=[toggle(lv, ch, show), motion(lv, spin=(0, speed, 0), alarm_on='L3.Alarm', alarm_scale=3.0)])
    # arcs at the dome (the kit's), boosted, and long arcs from the core to terminals on the shaft wall
    lv.entity('Dress_ReactorArcs', [particles(lv, PART + 'ReactorArcs.nmparticle', rate=1.5), xform((0, y0 + 1300, 0)),
                                    motion(lv, flicker=9, duty=0.75)])
    A = RING['apothem']
    for i, (a, y) in enumerate(((60, 1500), (150, 1900), (240, 1600), (20, 2250), (195, 2350))):
        wx, wz = compass(a, A)
        yaw = (a + 180) % 360            # the electrode's arm runs -Z: out of the wall toward the axis
        mesh(lv, f'Dress_Electrode_{i}', OWN + 'L3_Core_Electrode.nmmesh', (wx, y, wz), yaw=a)
        bx, bz = compass(a, A - 135)
        cx, cz = compass(a, 200)
        lv.entity(f'Dress_Arc_{i}', [particles(lv, PART + 'L3_Core_Arc.nmparticle', target=(bx, y, bz)),
                                     xform((cx, y0 + 700 + (i % 3) * 180, cz)),
                                     motion(lv, flicker=5 + i, duty=0.35)])
        if i < 2:   # two flash lights; the others are the beams' own glow
            light(lv, f'Dress_ArcFlash_{i}', (bx * 0.97, y, bz * 0.97), (170, 210, 255), 30, 650, flick=(5 + i, 0.35))
    # coolant steam rising out of the pit round the drum
    for i, a in enumerate((30, 120, 210, 300)):
        x, z = compass(a, (PIT['apothem'] + D['drum']['apothem'] + 30) / 2)
        lv.entity(f'Dress_PitSteam_{i}', [particles(lv, PART + 'SteamVent.nmparticle', rate=1.4), xform((x, -900, z))])
    lv.entity('Dress_ShaftMotes', [particles(lv, PART + 'DustMotes.nmparticle'), xform((-900, 900, 900))])
    lv.entity('Dress_ShaftMotes_2', [particles(lv, PART + 'DustMotes.nmparticle'), xform((900, 400, -900))])


def vault(lv):
    """The plinth and the intel; the locker by the bridge."""
    for state, show, color in (('Calm', False, None), ('Alarm', True, (1.0, 0.1, 0.05))):
        mesh(lv, f'Dress_Plinth_{state}', KIT + 'VaultPlinth.nmmesh', (0, 0, 0),
             params=(('EmissiveColor', 3, color), ('PulseSpeed', 3, (4,))) if color else (),
             extra=[toggle(lv, 'L3.Alarm', show)])
    restyle(lv, 'Intel', mesh=KIT + 'Vault_Intel.nmmesh', pos=D['points']['Intel'][:3])
    light(lv, 'Dress_IntelGlow', (0, 175, 0), (90, 255, 140), 10, 450, channel='L3.Intel', show_when_on=False)
    light(lv, 'Dress_VaultCeiling', (0, 680, 0), (255, 222, 175), 70, 900, channel='L3.Alarm', show_when_on=False,
          rot=(90, 0, 0), spot_cone=(14, 38), rng=900)
    light(lv, 'Dress_VaultRed', (0, 600, 0), RED, 9, 900, channel='L3.Alarm', flick=(1.4, 0.6))
    # the locker (the sidearm), facing the gallery; its door opens once the pistol is taken
    lx, _, lz, _ = D['points']['Locker']
    mesh(lv, 'Dress_Locker', KIT + 'Locker.nmmesh', (lx, 0, lz - 30), yaw=180, collide='box')
    mesh(lv, 'Dress_LockerDoor', KIT + 'Locker_Door.nmmesh', (lx + 29, 0, lz - 30), yaw=180,
         extra=[toggle(lv, 'L3.Armed', False)])
    mesh(lv, 'Dress_LockerDoorOpen', KIT + 'Locker_Door.nmmesh', (lx + 29, 0, lz - 30), yaw=290,
         extra=[toggle(lv, 'L3.Armed', True)])
    restyle(lv, 'Locker', hidden=True)


COVER = [   # (mesh, x, z, yaw): the gallery floor (W1), the tunnel, the pump hall (W2)
    ('CrateLow', -1050, -1150, 35), ('CrateLow', 1050, -1150, -35), ('Crate', -1500, 250, 10),
    ('Crate', 1480, -300, -20), ('Crate', 1350, -420, 15), ('Console', -1250, 950, 225), ('PipeStack', 1150, 1050, 45),
    ('CrateLow', 380, -1480, 90), ('Console', -1500, -650, 280),
    ('CrateLow', -125, -2600, 90), ('CrateLow', 125, -3400, 90),
    ('CrateLow', 650, -5200, 10), ('Crate', 2350, -4700, 30), ('PipeStack', 300, -5650, 90), ('Crate', 3150, -4800, 0),
]


def gallery(lv):
    for i, (m, x, z, yaw) in enumerate(COVER):
        mesh(lv, f'Dress_Cover_{i}', OWN + f'L3_Core_{m}.nmmesh', (x, 0, z), yaw=yaw, collide='box')
    for name in ('AmmoCrate_Ring', 'AmmoCrate_Pump'):
        p = D['points'][name]
        restyle(lv, name, mesh=OWN + 'L3_Core_AmmoBox.nmmesh', pos=(p[0], 0, p[2]), box_shape=True)


def alarm(lv):
    """C5: shutters slam (the gates' kit shutters), vents blow open where W1 comes in, klaxons."""
    A = RING['apothem'] + L.WALL_THICKNESS / 2
    nx, nz = compass(315, A)
    lv.entity('Shutter_Elevator', [
        lv.fields('LabGate', OpenOn='L3.Alarm', OpenHeight=350, Speed=240, Invert=True, Open=True,
                  Sound=SND + 'World/Shutter_Slam.nmsound'),
        render(OWN + 'L3_Core_Shutter.nmmesh'), body(1, 2, motion=1), xform((nx, ARRIVE + 165, nz), (0, 315, 0))])
    # the way back up: a shutter drops across the helix's foot (and keeps the fight off the catwalk)
    a = SP['start'] + 22.5 * SP['walk'] - 360 - 4
    hx, hz = compass(a, (SP['r_in'] + SP['r_out']) / 2)
    lv.entity('Dress_HelixShutter', [
        lv.fields('LabGate', OpenOn='L3.Alarm', OpenHeight=380, Speed=300, Invert=True, Open=True,
                  Sound=SND + 'World/Shutter_Slam.nmsound'),
        render(OWN + 'L3_Core_Shutter.nmmesh'), body(1, 2, motion=1),
        xform((hx, 170, hz), (0, a - 90, 0), (0.62, 1.05, 1))])
    restyle(lv, 'TunnelGate', mesh=OWN + 'L3_Core_Shutter.nmmesh', rot=(0, 0, 0), box_shape=True)
    restyle(lv, 'FreightGate', mesh=OWN + 'L3_Core_Shutter.nmmesh', rot=(0, 90, 0), box_shape=True)
    # the vents: covers that slide up on the alarm, steam out, the Watchmen's spawns in front
    for i, a in enumerate(D['vents']):
        x, z = compass(a, RING['apothem'])
        mesh(lv, f'Dress_Vent_{i}', OWN + 'L3_Core_Vent.nmmesh', (x, 0, z), yaw=a)
        lv.entity(f'Dress_VentCover_{i}', [
            lv.fields('LabGate', OpenOn='L3.Alarm', OpenHeight=160, Speed=300, Invert=False, Open=False,
                      Sound=SND + 'World/Steam_Vent_Burst.nmsound'),
            render(OWN + 'L3_Core_VentCover.nmmesh'), xform((x, 0, z), (0, a, 0))])
        sx, sz = compass(a, RING['apothem'] - 40)
        lv.entity(f'Dress_VentSteam_{i}', [particles(lv, PART + 'SteamJet.nmparticle', emitting=False),
                                           xform((sx, 70, sz), (90, a + 180, 0)), toggle(lv, 'L3.Alarm')])
    # klaxons: red domes with a sweeping spot under each, on the alarm
    for i, (a, y) in enumerate(((0, 520), (90, 520), (180, 520), (270, 520), (135, 1250), (45, 1700), (225, 2100))):
        x, z = compass(a, RING['apothem'])
        mesh(lv, f'Dress_Klaxon_{i}', OWN + 'L3_Core_Klaxon.nmmesh', (x, y, z), yaw=a,
             params=(('EmissiveColor', 2, (1.0, 0.08, 0.03)), ('EmissiveIntensity', 2, (0.3,))),
             extra=[toggle(lv, 'L3.Alarm', False)])
        mesh(lv, f'Dress_KlaxonOn_{i}', OWN + 'L3_Core_Klaxon.nmmesh', (x, y, z), yaw=a,
             params=(('EmissiveColor', 2, (1.0, 0.08, 0.03)), ('EmissiveIntensity', 2, (40,)), ('PulseSpeed', 2, (6,))),
             extra=[toggle(lv, 'L3.Alarm', True)])
        lx, lz = compass(a, RING['apothem'] - 40)
        if i >= 4:      # the high klaxons glow; the four on the floor sweep a beam
            continue
        lv.entity(f'Dress_KlaxonBeam_{i}', [spot(lv, RED, 40, 2600, 8, 16), xform((lx, y, lz), (12, a, 0)),
                                            toggle(lv, 'L3.Alarm'), motion(lv, spin=(0, 200, 0), spin_on='L3.Alarm')])


def tunnel_and_pumps(lv):
    for i, (x, z) in enumerate(D['pumps']):
        mesh(lv, f'Dress_Pump_{i}', KIT + 'CoolantPump.nmmesh', (x, 0, z), yaw=180, collide='box')
        mesh(lv, f'Dress_PumpFan_{i}', KIT + 'CoolantPump_Fan.nmmesh', (x - 162, 120, z), yaw=180, static=False,
             extra=[motion(lv, spin=(420 + 60 * i, 0, 0), alarm_on='L3.BeaconCharging', alarm_scale=1.8)])
        lv.entity(f'Dress_PumpSteam_{i}', [particles(lv, PART + 'SteamJet.nmparticle'), xform((x + 120, 440, z + 150))])
    # the beacon: tripod behind its two consoles; the lamp charges over the 40 s, the reflector
    # sweeps when it fires
    bx, bz = 1425, -5250
    mesh(lv, 'Dress_Beacon', KIT + 'Beacon.nmmesh', (bx, 0, bz), collide='box',
         extra=[motion(lv, ramp_on='L3.BeaconCharging', ramp_seconds=40, ramp_param='EmissiveIntensity', ramp_slot=5,
                       ramp_from=0.4, ramp_to=60)])
    mesh(lv, 'Dress_BeaconReflector', KIT + 'Beacon_Reflector.nmmesh', (bx, 325, bz), static=False,
         extra=[motion(lv, spin=(0, 90, 0), spin_on='L3.BeaconCharging', alarm_on='L3.BeaconFired', alarm_scale=5)])
    lv.entity('Dress_BeaconGlow', [point(lv, (255, 120, 40), 1, 1400), xform((bx, 330, bz)),
                                   motion(lv, ramp_on='L3.BeaconCharging', ramp_seconds=40, ramp_light=70)])
    lv.entity('Dress_BeaconSweep', [spot(lv, (255, 150, 70), 140, 4000, 6, 12), xform((bx, 330, bz), (-8, 0, 0)),
                                    toggle(lv, 'L3.BeaconFired'), motion(lv, spin=(0, 450, 0))])
    for name in ('Beacon_Start', 'Beacon_Fire'):
        p = D['points'][name]
        mesh(lv, f'Dress_{name}_Console', OWN + 'L3_Core_Console.nmmesh', (p[0], 0, p[2] - 56), yaw=180, collide='box')
        restyle(lv, name, hidden=True)
    # the freight lift's platform, open side west to the hall
    fx0, fz0, fx1, fz1 = D['rooms']['FreightLift'][:4]
    mesh(lv, 'Dress_FreightLift', KIT + 'FreightLift.nmmesh', ((fx0 + fx1) / 2, 0, (fz0 + fz1) / 2), yaw=90)
    restyle(lv, 'FreightPanel', hidden=True)
    # pipe runs along the pump hall's north wall and the gauges on them
    x0, z0, x1, z1 = D['rooms']['PumpHall'][:4]
    for i, x in enumerate(range(int(x0) + 150, int(x1) - 100, 200)):
        if abs(x - 0) < 260:
            continue
        mesh(lv, f'Dress_HallPipe_{i}', L2KIT + 'Pipe_Straight.nmmesh', (x, 520, z1 - 60), shadows=False)
        mesh(lv, f'Dress_HallPipeLo_{i}', L2KIT + 'Pipe_Straight.nmmesh', (x, 470, z1 - 60), shadows=False)
        if i % 4 == 1:
            mesh(lv, f'Dress_HallGauge_{i}', L2KIT + 'Gauge.nmmesh', (x, 545, z1 - 74), yaw=0, shadows=False)


def lighting(lv):
    A = RING['apothem']
    # calm: cold work light; alarm: red
    for i, a in enumerate((22.5, 67.5, 112.5, 157.5, 202.5, 247.5, 292.5, 337.5)):
        x, z = compass(a, A - 25)
        yaw = a
        mesh(lv, f'Dress_Bulkhead_{i}', OWN + 'L3_Core_Bulkhead.nmmesh', (x, 360, z), yaw=yaw,
             params=(('EmissiveColor', 1, (0.75, 0.85, 1.0)), ('EmissiveIntensity', 1, (10,))),
             extra=[toggle(lv, 'L3.Alarm', False)])
        mesh(lv, f'Dress_BulkheadRed_{i}', OWN + 'L3_Core_Bulkhead.nmmesh', (x, 360, z), yaw=yaw,
             params=(('EmissiveColor', 1, (1.0, 0.1, 0.05)), ('EmissiveIntensity', 1, (14,))),
             extra=[toggle(lv, 'L3.Alarm', True)])
        lx, lz = compass(a, A - 90)
        if i % 2 == 0:   # every other lamp lights the floor; the rest are emissive only
            light(lv, f'Dress_Wash_{i}', (lx, 340, lz), CALM, 24, 1100, channel='L3.Alarm', show_when_on=False)
        if i % 2 == 0:   # emergency light: half the lamps, dim, one flickering
            light(lv, f'Dress_WashRed_{i}', (lx, 340, lz), RED, 5, 1100, channel='L3.Alarm',
                  flick=(1.2, 0.8) if i == 2 else None)
    # floodlights in the ceiling: cones down the shaft through the haze
    for i, a in enumerate((45, 135, 225, 315)):
        x, z = compass(a, 1250)
        mesh(lv, f'Dress_Flood_{i}', OWN + 'L3_Core_Floodlight.nmmesh', (x, RING['height'] - 60, z),
             params=(('EmissiveColor', 1, (0.8, 0.9, 1.0)), ('EmissiveIntensity', 1, (30,))), shadows=False)
        if i % 2:
            continue
        light(lv, f'Dress_FloodBeam_{i}', (x, RING['height'] - 100, z), (215, 230, 255), 110, 3400, channel='L3.Alarm',
              show_when_on=False, rot=(90, 0, 0), spot_cone=(10, 22), rng=3400, shadows=(i == 0))
    # the reactor's own light: blue, then red
    light(lv, 'Dress_CoreLight', (0, DRUM_TOP + 600, 0), (140, 205, 255), 90, 2400, channel='L3.Alarm',
          show_when_on=False)
    light(lv, 'Dress_CoreLightRed', (0, DRUM_TOP + 600, 0), (255, 50, 25), 45, 2400, channel='L3.Alarm',
          flick=(3, 0.85))
    light(lv, 'Dress_PoolLight', (0, -1250, 0), (90, 180, 255), 60, 1600)
    light(lv, 'Dress_ArrivalLamp', (-1350, ARRIVE + 300, 1350), (255, 214, 170), 18, 800)
    # the tunnel, the pump hall, the freight shaft
    for i, z in enumerate((-2200, -3000, -3800)):
        mesh(lv, f'Dress_TunnelLamp_{i}', OWN + 'L3_Core_Bulkhead.nmmesh', (0, 395, z), rot=(-90, 0, 0),
             params=(('EmissiveColor', 1, (0.75, 0.85, 1.0)),), extra=[toggle(lv, 'L3.Alarm', False)])
        mesh(lv, f'Dress_TunnelLampRed_{i}', OWN + 'L3_Core_Bulkhead.nmmesh', (0, 395, z), rot=(-90, 0, 0),
             params=(('EmissiveColor', 1, (1.0, 0.1, 0.05)),), extra=[toggle(lv, 'L3.Alarm', True)])
        if i != 1:
            light(lv, f'Dress_TunnelLight_{i}', (0, 340, z), CALM, 18, 900, channel='L3.Alarm', show_when_on=False)
        else:
            light(lv, f'Dress_TunnelRed_{i}', (0, 340, z), RED, 10, 1200, channel='L3.Alarm', flick=(1.5, 0.7))
    for i, x in enumerate((700, 2400)):
        light(lv, f'Dress_HallLight_{i}', (x, 640, -4900), (230, 236, 248), 20, 1500)
    light(lv, 'Dress_HallRed', (1500, 600, -5400), RED, 9, 2200, flick=(1.0, 0.7))
    light(lv, 'Dress_FreightLight', (3800, 900, -5400), (255, 220, 170), 30, 1400)
    # post process: a cold calm grade, the alarm's red over it
    lv.entity('Dress_PostCalm', [lv.fields('Post Process', version=2, Stack=PP + 'L3_Core_Calm.pp.nmgraph', Priority=0,
                                           Enabled=True, BlendDistance=0), xform((0, 0, 0))])
    lv.entity('Dress_PostAlarm', [lv.fields('Post Process', version=2, Stack=PP + 'L3_Core_Alarm.pp.nmgraph', Priority=1,
                                            Enabled=False, BlendDistance=0), xform((0, 0, 0)), toggle(lv, 'L3.Alarm')])
    # GI: one DDGI volume over the shaft, one over the pump hall (the nearest lights), with the
    # stacks' SSGI over it. Each probe update draws the whole level six times (Captures ~29 ms GPU on
    # the Arc B390 before the engine's capture budget): DEMOLAB_L3_DDGI=0 writes them off.
    ddgi = os.environ.get('DEMOLAB_L3_DDGI', '1') == '1'
    lv.entity('Dress_GIShaft', [lv.fields('GI Volume', Extent=(1850, 1300, 1850), ProbeSpacing=460, Intensity=1,
                                          Enabled=ddgi), xform((0, 1100, 0))])
    lv.entity('Dress_GIPumpHall', [lv.fields('GI Volume', Extent=(1950, 380, 1050), ProbeSpacing=330, Intensity=1,
                                             Enabled=ddgi), xform((1500, 350, -5400))])
    for i, (pos, radius, box) in enumerate((((0, 300, -1300), 1900, (0, 0, 0)), ((0, 1300, 1100), 2400, (0, 0, 0)),
                                            ((0, 330, 0), 620, (600, 350, 600)), ((1500, 300, -5400), 2200, (0, 0, 0)))):
        lv.entity(f'Dress_Probe_{i}', [lv.fields('Reflection Probe', Radius=radius, BoxExtent=box, Intensity=1,
                                                 Resolution=256, Frequency=2, Interval=1, NearClip=5, FarClip=8000,
                                                 CaptureShadows=True, Enabled=True), xform(pos)], static=True)
    lv.entity('Ambient', [lv.fields('Sky Light', version=2, SkyColor=(70, 85, 110), GroundColor=(40, 38, 36),
                                    Intensity=0.12, Active=True, Environment='', EnvironmentIntensity=1,
                                    EnvironmentRotation=0, DrawBackground=False), xform((0, 1000, 0))])


# =============================================================================
# Review copies (DEMOLAB_REVIEW=1 with DEMOLAB_L3_STATE=calm|alarm|beacon): the editor runs no
# level logic, so the copy keeps only what that state shows -- for screenshots
# =============================================================================
STATES = {'calm': set(), 'alarm': {'L3.Intel', 'L3.Alarm', 'L3.Armed'},
          'beacon': {'L3.Intel', 'L3.Alarm', 'L3.Armed', 'L3.W1Done', 'L3.PumpHall', 'L3.BeaconCharging'}}


def review_state(lv, state):
    on = STATES[state]
    keep = []
    for g, text in lv.entities:
        m = re.search(r'<Component Type="LabSignalToggle".*?<Channel Value="([^"]*)" />\s*<ShowWhenOn Value="(\w+)" />',
                      text, re.S)
        if m:
            if (m.group(1) in on) != (m.group(2) == 'true'):
                lv.names.discard(name_of(text))
                continue
            text = text.replace('<Enabled Value="false" />', '<Enabled Value="true" />')
            text = text.replace('<Emitting Value="false" />', '<Emitting Value="true" />')
        if state == 'calm' and name_of(text) == 'Shutter_Elevator':     # open until the alarm
            text = text.replace(f'Y="{f(ARRIVE + 165)}"', f'Y="{f(ARRIVE + 515)}"')
        if state == 'calm' and name_of(text) == 'Dress_HelixShutter':
            text = text.replace('Y="170"', 'Y="550"')
        if state != 'calm' and 'Dress_VentCover' in name_of(text):
            text = re.sub(r'(<Position X="[^"]*" Y=")0(")', r'\g<1>160\2', text)
        if state == 'beacon' and name_of(text) == 'Dress_Beacon':
            text = text.replace('<ParameterCount Value="0" />',
                                '<ParameterCount Value="1" />\n    <Parameter0Name Value="EmissiveIntensity" />\n'
                                '    <Parameter0Slot Value="5" />\n    <Parameter0X Value="40" />\n'
                                '    <Parameter0Y Value="0" />\n    <Parameter0Z Value="0" />\n    <Parameter0W Value="0" />')
        keep.append((g, text))
    lv.entities = keep
