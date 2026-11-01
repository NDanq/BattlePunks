"""
Level 2's art pass: dresses the grey-box L2_Laboratory make_levels.make_l2() builds, in place,
before it is written (make_l2 calls dress(lv, module) as its last step).

What it does to the grey box:
  - the boxes stay as the collision (walls, floors, benches, turbines, stairs) but are hidden;
    the kit (Tools/Art/assets.json) draws instead: riveted wall panels with the wainscot,
    marble floors, beamed ceilings, the octagonal atrium under the glass dome, the orrery,
    the workshop's ten shading-model samples on plinths with engraved plaques, the archive's
    bookshelves and velvet curtains, the turbine hall, the elevator cage, the conservatory
    behind glass (Tools/Art/make_l2_dress.py writes the L2_* extras);
  - its placeholder lights go: every fixture here is a pair of meshes (lit / dark) and a
    light, switched by the power signals C3's Event keys raise one group after another
    (rewritten here: the cascade runs over about four seconds), plus emergency lights until
    the power is on;
  - the Calm_ / Alarm_ state groups (LabLogic::ApplyStateGroups) keep their contract: Calm_*
    is the powered lab at mid-afternoon, Alarm_* the alarm at dusk -- red fills, sweeping
    beacons (LabSpinner), the normal lamps dark, sprinklers, sparks, steam and smoke, debris
    with its own collision, shutter meshes on the gates, glass that shatters when shot
    (Destructible, LabCombat);
  - lighting: TimeOfDay sun through the dome, DDGI GI Volumes (one over the airlock, foyer
    and atrium so C3's bounce arrives live; one per other room), a reflection probe a room,
    a post process stack a state (SSGI, AO, SSR, volumetric fog, bloom, exposure).

Gameplay entities (starts, interactables, gates, triggers, spawns, waypoints, sequence
players) are kept; a few get a kit look (Render mesh/material/scale only).
"""
import math
import re

import layout as L

T = L.WALL_THICKNESS
KM = 'Content/Meshes/L2/{}.nmmesh'
K1 = 'Content/Meshes/L1/{}.nmmesh'
K3 = 'Content/Meshes/L3/{}.nmmesh'
DM = 'Content/Meshes/L2_Laboratory/{}.nmmesh'
MAT = 'Content/Materials/Kit/{}.material.nmgraph'
BOX = 'Engine/Content/Meshes/Box.nmmesh'
CYL = 'Engine/Content/Meshes/Cylinder.nmmesh'
PART = 'Content/Particles/{}.nmparticle'
DPART = 'Content/Particles/L2_Laboratory/{}.nmparticle'
WALL_SY = 330.0 / 280.0          # the kit doorway (280) scaled to the gameplay door height (330)

WARM = (255, 196, 128)
COOL = (150, 175, 255)
RED = (255, 32, 18)


# =============================================================================
# Entity writing (the same text Level.entity writes, plus DetailMode)
# =============================================================================
class Dresser:
    def __init__(self, lv, ml):
        self.lv, self.ml = lv, ml
        self.review = ml.REVIEW

    # --- components ---------------------------------------------------------
    @staticmethod
    def render(mesh, materials=(), params=(), shadows=True, visible=True):
        out = ['  <Component Type="Render" Version="7">', f'    <Visible Value="{"true" if visible else "false"}" />']
        for i in range(8):
            out.append(f'    <Material{i} Value="{materials[i] if i < len(materials) and materials[i] else ""}" />')
        out.append(f'    <Mesh Value="{mesh}" />')
        out.append(f'    <ParameterCount Value="{len(params)}" />')
        for i, (slot, name, value) in enumerate(params):
            v = list(value) if isinstance(value, (tuple, list)) else [value]
            v += [0] * (4 - len(v))
            out += [f'    <Parameter{i}Name Value="{name}" />', f'    <Parameter{i}Slot Value="{slot}" />',
                    f'    <Parameter{i}X Value="{v[0]:g}" />', f'    <Parameter{i}Y Value="{v[1]:g}" />',
                    f'    <Parameter{i}Z Value="{v[2]:g}" />', f'    <Parameter{i}W Value="{v[3]:g}" />']
        out += [f'    <CastShadows Value="{"true" if shadows else "false"}" />', '    <CastInsetShadow Value="false" />',
                '    <Occluder Value="false" />', '    <CustomPassGroups Value="0" />', '    <RevealPlayers Value="false" />',
                '  </Component>']
        return out

    def add(self, name, comps, parent=None, static=False, detail=None):
        lv = self.lv
        name = lv.unique(name)
        lv.names.add(name)
        g = lv.guid(name)
        head = (f'<Entity Guid="{g}" Name="{name}"' + (f' Parent="{lv.guid(parent)}"' if parent else '')
                + (' Mobility="Static"' if static else '') + (f' DetailMode="{detail}"' if detail else '') + '>')
        body = [x for c in comps for x in c]
        lv.entities.append((g, '\n'.join(['<?xml version="1.0" encoding="UTF-8"?>', head] + body + ['</Entity>']) + '\n'))
        return name

    def prop(self, name, mesh, pos, yaw=0.0, scale=(1, 1, 1), rot=None, parent=None, materials=(), params=(),
             shadows=True, detail=None, extra=(), static=True, visible=True):
        if not isinstance(scale, (tuple, list)):
            scale = (scale, scale, scale)
        r = rot if rot is not None else (0, yaw, 0)
        comps = [self.render(mesh, materials, params, shadows, visible), self.lv.transform(pos, r, scale)] + list(extra)
        return self.add(name, comps, parent, static=static and parent is None and not extra, detail=detail)

    def blocker(self, name, centre, size, yaw=0.0):
        """Invisible collision for a prop the player and bots must not walk through."""
        return self.lv.box(name, centre, size, yaw=yaw, visible=False)

    def toggle(self, channel, show_when_on=True):
        return self.lv.fields('LabSignalToggle', Channel=channel, ShowWhenOn=show_when_on)

    def point(self, name, pos, color, intensity, radius, channel=None, show_when_on=True, source=8, shadows=False,
              parent=None):
        comps = [self.lv.fields('Point Light', version=2, Color=color, Intensity=intensity, Radius=radius, Active=True,
                                CastShadows=shadows, ShadowTechnique=0, SourceRadius=source),
                 self.lv.transform(pos)]
        if channel:
            comps.insert(1, self.toggle(channel, show_when_on))
        return self.add(name, comps, parent)

    def spot(self, name, pos, rot, color, intensity, rng, inner, outer, parent=None, channel=None, shadows=False):
        comps = [['  <Component Type="Spot Light" Version="2">', f'    <Color R="{color[0]}" G="{color[1]}" B="{color[2]}" A="255" />',
                  f'    <Intensity Value="{intensity}" />', f'    <Range Value="{rng}" />',
                  f'    <InnerConeAngle Value="{inner}" />', f'    <OuterConeAngle Value="{outer}" />',
                  '    <Active Value="true" />', f'    <CastShadows Value="{"true" if shadows else "false"}" />',
                  '    <ShadowTechnique Value="1" />', '    <ShadowTint R="20" G="22" B="28" A="255" />',
                  '    <ShadowBias Value="0.002" />', '    <ShadowNormalBias Value="1.5" />', '    <SourceRadius Value="6" />',
                  '  </Component>'],
                 self.lv.transform(pos, rot)]
        if channel:
            comps.insert(1, self.toggle(channel))
        return self.add(name, comps, parent)

    def rect(self, name, pos, rot, color, intensity, width, height, rng, channel=None, show_when_on=True):
        comps = [self.lv.fields('Rect Light', Color=color, Intensity=intensity, Width=width, Height=height, Range=rng,
                                Active=True), self.lv.transform(pos, rot)]
        if channel:
            comps.insert(1, self.toggle(channel, show_when_on))
        return self.add(name, comps)

    def particles(self, name, system, pos, rot=(0, 0, 0), tint=(255, 255, 255), rate=1.0, parent=None, detail=None):
        comps = [['  <Component Type="Particle System" Version="3">', f'    <System Value="{system}" />',
                  '    <Emitting Value="true" />', f'    <Tint R="{tint[0]}" G="{tint[1]}" B="{tint[2]}" A="255" />',
                  f'    <SpawnRateScale Value="{rate:g}" />', '    <TimeScale Value="1" />', '    <Seed Value="0" />',
                  '    <UseBeamTarget Value="false" />', '    <BeamTarget X="0" Y="0" Z="0" />', '  </Component>'],
                 self.lv.transform(pos, rot)]
        return self.add(name, comps, parent, detail=detail)

    def spinner(self, channel, axis, speed, spin_up=1.5):
        return self.lv.fields('LabSpinner', Channel=channel, Axis=axis, Speed=speed, SpinUp=spin_up)

    # --- grey-box edits -----------------------------------------------------
    def names(self):
        return [(i, re.search(r' Name="([^"]+)"', t).group(1)) for i, (_, t) in enumerate(self.lv.entities)]

    def drop(self, pattern):
        rx = re.compile(pattern)
        keep = [(g, t) for g, t in self.lv.entities if not rx.fullmatch(re.search(r' Name="([^"]+)"', t).group(1))]
        gone = len(self.lv.entities) - len(keep)
        self.lv.entities[:] = keep
        return gone

    def edit(self, pattern, fn):
        rx = re.compile(pattern)
        n = 0
        for i, (g, t) in enumerate(self.lv.entities):
            if rx.fullmatch(re.search(r' Name="([^"]+)"', t).group(1)):
                self.lv.entities[i] = (g, fn(t))
                n += 1
        return n

    def hide(self, pattern):
        return self.edit(pattern, lambda t: t.replace('    <Visible Value="true" />', '    <Visible Value="false" />', 1))

    def restyle(self, pattern, mesh=None, material=None, scale=None):
        def fn(t):
            if mesh:
                t = re.sub(r'<Mesh Value="[^"]*" />', f'<Mesh Value="{mesh}" />', t, count=1)
            if material:
                t = t.replace('    <Mesh Value=', f'    <Material0 Value="{material}" />\n    <Mesh Value=', 1)
            if scale:
                t = re.sub(r'<Scale X="[^"]*" Y="[^"]*" Z="[^"]*" />',
                           f'<Scale X="{scale[0]:g}" Y="{scale[1]:g}" Z="{scale[2]:g}" />', t, count=1)
            return t
        return self.edit(pattern, fn)


