"""
dress_l1.py -- L1 Foothills' dressing (and the MainMenu backdrop, which is the
same plaza), called from make_levels.py's make_l1() / make_menu() just before
the world is written. It replaces the grey box with:

    Ground     a 385 x 385 terrain a metre apart (Content/Levels/L1_Foothills/
               Terrain/Ground.nmterrain, Terrain/Island material): the sea floor,
               the beach, foothills rising north to the plaza, the cliff over it
               and a highland behind; the road cut and filled to its spline's
               heights; painted Grass / Dirt (the road, the plaza) / Rock (steep
               ground, the cliff, the scree) / Sand (the beach, under the sea).
               A heightfield Rigid Body walks the player on it.
    Road       the one Spline (layout.L1['road']): its lamps (a Spline Path
               spawner), and on copies of it the guard rails (uplands) and the
               fence posts (lowlands); RoadClear keeps the plants off it
    Forest     spruce Box spawners (slope, height, layer, noise), the meadow's
               grass and wildflowers (Spline Area, noise, layer), scree (Box on
               the Rock layer under the cliff, slope-limited)
    Kit        the pier, the gatehouse with its crank and barrier arm, the
               facade, the blast door (frame, leaves, cog: C2 animates them),
               the cliff walls and pieces, the signal mast and its anemometer,
               steam vents, rocks
    Sea        the kit's Sea_Plane tiles, Sea_Far and foam strips along the beach
    Light      golden hour (Time Of Day 17.5), the post-process stack (fog, SSR,
               SSGI, grade), a GI volume, reflection probes, the road lamps'
               lights and the plaza's floodlights (on with L1.Plaza)

Gameplay entities (entries, triggers, the crank, the gate, the blast door,
routes, cinematics) stay make_levels.py's; this patches the gate and the blast
door to invisible blockers behind the kit's barrier arm and door leaves.
"""
import hashlib
import math
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout as L  # noqa: E402

GAME = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TERRAIN_FILE = 'Content/Levels/L1_Foothills/Terrain/Ground.nmterrain'
ISLAND = 'Content/Materials/Terrain/Island.material.nmgraph'
K = 'Content/Meshes/L1/'
FOL = 'Content/Meshes/Foliage/'
SND = 'Content/Sounds/'

RES, SP = 385, 100.0
OX, OZ = -16000.0, -12000.0          # the terrain's corner (sample 0, 0)
SEA = L.L1['sea_level']
D = L.L1


# =============================================================================
# The road's curve (GameCore's centripetal Catmull-Rom, open, mirrored ends)
# =============================================================================
def catmull_rom(points, steps=24):
    p = [np.array(q, dtype=np.float64) for q in points]
    ext = [2 * p[0] - p[1]] + p + [2 * p[-1] - p[-2]]
    out = []
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        t0 = 0.0
        t1 = t0 + max(math.sqrt(np.linalg.norm(p1 - p0)), 1e-4)
        t2 = t1 + max(math.sqrt(np.linalg.norm(p2 - p1)), 1e-4)
        t3 = t2 + max(math.sqrt(np.linalg.norm(p3 - p2)), 1e-4)
        for s in range(steps):
            u = t1 + (t2 - t1) * s / steps
            a1 = p0 + (p1 - p0) * (u - t0) / (t1 - t0)
            a2 = p1 + (p2 - p1) * (u - t1) / (t2 - t1)
            a3 = p2 + (p3 - p2) * (u - t2) / (t3 - t2)
            b1 = a1 + (a2 - a1) * (u - t0) / (t2 - t0)
            b2 = a2 + (a3 - a2) * (u - t1) / (t3 - t1)
            out.append(b1 + (b2 - b1) * (u - t1) / (t2 - t1))
    out.append(p[-1])
    return np.array(out)


ROAD = catmull_rom(D['road'])