# =============================================================================
# Architecture
# =============================================================================
def wall_run(d, name, centre, yaw, length, doors, sy, detail=None):
    """Kit walls along a wall's inner face: centre (x, z) of the face, yaw as the kit wants it
    (front toward the room), length along its local X, doors as (local offset, width)."""
    ux, uz = math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))
    pieces = []   # (a, b, mesh)
    cursor = -length / 2
    for off, w in sorted(doors):
        dw = 400.0 * w / 160.0
        a, b = off - dw / 2, off + dw / 2
        if a - cursor < 60:            # too thin a sliver beside the door: the door piece takes it
            a = cursor
        pieces.append((cursor, a, 'Wall'))
        pieces.append((a, b, 'WallDoor'))
        cursor = b
    if length / 2 - cursor < 60 and pieces:
        a, b, m = pieces[-1]
        pieces[-1] = (a, length / 2, m)
    else:
        pieces.append((cursor, length / 2, 'Wall'))
    k = 0
    for a, b, mesh in pieces:
        span = b - a
        if span < 1:
            continue
        n = 1 if mesh == 'WallDoor' else max(1, round(span / 400.0))
        for i in range(n):
            pa = a + span * i / n
            pb = a + span * (i + 1) / n
            mid = (pa + pb) / 2
            d.prop(f'{name}_{k}', KM.format(mesh), (centre[0] + ux * mid, 0, centre[1] + uz * mid), yaw,
                   ((pb - pa) / 400.0, sy, 1), detail=detail)
            k += 1


def rect_room_kit(d, name, x0, z0, x1, z1, h, doors, ceiling=True, skip=()):
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    sy = h / 500.0
    wall_run(d, f'K_{name}_WallN', (cx, z1 - T), 0, x1 - x0 - 2 * T, [(x - cx, w) for x, w in doors.get('N', [])], sy)
    wall_run(d, f'K_{name}_WallS', (cx, z0 + T), 180, x1 - x0 - 2 * T, [(-(x - cx), w) for x, w in doors.get('S', [])], sy)
    wall_run(d, f'K_{name}_WallE', (x1 - T, cz), 90, z1 - z0 - 2 * T, [(-(z - cz), w) for z, w in doors.get('E', [])], sy)
    if 'W' not in skip:
        wall_run(d, f'K_{name}_WallW', (x0 + T, cz), 270, z1 - z0 - 2 * T, [((z - cz), w) for z, w in doors.get('W', [])], sy)
    tiles(d, f'K_{name}_Floor', 'Floor', x0 + T, z0 + T, x1 - T, z1 - T, 0,
          materials=[MAT.format('Concrete')] if name in ('TurbineHall', 'Airlock') else
          [MAT.format('Planks')] if name == 'Conservatory' else ())
    if ceiling and not d.review:
        tiles(d, f'K_{name}_Ceiling', 'Ceiling', x0 + T, z0 + T, x1 - T, z1 - T, h, nominal=430.0)


def tiles(d, name, mesh, x0, z0, x1, z1, y, nominal=400.0, materials=()):
    nx, nz = max(1, math.ceil((x1 - x0) / 400.0 - 0.05)), max(1, math.ceil((z1 - z0) / 400.0 - 0.05))
    sx, sz = (x1 - x0) / nx / 400.0, (z1 - z0) / nz / 400.0
    centres = []
    for i in range(nx):
        for j in range(nz):
            x, z = x0 + (i + 0.5) * (x1 - x0) / nx, z0 + (j + 0.5) * (z1 - z0) / nz
            d.prop(f'{name}_{i}_{j}', KM.format(mesh), (x, y, z), 0, (sx, 1, sz), shadows=(mesh != 'Floor'), materials=materials)
            centres.append((x, z))
    return centres


def ceiling_centres(x0, z0, x1, z1):
    x0, z0, x1, z1 = x0 + T, z0 + T, x1 - T, z1 - T
    nx, nz = max(1, math.ceil((x1 - x0) / 400.0 - 0.05)), max(1, math.ceil((z1 - z0) / 400.0 - 0.05))
    return [[(x0 + (i + 0.5) * (x1 - x0) / nx, z0 + (j + 0.5) * (z1 - z0) / nz) for j in range(nz)] for i in range(nx)]


def atrium_kit(d):
    a = L.L2['atrium']
    cx, cz = a['centre']
    ap, h = a['apothem'], a['height']
    sy = h / 500.0
    side = 2 * ap * math.tan(math.radians(22.5))
    for label, ang in d.ml.Level.COMPASS.items():
        x, z = cx + ap * math.sin(math.radians(ang)), cz + ap * math.cos(math.radians(ang))
        if label in a['glass']:
            # The conservatory window: a walnut sill, a glass wall in a brass frame.
            ux, uz = math.cos(math.radians(ang)), -math.sin(math.radians(ang))
            fx, fz = cx + (ap + T / 2) * math.sin(math.radians(ang)), cz + (ap + T / 2) * math.cos(math.radians(ang))
            d.prop('K_Atrium_Sill', BOX, (fx, 50, fz), ang, (side, 100, T + 10), materials=[MAT.format('Walnut')])
            d.prop('K_Atrium_SillCap', BOX, (fx, 102, fz), ang, (side + 10, 6, T + 24), materials=[MAT.format('BrassAged')])
            d.prop('K_Atrium_Glass', BOX, (fx, 105 + (h - 105) / 2, fz), ang, (side, h - 105, 3),
                   materials=[MAT.format('GlassClear')], shadows=False)
            for k in range(5):
                o = -side / 2 + side * k / 4
                d.prop(f'K_Atrium_Mullion_{k}', BOX, (fx + ux * o, 105 + (h - 105) / 2, fz + uz * o), ang, (8, h - 105, 12),
                       materials=[MAT.format('BrassAged')])
            for y in (330, h - 6):
                d.prop('K_Atrium_Transom', BOX, (fx, y, fz), ang, (side, 8, 12), materials=[MAT.format('BrassAged')])
            continue
        gaps = [(0, a['doors'][label])] if label in a['doors'] else []
        wall_run(d, f'K_Atrium_Wall{label}', (x, z), ang, side, gaps, sy)
    # The floor: marble tiles over the octagon's square (hidden under the walls at the corners).
    tiles(d, 'K_Atrium_Floor', 'Floor', cx - ap - T, cz - ap - T, cx + ap + T, cz + ap + T, 0.5)
    if d.review:
        return
    d.prop('K_Atrium_Ring', DM.format('L2_AtriumRing'), (cx, h, cz), 0)
    for k in range(8):
        d.prop(f'K_Atrium_Dome_{k}', KM.format('DomeSegment'), (cx, h + 44, cz), k * 45.0, (2.0, 1.7, 2.0), shadows=True,
               materials=['Content/Materials/L2_Laboratory/L2_DomeGlass.material.nmgraph'])


def gantry_kit(d):
    g = L.L2['gantry']
    steel, brass = MAT.format('SteelDark'), MAT.format('BrassAged')
    zc = (g['z0'] + g['z1']) / 2
    width = g['z1'] - g['z0']
    for i in range(g['steps']):
        x = g['x0'] + (i + 0.5) * g['run']
        top = (i + 1) * g['rise']
        d.prop(f'K_Gantry_Tread_{i}', BOX, (x, top - 3, zc), 0, (g['run'] + 4, 6, width - 20), materials=[steel])
        d.prop(f'K_Gantry_Nosing_{i}', BOX, (x - g['run'] / 2 + 2, top - 2, zc), 0, (5, 5, width - 20), materials=[brass])
    run = g['steps'] * g['run']
    rise = g['steps'] * g['rise']
    ang = math.degrees(math.atan2(rise, run))
    length = math.hypot(run, rise)
    for z in (g['z0'] + 8, g['z1'] - 8):
        d.prop('K_Gantry_Stringer', BOX, (g['x0'] + run / 2, rise / 2 - 12, z), rot=(0, 0, ang), scale=(length, 26, 10),
               materials=[steel])
        d.prop('K_Gantry_Handrail', BOX, (g['x0'] + run / 2, rise / 2 + 95, z), rot=(0, 0, ang), scale=(length, 5, 5),
               materials=[brass])
        for k in range(1, 6):
            x = g['x0'] + run * k / 6
            y = rise * k / 6
            d.prop('K_Gantry_Baluster', BOX, (x, y + 48, z), 0, (4, 96, 4), materials=[brass])
    px0, pz0, px1, pz1 = g['platform']
    top = rise
    for k in range(3):
        d.prop(f'K_Gantry_Deck_{k}', K3.format('Catwalk_Straight'), (px0 + 80 + k * 158, top, pz0), 0)
    for x in (px0 + 10, px1 - 10):
        for z in (pz0 + 10, pz1 - 10):
            d.prop('K_Gantry_Post', BOX, (x, top / 2, z), 0, (18, top, 18), materials=[steel])
    d.prop('K_Gantry_RailE', K3.format('Railing_Straight'), (px1 - 3, top, pz0), 0)
    d.prop('K_Gantry_RailN', K3.format('Railing_Straight'), (px0, top, pz1 - 3), 90, (1, 1, 1.25))


# =============================================================================
# Fixtures (lit / dark pairs, power-switched) and lighting
# =============================================================================
def dark_params(slots=8):
    return [(s, 'EmissiveIntensity', 0) for s in range(slots)]


def fixture(d, name, mesh, pos, yaw, channel, light_at, color, intensity, radius, slots=5, scale=1, source=10,
            shadows=False, alarm_red=None):
    """A lamp that comes on with channel: lit and dark meshes swapped by LabSignalToggle, its
    light; on the alarm it hangs dark (alarm_red: a dim red glow in it instead)."""
    d.prop(f'Calm_{name}_Lit', mesh, pos, yaw, scale, extra=[d.toggle(channel)], static=False)
    d.prop(f'Calm_{name}_Dark', mesh, pos, yaw, scale, params=dark_params(slots), extra=[d.toggle(channel, False)],
           static=False)
    d.point(f'Calm_{name}_Light', light_at, color, intensity, radius, channel=channel, source=source, shadows=shadows)
    d.prop(f'Alarm_{name}_Dark', mesh, pos, yaw, scale, params=dark_params(slots))


def pendant(d, name, x, z, h, channel, intensity=40, radius=1100, color=WARM):
    fixture(d, name, KM.format('PendantLamp'), (x, h, z), 0, channel, (x, h - 147, z), color, intensity, radius, slots=3,
            source=9)


def road_lamp(d, name, ang, r, channel):
    x, z = r * math.sin(math.radians(ang)), r * math.cos(math.radians(ang))
    lx, lz = x - 60 * math.sin(math.radians(ang)), z - 60 * math.cos(math.radians(ang))
    fixture(d, name, K1.format('RoadLamp'), (x, 0, z), ang, channel, (lx, 385, lz), WARM, 30, 1300, slots=4, source=12)
    d.blocker(f'K_{name}_Block', (x, 200, z), (30, 400, 30))


def light_strip(d, name, x, z, y, yaw, channel, intensity=6, width=290, height=14, rng=1200):
    """A hung bar and its area light, facing down; dark on the alarm."""
    mesh = DM.format('L2_LightStrip')
    d.prop(f'Calm_{name}_Lit', mesh, (x, y, z), yaw, extra=[d.toggle(channel)], static=False)
    d.prop(f'Calm_{name}_Dark', mesh, (x, y, z), yaw, params=dark_params(3), extra=[d.toggle(channel, False)], static=False)
    d.prop(f'Alarm_{name}_Dark', mesh, (x, y, z), yaw, params=dark_params(3))
    # Rect Light: Width along +X, Height along +Y, facing +Z: pitched 90 it faces down.
    d.rect(f'Calm_{name}_Area', (x, y - 14, z), (90, yaw, 0), (255, 222, 180), intensity, width, height, rng, channel=channel)


def beacon(d, name, x, y, z, speed=280):
    """An alarm beacon under the ceiling: its reflector and two red spots sweep round."""
    b = d.prop(f'Alarm_{name}', DM.format('L2_Beacon'), (x, y, z), rot=(180, 0, 0), static=False)
    spin = d.add(f'Alarm_{name}_Spin', [d.lv.transform((0, 0, 0)), d.spinner('', 1, speed, 0.5)], parent=b)
    d.prop(f'Alarm_{name}_Reflector', DM.format('L2_BeaconReflector'), (0, 0, 0), 0, parent=spin, shadows=False,
           static=False)
    for yaw in (0, 180):
        d.spot(f'Alarm_{name}_Spot', (0, 16, 0), (-22, yaw, 0), RED, 22, 1800, 14, 26, parent=spin)
    d.point(f'Alarm_{name}_Glow', (x, y - 30, z), RED, 3, 400, source=12)


def probe(d, name, centre, extent, interval=3.0):
    d.add(name, [['  <Component Type="Reflection Probe" Version="1">',
                  f'    <Radius Value="{max(extent) * 1.6:g}" />',
                  f'    <BoxExtent X="{extent[0]:g}" Y="{extent[1]:g}" Z="{extent[2]:g}" />',
                  '    <Intensity Value="1" />', '    <Resolution Value="128" />', '    <Frequency Value="1" />',
                  f'    <Interval Value="{interval:g}" />', '    <NearClip Value="5" />', '    <FarClip Value="12000" />',
                  '    <CaptureShadows Value="true" />', '    <Enabled Value="true" />', '  </Component>'],
                 d.lv.transform(centre)])


def gi_volume(d, name, centre, extent, spacing):
    d.add(name, [d.lv.fields('GI Volume', Extent=tuple(extent), ProbeSpacing=spacing, Intensity=1, Enabled=True),
                 d.lv.transform(centre)])