def road_samples(spacing, offset=0.0):
    """Points every spacing along the road (arc length on the curve), moved offset to the right:
    [(x, z, heading x, heading z)]."""
    seg = np.linalg.norm(np.diff(ROAD, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    out = []
    for d in np.arange(0.0, s[-1] + 1e-3, spacing):
        i = int(min(max(np.searchsorted(s, d) - 1, 0), len(seg) - 1))
        f = (d - s[i]) / seg[i]
        q = ROAD[i] + (ROAD[i + 1] - ROAD[i]) * f
        t = ROAD[i + 1] - ROAD[i]
        hx, hz = t[0], t[2]
        n = math.hypot(hx, hz)
        hx, hz = hx / n, hz / n
        out.append((q[0] + hz * offset, q[2] - hx * offset, hx, hz))
    return out


def nearest_on_road(wx, wz):
    """Per grid point: the distance (X/Z) to the road's centreline and the road's height there."""
    best = np.full(wx.shape, 1e30)
    y = np.zeros(wx.shape)
    for a, b in zip(ROAD[:-1], ROAD[1:]):
        dx, dz = b[0] - a[0], b[2] - a[2]
        l2 = max(dx * dx + dz * dz, 1e-9)
        t = np.clip(((wx - a[0]) * dx + (wz - a[2]) * dz) / l2, 0.0, 1.0)
        px, pz = a[0] + dx * t - wx, a[2] + dz * t - wz
        d2 = px * px + pz * pz
        closer = d2 < best
        best = np.where(closer, d2, best)
        y = np.where(closer, a[1] + (b[1] - a[1]) * t, y)
    return np.sqrt(best), y


# =============================================================================
# The terrain
# =============================================================================
def ss(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def fbm(x, z, octaves=5, seed=7):
    rng = np.random.default_rng(seed)
    total = np.zeros_like(x)
    amp, freq, norm = 1.0, 1.0, 0.0
    for _ in range(octaves):
        grid = rng.random((70, 70))
        u, v = x * freq, z * freq
        x0, z0 = np.floor(u).astype(int) % 64, np.floor(v).astype(int) % 64
        fx, fz = u - np.floor(u), v - np.floor(v)
        fx, fz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
        a, b = grid[z0, x0], grid[z0, x0 + 1]
        c, d = grid[z0 + 1, x0], grid[z0 + 1, x0 + 1]
        total += ((a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz) * amp
        norm += amp
        amp *= 0.5
        freq *= 2.0
    return total / norm * 2.0 - 1.0


def rect_distance(wx, wz, x0, z0, x1, z1):
    dx = np.maximum(np.maximum(x0 - wx, wx - x1), 0.0)
    dz = np.maximum(np.maximum(z0 - wz, wz - z1), 0.0)
    return np.hypot(dx, dz)


def cliff_line(x):
    """Where the cliff's face starts (Z) along X: set back behind the facade (its back runs to ~7120)."""
    behind = 300.0 * (1 - ss(1900, 3000, np.abs(x - 3700)))
    return 7080.0 + behind + 100.0 * np.sin(x / 650.0) + 60.0 * np.sin(x / 230.0 + 1.0)


_CACHE = {}


def build_terrain():
    """Heights (cm) and paint (Grass, Dirt, Rock, Sand) on the grid; rows are Z, columns X."""
    if 'h' in _CACHE:
        return _CACHE['h'], _CACHE['w']
    idx = np.arange(RES, dtype=np.float64)
    I, J = np.meshgrid(idx, idx)
    wx, wz = OX + I * SP, OZ + J * SP

    # Foothills rising north (the road's heights), hills east, the mast's ridge.
    f = np.interp(wz, [-4000, -2600, -1500, -150, 800, 1800, 2650, 3650, 5700, 7000, 12000],
                  [-700, -300, 0, 200, 300, 495, 640, 850, 1200, 1250, 1500])
    ridge_x = D['mast'][0] + 0.25 * (wz - D['mast'][1])
    ridge = 380.0 * np.exp(-((wx - ridge_x) / 900.0) ** 2) * ss(-500, 1500, wz) * (1 - ss(6000, 7500, wz))
    hills = 900.0 * ss(7500, 14000, wx) * (0.6 + 0.4 * fbm(wx / 3000.0 + 7, wz / 3000.0 + 3, 3, 3))
    land = f + ridge + hills
    # The coast: the beach shelf, the sea floor falling away south-west.
    u = 0.85 * (wx + 1200) + 0.53 * (wz + 1500)
    coast = -120 + 150 * ss(-900, 400, u) + 0.35 * np.maximum(0, u - 400) - 0.1 * np.maximum(0, -u - 300)
    k = 150.0
    hm = np.clip(0.5 + 0.5 * (coast - land) / k, 0, 1)
    h = coast * (1 - hm) + land * hm - k * hm * (1 - hm)          # a smooth min
    # Ground noise: gentle near the play area, rolling away from it.
    near = rect_distance(wx, wz, -3000, -2500, 7000, 7200)
    amp = 70.0 + 330.0 * ss(0, 6000, near)
    h += fbm(wx / 1400.0, wz / 1400.0, 5, 11) * amp * ss(-600, 200, u)
    # The road: cut and filled to its spline, the shoulders blending out.
    dist, ry = nearest_on_road(wx, wz)
    half = D['road_width'] / 2
    wroad = 1 - ss(half + 40, half + 750, dist)
    h = h * (1 - wroad) + ry * wroad
    # Pads: the plaza, the gatehouse landing, the overlook.
    px0, pz0, px1, pz1, ptop = D['plaza']
    wpl = 1 - ss(0, 600, rect_distance(wx, wz, px0, pz0, px1, pz1 + 600))
    h = h * (1 - wpl) + ptop * wpl
    gx, gz = D['gatehouse_centre']
    wg = 1 - ss(0, 500, rect_distance(wx, wz, gx - 1000, gz - 750, gx + 500, gz + 400))
    h = h * (1 - wg) + 650 * wg
    ox, oz = D['overlook']
    wo = 1 - ss(250, 650, np.hypot(wx - ox, wz - oz))
    h = h * (1 - wo) + 495 * wo
    # The cliff over the plaza and the highland behind it; scree banked at its foot.
    zc = cliff_line(wx)
    across = np.abs(wx - 3700)
    amp_c = 1 - ss(3800, 7500, across)
    in_plaza = (1 - ss(0, 500, rect_distance(wx, wz, px0 - 200, pz0, px1 + 200, pz1 + 1000)))
    talus = 480 * ss(zc - 1000, zc - 40, wz) * amp_c * (1 - in_plaza)
    h = h + talus * ss(-300, 0, -(wz - zc))
    top = 3700 + 0.06 * np.maximum(wz - 7200, 0) + fbm(wx / 900.0, wz / 900.0, 4, 5) * 260
    tc = ss(zc - 70, zc + 240, wz) * amp_c
    h = h + tc * np.maximum(top - h, 0)
    # The island's far edges go down into the sea.
    r = np.hypot((wx - 4500) / 15500.0, (wz - 7000) / 17500.0)
    fall = ss(0.82, 1.05, r)
    h = h * (1 - fall) + (-900) * fall

    # Paint.
    gzr, gxr = np.gradient(h, SP)
    slope = np.hypot(gxr, gzr)
    rock = np.maximum(ss(0.8, 1.2, slope), np.clip(tc * (1 - ss(0.3, 0.6, 1 - amp_c)) * 1.4, 0, 1))
    rock = np.maximum(rock, ss(150, 420, talus) * 0.85)
    sand = (1 - ss(40, 120, h - SEA - 120)) * (1 - rock)
    patchy = ss(0.25, 0.6, fbm(wx / 900.0 + 3, wz / 900.0 + 9, 4, 21))
    dirt = np.maximum(1 - ss(half - 60, half + 120, dist), np.maximum(wpl, wg * 0.8))
    dirt = np.maximum(dirt, patchy * 0.35 + ss(80, 300, talus) * 0.5) * (1 - rock * 0.7) * (1 - sand)
    grass = np.clip(1 - rock - sand - dirt, 0, 1)
    w = np.stack([grass, dirt, rock, sand], axis=-1)
    w = w / np.maximum(w.sum(axis=-1, keepdims=True), 1e-6)
    q = np.floor(w * 255.0 + 0.5).astype(np.int32)
    q[..., 0] += 255 - q.sum(axis=-1)
    q = np.clip(q, 0, 255).astype(np.uint8)
    _CACHE['h'], _CACHE['w'] = h, q
    return h, q


def height_at(x, z):
    """The terrain's height at a world X/Z, bilinear (the render mesh's diagonal split aside)."""
    h, _ = build_terrain()
    fx, fz = (x - OX) / SP, (z - OZ) / SP
    i, j = int(math.floor(fx)), int(math.floor(fz))
    i, j = min(max(i, 0), RES - 2), min(max(j, 0), RES - 2)
    tx, tz = fx - i, fz - j
    return float(h[j, i] * (1 - tx) * (1 - tz) + h[j, i + 1] * tx * (1 - tz) + h[j + 1, i] * (1 - tx) * tz
                 + h[j + 1, i + 1] * tx * tz)


def chunk(name, payload, version=1):
    tag = (ord(name[0]) << 24) | (ord(name[1]) << 16) | (ord(name[2]) << 8) | ord(name[3])
    return struct.pack('<IHI', tag, version, len(payload)) + payload


def string(text):
    b = text.encode()
    return struct.pack('<I', len(b)) + b


def write_terrain():
    h, w = build_terrain()
    lo, hi = float(h.min()), float(h.max())
    samples = np.clip(np.rint((h - lo) * 65535.0 / (hi - lo)), 0, 65535).astype('<u2')
    body = (chunk('THDR', struct.pack('<Ifff', RES, SP, lo, hi)) + chunk('HGHT', samples.tobytes())
            + chunk('SPLT', w.tobytes()) + chunk('LAYR', b''.join(string(s) for s in ('Grass', 'Dirt', 'Rock', 'Sand'))))
    path = os.path.join(GAME, TERRAIN_FILE)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as out:
        out.write(chunk('NTRN', body))
    return lo, hi


# =============================================================================
# World editing helpers (over make_levels.Level)
# =============================================================================
def name_of(text):
    return re.search(r' Name="([^"]*)"', text).group(1)


def remove(lv, pattern):
    rx = re.compile(pattern)
    keep = []
    for g, text in lv.entities:
        n = name_of(text)
        if rx.fullmatch(n):
            lv.names.discard(n)
        else:
            keep.append((g, text))
    lv.entities = keep


def patch(lv, name, fn):
    for i, (g, text) in enumerate(lv.entities):
        if name_of(text) == name:
            lv.entities[i] = (g, fn(text))
            return
    raise KeyError(name)


def set_field(text, field, value):
    out, n = re.subn(rf'<{field} Value="[^"]*" />', f'<{field} Value="{value}" />', text, count=1)
    assert n == 1, field
    return out


def set_transform(text, pos, rot=(0, 0, 0), scale=(1, 1, 1)):
    from make_levels import Level
    block = '\n'.join(Level.transform(pos, rot, scale))
    return re.sub(r'  <Component Type="Transform".*?</Component>', lambda m: block, text, count=1, flags=re.S)


def mesh(lv, name, path, pos, yaw=0.0, collide=False, parent=None, shadows=True, scale=1.0, rot=None, detail=None,
         params=(), static=True, material=None):
    comps = [lv.render(path, shadows=shadows, material=material), lv.transform(pos, rot or (0, yaw, 0), (scale, scale, scale))]
    if params:
        r = comps[0]
        extra = [f'    <ParameterCount Value="{len(params)}" />']
        for i, (pname, slot, v) in enumerate(params):
            v = tuple(v) + (0,) * (4 - len(v))
            extra += [f'    <Parameter{i}Name Value="{pname}" />', f'    <Parameter{i}Slot Value="{slot}" />',
                      f'    <Parameter{i}X Value="{v[0]}" />', f'    <Parameter{i}Y Value="{v[1]}" />',
                      f'    <Parameter{i}Z Value="{v[2]}" />', f'    <Parameter{i}W Value="{v[3]}" />']
        comps[0] = r[:-2] + extra + r[-2:]
    if collide:
        comps.insert(1, lv.body(2))
    n = lv.entity(name, comps, parent, static=static and parent is None)
    if detail:
        g = lv.guid(n)
        lv.entities = [(gg, t.replace(f'Name="{n}"', f'Name="{n}" DetailMode="{detail}"', 1) if gg == g else t)
                       for gg, t in lv.entities]
    return n


def colours(comp, *keys):
    """lv.fields writes a 3-tuple as a vector unless its key ends in Color: these keys are colours."""
    out = []
    for line in comp:
        for k in keys:
            m = re.fullmatch(rf'(\s*)<{k} X="([^"]*)" Y="([^"]*)" Z="([^"]*)" />', line)
            if m:
                line = f'{m.group(1)}<{k} R="{m.group(2)}" G="{m.group(3)}" B="{m.group(4)}" A="255" />'
        out.append(line)
    return out


def spline_comp(lv, points, closed=False, width=0.0, linear=False):
    vals = {'Closed': closed, 'Type': 1 if linear else 0, 'Width': width, 'PointCount': len(points)}
    for i, p in enumerate(points):
        vals[f'Point{i}'] = tuple(float(c) for c in p)
    return lv.fields('Spline', **vals)


def spawner_comp(lv, fields, meshes, exclusions=(), box=(1000, 500, 1000)):
    values = {
        'Visible': True, 'Source': 2, 'Project': 1, 'TerrainLayer': 0, 'Seed': 1, 'BoxExtent': tuple(box), 'Spacing': 200,
        'Jitter': 1, 'ProjectRange': 5000, 'PathOffset': 0, 'BothSides': False, 'Density': 1, 'NoiseScale': 0,
        'NoiseThreshold': 0.5, 'MaxSlope': 90, 'MinHeight': -1000000, 'MaxHeight': 1000000, 'LayerThreshold': 0.5,
        'ScaleMin': 0.8, 'ScaleMax': 1.2, 'AlignToNormal': 0, 'Sink': 0, 'ColorVariation': 0.1, 'RandomYaw': True,
        'AlignToPath': False, 'CastShadows': True, 'CellSize': 4000, 'LODDistance': 0, 'LOD1Density': 0.5,
        'LOD2Density': 0.25, 'CullDistance': 0, 'MaxInstances': 200000,
    }
    values.update(fields)
    values['MeshCount'] = len(meshes)
    for i, (m, weight, scale, far) in enumerate(meshes):
        values[f'Mesh{i}Path'] = m
        values[f'Mesh{i}Far'] = far
        values[f'Mesh{i}Material'] = ''
        values[f'Mesh{i}Weight'] = weight
        values[f'Mesh{i}Scale'] = scale
    values['ExclusionCount'] = len(exclusions)
    for i, n in enumerate(exclusions):
        values[f'Exclusion{i}'] = n
    return lv.fields('Procedural Spawner', **values)


def point_light(lv, name, pos, color, intensity, radius, toggle=None, shadows=False, source=None):
    comps = [colours(lv.fields('Point Light', version=2, Color=color, Intensity=intensity, Radius=radius, Active=True,
                       CastShadows=shadows, ShadowTechnique=0, ShadowTint=(40, 50, 80), ShadowBias=0.004,
                       ShadowNormalBias=2, SourceRadius=source if source is not None else 8), 'ShadowTint'),
             lv.transform(pos)]
    if toggle:
        comps.insert(1, lv.fields('LabSignalToggle', Channel=toggle, ShowWhenOn=True))
    return lv.entity(name, comps)


def spot_light(lv, name, pos, target, color, intensity, rng, inner, outer, toggle=None, shadows=True):
    from make_levels import look
    comps = [colours(lv.fields('Spot Light', version=2, Color=color, Intensity=intensity, Range=rng, InnerConeAngle=inner,
                       OuterConeAngle=outer, Active=True, CastShadows=shadows, ShadowTechnique=0,
                       ShadowTint=(20, 22, 28), ShadowBias=0.002, ShadowNormalBias=1.5, SourceRadius=10), 'ShadowTint'),
             lv.transform(pos, look(pos, target))]
    if toggle:
        comps.insert(1, lv.fields('LabSignalToggle', Channel=toggle, ShowWhenOn=True))
    return lv.entity(name, comps)


def particles(lv, name, system, pos, rot=(0, 0, 0)):
    return lv.entity(name, [colours(lv.fields('Particle System', version=3, System=system, Emitting=True,
                                              Tint=(255, 255, 255), SpawnRateScale=1, TimeScale=1, Seed=0,
                                              UseBeamTarget=False, BeamTarget=(0, 0, 0)), 'Tint'),
                            lv.transform(pos, rot)])


def sequence_player(lv, name, seq, autoplay=False, loop=False):
    return lv.entity(name, [lv.fields('Sequence Player', version=2, Sequence=seq, PlayRate=1, AutoPlay=autoplay,
                                      Loop=loop, RestoreState=False, DestroyWhenFinished=False, Cinematic=False,
                                      Skippable=False, BlendIn=0, BlendOut=0, BlendEase=0, ReleaseControl=0,
                                      ReleaseBeforeEnd=0),
                            lv.transform((0, 0, 0))])


def channel(name, keys, interp='CubicInOut'):
    return ([f'      <Channel name="{name}">'] + [f'        <Key f="{fr}" v="{v:g}" interp="{interp}"/>' for fr, v in keys]
            + ['      </Channel>'])


def binding(bid, guid, name, channels):
    out = [f'  <Binding id="{bid}" kind="possess" guid="{guid}" name="{name}">', '    <Track type="Transform">']
    for ch in channels:
        out += ch
    return out + ['    </Track>', '  </Binding>']


def write_sequence(name, length, body):
    path = os.path.join(GAME, 'Content', 'Cinematics', name + '.nmseq')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = ['<?xml version="1.0"?>', f'<Sequence version="1" fps="30" length="{length}">'] + body + ['</Sequence>']
    with open(path, 'w') as out:
        out.write('\n'.join(text) + '\n')


def add_to_sequence(name, body):
    """Bindings added to a sequence make_levels.py already wrote (C2's door parts)."""
    path = os.path.join(GAME, 'Content', 'Cinematics', name + '.nmseq')
    with open(path) as src:
        text = src.read()
    text = text.replace('</Sequence>', '\n'.join(body) + '\n</Sequence>')
    with open(path, 'w') as out:
        out.write(text)


# =============================================================================
# Shared pieces: the plaza, the facade and door, the cliff (L1 and the menu)
# =============================================================================
LAMP_COLOUR = (255, 178, 105)


def ground(lv):
    lv.entity('Ground', [lv.fields('Terrain', version=3, Terrain=TERRAIN_FILE, Material=ISLAND, Visible=True,
                                   CastShadows=True, Occluder=True, Walkable=True, UVTileSize=100, LODDistance=0,
                                   Foliage='', FoliageVisible=True),
                         lv.fields('Rigid Body', Enabled=True, Motion=2, Shape=7, ShapeSource=0, Friction=0.8,
                                   Layer=1, Mask=-1),
                         lv.transform((OX, 0, OZ))], static=True)


def facade(lv, door_parts=True):
    """The facade across the plaza's north side: the arch round the blast door, wall bays either
    side, the cliff walls behind them; returns the door parts' names."""
    bx, by, bz, _ = D['points']['BlastDoor']
    fz = bz - 35
    mesh(lv, 'Facade_Arch', K + 'Facade_Arch.nmmesh', (bx, by, fz), collide=True)
    for i, dx in enumerate((-710, 710, -1330, 1330)):     # the arch is 8 m wide, a bay 6.2 m
        mesh(lv, f'Facade_Wall_{i}', K + 'Facade_Wall.nmmesh', (bx + dx, by, fz + 15), collide=True)
    # The cliff walls: one row behind the facade, rising over it, two more up the face to the top.
    rows = [(-80, 330, ((-1050, 'A', 3), (1050, 'A', -4), (-2600, 'B', 8), (2650, 'B', -10))),
            (820, 230, ((-1900, 'B', -6), (-500, 'A', 2), (1000, 'A', -3), (2400, 'B', 7))),
            (1550, 480, ((-1400, 'A', 4), (300, 'B', -5), (1800, 'A', 2)))]
    i = 0
    for dy, dz, row in rows:
        for dx, m, yaw in row:
            mesh(lv, f'CliffWall_{i}', K + f'Cliff_Wall_{m}.nmmesh', (bx + dx, by + dy, fz + dz), yaw,
                 scale=1.0 + 0.08 * (i % 3))
            i += 1
    mesh(lv, 'Tunnel_Back', 'Engine/Content/Meshes/Box.nmmesh', (bx, by + 300, fz + 190),
         material='Content/Materials/Kit/PaintBlack.material.nmgraph')
    patch(lv, 'Tunnel_Back', lambda t: set_transform(t, (bx, by + 300, fz + 190), (0, 0, 0), (480, 600, 20)))
    parts = {}
    if door_parts:
        mesh(lv, 'BlastDoor_Frame', K + 'BlastDoor_Frame.nmmesh', (bx, by, fz))
        parts['LeafL'] = mesh(lv, 'BlastDoor_LeafL', K + 'BlastDoor_LeafL.nmmesh', (bx, by, fz), static=False)
        parts['LeafR'] = mesh(lv, 'BlastDoor_LeafR', K + 'BlastDoor_LeafR.nmmesh', (bx, by, fz), static=False)
        parts['Cog'] = mesh(lv, 'BlastDoor_Cog', K + 'BlastDoor_Cog.nmmesh', (bx, by + 260, fz - 2), static=False)
    return parts, (bx, by, fz)


def cliff_pieces(lv, x_range=(-3400, 9400), skip=(1900, 5500)):
    """Kit cliff pieces along the cliff line, sunk into the terrain: as many rows as the face is tall
    there (it fades out east and west), so no stretched terrain face shows."""
    rng = np.random.default_rng(5)
    x = x_range[0]
    i = 0
    while x < x_range[1]:
        if not (skip[0] < x < skip[1]):
            zc = float(cliff_line(np.array([x]))[0])
            base = height_at(x, zc - 150)
            tall = height_at(x, zc + 350) - base
            for row, (m, dy) in enumerate((('Cliff_A', -150), ('Cliff_B', 700), ('Cliff_C', 1350), ('Cliff_B', 1900))):
                if tall < 250 or dy > tall - 300 and row > 0:
                    continue
                pick = m if rng.random() < 0.6 else ('Cliff_A', 'Cliff_B', 'Cliff_C')[int(rng.integers(0, 3))]
                yaw = float(rng.uniform(-18, 18))
                px = x + float(rng.uniform(-150, 150))
                if row > 0 and base + dy > min(height_at(px - 300, zc + 450), height_at(px + 300, zc + 450)) - 250:
                    continue
                mesh(lv, f'Cliff_{i}', K + pick + '.nmmesh', (px, base + dy, zc + 40 + row * 60),
                     yaw, scale=float(rng.uniform(0.9, 1.25)))
                i += 1
        x += float(rng.uniform(650, 850))


def plaza_props(lv, toggle=None):
    """Steam vents, lamps and floodlights round the plaza; toggle: the signal that lights the floods."""
    bx, by, bz, _ = D['points']['BlastDoor']
    px0, pz0, px1, pz1, top = D['plaza']
    for i, (x, z) in enumerate(((bx - 1950, bz - 1150), (bx + 2000, bz - 1250))):
        mesh(lv, f'SteamVent_{i}', K + 'SteamVent.nmmesh', (x, top, z))
        particles(lv, f'SteamVent_{i}_Plume', 'Content/Particles/SteamVent.nmparticle', (x, top + 30, z))
    particles(lv, 'SteamJet_Facade', 'Content/Particles/SteamJet.nmparticle', (bx + 1650, top + 620, bz - 140))
    for i, (x, z) in enumerate(((px0 + 150, pz0 + 200), (px1 - 150, pz0 + 200), (px0 + 300, bz - 600), (px1 - 300, bz - 600))):
        yaw = 90 if x < bx else -90
        mesh(lv, f'PlazaLamp_{i}', K + 'RoadLamp.nmmesh', (x, top, z), yaw + 180, collide=True)
        lx = x + (60 if x < bx else -60)
        point_light(lv, f'PlazaLamp_{i}_Light', (lx, top + 385, z), LAMP_COLOUR, 14, 1300, source=6)
    for i, dx in enumerate((-900, 900)):
        spot_light(lv, f'Flood_{i}', (bx + dx * 2.2, top + 900, bz - 1500), (bx + dx * 0.25, top + 350, bz),
                   (255, 205, 150), 60, 4000, 16, 32, toggle=toggle, shadows=False)
    point_light(lv, 'Door_Glow', (bx, top + 560, bz - 200), (255, 150, 80), 10, 700, toggle=toggle)
    for i, (x, z) in enumerate(((px0 - 300, pz1 - 200), (px1 + 350, pz1 - 300), (px0 + 200, pz1 + 50), (px1 - 100, pz1 + 80))):
        mesh(lv, f'PlazaRock_{i}', K + ('Rock_E' if i < 2 else 'Rock_C') + '.nmmesh', (x, height_at(x, z) - 15, z), i * 67.0,
             collide=True)


def paving(lv):
    """The plaza paved: the L2 kit's 4 m floor slab in StoneBlocks, laid by a Box spawner (one
    instanced draw, no collision needed: it lies on the flat terrain)."""
    px0, pz0, px1, pz1, top = D['plaza']
    cx, cz = (px0 + px1) / 2, (pz0 + pz1 + 400) / 2
    pave = spawner_comp(lv, {'Source': 2, 'Project': 0, 'Spacing': 400, 'Jitter': 0, 'RandomYaw': False,
                             'ScaleMin': 1, 'ScaleMax': 1, 'ColorVariation': 0.12, 'CellSize': 4000, 'LOD1Density': 1,
                             'LOD2Density': 1, 'Seed': 2},
                        [('Content/Meshes/L2/Floor.nmmesh', 1, 1, '')], box=((px1 - px0) / 2, 5, (pz1 + 400 - pz0) / 2))
    pave = [re.sub(r'<Mesh0Material Value="" />', '<Mesh0Material Value="Content/Materials/Kit/StoneBlocks.material.nmgraph" />', line)
            for line in pave]
    lv.entity('Plaza_Paving', [pave, lv.transform((cx, top + 1, cz))])


def rail_blockers(lv):
    """Spawned instances have no collision: invisible walls along the guard rails' lines (both sides,
    where the rails spawner puts rails) and a post at each lamp."""
    hp = [D['road'][6], D['road'][10], D['road'][17]]
    skip = lambda x, z: (inside(x, z, ZONES['Zone_Lowland']) or inside(x, z, ZONES['Zone_Plaza'])
                         or inside(x, z, ZONES['Zone_Gate']) or any(math.hypot(x - a, z - c) < 650 for a, _, c in hp))
    n = 0
    for side in (1, -1):
        pts = road_samples(400, side * (D['road_width'] / 2 + 40))
        for (x0, z0, _, _), (x1, z1, _, _) in zip(pts, pts[1:]):
            mx, mz = (x0 + x1) / 2, (z0 + z1) / 2
            if skip(mx, mz):
                continue
            length = math.hypot(x1 - x0, z1 - z0)
            yaw = math.degrees(math.atan2(x1 - x0, z1 - z0))
            y = height_at(mx, mz)
            lv.box(f'RailBlock_{n}', (mx, y + 60, mz), (20, 160, length + 10), yaw, visible=False)
            n += 1
    for i, (x, z, hx, hz) in enumerate(road_samples(1700, D['road_width'] / 2 + 110)):
        if inside(x, z, ZONES['Zone_Plaza']) or inside(x, z, ZONES['Zone_Gate']):
            continue
        lv.box(f'LampBlock_{i}', (x, height_at(x, z) + 150, z), (40, 300, 40), visible=False)
    return n


AIRSHIP = 'Content/Meshes/Airship/'


def airship(lv):
    """The airship that brought the player, hanging off the pier's end, nose to the island (+X), its
    propellers turning; C1's real flight comes later. Returns its parts' names for the ambient track."""
    x0, z0, x1, z1 = D['pier']
    # C1 flies the rig in (Tools/Cinematics); the ambient loop bobs the hull on it.
    lv.entity('Airship_Flight', [lv.transform((x0 - 1400, 0, (z0 + z1) / 2 - 300))])
    hull = mesh(lv, 'Airship', AIRSHIP + 'Airship_Hull.nmmesh', (0, 1500, 0), 0, parent='Airship_Flight', static=False)
    mesh(lv, 'Airship_Envelope', AIRSHIP + 'Airship_Envelope.nmmesh', (0, 0, 0), parent=hull, static=False)
    props = {}
    for part, off in (('PropLeft', (-195, 220, -400)), ('PropRight', (-195, 220, 400)), ('PropTail', (-1860, 900, 0))):
        props[part] = mesh(lv, f'Airship_{part}', AIRSHIP + f'Airship_{part}.nmmesh', off, parent=hull, static=False)
    point_light(lv, 'Airship_Lamp', (x0 - 1400, 1450, (z0 + z1) / 2 - 300), LAMP_COLOUR, 10, 1500, source=10)
    patch(lv, 'Airship_Lamp', lambda t: set_transform(t.replace('<Entity ', f'<Entity Parent="{lv.guid("Airship_Flight")}" ', 1),
                                                      (0, 1450, 0), (0, 0, 0), (1, 1, 1)))
    return hull, props


def post(lv, stack, gi_centre, gi_extent, probes):
    lv.entity('PostProcess', [lv.fields('Post Process', version=2, Stack=stack, Priority=0, Enabled=True,
                                        BlendDistance=0), lv.transform((0, 0, 0))])
    # DDGI over the plaza and the road below it only (the open hillside takes the sky and SSGI).
    lv.entity('GI_Volume', [lv.fields('GI Volume', Extent=gi_extent, ProbeSpacing=300, Intensity=1, Enabled=True),
                            lv.transform(gi_centre)])
    # Probes captured once, on demand (Manual + Capture Now): the light does not move.
    for i, (p, r) in enumerate(probes):
        lv.entity(f'ReflectionProbe_{i}', [lv.fields('Reflection Probe', Radius=r, BoxExtent=(0, 0, 0), Intensity=1,
                                                     Resolution=256, Frequency=3, Interval=1, NearClip=5, FarClip=40000,
                                                     CaptureShadows=True, CaptureNow=True, Enabled=True),
                                           lv.transform(p)])


# =============================================================================
# L1
# =============================================================================
GREY = (r'Ground|Sea|Pier|PierPost_\d+(_\d+)?|Road_Leg[12]_\d+|Road_Landing|Overlook_Rail|Plaza|Cliff_W|Cliff_E|'
        r'Cliff_Lintel|Facade|Tunnel_Back|Gatehouse|Spruce_\d+|SpruceCrown_\d+|Scree_\d+|SignalMast|Crank_Post|'
        r'Crank_Gatehouse_Wheel|Crank_Gatehouse_Spoke')


def dress(lv):
    remove(lv, GREY)
    lo, hi = write_terrain()
    ground(lv)
    p = D['points']

    # --- Sea ------------------------------------------------------------------
    for i in (-1, 0, 1):
        for j in (-1, 0, 1):
            mesh(lv, f'Sea_{i + 1}{j + 1}', K + 'Sea_Plane.nmmesh', (-6000 + i * 20000, SEA, -2000 + j * 20000),
                 shadows=False)
    mesh(lv, 'Sea_Far', K + 'Sea_Far.nmmesh', (0, SEA, 0), shadows=False)
    shore = shoreline()
    for i, (x, z, yaw) in enumerate(shore):
        mesh(lv, f'SeaFoam_{i}', K + 'Sea_FoamStrip.nmmesh', (x, SEA, z), yaw, shadows=False)

    # --- The pier -------------------------------------------------------------
    x0, z0, x1, z1 = D['pier']
    zc = (z0 + z1) / 2
    for i, x in enumerate(range(int(x0), int(x1), 400)):
        mesh(lv, f'Dock_Deck_{i}', K + 'Dock_Deck.nmmesh', (x, 0, zc), 90, collide=True)
    for i, x in enumerate(range(int(x0), int(x1) + 1, 400)):
        for s, side in enumerate((-150, 150)):
            mesh(lv, f'Dock_Post_{i}_{s}', K + 'Dock_Post.nmmesh', (x, 0, zc + side), collide=True)
    for i, (x, side) in enumerate(((x0 + 200, 120), (x0 + 1400, -120), (x0 + 2400, 120))):
        mesh(lv, f'Dock_Bollard_{i}', K + 'Dock_Bollard.nmmesh', (x, 0, zc + side), collide=True)
    point_light(lv, 'Dock_Lamp_Light', (x0 + 60, 385, zc - 150), LAMP_COLOUR, 12, 1200)
    mesh(lv, 'Dock_Lamp', K + 'RoadLamp.nmmesh', (x0 + 0, 0, zc - 150 + 60), 0, collide=True)

    # --- The road: one spline; lamps on it, rails and fence posts on copies --------------------
    road_pts = [(x, y, z) for x, y, z in D['road']]
    width = D['road_width']
    lamp_offset = width / 2 + 110
    lamps = spawner_comp(lv, {'Source': 0, 'Project': 1, 'Spacing': 1700, 'Jitter': 0, 'ProjectRange': 3000,
                              'PathOffset': lamp_offset, 'AlignToPath': True, 'RandomYaw': False, 'ScaleMin': 1,
                              'ScaleMax': 1, 'ColorVariation': 0, 'Seed': 3, 'CellSize': 3000},
                         [(K + 'L1_RoadLamp_X.nmmesh', 1, 1, '')], exclusions=['Zone_Plaza', 'Zone_Gate'])
    lv.entity('Road', [spline_comp(lv, road_pts, width=width), lamps, lv.transform((0, 0, 0))])
    rails = spawner_comp(lv, {'Source': 0, 'Project': 1, 'Spacing': 400, 'Jitter': 0, 'ProjectRange': 3000,
                              'PathOffset': width / 2 + 40, 'BothSides': True, 'AlignToPath': True, 'RandomYaw': False,
                              'ScaleMin': 1, 'ScaleMax': 1, 'ColorVariation': 0.15, 'Seed': 4, 'CellSize': 3000,
                              'LOD1Density': 1, 'LOD2Density': 0.5, 'CullDistance': 14000},
                         [(K + 'L1_GuardRail_Z.nmmesh', 1, 1, '')],
                         exclusions=['Zone_Lowland', 'Zone_Plaza', 'Zone_Gate', 'Zone_Hairpins'])
    lv.entity('Road_Rails', [spline_comp(lv, road_pts, width=width), rails, lv.transform((0, 0, 0))])
    fence = spawner_comp(lv, {'Source': 0, 'Project': 1, 'Spacing': 250, 'Jitter': 0, 'ProjectRange': 3000,
                              'PathOffset': width / 2 + 60, 'BothSides': True, 'AlignToPath': True, 'RandomYaw': False,
                              'ScaleMin': 1, 'ScaleMax': 1, 'ColorVariation': 0.2, 'Seed': 6, 'CellSize': 3000,
                              'LOD1Density': 1, 'LOD2Density': 0.5, 'CullDistance': 9000},
                         [(K + 'L1_FencePost_Z.nmmesh', 1, 1, '')], exclusions=['Zone_Upland'])
    lv.entity('Road_Fence', [spline_comp(lv, road_pts, width=width), fence, lv.transform((0, 0, 0))])
    zones = exclusion_zones(lv)
    # the hairpins' insides: one closed spline round each apex (an Exclusion takes one entity's spline;
    # three loops in one entity would join, so the rails skip a disc round each apex instead)
    hp = [D['road'][6], D['road'][10], D['road'][17]]
    lv.entity('Zone_Hairpins', [spline_comp(lv, hp_hull(hp), closed=True, linear=True), lv.transform((0, 0, 0))])

    # The lamps' lights, where the spawner puts the lamps (every 1700 along the curve, on its right).
    for i, (x, z, hx, hz) in enumerate(road_samples(1700, lamp_offset)):
        if inside(x, z, zones['Zone_Plaza']) or inside(x, z, zones['Zone_Gate']):
            continue
        lx, lz = x - hz * 60, z + hx * 60          # the lantern hangs 60 toward the road (left)
        point_light(lv, f'RoadLamp_{i}_Light', (lx, height_at(x, z) + 380, lz), LAMP_COLOUR, 10, 1300, source=6)

    # --- The gatehouse, its barrier and crank -----------------------------------------------
    gx, gz = D['gatehouse_centre']
    gy = 650
    mesh(lv, 'Gatehouse', K + 'Gatehouse.nmmesh', (gx, gy, gz), collide=True)
    arm = mesh(lv, 'Gate_BarrierArm', K + 'Gatehouse_Barrier.nmmesh', (gx - 240, gy + 100, gz - 200), static=False)
    for i, z in enumerate((gz - 200, gz - 760)):
        mesh(lv, f'Gate_Post_{i}', 'Engine/Content/Meshes/Cylinder.nmmesh', (gx - 240, gy + 55, z),
             material='Content/Materials/Kit/SteelDark.material.nmgraph')
        patch(lv, f'Gate_Post_{i}', lambda t, z=z: set_transform(t, (gx - 240, gy + 55, z), (0, 0, 0), (22, 110, 22)))
    mesh(lv, 'Crank_Gatehouse_Kit', K + 'Gatehouse_Crank.nmmesh', (0, 0, 0), -90, parent='Handle', static=False)
    patch(lv, 'Crank_Gatehouse', lambda t: set_field(t, 'Sequence', 'Gate_Barrier'))
    point_light(lv, 'Gatehouse_Light', (gx - 330, gy + 330, gz - 150), LAMP_COLOUR, 8, 900)
    # the gate: an invisible blocker the length of the arm and the shoulders
    gp = p['Gate']
    patch(lv, 'GatehouseGate', lambda t: set_transform(set_field(t, 'Visible', 'false'),
                                                       (gp[0], gp[1] + 37 + 180, gp[2]), (0, gp[3], 0), (1100, 360, 40)))
    sequence_player(lv, 'Gate_Barrier', 'Content/Cinematics/Ambient/L1_GateBarrier.nmseq')
    write_sequence('Ambient/L1_GateBarrier', 75, binding('arm', lv.guid(arm), arm,
                                                 [channel('Rotation.X', [(0, 0), (20, -4), (75, 80)])]))

    # --- The plaza, the facade, the door, the cliff --------------------------------------------
    parts, (bx, by, fz) = facade(lv)
    patch(lv, 'BlastDoor', lambda t: set_field(t, 'Visible', 'false'))
    # C2 turns the cog once, then parts the leaves, the cog riding with the left one (Tools/Cinematics).
    assert parts == {'LeafL': 'BlastDoor_LeafL', 'LeafR': 'BlastDoor_LeafR', 'Cog': 'BlastDoor_Cog'}
    plaza_props(lv, toggle='L1.Plaza')
    cliff_pieces(lv)

    # --- The overlook: a timber platform out over the slope, railed, a lamp ---------------
    ox, oz = D['overlook']
    yaw = -128.0
    fx, fz_ = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))      # out (+Z of the deck)
    sx, sz = fz_, -fx                                                       # across (+X of the deck)
    for i, side in enumerate((-155, 155)):
        mesh(lv, f'Overlook_Deck_{i}', K + 'Dock_Deck.nmmesh', (ox + sx * side - fx * 150, 500, oz + sz * side - fz_ * 150),
             yaw, collide=True)
    for i, (a, b, ry) in enumerate(((0, 400, 0), (-310, 200, 90), (310, 200, 90))):
        mesh(lv, f'Overlook_Rail_{i}', K + 'GuardRail.nmmesh',
             (ox + sx * a + fx * (b - 150), 500, oz + sz * a + fz_ * (b - 150)), yaw + ry, collide=True)
    mesh(lv, 'Overlook_Lamp', K + 'RoadLamp.nmmesh', (ox + sx * 330 - fx * 120, 500, oz + sz * 330 - fz_ * 120), yaw + 90,
         collide=True)
    point_light(lv, 'Overlook_Lamp_Light', (ox + sx * 330 - fx * 120 - sx * 60, 885, oz + sz * 330 - fz_ * 120 - sz * 60),
                LAMP_COLOUR, 10, 1200, source=6)

    # --- The signal mast, rocks ----------------------------------------------------------------
    mx, mz = D['mast']
    my = height_at(mx, mz)
    mesh(lv, 'SignalMast', K + 'SignalMast.nmmesh', (mx, my - 10, mz), 20, collide=True)
    anem = mesh(lv, 'SignalMast_Anemometer', K + 'SignalMast_Anemometer.nmmesh', (mx, my - 10 + 1320, mz), static=False)
    point_light(lv, 'SignalMast_Beacon', (mx, my + 1150, mz), (255, 60, 40), 12, 1500, source=10)
    hull, props = airship(lv)
    hy, hz = 1500, (D['pier'][1] + D['pier'][3]) / 2 - 300
    sequence_player(lv, 'L1_Ambient', 'Content/Cinematics/Ambient/L1_Ambient.nmseq', autoplay=True, loop=True)
    # 8 s, looped: the anemometer a turn every 2 s, the propellers 8 a second, the airship bobbing.
    body = binding('anem', lv.guid(anem), anem, [channel('Rotation.Y', [(0, 0), (240, 1440)], 'Linear')])
    for k, (part, n) in enumerate(props.items()):
        body += binding(f'prop{k}', lv.guid(n), n, [channel('Rotation.X', [(0, 0), (240, 23040)], 'Linear')])
    body += binding('ship', lv.guid(hull), hull, [channel('Position.Y', [(0, hy), (120, hy + 45), (240, hy)]),
                                                  channel('Rotation.Z', [(0, 0), (120, 1.5), (240, 0)])])
    write_sequence('Ambient/L1_Ambient', 240, body)
    rng = np.random.default_rng(17)
    for i, (x, z) in enumerate(((-700, -1700), (-200, -1000), (200, -1650), (900, -500), (2300, 400), (4200, -200),
                                (300, 1100), (-100, 2300), (4700, 1800), (5600, 2600), (5800, 4800), (1500, 4300),
                                (2400, 3800), (6000, 1200))):
        m = ('Rock_C', 'Rock_E', 'Rock_B', 'Rock_D')[i % 4]
        mesh(lv, f'Rock_{i}', K + m + '.nmmesh', (x, height_at(x, z) - 20, z), float(rng.uniform(0, 360)),
             collide=True, scale=float(rng.uniform(0.9, 1.4)))

    # --- Plants, the plaza's paving, the rails' blockers ----------------------------------------
    foliage(lv)
    paving(lv)
    blockers = rail_blockers(lv)

    # --- Light and air -----------------------------------------------------------------------
    post(lv, 'Content/PostProcess/L1_Foothills.pp.nmgraph', (3800, 1250, 5000), (2200, 600, 2100),
         [((-2400, 300, -1500), 4000), ((1800, 800, 1200), 4500), ((3700, 1500, 6000), 3500)])
    patch(lv, 'Weather', lambda t: set_field(set_field(t, 'MistDensity', 0.45), 'MistHeight', -60))
    print(f'  {TERRAIN_FILE}: {RES}x{RES}, heights {lo:.0f}..{hi:.0f} cm; {len(shore)} foam strips, '
          f'{blockers} rail blockers')


ZONES = {
    'Zone_Lowland': [(-6000, 0, -6000), (6000, 0, -6000), (6000, 0, -500), (-6000, 0, -500)],
    'Zone_Upland': [(-6000, 0, -500), (9000, 0, -500), (9000, 0, 9000), (-6000, 0, 9000)],
    'Zone_Plaza': [(2000, 0, 5250), (5400, 0, 5250), (5400, 0, 7600), (2000, 0, 7600)],
    'Zone_Gate': [(2700, 0, 2200), (3800, 0, 2200), (3800, 0, 3500), (2700, 0, 3500)],
}


def exclusion_zones(lv):
    """The splines the spawners keep out of: the road's band (RoadClear) and closed zones."""
    lv.entity('RoadClear', [spline_comp(lv, D['road'], width=D['road_width'] + 700), lv.transform((0, 0, 0))])
    for n, pts in ZONES.items():
        lv.entity(n, [spline_comp(lv, pts, closed=True, linear=True), lv.transform((0, 0, 0))])
    return ZONES


def foliage(lv, menu=False):
    """The spruce forest, the meadow, the scree. Every spawner is named Foliage_* (the game scales
    their Density by the Foliage density setting)."""
    spruce = [(FOL + 'L1_Spruce.nmmesh', 1, 1, FOL + 'L1_Spruce_LOD1.nmmesh')]
    forest = spawner_comp(lv, {'Source': 2, 'Project': 1, 'Spacing': 520, 'NoiseScale': 3800, 'NoiseThreshold': 0.32,
                               'MaxSlope': 33, 'MinHeight': 120,
                               'ScaleMin': 0.75, 'ScaleMax': 1.5, 'Sink': 0.04, 'ColorVariation': 0.25,
                               'CellSize': 1500, 'LODDistance': 1200, 'LOD1Density': 1, 'LOD2Density': 0.7,
                               'CullDistance': 30000, 'Seed': 5},
                          spruce, exclusions=['RoadClear', 'Zone_Plaza', 'Zone_Gate'], box=(4200, 4000, 2600))
    lv.entity('Foliage_Forest', [forest, lv.transform((-2300, 1000, 5400), (0, 12, 0))])
    far = spawner_comp(lv, {'Source': 2, 'Project': 1, 'Spacing': 700, 'Density': 0.8, 'NoiseScale': 6000,
                            'NoiseThreshold': 0.35, 'MaxSlope': 30, 'MinHeight': 200, 'ScaleMin': 0.9, 'ScaleMax': 1.6,
                            'Sink': 0.04, 'ColorVariation': 0.3, 'CellSize': 6000, 'LODDistance': 4000,
                            'LOD1Density': 0.8, 'LOD2Density': 0.5, 'CastShadows': False, 'Seed': 8},
                       [(FOL + 'L1_Spruce_LOD1.nmmesh', 1, 1, FOL + 'L1_Spruce_LOD2.nmmesh')],
                       exclusions=['RoadClear', 'Zone_Plaza'], box=(8000, 5000, 7000))
    lv.entity('Foliage_FarWoods', [far, lv.transform((11000, 1500, 6000), (0, -10, 0))])
    high = spawner_comp(lv, {'Source': 2, 'Project': 1, 'Spacing': 650, 'Density': 0.85, 'NoiseScale': 5000,
                             'NoiseThreshold': 0.3, 'MaxSlope': 38, 'MinHeight': 1500, 'ScaleMin': 0.9,
                             'ScaleMax': 1.5, 'Sink': 0.04, 'ColorVariation': 0.3, 'CellSize': 6000,
                             'LODDistance': 4000, 'LOD1Density': 0.8, 'LOD2Density': 0.5, 'CastShadows': False,
                             'Seed': 12},
                        [(FOL + 'L1_Spruce_LOD1.nmmesh', 1, 1, FOL + 'L1_Spruce_LOD2.nmmesh')],
                        box=(10000, 5000, 4500))
    lv.entity('Foliage_Highland', [high, lv.transform((3500, 4000, 12500))])
    meadow_outline = [(-600, 0, -900), (2600, 0, -900), (4500, 0, -500), (8000, 0, -200), (9000, 0, 2500),
                      (8000, 0, 5500), (5600, 0, 6200), (5300, 0, 5200), (2100, 0, 5200), (1200, 0, 3300),
                      (-200, 0, 2600), (-900, 0, 900)]
    grass = spawner_comp(lv, {'Source': 1, 'Project': 1, 'Spacing': 60, 'ProjectRange': 4000, 'TerrainLayer': 1,
                              'LayerThreshold': 0.45, 'NoiseScale': 2200, 'NoiseThreshold': 0.28, 'MaxSlope': 40,
                              'AlignToNormal': 0.7, 'ScaleMin': 0.5, 'ScaleMax': 0.95, 'ColorVariation': 0.3,
                              'CastShadows': False, 'CellSize': 2500, 'LODDistance': 2500, 'LOD1Density': 0.4,
                              'LOD2Density': 0.12, 'CullDistance': 9000, 'Seed': 11},
                         [(FOL + 'L1_GrassTuft.nmmesh', 1, 1, '')], exclusions=['RoadClear', 'Zone_Gate', 'Zone_Plaza'])
    lv.entity('Foliage_Meadow', [spline_comp(lv, meadow_outline, closed=True), grass, lv.transform((0, 0, 0))])
    flowers = spawner_comp(lv, {'Source': 1, 'Project': 1, 'Spacing': 110, 'ProjectRange': 4000, 'TerrainLayer': 1,
                                'LayerThreshold': 0.55, 'NoiseScale': 1500, 'NoiseThreshold': 0.55, 'MaxSlope': 30,
                                'AlignToNormal': 0.5, 'ScaleMin': 0.7, 'ScaleMax': 1.1, 'ColorVariation': 0.2,
                                'CastShadows': False, 'CellSize': 2500, 'LODDistance': 2000, 'LOD1Density': 0.4,
                                'LOD2Density': 0.1, 'CullDistance': 6000, 'Seed': 19},
                           [(FOL + 'L1_Wildflowers.nmmesh', 1, 1, '')],
                           exclusions=['RoadClear', 'Zone_Gate', 'Zone_Plaza'])
    lv.entity('Foliage_Flowers', [spline_comp(lv, meadow_outline, closed=True), flowers, lv.transform((0, 0, 0))])
    scree = spawner_comp(lv, {'Source': 2, 'Project': 1, 'Spacing': 170, 'TerrainLayer': 3, 'LayerThreshold': 0.35,
                              'MaxSlope': 48, 'AlignToNormal': 1, 'Sink': 0.15, 'ScaleMin': 0.5, 'ScaleMax': 1.6,
                              'ColorVariation': 0.25, 'CellSize': 3000, 'LODDistance': 3000, 'LOD1Density': 0.6,
                              'LOD2Density': 0.3, 'CullDistance': 16000, 'Seed': 9},
                         [(K + 'Rock_A.nmmesh', 1, 1, ''), (K + 'Rock_B.nmmesh', 0.5, 1, ''),
                          (K + 'Rock_D.nmmesh', 0.6, 1, '')],
                         exclusions=['Zone_Plaza', 'RoadClear'], box=(6000, 2500, 900))
    lv.entity('Foliage_Scree', [scree, lv.transform((3700, 1500, 6500))])


def inside(x, z, poly):
    c = False
    for (x0, _, z0), (x1, _, z1) in zip(poly, poly[1:] + poly[:1]):
        if (z0 > z) != (z1 > z) and x < x0 + (z - z0) * (x1 - x0) / (z1 - z0):
            c = not c
    return c


def hp_hull(hp):
    """A thin closed loop joining discs round the hairpins' apexes (an Exclusion is one closed spline):
    out round each disc and back along a hair-thin corridor, so only the discs exclude much."""
    pts = []
    for x, _, z in hp:
        pts += [(x + 650 * math.cos(a), 0, z + 650 * math.sin(a)) for a in np.linspace(-math.pi / 2, 1.5 * math.pi, 10)]
    back = [(x + 5, 0, z - 650) for x, _, z in reversed(hp)]
    return pts + back


def shoreline():
    """Foam strips along the beach: walk the coast (the terrain crossing the water) between the pier's
    south and the north-west, a strip every 20 m, each facing out to sea."""
    out = []
    nx, nz = 0.85, 0.53                       # inland
    tx, tz = -nz, nx                          # along the coast, north-west
    base_x, base_z = -1200.0, -1500.0
    for s in np.arange(-6000, 9000, 2000):
        cx, cz = base_x + tx * s, base_z + tz * s
        prev = None
        for d in np.arange(-3000, 3000, 25):
            x, z = cx + nx * d, cz + nz * d
            above = height_at(x, z) > SEA + 3
            if prev is not None and above and not prev:
                yaw = math.degrees(math.atan2(-nx, -nz))
                out.append((x, z, yaw))
                break
            prev = above
    return out


# =============================================================================
# The menu: the plaza at dusk
# =============================================================================
def dress_menu(lv):
    remove(lv, r'Ground|Facade_.*|Plaza|FacadeLamp_\d+')
    build_terrain()
    ground(lv)
    facade(lv, door_parts=True)
    plaza_props(lv)
    paving(lv)
    cliff_pieces(lv)
    exclusion_zones(lv)
    foliage(lv, menu=True)
    for i, (x, z) in enumerate(((2400, 4500), (5000, 4600))):
        point_light(lv, f'Menu_Fill_{i}', (x, height_at(x, z) + 600, z), (120, 140, 190), 4, 3000)
    post(lv, 'Content/PostProcess/L1_Menu.pp.nmgraph', (3700, 1500, 6100), (2200, 600, 1300),
         [((3700, 1500, 6000), 3500)])
    patch(lv, 'Sun', lambda t: set_field(t, 'Hour', 18.2))
    lv.weather(mist=0.5, mist_height=1100, wind=1.0, wetness=0.2)