def sun(d, prefix, hour, sky_intensity):
    lv = d.lv
    d.add(prefix + 'Sun', [
        lv.fields('Directional Light', version=3, Color=(255, 238, 215), Intensity=6, Active=True, CastShadows=True,
                  ShadowTechnique=2, ShadowBias=0.0012, ShadowNormalBias=2, ShadowDistance=6000, CascadeCount=4,
                  CascadeSplitLambda=0.8, SourceAngle=0.6),
        lv.fields('Sky Light', version=2, SkyColor=(150, 160, 190), GroundColor=(60, 52, 44), Intensity=sky_intensity,
                  Active=True, Environment='', EnvironmentIntensity=1, EnvironmentRotation=0, DrawBackground=True),
        lv.fields('Time Of Day', Hour=hour, DayLengthMinutes=20, Running=False, NorthYaw=35, Latitude=35,
                  SunIntensityScale=2.5, Preset='TimeOfDay:/Presets/Default.todpreset'),
        lv.transform((0, 8000, 0), (32, 215, 0))])


def post(d, name, stack):
    d.add(name, [d.lv.fields('Post Process', version=2, Stack=stack, Priority=0, Enabled=True, BlendDistance=0),
                 d.lv.transform((0, 0, 0))])


# =============================================================================
# Rooms
# =============================================================================
def pipe_run(d, name, a, b, y, along):
    """Copper pipes along a wall, chained Pipe_Straight (2 m) between a and b on one axis."""
    length = abs(b - a)
    n = max(1, round(length / 200.0))
    for i in range(n):
        c = min(a, b) + (i + 0.5) * length / n
        pos = (c, y[0], y[1]) if along == 'x' else (y[1], y[0], c)
        d.prop(f'{name}_{i}', KM.format('Pipe_Straight'), pos, 0 if along == 'x' else 90, (length / n / 200.0, 1, 1),
               shadows=False)


def gauge(d, name, pos, yaw, reading=40):
    g = d.prop(name, KM.format('Gauge'), pos, yaw, static=False)
    d.prop(f'{name}_Needle', KM.format('Gauge_Needle'), (0, 0, -4.8), rot=(0, 0, reading), parent=g, shadows=False,
           static=False)


def dress_foyer_airlock(d):
    rooms = L.L2['rooms']
    x0, z0, x1, z1, h, _ = rooms['Foyer']
    p = L.L2['points']
    # The breaker panel on the west wall (the Calm_Breaker interactable stands at it).
    bx, by, bz, _ = p['Breaker']
    wx = x0 + T
    d.prop('K_BreakerPanel', KM.format('BreakerPanel'), (wx, 0, bz), 270)
    for prefix, angle in (('Calm_', -35), ('Alarm_', 35)):
        pivot = d.add(f'{prefix}BreakerLeverPivot', [d.lv.transform((wx + 14, 92, bz), (0, 270, 0))])
        d.prop(f'{prefix}BreakerLever', KM.format('BreakerPanel_Lever'), (0, 0, 0), rot=(angle, 0, 0), parent=pivot,
               static=False)
    gauge(d, 'K_Foyer_Gauge_0', (wx + 6, 200, bz - 80), 270, 20)
    gauge(d, 'K_Foyer_Gauge_1', (wx + 6, 200, bz + 80), 270, 70)
    # A hurricane lamp on the floor by the panel: the only light before the breaker.
    d.prop('Calm_HandLamp', DM.format('L2_HandLamp'), (wx + 70, 0, bz + 70), 20, static=False)
    d.point('Calm_HandLamp_Light', (wx + 70, 22, bz + 70), (255, 170, 90), 6, 600, channel='L2.Power.All',
            show_when_on=False, source=6)
    d.point('Calm_HandLamp_LightOn', (wx + 70, 22, bz + 70), (255, 170, 90), 2, 300, channel='L2.Power.All', source=6)
    d.point('Calm_BreakerGlow', (wx + 40, 140, bz), (255, 120, 60), 2.5, 350, source=20)
    # Pipes along the north and south walls under the ceiling, a desk, a bench.
    pipe_run(d, 'K_Foyer_PipeN', x0 + 60, x1 - 60, (h - 70, z1 - T - 25), 'x')
    pipe_run(d, 'K_Foyer_PipeS', x0 + 60, x1 - 60, (h - 70, z0 + T + 25), 'x')
    d.prop('K_Foyer_Desk', KM.format('Workbench'), (600, 0, -1950), 90)
    d.blocker('K_Foyer_DeskBlock', (600, 50, -1950), (125, 100, 200))
    d.prop('K_Foyer_Shelf', KM.format('Bookshelf'), (x1 - T - 38, 0, -1700), 90)
    d.blocker('K_Foyer_ShelfBlock', (x1 - T - 20, 125, -1700), (46, 250, 216))
    # Lamps: two pendants (each its own beat of the cascade), the airlock's.
    cc = ceiling_centres(x0, z0, x1, z1)
    pendant(d, 'Foyer_Lamp_0', cc[1][1][0], cc[1][1][1], h, 'L2.Power.Foyer')
    pendant(d, 'Foyer_Lamp_1', cc[3][1][0], cc[3][1][1], h, 'L2.Pwr.Foyer2')
    ax0, az0, ax1, az1, ah, _ = rooms['Airlock']
    pendant(d, 'Airlock_Lamp', (ax0 + ax1) / 2, (az0 + az1) / 2, ah, 'L2.Power.Foyer', intensity=14, radius=700)
    for k, (x, z) in enumerate(((ax0 + 70, az0 + 70), (ax1 - 70, az0 + 70))):
        d.prop(f'K_Airlock_Vent_{k}', K1.format('SteamVent'), (x, 0, z), 0)
        d.particles(f'K_Airlock_Steam_{k}', PART.format('SteamVent'), (x, 30, z), rate=0.5, detail='Medium')
    pipe_run(d, 'K_Airlock_Pipe', az0 + 40, az1 - 40, (ah - 60, ax0 + T + 25), 'z')
    # Emergency light before the power.
    d.point('Calm_Emergency_Foyer', (0, h - 40, -1300), COOL, 1.2, 900, channel='L2.Power.All', show_when_on=False)
    d.point('Calm_Emergency_Airlock', (0, ah - 40, -2700), COOL, 2.5, 600, channel='L2.Power.All', show_when_on=False)
    probe(d, 'K_Probe_Foyer', (0, h / 2, (z0 + z1) / 2), ((x1 - x0) / 2, h / 2, (z1 - z0) / 2))
    probe(d, 'K_Probe_Airlock', (0, ah / 2, -2700), (300, ah / 2, 300))
    # Alarm: red fill, a beacon, a steam leak from the pipe.
    beacon(d, 'Beacon_Foyer', 0, h - 4, -1800)
    d.point('Alarm_Red_Foyer', (0, 300, -1800), RED, 1, 1100)
    d.particles('Alarm_Foyer_SteamJet', PART.format('SteamJet'), (x0 + 300, h - 70, z1 - T - 25), rot=(90, 0, 0))


def dress_atrium(d):
    a = L.L2['atrium']
    h = a['height']
    # The orrery: plinth, a gimbal of three rings (Outer > Middle > Inner) and the planets.
    d.prop('Calm_Orrery', KM.format('Orrery'), (0, 0, 0), 0)
    d.prop('Alarm_Orrery', KM.format('Orrery'), (0, 0, 0), 0,
           params=[(s, 'EmissiveColor', (1.0, 0.12, 0.05)) for s in range(7)] + [(6, 'EmissiveIntensity', 4)])
    for prefix, channel in (('Calm_', 'L2.Power.Atrium'), ('Alarm_', '')):
        outer = d.prop(f'{prefix}Orrery_RingOuter', KM.format('Orrery_RingOuter'), (0, 400, 0), 0,
                       extra=[d.spinner(channel, 0, 7, 4)], static=False)
        middle = d.prop(f'{prefix}Orrery_RingMiddle', KM.format('Orrery_RingMiddle'), (0, 0, 0), 0, parent=outer,
                        extra=[d.spinner(channel, 2, -11, 4)], static=False)
        d.prop(f'{prefix}Orrery_RingInner', KM.format('Orrery_RingInner'), (0, 0, 0), 0, parent=middle,
               extra=[d.spinner(channel, 1, 17, 4)], static=False)
        d.prop(f'{prefix}Orrery_Planets', KM.format('Orrery_Planets'), (0, 400, 0), 0,
               extra=[d.spinner(channel, 1, 5, 4)], static=False)
    d.point('Calm_Orrery_Sun', (0, 400, 0), (255, 186, 110), 18, 1400, channel='L2.Power.Atrium', source=22, shadows=True)
    d.point('Alarm_Orrery_Sun', (0, 400, 0), (255, 120, 70), 10, 900, source=22)
    gantry_kit(d)
    # Lamp posts round the floor, each its own beat.
    for k, (ang, ch) in enumerate(((90, 'L2.Power.Atrium'), (140, 'L2.Pwr.Atrium2'), (200, 'L2.Pwr.Atrium2'),
                                   (250, 'L2.Pwr.Atrium3'), (330, 'L2.Pwr.Atrium3'))):
        road_lamp(d, f'Atrium_Post_{k}', ang, 1000 if ang in (90,) else 1000, ch)
    # Palms in planters on the south-east and south-west sides.
    for k, ang in enumerate((135, 225)):
        x, z = 1020 * math.sin(math.radians(ang)), 1020 * math.cos(math.radians(ang))
        d.prop(f'K_Atrium_Planter_{k}', DM.format('L2_Planter'), (x, 0, z), ang + 0.0)
        for j, o in enumerate((-70, 70)):
            ux, uz = math.cos(math.radians(ang)), -math.sin(math.radians(ang))
            d.prop(f'K_Atrium_Palm_{k}_{j}', DM.format('L2_Fern' if j else 'L2_Palm'), (x + ux * o, 64, z + uz * o),
                   ang * 3 + j * 50, (0.8, 0.8, 0.8), shadows=True)
        d.blocker(f'K_Atrium_PlanterBlock_{k}', (x, 40, z), (250, 80, 70), ang)
    # Dust in the dome's light; it is lit by the sun, so it shows in the shaft.
    d.particles('K_Atrium_Dust', PART.format('DustMotes'), (0, 420, 0), rate=1.0, detail='Medium')
    d.particles('K_Atrium_Dust2', PART.format('DustMotes'), (-300, 650, 250), rate=0.7, detail='High')
    probe(d, 'K_Probe_Atrium', (0, 400, 0), (1200, 500, 1200), interval=2.0)
    # Alarm: beacons on the ring, red, smoke rising into the dome, sparks from a lamp post's junction box.
    for k, ang in enumerate((45, 225)):
        beacon(d, f'Beacon_Atrium_{k}', 1150 * math.sin(math.radians(ang)), h - 10, 1150 * math.cos(math.radians(ang)))
    d.point('Alarm_Red_Atrium', (0, 500, 0), RED, 2, 1700)
    d.particles('Alarm_Atrium_Smoke', 'Content/Particles/L2_Laboratory/L2_Smoke.nmparticle', (-800, 20, 700), rate=1.0)
    d.particles('Alarm_Atrium_Sparks', PART.format('SparksLoop'), (1000 * math.sin(math.radians(140)), 300,
                                                                    1000 * math.cos(math.radians(140))))
    for k, (x, z, yaw) in enumerate(((-760, 760, 30), (820, -260, 75))):
        d.prop(f'Alarm_Atrium_Rubble_{k}', DM.format('L2_Rubble'), (x, 0, z), yaw)
        d.blocker(f'Alarm_Atrium_RubbleBlock_{k}', (x, 15, z), (130, 30, 100), yaw)


def dress_workshop(d):
    x0, z0, x1, z1, h, _ = L.L2['rooms']['Workshop']
    for i, (bx, bz) in enumerate(L.L2['benches']):
        yaw = 0 if i == 0 else 180
        d.prop(f'K_Workbench_{i}', KM.format('Workbench'), (bx, 0, bz + (0 if i == 0 else 15)), yaw, (1.4, 1, 1))
    # Ten plinths in two rows facing north (the route side), a sample and its plaque on each.
    rows = [(-230, [-3000, -2700, -2400, -2100, -1800]), (-470, [-2550, -2250, -1950, -1650, -1350])]
    spots = [(x, z) for z, xs in rows for x in xs]
    breaks = {'Sample_FrostedGlass': 'Glass', 'Sample_LeafPanel': 'Glass', 'Sample_Marble': 'Stone',
              'Sample_Walnut': 'Wood', 'Sample_Wax': 'Wood'}
    samples = ['Sample_BrushedBrass', 'Sample_Walnut', 'Sample_Enamel', 'Sample_Velvet', 'Sample_Leather', 'Sample_Marble',
               'Sample_Wax', 'Sample_LeafPanel', 'Sample_FrostedGlass', 'Sample_Steam']
    for i, (sample, (x, z)) in enumerate(zip(samples, spots)):
        d.prop(f'K_Plinth_{i}', KM.format('SamplePedestal'), (x, 0, z), 180)
        d.blocker(f'K_PlinthBlock_{i}', (x, 46, z), (66, 92, 66))
        # The pedestal faces -Z turned 180: its label plate is on the north face at 78 cm, tilted back 35.
        d.prop(f'K_Plaque_{i}', DM.format('L2_Plaque_' + sample.split('_', 1)[1]), (x, 78.5, z + 34.2), rot=(35, 180, 0),
               shadows=False)
        extra = []
        if sample in breaks:
            extra = [d.lv.body(2), d.lv.fields('Destructible', Enabled=True, Mode=0, Structural=False, Anchored=True,
                                               Material=breaks[sample], Fracture='', Pieces=0, Seed=300 + i, Strength=0,
                                               Health=0, DebrisLifetime=0)]
        d.prop(f'K_Sample_{i}', KM.format(sample), (x, 92, z), 180 + (20 if sample == 'Sample_Enamel' else 0), extra=extra,
               static=not extra)
        if sample == 'Sample_LeafPanel':
            d.point('Calm_Sample_LeafBacklight', (x, 120, z + 12), (255, 220, 160), 3, 120, channel='L2.Power.Workshop',
                    source=4)
    # Alarm: glass cloches over the front row, ready to shatter.
    for i, (x, z) in enumerate(spots[:5]):
        d.prop(f'Alarm_Cloche_{i}', BOX, (x, 92 + 30, z), 0, (44, 58, 44), materials=[MAT.format('GlassClear')],
               shadows=False, extra=[d.lv.body(2), d.lv.fields('Destructible', Enabled=True, Mode=0, Structural=False,
                                                                 Anchored=True, Material='Glass', Fracture='', Pieces=0,
                                                                 Seed=400 + i, Strength=0, Health=0, DebrisLifetime=0)],
               static=False)
    # Gallery lights over the rows, pendants over the benches.
    for k, (z, xs) in enumerate(rows):
        light_strip(d, f'Workshop_Strip_{k}', sum(xs) / len(xs), z, h - 120, 0, 'L2.Power.Workshop', intensity=7)
    cc = ceiling_centres(x0, z0, x1, z1)
    pendant(d, 'Workshop_Lamp_0', cc[1][3][0], cc[1][3][1], h, 'L2.Power.Workshop')
    pendant(d, 'Workshop_Lamp_1', cc[3][3][0], cc[3][3][1], h, 'L2.Power.Workshop')
    pipe_run(d, 'K_Workshop_PipeW', z0 + 60, z1 - 60, (h - 70, x0 + T + 25), 'z')
    for k, z in enumerate((-500, 300)):
        gauge(d, f'K_Workshop_Gauge_{k}', (x0 + T + 26, h - 100, z), 270, 35 + 30 * k)
    d.point('Calm_Emergency_Workshop', (-2200, h - 40, 0), COOL, 2, 800, channel='L2.Power.All', show_when_on=False)
    probe(d, 'K_Probe_Workshop', (-2200, h / 2, 0), (1000, h / 2, 900))
    # Alarm: sprinklers, a beacon, red, sparks from a junction box.
    for k, (x, z) in enumerate(((-2800, -400), (-1700, -400), (-2800, 450), (-1700, 450))):
        d.particles(f'Alarm_Workshop_Sprinkler_{k}', PART.format('SprinklerSpray'), (x, h - 12, z), rot=(180, 0, 0),
                    detail='Medium' if k else None)
        d.prop(f'Alarm_Workshop_SprinklerHead_{k}', KM.format('Pipe_Tee'), (x, h - 10, z), rot=(180, 0, 0), scale=0.35,
               shadows=False)
    beacon(d, 'Beacon_Workshop', -2200, h - 4, 0)
    d.point('Alarm_Red_Workshop', (-2200, 300, 0), RED, 1, 1200)
    d.particles('Alarm_Workshop_Sparks', PART.format('SparksLoop'), (x0 + T + 20, h - 100, -500))


def dress_archive(d):
    x0, z0, x1, z1, h, _ = L.L2['rooms']['Archive']
    shelf, block = KM.format('Bookshelf'), (46, 250, 216)
    # The west wall, shelves end to end facing east.
    n = 7
    for k in range(n):
        z = z0 + T + 108 + k * (z1 - z0 - 2 * T - 216) / (n - 1)
        d.prop(f'K_Archive_ShelfW_{k}', shelf, (x0 + T + 38, 0, z), 270)
    d.blocker('K_Archive_ShelfWBlock', (x0 + T + 23, 125, (z0 + z1) / 2), (46, 250, z1 - z0 - 2 * T))
    # North and south walls either side of the doors.
    for k, x in enumerate((-2900, -2650, -1750, -1500)):
        d.prop(f'K_Archive_ShelfN_{k}', shelf, (x, 0, z1 - T - 38), 0)
        d.prop(f'K_Archive_ShelfS_{k}', shelf, (x, 0, z0 + T + 38), 180)
        d.blocker(f'K_Archive_ShelfNBlock_{k}', (x, 125, z1 - T - 23), (216, 250, 46))
        d.blocker(f'K_Archive_ShelfSBlock_{k}', (x, 125, z0 + T + 23), (216, 250, 46))
    # Two back-to-back rows out from the west wall, the aisle toward the route.
    for r, z in enumerate((1450, 2100)):
        for k, x in enumerate((-2980, -2764)):
            d.prop(f'K_Archive_Row_{r}_{k}_S', shelf, (x, 0, z - 36), 0)
            d.prop(f'K_Archive_Row_{r}_{k}_N', shelf, (x, 0, z + 36), 180)
        d.blocker(f'K_Archive_RowBlock_{r}', (-2872, 125, z), (432, 250, 76))
    # Velvet curtains in pairs on the east wall, as if over tall windows; a reading table.
    for k, z in enumerate((1300, 2200)):
        d.prop(f'K_Archive_Rod_{k}', DM.format('L2_CurtainRod'), (x1 - T - 12, h - 70, z), 90)
        for j, o in enumerate((-115, 115)):
            d.prop(f'K_Archive_Curtain_{k}_{j}', DM.format('L2_Curtain'), (x1 - T - 6, h - 70, z + o), 90 + (8 if j else -8))
    d.prop('K_Archive_Table', KM.format('Workbench'), (-1560, 0, 1750), 270)
    d.blocker('K_Archive_TableBlock', (-1560, 50, 1750), (125, 100, 200))
    fixture(d, 'Archive_TableLamp', DM.format('L2_HandLamp'), (-1560, 95, 1680), 0, 'L2.Pwr.Archive', (-1560, 122, 1680),
            (255, 175, 95), 5, 450, slots=2, source=5)
    cc = ceiling_centres(x0, z0, x1, z1)
    for k, (i, j) in enumerate(((1, 1), (3, 1), (1, 3), (3, 3))):
        pendant(d, f'Archive_Lamp_{k}', cc[i][j][0], cc[i][j][1], h, 'L2.Pwr.Archive', intensity=34)
    d.point('Calm_Emergency_Archive', (-2200, h - 40, 1750), COOL, 2, 800, channel='L2.Power.All', show_when_on=False)
    probe(d, 'K_Probe_Archive', (-2200, h / 2, 1750), (1000, h / 2, 850))
    # Alarm: sprinklers, a toppled shelf and spilled books, red, a beacon.
    for k, (x, z) in enumerate(((-2600, 1200), (-1800, 1200), (-2600, 2300), (-1800, 2300))):
        d.particles(f'Alarm_Archive_Sprinkler_{k}', PART.format('SprinklerSpray'), (x, h - 12, z), rot=(180, 0, 0),
                    detail='Medium' if k else None)
        d.prop(f'Alarm_Archive_SprinklerHead_{k}', KM.format('Pipe_Tee'), (x, h - 10, z), rot=(180, 0, 0), scale=0.35,
               shadows=False)
    d.prop('Alarm_Archive_Toppled', shelf, (-1640, 23, 1250), rot=(-90, 75, 0))
    d.blocker('Alarm_Archive_ToppledBlock', (-1640, 25, 1250), (240, 50, 120), 75)
    d.prop('Alarm_Archive_Rubble', DM.format('L2_Rubble'), (-1550, 0, 2300), 140)
    d.blocker('Alarm_Archive_RubbleBlock', (-1550, 15, 2300), (130, 30, 100), 140)
    beacon(d, 'Beacon_Archive', -2200, h - 4, 1750)
    d.point('Alarm_Red_Archive', (-2200, 300, 1750), RED, 1, 1200)


def dress_turbine_hall(d):
    x0, z0, x1, z1, h, _ = L.L2['rooms']['TurbineHall']
    p = L.L2['points']
    for i, (x, z) in enumerate(L.L2['turbines']):
        t = d.prop(f'K_Turbine_{i}', KM.format('Turbine'), (x, 0, z), 0)
        for prefix, channel in (('Calm_', 'L2.ValvesDone'), ('Alarm_', '')):
            d.prop(f'{prefix}Turbine_{i}_Rotor', KM.format('Turbine_Rotor'), (x, 160, z), 0,
                   extra=[d.spinner(channel, 0, 420, 5)], static=False)
            d.prop(f'{prefix}Turbine_{i}_Governor', KM.format('Turbine_Governor'), (x + 155, 289, z + 35), 0,
                   extra=[d.spinner(channel, 1, 105, 5)], static=False)
        # Light inside the casing's window, and over the flywheel.
        d.point(f'Calm_Turbine_{i}_Inner', (x - 10, 170, z - 40), (255, 190, 120), 4, 300, channel=f'L2.Pwr.Turbine{i + 1}',
                source=20)
        d.point(f'Alarm_Turbine_{i}_Inner', (x - 10, 170, z - 40), (255, 90, 50), 3, 300, source=20)
    # Valves at the north wall, under the interactables.
    for k in (1, 2, 3):
        vx = p[f'Valve{k}'][0]
        d.prop(f'K_Valve_{k}', KM.format('ValveWheel'), (vx, 0, z1 - T - 22), 0)
        d.prop(f'K_Valve_{k}_Wheel', KM.format('ValveWheel_Wheel'), (vx, 160, z1 - T - 22), 0)
        gauge(d, f'K_Valve_{k}_Gauge', (vx + 70, 190, z1 - T - 6), 0, 25 + 30 * k)
    d.hide(r'Calm_Valve\d_(Wheel|Spoke)')
    # Pipes from the turbines up the north wall; a header along it.
    pipe_run(d, 'K_Turbine_Header', x0 + 400, x1 - 200, (h - 110, z1 - T - 30), 'x')
    pipe_run(d, 'K_Turbine_Header2', x0 + 400, x1 - 200, (h - 160, z1 - T - 30), 'x')
    # The freight lift at the west end.
    d.prop('K_FreightLift', K3.format('FreightLift'), (-2950, 0, 3400), 270)
    # Area-light bars down the hall.
    for k, x in enumerate((-2200, -900, 400, 1700)):
        light_strip(d, f'Turbine_Strip_{k}', x, 3400, h - 140, 0, f'L2.Pwr.Turbine{min(3, k + 1) if k < 3 else 3}',
                    intensity=14, rng=1600)
    # Riser pipes from each turbine's inlet to the header, gauge boards and lockers on the south wall.
    for i, (x, z) in enumerate(L.L2['turbines']):
        for k, dx in enumerate((-120, 120)):
            d.prop(f'K_Turbine_{i}_Riser_{k}', KM.format('Pipe_Straight'), (x + dx, (445 + h - 110) / 2, z1 - T - 30),
                   rot=(0, 0, 90), scale=((h - 110 - 445) / 200.0 + 0.1, 1.4, 1.4), shadows=False)
            d.prop(f'K_Turbine_{i}_Feed_{k}', KM.format('Pipe_Straight'), (x + dx, 445, (z + 200 + z1 - T - 30) / 2), 90,
                   ((z1 - T - 30 - z - 200) / 200.0, 1.4, 1.4), shadows=False)
    for k, x in enumerate((-2600, -1600, 1300, 2400)):
        d.prop(f'K_Turbine_Board_{k}', KM.format('BreakerPanel'), (x, 0, z0 + T), 180)
        d.blocker(f'K_Turbine_BoardBlock_{k}', (x, 90, z0 + T + 18), (100, 180, 36))
        for j, dx in enumerate((-160, 160)):
            gauge(d, f'K_Turbine_BoardGauge_{k}_{j}', (x + dx, 170, z0 + T + 6), 180, 20 + 37 * (j + k))
    for k, x in enumerate((-400, 2600)):
        d.prop(f'K_Turbine_Locker_{k}', K3.format('Locker'), (x, 0, z0 + T + 2), 180)
        d.prop(f'K_Turbine_Locker_{k}_Door', K3.format('Locker_Door'), (x + 29, 0, z0 + T + 2), 180)
        d.blocker(f'K_Turbine_LockerBlock_{k}', (x, 95, z0 + T + 25), (60, 190, 50))
    d.point('Calm_Emergency_Turbine', (-200, h - 40, 3400), COOL, 2.5, 1200, channel='L2.Power.All', show_when_on=False)
    for k, (cx, ex) in enumerate(((-2200, 1000), (0, 1200), (2000, 800))):
        probe(d, f'K_Probe_Turbine_{k}', (cx, h / 2, 3400), (ex, h / 2, 800))
    # Alarm: beacons, red, steam jets from the header, smoke, sparks, a fallen ceiling panel.
    for k, x in enumerate((-2200, -200, 1800)):
        beacon(d, f'Beacon_Turbine_{k}', x, h - 4, 3000)
        d.point(f'Alarm_Red_Turbine_{k}', (x, 350, 3400), RED, 2, 1300)
    for k, x in enumerate((-1500, 300, 1400)):
        d.particles(f'Alarm_Turbine_SteamJet_{k}', PART.format('SteamJet'), (x, h - 110, z1 - T - 30), rot=(90, 0, 0))
    d.particles('Alarm_Turbine_Smoke', 'Content/Particles/L2_Laboratory/L2_Smoke.nmparticle', (2400, 20, 2900), rate=1.2)
    for k, (x, y, z) in enumerate(((-1500, 330, z0 + T + 10), (900, 260, z1 - T - 10))):
        d.particles(f'Alarm_Turbine_Sparks_{k}', PART.format('SparksLoop'), (x, y, z))
    d.prop('Alarm_Turbine_FallenPanel', KM.format('Ceiling'), (2300, 30, 3900), rot=(14, 30, 8), scale=(0.8, 1, 0.8))
    d.blocker('Alarm_Turbine_FallenBlock', (2300, 40, 3900), (300, 80, 300), 30)
    d.prop('Alarm_Turbine_Rubble', DM.format('L2_Rubble'), (-1700, 0, 2800), 20)
    d.blocker('Alarm_Turbine_RubbleBlock', (-1700, 15, 2800), (130, 30, 100), 20)


def dress_elevator(d):
    x0, z0, x1, z1, h, _ = L.L2['rooms']['Elevator']
    d.prop('K_ElevatorCage', KM.format('ElevatorCage'), (2450, 0, 1800), 90)
    d.prop('K_ElevatorCage_Gate', KM.format('ElevatorCage_Gate'), (2450 - 127, 0, 1800), 90)
    d.blocker('K_ElevatorCageBlock', (2450, 175, 1800), (262, 350, 262))
    cc = ceiling_centres(x0, z0, x1, z1)
    pendant(d, 'Elevator_Lamp_0', cc[1][1][0], cc[1][1][1], h, 'L2.Pwr.Elevator')
    pendant(d, 'Elevator_Lamp_1', cc[1][2][0], cc[1][2][1], h, 'L2.Pwr.Elevator')
    pipe_run(d, 'K_Elevator_Pipe', z0 + 60, z1 - 60, (h - 70, x0 + T + 25), 'z')
    d.point('Calm_Emergency_Elevator', (2000, h - 40, 1800), COOL, 2, 700, channel='L2.Power.All', show_when_on=False)
    probe(d, 'K_Probe_Elevator', (2000, h / 2, 1800), (800, h / 2, 800))
    d.point('Alarm_Red_Elevator', (2000, 300, 1800), RED, 1, 1000)


def dress_conservatory(d):
    x0, z0, x1, z1, h, _ = L.L2['rooms']['Conservatory']
    glass, brass = MAT.format('GlassClear'), MAT.format('BrassAged')
    # A glass roof on brass ribs in place of the ceiling.
    if not d.review:
        d.prop('K_Conservatory_Roof', BOX, ((x0 + x1) / 2, h + 4, (z0 + z1) / 2), 0, (x1 - x0, 4, z1 - z0),
               materials=[glass], shadows=False)
        for k in range(6):
            x = x0 + (x1 - x0) * k / 5
            d.prop(f'K_Conservatory_Rib_{k}', BOX, (x, h, (z0 + z1) / 2), 0, (10, 16, z1 - z0), materials=[brass])
        for k in range(5):
            z = z0 + (z1 - z0) * k / 4
            d.prop(f'K_Conservatory_Purlin_{k}', BOX, ((x0 + x1) / 2, h - 2, z), 0, (x1 - x0, 10, 8), materials=[brass])
    d.hide(r'Conservatory_Ceiling')
    # Planting beds and pots.
    beds = [(1750, -650, 0), (2650, -650, 0), (1750, 650, 180), (2650, 650, 180), (3000, 0, 90)]
    for k, (x, z, yaw) in enumerate(beds):
        d.prop(f'K_Conservatory_Bed_{k}', DM.format('L2_Planter'), (x, 0, z), yaw)
        for j, o in enumerate((-70, 0, 70)):
            ux, uz = math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))
            mesh = DM.format('L2_Fern' if (j + k) % 2 else 'L2_Palm')
            d.prop(f'K_Conservatory_Plant_{k}_{j}', mesh, (x + ux * o, 64, z + uz * o), k * 47 + j * 113,
                   1.15 if 'Palm' in mesh else 1.3)
    for k, (x, z) in enumerate(((1500, 0), (2200, 300), (2200, -300))):
        d.prop(f'K_Conservatory_Pot_{k}', DM.format('L2_Palm'), (x, 0, z), k * 80, 1.4)
    d.prop('K_Conservatory_Tank', DM.format('L2_Tank'), (2500, 0, 0), 270)
    d.particles('K_Conservatory_Bubbles', DPART.format('L2_Bubbles'), (2500, 84, 0), rot=(0, 90, 0), detail='High')
    d.particles('K_Conservatory_Mist', DPART.format('L2_Mist'), ((x0 + x1) / 2, 20, (z0 + z1) / 2), detail='Medium')
    d.prop('K_Conservatory_Vent', K1.format('SteamVent'), (3050, 0, 750), 0)
    d.particles('K_Conservatory_Steam', PART.format('SteamVent'), (3050, 30, 750), rate=0.6, detail='Medium')
    d.point('Calm_Conservatory_Grow_0', (1900, h - 120, 0), (255, 214, 170), 30, 1300, channel='L2.Pwr.Conservatory')
    d.point('Calm_Conservatory_Grow_1', (2800, h - 120, 0), (220, 255, 200), 24, 1200, channel='L2.Pwr.Conservatory')
    d.point('Alarm_Red_Conservatory', (2200, 300, 0), RED, 1, 1000)
    probe(d, 'K_Probe_Conservatory', (2200, h / 2, 0), (1000, h / 2, 1000))
    # Its west wall is the atrium's window: kit wall only beyond the glass.
    d.drop(r'Conservatory_WallW_\d+')
    side = 2 * L.L2['atrium']['apothem'] * math.tan(math.radians(22.5))
    span = (z1 - z0 - 2 * T - side) / 2
    for k, sgn in enumerate((-1, 1)):
        zc = sgn * (side / 2 + span / 2)
        d.prop(f'K_Conservatory_WallW_{k}', KM.format('Wall'), (x0 + T, 0, zc), 270, (span / 400.0, h / 500.0, 1))


def dress_gameplay(d):
    """Kit looks on the gameplay entities (their Render only)."""
    d.hide(r'Calm_Breaker')
    d.restyle(r'Calm_Keycard', material=MAT.format('BrassAged'), scale=(9, 0.8, 6))
    d.restyle(r'Calm_KeycardReader', material=MAT.format('PanelCream'), scale=(18, 26, 8))
    d.restyle(r'Calm_ElevatorPanel', material=MAT.format('PanelGreen'), scale=(45, 70, 14))
    d.restyle(r'Alarm_RoofHatch', mesh=CYL, material=MAT.format('BrassAged'), scale=(160, 12, 160))
    d.restyle(r'Alarm_AmmoCrate_\w+', material=MAT.format('Planks'))
    for gate, size, yaw in (('Calm_FoyerDoor', (300, 330, 30), 0), ('Calm_KeycardDoor', (240, 330, 40), 0),
                            ('Alarm_Shutter_Archive', (300, 330, 40), 0), ('Alarm_Shutter_Workshop', (40, 330, 300), 90),
                            ('Alarm_Shutter_Elevator', (300, 330, 60), 0)):
        d.hide(gate)
        sx, sy, sz = size
        scale = (1.0 / 170.0, 1.0 / 259.0, 1.0 / (sx if yaw else sz))
        state = gate.split('_', 1)[0] + '_'
        d.prop(f'{state}K_{gate.split("_", 1)[1]}_Shutter', KM.format('WallDoor_Shutter'), (0, -0.5, 0), yaw, scale,
               parent=gate, static=False)


# =============================================================================
# C3: the power-up, rewritten with the light cascade and the lever
# =============================================================================
CASCADE = [(36, 'L2.Power.Foyer'), (46, 'L2.Pwr.Foyer2'), (80, 'L2.Power.Atrium'), (88, 'L2.Pwr.Atrium2'),
           (96, 'L2.Pwr.Atrium3'), (104, 'L2.Pwr.Conservatory'), (112, 'L2.Power.Workshop'), (122, 'L2.Pwr.Archive'),
           (130, 'L2.Pwr.Turbine1'), (136, 'L2.Pwr.Turbine2'), (142, 'L2.Pwr.Turbine3'), (148, 'L2.Pwr.Elevator'),
           (152, 'L2.Power.All')]


def rewrite_c3(d):
    ml, lv = d.ml, d.lv
    shots_a = [(0, (-430, 210, -1560), (-870, 140, -1800)), (40, (-470, 220, -1620), (-870, 150, -1800)),
               (76, (-300, 300, -1450), (-700, 260, -2000))]
    shots_b = [(76, (900, 330, -950), (0, 380, 200)), (130, (500, 520, -1050), (-500, 330, 150)),
               (180, (-150, 620, -1000), (-600, 300, 300))]
    body = ml.camera_track('cam0', lv.guid('Calm_CineCam_C3_Breaker_A'), 'Calm_CineCam_C3_Breaker_A', shots_a)
    body += ml.camera_track('cam1', lv.guid('Calm_CineCam_C3_Breaker_B'), 'Calm_CineCam_C3_Breaker_B', shots_b)
    body += [f'  <Binding id="lever" kind="possess" guid="{lv.guid("Calm_BreakerLever")}" name="Calm_BreakerLever">',
             '    <Track type="Transform">']
    for axis in ('X', 'Y', 'Z'):
        body += ml.channel_keys(f'Position.{axis}', [(0, 0), (180, 0)])
    body += ml.channel_keys('Rotation.X', [(0, -35), (26, -35), (34, 35)])
    body += ['    </Track>', '  </Binding>']
    body += ['  <Track type="CameraCut" name="Shots">', '    <Section start="0" end="76" camera="cam0"/>',
             '    <Section start="76" end="180" camera="cam1"/>', '  </Track>']
    body += ['  <Track type="Fade">', '    <Key f="0" v="0"/>', '  </Track>']
    body += ['  <Track type="Subtitle">',
             '    <Section start="10" end="90" speaker="Pilot" text="Power is back. Look at that place light up."/>',
             '  </Track>']
    body += ['  <Track type="Audio">', f'    <Section start="20" end="180" sound="{ml.SND}World/Power_Up_Sweep.nmsound" volume="1"/>',
             '  </Track>']
    body.append('  <Track type="Event">')
    for fr, ch in CASCADE + [(178, 'L2.Powered'), (179, 'Checkpoint.Powered')]:
        body.append(f'    <Key f="{fr}" event="Lab.Signal" arg0="{ch}" arg1="true" fireOnSkip="true"/>')
    body.append('  </Track>')
    ml.write_seq('C3_Breaker', 180, body)


# =============================================================================
def dress(lv, ml):
    d = Dresser(lv, ml)
    rooms = L.L2['rooms']

    # The grey box: collision stays, hidden; placeholders without collision go.
    d.hide(r'(Airlock|Foyer|Workshop|Archive|TurbineHall|Elevator|Conservatory|Atrium)_(Floor|Ceiling|Wall\w*|Sill\w*)(_\d+)?')
    d.drop(r'Atrium_Ceiling|Orrery_Ring\d|Orrery_Sun|Sample_\w+|Plant_\d+|FreightLift_Platform|Ambient')
    d.drop(r'Calm_Light_\w+|Calm_Emergency_\w+|Alarm_Red_\w+')
    d.hide(r'Orrery_Base|Gantry_Step_\d+|Gantry_Platform|Turbine_\d|Bench_\d')

    for name, (x0, z0, x1, z1, h, doors) in rooms.items():
        rect_room_kit(d, name, x0, z0, x1, z1, h, doors, ceiling=(name != 'Conservatory'),
                      skip=('W',) if name == 'Conservatory' else ())
    atrium_kit(d)
    dress_gameplay(d)
    dress_foyer_airlock(d)
    dress_atrium(d)
    dress_workshop(d)
    dress_archive(d)
    dress_turbine_hall(d)
    dress_elevator(d)
    dress_conservatory(d)

    # Light and air: the sun (mid-afternoon calm, dusk on the alarm), the GI volumes, the stacks.
    sun(d, 'Calm_', 15.2, 0.2)
    sun(d, 'Alarm_', 19.3, 0.25)
    gi_volume(d, 'K_GI_Core', (0, 420, -600), (1250, 420, 1880), 350)
    gi_volume(d, 'K_GI_Workshop', (-2200, 295, 0), (985, 295, 885), 300)
    gi_volume(d, 'K_GI_Archive', (-2200, 295, 1750), (985, 295, 835), 300)
    gi_volume(d, 'K_GI_TurbineW', (-2200, 350, 3400), (1000, 350, 780), 350)
    gi_volume(d, 'K_GI_TurbineMid', (0, 350, 3400), (1200, 350, 780), 350)
    gi_volume(d, 'K_GI_TurbineE', (2000, 350, 3400), (800, 350, 780), 350)
    gi_volume(d, 'K_GI_Elevator', (2000, 295, 1800), (785, 295, 785), 300)
    post(d, 'Calm_PostProcess', 'Content/PostProcess/L2_Laboratory/L2_Calm.pp.nmgraph')
    post(d, 'Alarm_PostProcess', 'Content/PostProcess/L2_Laboratory/L2_Alarm.pp.nmgraph')

    rewrite_c3(d)
