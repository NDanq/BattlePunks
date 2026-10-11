"""
steampunk_kit.py -- the Steampunk street corner's meshes, built in Python
(nmformats.MeshBuilder): Victorian buildings with real depth -- recessed
sash windows, stone sills and lintels, shopfronts, cornices on brackets,
mansard roofs with dormers -- a clock tower, gas lamps, and street props.

Every builder returns a MeshBuilder in the engine's frame (left-handed, +Y
up, 1 unit = 1 cm). A building is built in its own frame: its street facade
on z = 0 facing -Z, x from 0 to its width, the building running back into +Z.
make_steampunk_street.py turns and places each one.

Vertex colours are grime: soot under cornices and down from the roofline,
rising damp along the ground -- the materials multiply their base colour by
it, so one brick texture reads differently on every wall.
"""

import math
import random

from nmformats import MeshBuilder, v_add, v_mul, v_norm, v_sub, v_cross

MAT = "Content/Materials/Steampunk/"
COBBLE = MAT + "Cobble.material.nmgraph"
FLAG = MAT + "Flagstone.material.nmgraph"
CURB = MAT + "Curb.material.nmgraph"
BRICK = MAT + "Brick.material.nmgraph"
STONE = MAT + "Stone.material.nmgraph"
BRASS = MAT + "Brass.material.nmgraph"
COPPER = MAT + "Copper.material.nmgraph"
IRON = MAT + "Iron.material.nmgraph"
PLATE = MAT + "Plate.material.nmgraph"
WOOD = MAT + "Wood.material.nmgraph"
PAINT = MAT + "Paint.material.nmgraph"
SLATE = MAT + "Slate.material.nmgraph"
GLASS = MAT + "Glass.material.nmgraph"
WINDOW_LIT = MAT + "WindowLit.material.nmgraph"
LAMP_GLOW = MAT + "LampGlow.material.nmgraph"
CLOCK = MAT + "ClockFace.material.nmgraph"
GAUGE = MAT + "Gauge.material.nmgraph"
FABRIC = MAT + "Fabric.material.nmgraph"

FRONT = (0.0, 0.0, -1.0)


def shade(c, k):
    return (c[0] * k, c[1] * k, c[2] * k, 1.0)


# ==============================================================================================
# Walls with openings
# ==============================================================================================

def wall(mb, material, x0, x1, y0, y1, holes, z=0.0, tint=(1, 1, 1), grime=None, extra_ys=()):
    """A wall on the plane z facing -Z from x0-x1, y0-y1, with rectangular holes
    [(hx0, hx1, hy0, hy1)]. Cells between the holes' edges, merged along rows.
    grime(y) -> brightness 0-1 at height y (vertex colour)."""
    xs = sorted({x0, x1} | {h[0] for h in holes} | {h[1] for h in holes})
    ys = sorted({y0, y1} | {h[2] for h in holes} | {h[3] for h in holes} | {y for y in extra_ys if y0 < y < y1})
    xs = [x for x in xs if x0 <= x <= x1]
    ys = [y for y in ys if y0 <= y <= y1]

    def inside(cx, cy):
        return any(h[0] <= cx <= h[1] and h[2] <= cy <= h[3] for h in holes)

    def col(y):
        k = grime(y) if grime else 1.0
        return (tint[0] * k, tint[1] * k, tint[2] * k, 1.0)

    for j in range(len(ys) - 1):
        ya, yb = ys[j], ys[j + 1]
        run = None
        for i in range(len(xs) - 1):
            xa, xb = xs[i], xs[i + 1]
            solid = not inside((xa + xb) / 2, (ya + yb) / 2)
            if solid and run is None:
                run = xa
            if (not solid or i == len(xs) - 2) and run is not None:
                end = xb if solid else xa
                if end > run:
                    mb.quad(material, (run, ya, z), (end, ya, z), (end, yb, z), (run, yb, z), normal=FRONT,
                            colours=[col(ya), col(ya), col(yb), col(yb)])
                run = None


def reveal(mb, material, hx0, hx1, hy0, hy1, depth, z=0.0, colour=(1, 1, 1, 1), sides="lrtb"):
    """The inner faces of an opening from z to z + depth."""
    zb = z + depth
    c = [colour] * 4
    if "l" in sides:
        mb.quad(material, (hx0, hy0, z), (hx0, hy0, zb), (hx0, hy1, zb), (hx0, hy1, z), normal=(1, 0, 0), colours=c)
    if "r" in sides:
        mb.quad(material, (hx1, hy0, zb), (hx1, hy0, z), (hx1, hy1, z), (hx1, hy1, zb), normal=(-1, 0, 0), colours=c)
    if "t" in sides:
        mb.quad(material, (hx0, hy1, z), (hx0, hy1, zb), (hx1, hy1, zb), (hx1, hy1, z), normal=(0, -1, 0), colours=c)
    if "b" in sides:
        mb.quad(material, (hx0, hy0, zb), (hx0, hy0, z), (hx1, hy0, z), (hx1, hy0, zb), normal=(0, 1, 0), colours=c)


def box(mb, material, lo, hi, colour=(1, 1, 1, 1), skip=()):
    mb.box(material, lo, hi, colour_fn=lambda p, n: colour, skip=skip)


def sash_window(mb, x0, x1, y0, y1, z, lit, frame_colour, panes=(2, 3), frame=7.0, bar=3.0):
    """A painted timber sash window in the opening plane z (facing -Z): an outer
    frame, a meeting rail, glazing bars, and glass behind them."""
    glass = WINDOW_LIT if lit else GLASS
    gz = z + 3.0
    mb.quad(glass, (x0, y0, gz), (x1, y0, gz), (x1, y1, gz), (x0, y1, gz), normal=FRONT,
            uvs=[(0, 1), (1, 1), (1, 0), (0, 0)] if lit else None)
    fc = frame_colour
    box(mb, PAINT, (x0, y0, z - 2), (x0 + frame, y1, z + 4), fc, skip=("+z",))
    box(mb, PAINT, (x1 - frame, y0, z - 2), (x1, y1, z + 4), fc, skip=("+z",))
    box(mb, PAINT, (x0, y0, z - 2), (x1, y0 + frame, z + 4), fc, skip=("+z",))
    box(mb, PAINT, (x0, y1 - frame, z - 2), (x1, y1, z + 4), fc, skip=("+z",))
    mid = (y0 + y1) / 2
    box(mb, PAINT, (x0, mid - 4, z - 4), (x1, mid + 4, z + 2), fc, skip=("+z",))      # meeting rail
    cols, rows = panes
    for c in range(1, cols):
        x = x0 + (x1 - x0) * c / cols
        box(mb, PAINT, (x - bar / 2, y0, z - 1), (x + bar / 2, y1, z + 2), fc, skip=("+z",))
    for half in ((y0, mid), (mid, y1)):
        for r in range(1, rows // 2 + (rows % 2)):
            y = half[0] + (half[1] - half[0]) * r / max(rows // 2 + (rows % 2), 1)
            box(mb, PAINT, (x0, y - bar / 2, z - 1), (x1, y + bar / 2, z + 2), fc, skip=("+z",))


# ==============================================================================================
# Buildings
# ==============================================================================================

PAINT_COLOURS = [(0.12, 0.25, 0.18), (0.38, 0.08, 0.07), (0.10, 0.14, 0.26), (0.16, 0.13, 0.10),
                 (0.30, 0.22, 0.08), (0.08, 0.08, 0.08)]
BRICK_TINTS = [(1.0, 1.0, 1.0), (0.85, 0.80, 0.78), (1.05, 0.92, 0.85), (0.72, 0.70, 0.70), (0.95, 0.85, 0.72)]
STONE_TINTS = [(1.0, 1.0, 1.0), (0.9, 0.88, 0.85), (1.02, 0.97, 0.9)]


class Style:
    def __init__(self, seed, floors, ground=460.0, floor_h=340.0, roof="flat", bay=190.0, shop=True):
        self.rng = random.Random(seed)
        self.floors = floors
        self.ground = ground
        self.floor_h = floor_h
        self.roof = roof
        self.bay = bay
        self.shop = shop
        self.brick_tint = self.rng.choice(BRICK_TINTS)
        self.stone_tint = self.rng.choice(STONE_TINTS)
        self.paint = self.rng.choice(PAINT_COLOURS)
        self.frame = self.rng.choice([(0.85, 0.83, 0.78), (0.85, 0.83, 0.78), (0.15, 0.15, 0.14), self.paint])
        self.lit_chance = self.rng.uniform(0.08, 0.25)
        self.balcony = self.rng.random() < 0.4
        self.pipes = self.rng.random() < 0.7
        self.chimney_tops = []

    @property
    def top(self):
        return self.ground + self.floors * self.floor_h


def grime_fn(style):
    top = style.top

    def g(y):
        damp = 1.0 - 0.35 * math.exp(-max(y, 0) / 70.0)
        soot = 1.0 - 0.25 * math.exp(-max(top - y, 0) / 160.0)
        return damp * soot
    return g


def facade(mb, style, width, along_x=True, face_origin=(0, 0, 0), side=None, corner_chamfer=0.0):
    """One street facade, width wide, in the building frame (x along, facing -Z).
    Returns nothing; adds to mb. side='left'/'right' builds it on the end wall
    instead (handled by the caller transforming points) -- here always local."""
    rng = style.rng
    g = grime_fn(style)
    st = style.stone_tint
    bt = style.brick_tint
    top = style.top

    # --- Ground floor: rusticated stone piers round a timber shopfront.
    gh = style.ground
    pier = 45.0
    shop_holes = []
    if style.shop and width > 300:
        bays = max(1, int((width - pier) // 380))
        span = (width - pier * (bays + 1)) / bays
        for b in range(bays):
            sx0 = pier + b * (span + pier)
            shop_holes.append((sx0, sx0 + span, 0.0, gh - 90.0))
    else:
        bays = max(1, int(width // 200))
        for b in range(bays):
            cx = width * (b + 0.5) / bays
            shop_holes.append((cx - 55, cx + 55, 110.0, gh - 110.0))
    wall(mb, STONE, 0, width, 0, gh, shop_holes, tint=st, grime=g, extra_ys=(60.0,))
    for i, h in enumerate(shop_holes):
        if style.shop and width > 300:
            shopfront(mb, style, h, rng, i)
        else:
            reveal(mb, STONE, *h, 25.0, colour=shade(st, 0.8))
            sash_window(mb, h[0] + 4, h[1] - 4, h[2] + 4, h[3] - 4, 25.0, rng.random() < style.lit_chance,
                        style.frame)
            box(mb, STONE, (h[0] - 10, h[2] - 10, -6), (h[1] + 10, h[2], 25), shade(st, 0.9))
    # Plinth
    box(mb, STONE, (0, 0, -8), (width, 40, 0), shade(st, 0.65), skip=("+z",))

    # --- String course over the ground floor.
    box(mb, STONE, (-4, gh - 20, -22), (width + 4, gh + 8, 0), shade(st, 0.95), skip=("+z",))
    box(mb, STONE, (-2, gh - 30, -14), (width + 2, gh - 20, 0), shade(st, 0.8), skip=("+z",))

    # --- Upper floors: brick, windows in bays.
    n_bays = max(1, int(round(width / style.bay)))
    bay_w = width / n_bays
    holes = []
    win_w = min(115.0, bay_w * 0.6)
    for f in range(style.floors):
        fy = gh + f * style.floor_h
        win_h = 210.0 if f == 0 else (190.0 if f < style.floors - 1 else 160.0)
        sill = fy + 85.0
        for b in range(n_bays):
            cx = bay_w * (b + 0.5)
            holes.append((cx - win_w / 2, cx + win_w / 2, sill, sill + win_h, f))
    ys = [gh + f * style.floor_h for f in range(1, style.floors)] + [top - 60.0]
    wall(mb, BRICK, 0, width, gh + 8, top, [h[:4] for h in holes], tint=bt, grime=g, extra_ys=ys)
    for (hx0, hx1, hy0, hy1, f) in holes:
        reveal(mb, BRICK, hx0, hx1, hy0, hy1, 24.0, colour=shade(bt, 0.6 * g(hy0)), sides="lrt")
        # Stone sill, projecting, and a lintel with a keystone.
        box(mb, STONE, (hx0 - 12, hy0 - 12, -8), (hx1 + 12, hy0, 24), shade(st, 0.9 * g(hy0)))
        box(mb, STONE, (hx0 - 10, hy1, -3), (hx1 + 10, hy1 + 22, 0.5), shade(st, g(hy1)), skip=("+z",))
        box(mb, STONE, ((hx0 + hx1) / 2 - 11, hy1 - 6, -7), ((hx0 + hx1) / 2 + 11, hy1 + 28, 0.5),
            shade(st, g(hy1)), skip=("+z",))
        lit = rng.random() < style.lit_chance
        sash_window(mb, hx0, hx1, hy0, hy1, 24.0, lit, style.frame)
        # A balcony on the first floor: iron, on stone brackets.
        if style.balcony and f == 0:
            balcony(mb, hx0 - 25, hx1 + 25, hy0, rng)

    # Floor bands between storeys.
    for f in range(1, style.floors):
        y = gh + f * style.floor_h
        box(mb, STONE, (-2, y - 8, -6), (width + 2, y + 8, 0), shade(st, 0.85 * g(y)), skip=("+z",))

    # Pilasters at the ends (quoins).
    for x in (0.0, width - 32.0):
        for k in range(int((top - gh) // 45)):
            y = gh + 8 + k * 45
            w = 32 if k % 2 == 0 else 22
            xa = x if x == 0.0 else width - w
            box(mb, STONE, (xa, y + 2, -2.5), (xa + w, y + 43, 0.5), shade(st, 0.95 * g(y)), skip=("+z",))

    # --- Main cornice on brackets, a frieze, and a parapet.
    cornice(mb, style, width)

    # Drainpipe (copper) at one end, with brackets and a hopper.
    if style.pipes:
        px = width - 18.0 if rng.random() < 0.5 else 18.0
        mb.tube(COPPER, [(px, 5, -14), (px, top - 40, -14), (px, top - 25, -30)], 6.5, segments=10)
        box(mb, COPPER, (px - 14, top - 75, -32), (px + 14, top - 40, 2))
        for y in range(80, int(top - 80), 180):
            box(mb, IRON, (px - 9, y, -18), (px + 9, y + 6, 0))


def shopfront(mb, style, hole, rng, index):
    """A timber shopfront in a ground-floor opening: stall riser, display
    windows (some lit), a recessed door with a fanlight, and a fascia board."""
    x0, x1, y0, y1 = hole
    pc = style.paint
    recess = 30.0
    reveal(mb, STONE, x0, x1, y0, y1, recess, colour=shade(style.stone_tint, 0.7), sides="lrt")
    # Fascia: a painted sign board under the cornice, brass rule on it.
    box(mb, PAINT, (x0 - 10, y1 - 4, -16), (x1 + 10, y1 + 70, 0), shade(pc, 1.0), skip=("+z",))
    box(mb, BRASS, (x0 + 10, y1 + 26, -18), (x1 - 10, y1 + 30, -15))
    box(mb, BRASS, (x0 + 10, y1 + 45, -18), (x1 - 10, y1 + 49, -15))
    for x in (x0 - 18, x1 + 2):       # consoles at the fascia's ends
        box(mb, STONE, (x, y1 - 30, -24), (x + 16, y1 + 80, 0), shade(style.stone_tint, 0.9), skip=("+z",))
    door_w = 105.0
    door_left = rng.random() < 0.5
    dx0 = x0 + 15 if door_left else x1 - 15 - door_w
    dx1 = dx0 + door_w
    z = recess
    # Back wall of the recess (behind door and windows): painted panelling.
    mb.quad(PAINT, (x0, y0, z + 40), (x1, y0, z + 40), (x1, y1, z + 40), (x0, y1, z + 40), normal=FRONT,
            colours=[shade(pc, 0.4)] * 4)
    # Door recess sides
    box(mb, PAINT, (dx0, 0, z), (dx1, 2, z + 40), shade(pc, 0.5))
    # Door: panelled, with glass in the top half; brass handle.
    mb.quad(PAINT, (dx0 + 8, 0, z + 38), (dx1 - 8, 0, z + 38), (dx1 - 8, 225, z + 38), (dx0 + 8, 225, z + 38),
            normal=FRONT, colours=[shade(pc, 0.9)] * 4)
    for (py0, py1) in ((20, 95), (120, 210)):
        box(mb, PAINT, (dx0 + 18, py0, z + 34), (dx1 - 18, py1, z + 38), shade(pc, 0.8))
    box(mb, BRASS, (dx1 - 30, 105, z + 30), (dx1 - 22, 125, z + 38))
    # Fanlight over the door
    mb.quad(WINDOW_LIT if rng.random() < 0.5 else GLASS, (dx0 + 8, 235, z + 38), (dx1 - 8, 235, z + 38),
            (dx1 - 8, y1 - 10, z + 38), (dx0 + 8, y1 - 10, z + 38), normal=FRONT,
            uvs=[(0, 1), (1, 1), (1, 0), (0, 0)])
    box(mb, PAINT, (dx0, 225, z + 30), (dx1, 235, z + 40), shade(pc, 1.0))
    # Door frame posts
    box(mb, PAINT, (dx0 - 6, 0, z - 4), (dx0 + 8, y1, z + 40), shade(pc, 1.0))
    box(mb, PAINT, (dx1 - 8, 0, z - 4), (dx1 + 6, y1, z + 40), shade(pc, 1.0))
    # Display windows across the rest.
    wx0, wx1 = (dx1 + 6, x1) if door_left else (x0, dx0 - 6)
    if wx1 - wx0 > 40:
        riser = 70.0
        box(mb, PAINT, (wx0, 0, z - 6), (wx1, riser, z), shade(pc, 0.9), skip=("+z",))
        box(mb, PAINT, (wx0 - 2, riser, z - 14), (wx1 + 2, riser + 8, z), shade(pc, 1.1))   # sill
        lit = rng.random() < 0.35
        top = y1 - 10
        mb.quad(WINDOW_LIT if lit else GLASS, (wx0, riser + 8, z), (wx1, riser + 8, z), (wx1, top, z),
                (wx0, top, z), normal=FRONT, uvs=[(0, 1), (1, 1), (1, 0), (0, 0)])
        panes = max(1, int((wx1 - wx0) // 90))
        for p in range(1, panes):
            x = wx0 + (wx1 - wx0) * p / panes
            box(mb, PAINT, (x - 4, riser, z - 6), (x + 4, top, z + 1), shade(pc, 1.0))
        box(mb, PAINT, (wx0, top - 60, z - 5), (wx1, top - 54, z + 1), shade(pc, 1.0))   # transom bar
        box(mb, PAINT, (wx0, top, z - 6), (wx1, y1, z + 1), shade(pc, 1.0))
    # An awning, half the shops: striped canvas on iron arms.
    if rng.random() < 0.45:
        ay = y1 + 6
        depth = 140.0
        drop = 70.0
        stripe = 30.0
        n = max(1, int((x1 - x0 + 20) // stripe))
        c1 = (0.75, 0.70, 0.60, 1.0)
        c2 = shade(pc, 1.4)
        for s in range(n):
            sx0 = x0 - 10 + (x1 - x0 + 20) * s / n
            sx1 = x0 - 10 + (x1 - x0 + 20) * (s + 1) / n
            c = c1 if s % 2 == 0 else c2
            mb.quad(FABRIC, (sx0, ay - drop, -depth), (sx1, ay - drop, -depth), (sx1, ay, -16), (sx0, ay, -16),
                    colours=[c] * 4)
            mb.quad(FABRIC, (sx1, ay - drop, -depth), (sx0, ay - drop, -depth), (sx0, ay, -16), (sx1, ay, -16),
                    colours=[c] * 4)
            # valance
            mb.quad(FABRIC, (sx0, ay - drop - 25, -depth), (sx1, ay - drop - 25, -depth), (sx1, ay - drop, -depth),
                    (sx0, ay - drop, -depth), normal=FRONT, colours=[c] * 4)
        for x in (x0, x1):
            mb.tube(IRON, [(x, ay - 10, -16), (x, ay - drop - 5, -depth + 4)], 2.0, segments=6)


def balcony(mb, x0, x1, y, rng):
    depth = 70.0
    box(mb, STONE, (x0, y - 18, -depth), (x1, y, 0))
    for x in (x0 + 8, x1 - 22):
        box(mb, STONE, (x, y - 60, -depth + 10), (x + 14, y - 18, 0))
    # Railing: top rail, bottom rail, balusters
    h = 95.0
    for (a, b) in (((x0 + 3, -depth + 3), (x1 - 3, -depth + 3)), ((x0 + 3, -depth + 3), (x0 + 3, -2)),
                   ((x1 - 3, -depth + 3), (x1 - 3, -2))):
        mb.tube(IRON, [(a[0], y + h, a[1]), (b[0], y + h, b[1])], 2.5, segments=6)
        mb.tube(IRON, [(a[0], y + 12, a[1]), (b[0], y + 12, b[1])], 1.5, segments=6)
    x = x0 + 3
    while x <= x1 - 3:
        mb.tube(IRON, [(x, y, -depth + 3), (x, y + h, -depth + 3)], 1.1, segments=5, closed_ends=False)
        x += 11
    for zz in range(int(-depth + 14), -4, 11):
        for xx in (x0 + 3, x1 - 3):
            mb.tube(IRON, [(xx, y, zz), (xx, y + h, zz)], 1.1, segments=5, closed_ends=False)


def cornice(mb, style, width):
    top = style.top
    st = style.stone_tint
    c = shade(st, 0.8)
    # Frieze, then corbel brackets, then the crown moulding in steps.
    box(mb, STONE, (-4, top - 60, -8), (width + 4, top - 30, 0), shade(st, 0.75), skip=("+z",))
    x = 20.0
    while x < width - 20:
        box(mb, STONE, (x - 7, top - 30, -38), (x + 7, top, 0), shade(st, 0.7), skip=("+z",))
        x += 55.0
    steps = [(top, top + 10, -42), (top + 10, top + 22, -52), (top + 22, top + 30, -58)]
    for (ya, yb, dz) in steps:
        box(mb, STONE, (-10, ya, dz), (width + 10, yb, 0), c if yb < top + 25 else shade(st, 1.0), skip=("+z",))
    # Parapet (brick, stone coping).
    if style.roof != "mansard":
        box(mb, BRICK, (0, top + 30, -2), (width, top + 120, 25), shade(style.brick_tint, 0.7))
        box(mb, STONE, (-4, top + 120, -6), (width + 4, top + 132, 29), shade(st, 0.85))


def roof(mb, style, width, depth, rng):
    """The roof over x 0-width, z 0-depth: flat (tar-black boards) with chimney
    stacks, or a slate mansard with dormers along the street side."""
    top = style.top + 30
    if style.roof == "mansard":
        rise, inset = 300.0, 90.0
        # Steep lower slope on all four sides, then a flat top.
        p = [(0, top, 0), (width, top, 0), (width, top, depth), (0, top, depth)]
        q = [(inset, top + rise, inset), (width - inset, top + rise, inset), (width - inset, top + rise, depth - inset),
             (inset, top + rise, depth - inset)]
        for i in range(4):
            a, b = p[i], p[(i + 1) % 4]
            c_, d = q[(i + 1) % 4], q[i]
            mb.quad(SLATE, a, b, c_, d, uv_axes=slope_axes(a, b, d))
        mb.quad(PLATE, q[3], q[2], q[1], q[0], normal=(0, 1, 0))
        # Copper ridge cresting on the top's front edge.
        mb.tube(COPPER, [(inset, top + rise + 3, inset), (width - inset, top + rise + 3, inset)], 4.0, segments=8)
        # Dormers along the front slope.
        n = max(1, int(width // 260))
        for k in range(n):
            cx = width * (k + 0.5) / n
            dormer(mb, cx, top, inset, rise, rng.random() < style.lit_chance, style)
        chimney_z = depth * 0.6
    else:
        mb.quad(PLATE, (0, top - 10, depth), (width, top - 10, depth), (width, top - 10, 25), (0, top - 10, 25),
                normal=(0, 1, 0), colours=[(0.35, 0.33, 0.3, 1)] * 4)
        chimney_z = depth * 0.45
    # Chimney stacks with pots.
    for k in range(max(1, int(width // 450))):
        cx = width * (k + 0.5) / max(1, int(width // 450)) + rng.uniform(-60, 60)
        h = rng.uniform(220, 340) + (300 if style.roof == "mansard" else 0)
        base = top - 20
        bt = shade(style.brick_tint, 0.55)
        box(mb, BRICK, (cx - 60, base, chimney_z - 35), (cx + 60, base + h, chimney_z + 35), bt)
        box(mb, STONE, (cx - 66, base + h, chimney_z - 41), (cx + 66, base + h + 12, chimney_z + 41),
            shade(style.stone_tint, 0.6))
        style.chimney_tops.append((cx, base + h + 60, chimney_z))
        for px in (-30, 0, 30):
            mb.lathe(COPPER if rng.random() < 0.3 else STONE,
                     [(11, 0), (13, 8), (10, 30), (12, 40), (12, 44), (9, 44)],
                     centre=(cx + px, base + h + 12, chimney_z), segments=10,
                     colour=shade((0.6, 0.45, 0.35), 0.7))


def slope_axes(a, b, d):
    u = v_norm(v_sub(b, a))
    v = v_norm(v_sub(a, d))
    return u, v


def dormer(mb, cx, base, inset, rise, lit, style):
    w, h = 120.0, 200.0
    z0 = -10.0
    zb = inset * 1.6
    y0 = base + 40
    fc = style.frame
    # Cheeks (copper clad), front with window, a curved hood.
    box(mb, COPPER, (cx - w / 2, y0, z0 + 25), (cx - w / 2 + 12, y0 + h, zb))
    box(mb, COPPER, (cx + w / 2 - 12, y0, z0 + 25), (cx + w / 2, y0 + h, zb))
    hole = [(cx - 38, cx + 38, y0 + 30, y0 + h - 25)]
    wall(mb, COPPER, cx - w / 2, cx + w / 2, y0, y0 + h, hole, z=z0 + 25)
    reveal(mb, COPPER, *hole[0], 12, z=z0 + 25, sides="lrtb")
    sash_window(mb, hole[0][0], hole[0][1], hole[0][2], hole[0][3], z0 + 37, lit, fc, panes=(2, 2))
    prof = []
    for k in range(9):
        a = math.pi * k / 8
        prof.append((cx - (w / 2 + 10) * math.cos(a), y0 + h + 30 * math.sin(a)))
    for k in range(8):
        (xa, ya), (xb, yb) = prof[k], prof[k + 1]
        mb.quad(COPPER, (xa, ya, z0 + 15), (xb, yb, z0 + 15), (xb, yb, zb + 10), (xa, ya, zb + 10))
    mb.polygon(COPPER, [(x, y, z0 + 15) for x, y in prof[::-1]], normal=FRONT)


def building(style, width, depth, sides=(), seed=0):
    """A whole building: the front facade (z = 0), optional end facades
    ('left' on x = 0 facing -X, 'right' on x = width facing +X), plain brick
    elsewhere, and its roof."""
    mb = MeshBuilder("Building")
    facade(mb, style, width)
    for side in sides:
        sub = MeshBuilder("Side")
        facade(sub, style, depth)
        rotate_into(mb, sub, side, width, depth)
    # Back and plain ends (party walls), soot-dark brick.
    top = style.top + 30
    bt = shade(style.brick_tint, 0.45)
    mb.quad(BRICK, (width, 0, depth), (0, 0, depth), (0, top, depth), (width, top, depth), normal=(0, 0, 1),
            colours=[bt] * 4)
    if "left" not in sides:
        mb.quad(BRICK, (0, 0, depth), (0, 0, 0), (0, top, 0), (0, top, depth), normal=(-1, 0, 0), colours=[bt] * 4)
    if "right" not in sides:
        mb.quad(BRICK, (width, 0, 0), (width, 0, depth), (width, top, depth), (width, top, 0), normal=(1, 0, 0),
                colours=[bt] * 4)
    roof(mb, style, width, depth, style.rng)
    return mb


def rotate_into(mb, sub, side, width, depth):
    """sub, built as a front facade depth wide, turned onto an end wall of mb's
    building (rotations about Y, so nothing mirrors): 'left' is x = 0 facing -X,
    its left edge at the back; 'right' is x = width facing +X, its left edge at
    the front."""
    for material in sub.order:
        for tri in sub.sections[material]:
            verts = []
            for (p, n, uv, c, chart) in tri:
                if side == "left":
                    q, nn = (p[2], p[1], depth - p[0]), (n[2], n[1], -n[0])
                else:
                    q, nn = (width - p[2], p[1], p[0]), (-n[2], n[1], n[0])
                verts.append((q, nn, uv, c))
            mb.triangle_raw(material, verts, tri[0][4])


# ==============================================================================================
# Clock tower (the hero corner)
# ==============================================================================================

def clock_tower(base_y, size=420.0, shaft=650.0, seed=7):
    """A square brick tower, size wide, from base_y up shaft, with a clock in
    each face, a stone belfry stage, and a copper dome on a drum, crowned by a
    brass finial and a ring of gears. Centred on x = z = 0."""
    mb = MeshBuilder("ClockTower")
    h = size / 2
    st = (1.0, 0.97, 0.9)
    y1 = base_y + shaft
    # Brick shaft with stone quoins.
    for (n, a, b) in (((0, 0, -1), (-h, -h), (h, -h)), ((1, 0, 0), (h, -h), (h, h)),
                      ((0, 0, 1), (h, h), (-h, h)), ((-1, 0, 0), (-h, h), (-h, -h))):
        mb.quad(BRICK, (a[0], base_y, a[1]), (b[0], base_y, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1]), normal=n,
                colours=[shade((1, 1, 1), 0.7), shade((1, 1, 1), 0.7), shade((1, 1, 1), 0.9), shade((1, 1, 1), 0.9)])
    for k in range(int(shaft // 50)):
        y = base_y + k * 50
        w = 40 if k % 2 == 0 else 28
        for sx in (-1, 1):
            for sz in (-1, 1):
                x0 = sx * h - (w if sx > 0 else 0) if sx > 0 else -h
                cx = sx * (h - w / 2)
                cz = sz * (h - w / 2)
                box(mb, STONE, (cx - w / 2 - 3, y + 2, cz - w / 2 - 3), (cx + w / 2 + 3, y + 48, cz + w / 2 + 3),
                    shade(st, 0.95))
    # Clock stage: stone, projecting.
    cy0 = y1
    cy1 = cy0 + 520
    box(mb, STONE, (-h - 25, cy0 - 30, -h - 25), (h + 25, cy0, h + 25), shade(st, 0.8))
    box(mb, STONE, (-h - 15, cy0, -h - 15), (h + 15, cy1, h + 15), shade(st, 1.0))
    centre_y = (cy0 + cy1) / 2
    r = 170.0
    for face in range(4):
        clock_face(mb, face, h + 15, centre_y, r, seed + face)
    # Cornice and balustrade.
    box(mb, STONE, (-h - 35, cy1, -h - 35), (h + 35, cy1 + 25, h + 35), shade(st, 0.85))
    for k in range(-5, 6):
        for side in range(4):
            t = k * (size + 40) / 11
            x, z = [(t, -h - 25), (h + 25, t), (t, h + 25), (-h - 25, t)][side]
            mb.lathe(STONE, [(6, 0), (9, 10), (5, 30), (8, 45), (6, 50)], centre=(x, cy1 + 25, z), segments=8,
                     colour=shade(st, 0.9))
    box(mb, STONE, (-h - 35, cy1 + 75, -h - 35), (h + 35, cy1 + 88, h + 35), shade(st, 0.85))
    # Drum and copper dome.
    drum_y = cy1 + 25
    mb.lathe(STONE, [(h - 20, 0), (h - 20, 160), (h - 5, 170), (h - 5, 185)], centre=(0, drum_y, 0), segments=32,
             colour=shade(st, 0.95))
    dome = []
    for k in range(13):
        a = (math.pi / 2) * k / 12
        dome.append(((h - 10) * math.cos(a) * (1 + 0.12 * math.sin(a * 2)), 185 + (h + 40) * math.sin(a)))
    dome = [(h + 5, 185)] + dome[:-1] + [(18, 185 + h + 40), (18, 185 + h + 70)]
    mb.lathe(COPPER, dome, centre=(0, drum_y, 0), segments=40)
    # Ribs on the dome (brass).
    for k in range(8):
        a = 2 * math.pi * k / 8
        pts = [(math.cos(a) * rr, drum_y + yy, math.sin(a) * rr) for rr, yy in dome[1:-2:2]]
        mb.tube(BRASS, pts, 3.5, segments=6)
    # Finial: a brass spire with an orrery of rings.
    fy = drum_y + 185 + h + 70
    mb.lathe(BRASS, [(16, 0), (22, 10), (10, 40), (14, 60), (5, 70), (4, 260), (0, 300)], centre=(0, fy, 0),
             segments=16)
    for k, (rad, tilt) in enumerate(((55, 20), (42, -35), (30, 60))):
        ring = []
        for s in range(25):
            a = 2 * math.pi * s / 24
            x, y = math.cos(a) * rad, math.sin(a) * rad
            t = math.radians(tilt)
            ring.append((x, fy + 150 + y * math.sin(t) * 0.0 + math.sin(a) * rad * math.sin(t), y * math.cos(t)))
        mb.tube(BRASS, ring, 2.2, segments=6, closed_ends=False)
    mb.lathe(BRASS, [(0, -14), (14, 0), (0, 14)], centre=(0, fy + 150, 0), segments=12)
    # Gears on the shaft's two street faces.
    for face, y in ((0, base_y + shaft * 0.55), (3, base_y + shaft * 0.35)):
        gear_on_face(mb, face, h, y, 95.0, 14)
    return mb


def clock_face(mb, face, half, cy, r, seed):
    """A dial on the tower face (0: -Z, 1: +X, 2: +Z, 3: -X): brass bezel, the
    enamel face (UV 0-1), iron hands at ten past four."""
    def place(u, v, out):
        # u right, v up, out toward the viewer, on the face.
        if face == 0:
            return (u, cy + v, -half - out)
        if face == 1:
            return (half + out, cy + v, u)
        if face == 2:
            return (-u, cy + v, half + out)
        return (-half - out, cy + v, -u)
    normal = place(0, 0, 1)
    normal = v_norm(v_sub(normal, place(0, 0, 0)))
    segs = 48
    centre = place(0, 0, 4)
    ring = [place(r * math.cos(2 * math.pi * k / segs), r * math.sin(2 * math.pi * k / segs), 4) for k in range(segs)]
    uvs = [(0.5 + 0.5 * math.cos(2 * math.pi * k / segs), 0.5 - 0.5 * math.sin(2 * math.pi * k / segs))
           for k in range(segs)]
    for k in range(segs):
        a, b = ring[k], ring[(k + 1) % segs]
        mb.polygon(CLOCK, [centre, a, b], normal=normal, uvs=[(0.5, 0.5), uvs[k], uvs[(k + 1) % segs]])
    # Bezel: a brass ring standing proud.
    for k in range(segs):
        a0, a1 = 2 * math.pi * k / segs, 2 * math.pi * (k + 1) / segs
        pts = [place((r + rr) * math.cos(a), (r + rr) * math.sin(a), d) for (rr, d, a) in
               ((0, 14, a0), (0, 14, a1), (22, 14, a1), (22, 14, a0))]
        mb.polygon(BRASS, pts, normal=normal)
        side = [place(r * math.cos(a0), r * math.sin(a0), 4), place(r * math.cos(a1), r * math.sin(a1), 4),
                place(r * math.cos(a1), r * math.sin(a1), 14), place(r * math.cos(a0), r * math.sin(a0), 14)]
        mb.polygon(BRASS, side)
    # Hands: hour to ~4, minute to ~2 (ten past four), drawn as thin iron slabs.
    for (angle, length, width) in ((math.radians(90 - 125), r * 0.55, 9), (math.radians(90 - 60), r * 0.85, 6)):
        d = (math.cos(angle), math.sin(angle))
        p = (-d[1], d[0])
        tail = -0.18 * length
        pts = [place(d[0] * tail + p[0] * width, d[1] * tail + p[1] * width, 9),
               place(d[0] * length * 0.8 + p[0] * width * 0.6, d[1] * length * 0.8 + p[1] * width * 0.6, 9),
               place(d[0] * length, d[1] * length, 9),
               place(d[0] * length * 0.8 - p[0] * width * 0.6, d[1] * length * 0.8 - p[1] * width * 0.6, 9),
               place(d[0] * tail - p[0] * width, d[1] * tail - p[1] * width, 9)]
        mb.polygon(IRON, pts, normal=normal, colours=[(0.4, 0.4, 0.4, 1)] * 5)


def gear_profile(radius, teeth, depth=0.18):
    pts = []
    for k in range(teeth * 4):
        a = 2 * math.pi * k / (teeth * 4)
        phase = k % 4
        rr = radius * (1 + depth / 2) if phase in (1, 2) else radius * (1 - depth / 2)
        pts.append((rr * math.cos(a), rr * math.sin(a)))
    return pts


def gear_on_face(mb, face, half, cy, radius, teeth, thickness=10.0, material=BRASS):
    """A flat gear proud of a tower face, spokes and hub."""
    def place(u, v, out):
        if face == 0:
            return (u, cy + v, -half - out)
        if face == 1:
            return (half + out, cy + v, u)
        if face == 2:
            return (-u, cy + v, half + out)
        return (-half - out, cy + v, -u)
    gear_mesh(mb, place, radius, teeth, thickness, material)


def gear_mesh(mb, place, radius, teeth, thickness, material):
    """A gear with a rim, spokes and a hub, in a frame place(u, v, out)."""
    outer = gear_profile(radius, teeth)
    inner_r = radius * 0.72
    n = len(outer)
    normal = v_norm(v_sub(place(0, 0, 1), place(0, 0, 0)))
    # Rim: ring between the toothed outline and inner_r, front and the outer wall.
    for k in range(n):
        a, b = outer[k], outer[(k + 1) % n]
        ang_a, ang_b = math.atan2(a[1], a[0]), math.atan2(b[1], b[0])
        ia = (inner_r * math.cos(ang_a), inner_r * math.sin(ang_a))
        ib = (inner_r * math.cos(ang_b), inner_r * math.sin(ang_b))
        mb.polygon(material, [place(*ia, thickness), place(*a, thickness), place(*b, thickness), place(*ib, thickness)],
                   normal=normal)
        mb.polygon(material, [place(*a, 0), place(*b, 0), place(*b, thickness), place(*a, thickness)])
        mb.polygon(material, [place(*ib, 0), place(*ia, 0), place(*ia, thickness), place(*ib, thickness)])
    # Spokes and hub.
    for s in range(6):
        a = 2 * math.pi * s / 6
        d = (math.cos(a), math.sin(a))
        p = (-d[1] * 8, d[0] * 8)
        r0, r1 = radius * 0.18, inner_r + 2
        quad = [(d[0] * r0 + p[0], d[1] * r0 + p[1]), (d[0] * r1 + p[0], d[1] * r1 + p[1]),
                (d[0] * r1 - p[0], d[1] * r1 - p[1]), (d[0] * r0 - p[0], d[1] * r0 - p[1])]
        mb.polygon(material, [place(u, v, thickness * 0.8) for u, v in quad], normal=normal)
        for i in range(4):
            a2, b2 = quad[i], quad[(i + 1) % 4]
            mb.polygon(material, [place(*a2, 0), place(*b2, 0), place(*b2, thickness * 0.8),
                                  place(*a2, thickness * 0.8)])
    hub = [(radius * 0.2 * math.cos(2 * math.pi * k / 16), radius * 0.2 * math.sin(2 * math.pi * k / 16))
           for k in range(16)]
    mb.polygon(material, [place(u, v, thickness * 1.3) for u, v in hub], normal=normal)
    for k in range(16):
        a, b = hub[k], hub[(k + 1) % 16]
        mb.polygon(material, [place(*a, 0), place(*b, 0), place(*b, thickness * 1.3), place(*a, thickness * 1.3)])


# ==============================================================================================
# Street furniture and props
# ==============================================================================================

def gas_lamp():
    """A cast-iron gas lamp: a fluted base, a slender post with a ladder bar,
    and a four-sided lantern of glowing glass under a brass-trimmed cap. The
    mantle (the light) sits at y = 400."""
    mb = MeshBuilder("GasLamp")
    mb.lathe(IRON, [(26, 0), (26, 12), (20, 18), (20, 60), (14, 75), (16, 90), (9, 110), (8, 112)], segments=16,
             caps=True)
    mb.lathe(IRON, [(8, 110), (6.5, 120), (6.5, 340), (8, 345), (8, 352), (12, 356), (12, 362)], segments=12)
    mb.tube(IRON, [(-38, 330, 0), (38, 330, 0)], 2.5, segments=6)         # ladder bar
    for x in (-38, 38):
        mb.lathe(BRASS, [(4, -4), (4, 4), (0, 6)], centre=(x, 330, 0), segments=8)
    # Lantern: tapered glass sides between iron corner posts.
    y0, y1 = 362.0, 440.0
    r0, r1 = 13.0, 21.0
    corners0 = [(r0 * sx, y0, r0 * sz) for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    corners1 = [(r1 * sx, y1, r1 * sz) for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    for i in range(4):
        a0, b0 = corners0[i], corners0[(i + 1) % 4]
        a1, b1 = corners1[i], corners1[(i + 1) % 4]
        mb.quad(LAMP_GLOW, a0, b0, b1, a1)
        mb.tube(IRON, [a0, a1], 1.3, segments=5)
    mb.box(IRON, (-15, y0 - 4, -15), (15, y0, 15))
    # Cap: a flared iron roof, brass band, finial.
    mb.lathe(IRON, [(24, 0), (26, 3), (16, 22), (6, 34), (6, 38)], centre=(0, y1, 0), segments=16, caps=True)
    mb.lathe(BRASS, [(24.5, -3), (24.5, 2)], centre=(0, y1, 0), segments=16)
    mb.lathe(BRASS, [(4, 0), (6, 6), (0, 14)], centre=(0, y1 + 38, 0), segments=10)
    return mb


def bollard():
    mb = MeshBuilder("Bollard")
    mb.lathe(IRON, [(14, 0), (14, 10), (11, 16), (10, 75), (12, 80), (12, 86), (7, 96), (0, 100)], segments=14,
             caps=True)
    return mb


def barrel():
    mb = MeshBuilder("Barrel")
    prof = []
    for k in range(11):
        t = k / 10
        prof.append((30 + 7 * math.sin(math.pi * t), 95 * t))
    mb.lathe(WOOD, prof, segments=20, caps=True, v_scale=1.0)
    for t in (0.1, 0.3, 0.7, 0.9):
        r = 30 + 7 * math.sin(math.pi * t) + 0.6
        mb.lathe(IRON, [(r, 95 * t - 3), (r, 95 * t + 3)], segments=20)
    return mb


def crate(size=(80, 70, 80)):
    mb = MeshBuilder("Crate")
    w, h, d = size
    mb.box(WOOD, (-w / 2 + 3, 0, -d / 2 + 3), (w / 2 - 3, h - 3, d / 2 - 3))
    # Frame battens on every edge.
    t = 7
    for (lo, hi) in [((-w / 2, 0, -d / 2), (-w / 2 + t, h, -d / 2 + t)), ((w / 2 - t, 0, -d / 2), (w / 2, h, -d / 2 + t)),
                     ((-w / 2, 0, d / 2 - t), (-w / 2 + t, h, d / 2)), ((w / 2 - t, 0, d / 2 - t), (w / 2, h, d / 2)),
                     ((-w / 2, h - t, -d / 2), (w / 2, h, -d / 2 + t)), ((-w / 2, h - t, d / 2 - t), (w / 2, h, d / 2)),
                     ((-w / 2, h - t, -d / 2), (-w / 2 + t, h, d / 2)), ((w / 2 - t, h - t, -d / 2), (w / 2, h, d / 2)),
                     ((-w / 2, 0, -d / 2), (w / 2, t, -d / 2 + t)), ((-w / 2, 0, d / 2 - t), (w / 2, t, d / 2))]:
        mb.box(WOOD, lo, hi, colour_fn=lambda p, n: (0.75, 0.7, 0.65, 1))
    return mb


def advert_column():
    """A Victorian advertising column: a stone plinth, a drum pasted with
    posters (Paint, vertex-tinted panels), a brass ring and a copper dome."""
    mb = MeshBuilder("AdvertColumn")
    mb.lathe(STONE, [(70, 0), (70, 30), (62, 40), (62, 45)], segments=24, caps=True)
    rng = random.Random(4)
    segs = 24
    r = 58.0
    for k in range(segs):
        a0, a1 = 2 * math.pi * k / segs, 2 * math.pi * (k + 1) / segs
        poster = k // 3
        rng_p = random.Random(poster * 31 + 7)
        c = rng_p.choice([(0.62, 0.56, 0.44), (0.42, 0.20, 0.14), (0.20, 0.24, 0.30), (0.58, 0.48, 0.28), (0.26, 0.32, 0.24)])
        for (y0, y1, cc) in ((45, 140, c), (140, 260, shade(c, 0.85)[:3]), (260, 290, (0.2, 0.18, 0.15))):
            p = [(r * math.cos(a0), y0, r * math.sin(a0)), (r * math.cos(a1), y0, r * math.sin(a1)),
                 (r * math.cos(a1), y1, r * math.sin(a1)), (r * math.cos(a0), y1, r * math.sin(a0))]
            n0 = (math.cos(a0), 0, math.sin(a0))
            n1 = (math.cos(a1), 0, math.sin(a1))
            col = (cc[0], cc[1], cc[2], 1.0)
            u0, u1 = a0 * r / 100, a1 * r / 100
            verts = [(p[0], n0, (u0, y0 / 100), col), (p[1], n1, (u1, y0 / 100), col), (p[2], n1, (u1, y1 / 100), col),
                     (p[3], n0, (u0, y1 / 100), col)]
            mb.triangle_raw(PAINT, [verts[0], verts[1], verts[2]])
            mb.triangle_raw(PAINT, [verts[0], verts[2], verts[3]])
    mb.lathe(BRASS, [(60, 288), (64, 292), (64, 300), (60, 304)], segments=24)
    mb.lathe(COPPER, [(72, 304), (72, 312), (60, 320), (45, 345), (22, 362), (8, 372), (8, 385)], segments=24)
    mb.lathe(BRASS, [(8, 385), (12, 392), (0, 410)], segments=10)
    return mb


def boiler():
    """A riveted pressure boiler on iron saddles, with a brass gauge, a valve
    wheel, a stack and copper pipework. Steam rises from its relief valve at
    (0, 300, 0) local (the level puts a Steam emitter there)."""
    mb = MeshBuilder("Boiler")
    length, radius = 320.0, 85.0
    prof = [(0, -length / 2 - 30), (radius * 0.7, -length / 2 - 22), (radius, -length / 2)] + \
           [(radius, -length / 2 + k * length / 6) for k in range(1, 7)] + \
           [(radius * 0.7, length / 2 + 22), (0, length / 2 + 30)]
    mb.lathe(PLATE, prof, centre=(0, 120, 0), segments=28, axis="x")
    # Bands (rivet rows) round the drum.
    for k in range(5):
        x = -length / 2 + (k + 0.5) * length / 5
        mb.lathe(IRON, [(radius + 1.5, x - 4), (radius + 1.5, x + 4)], centre=(0, 120, 0), segments=28, axis="x")
    # Saddles.
    for x in (-length / 3, length / 3):
        mb.box(IRON, (x - 12, 0, -70), (x + 12, 70, 70))
        mb.box(IRON, (x - 18, 0, -80), (x + 18, 8, 80))
    # Stack and dome.
    mb.lathe(PLATE, [(30, 0), (30, 30), (20, 40), (18, 210), (24, 215), (24, 225)], centre=(-90, 120 + radius - 8, 0),
             segments=16)
    mb.lathe(BRASS, [(26, 0), (26, 20), (18, 35), (8, 45), (8, 60), (12, 64), (0, 70)], centre=(60, 120 + radius - 6, 0),
             segments=16)
    # Gauge on the front end (-X): a brass case with the dial.
    gx = -length / 2 - 34
    mb.tube(COPPER, [(gx + 20, 175, 0), (gx - 10, 175, 0), (gx - 10, 215, 0)], 3.0, segments=8)
    gauge_disc(mb, (gx - 10, 245, 0), (-1, 0, 0), 26)
    # Valve wheel on the side (-Z).
    wheel(mb, (40, 150, -radius - 25), (0, 0, -1), 28)
    mb.tube(BRASS, [(40, 150, -radius + 5), (40, 150, -radius - 22)], 4.0, segments=8)
    # Copper pipes from the drum, down and away (into the building).
    mb.tube(COPPER, [(100, 120 + radius - 10, 0), (100, 260, 0), (100, 260, 160), (100, 30, 160)], 6.0, segments=10)
    for y in (240, 90):
        mb.lathe(BRASS, [(9, -4), (9, 4)], centre=(100, y, 160), segments=10)
    return mb


def gauge_disc(mb, centre, normal, r):
    """A dial facing normal (an axis), UV 0-1, in a brass case."""
    n = v_norm(normal)
    up = (0.0, 1.0, 0.0)
    right = v_norm(v_cross(n, up))
    segs = 24
    pts = [v_add(centre, v_add(v_mul(right, r * math.cos(2 * math.pi * k / segs)),
                               v_mul(up, r * math.sin(2 * math.pi * k / segs)))) for k in range(segs)]
    face = [v_add(p, v_mul(n, 2)) for p in pts]
    uvs = [(0.5 + 0.5 * math.cos(2 * math.pi * k / segs), 0.5 - 0.5 * math.sin(2 * math.pi * k / segs))
           for k in range(segs)]
    c = v_add(centre, v_mul(n, 2))
    for k in range(segs):
        mb.polygon(GAUGE, [c, face[k], face[(k + 1) % segs]], normal=n,
                   uvs=[(0.5, 0.5), uvs[k], uvs[(k + 1) % segs]])
    back = [v_sub(p, v_mul(n, 10)) for p in pts]
    for k in range(segs):
        a, b = k, (k + 1) % segs
        rim_o = [v_add(centre, v_add(v_mul(right, (r + 5) * math.cos(2 * math.pi * i / segs)),
                                     v_mul(up, (r + 5) * math.sin(2 * math.pi * i / segs)))) for i in (a, b)]
        mb.polygon(BRASS, [v_sub(rim_o[0], v_mul(n, 10)), v_sub(rim_o[1], v_mul(n, 10)), v_add(rim_o[1], v_mul(n, 5)),
                           v_add(rim_o[0], v_mul(n, 5))])
        mb.polygon(BRASS, [v_add(rim_o[0], v_mul(n, 5)), v_add(rim_o[1], v_mul(n, 5)), v_add(pts[b], v_mul(n, 5)),
                           v_add(pts[a], v_mul(n, 5))], normal=n)


def wheel(mb, centre, normal, r):
    n = v_norm(normal)
    up = (0.0, 1.0, 0.0) if abs(n[1]) < 0.9 else (1.0, 0.0, 0.0)
    right = v_norm(v_cross(n, up))
    up = v_cross(right, n)
    ring = [v_add(centre, v_add(v_mul(right, r * math.cos(2 * math.pi * k / 20)), v_mul(up, r * math.sin(2 * math.pi * k / 20))))
            for k in range(21)]
    mb.tube(IRON, ring, 3.2, segments=8, closed_ends=False)
    for k in range(4):
        a = 2 * math.pi * k / 4 + 0.4
        mb.tube(IRON, [centre, v_add(centre, v_add(v_mul(right, r * math.cos(a)), v_mul(up, r * math.sin(a))))], 2.2,
                segments=6)


def manhole():
    mb = MeshBuilder("SteamGrate")
    mb.lathe(IRON, [(0, 0), (48, 0), (50, 2), (50, 4)], segments=24)
    # A grate of bars across the disc, raised.
    for k in range(-4, 5):
        x = k * 10.0
        half = math.sqrt(max(46.0 ** 2 - x * x, 0.0))
        mb.box(IRON, (x - 3, 0, -half), (x + 3, 5, half), colour_fn=lambda p, n: (0.8, 0.8, 0.8, 1))
    mb.lathe(BRASS, [(50, 0), (54, 1), (54, 5), (50, 6)], segments=24)
    return mb


def street_sign():
    """A cast-iron finger post with two painted arms."""
    mb = MeshBuilder("StreetSign")
    mb.lathe(IRON, [(12, 0), (12, 12), (7, 20), (5, 300), (7, 305), (7, 312), (0, 322)], segments=12, caps=True)
    for (yaw, y, c) in ((0.0, 270.0, (0.12, 0.2, 0.15)), (math.pi / 2, 240.0, (0.35, 0.08, 0.06))):
        d = (math.cos(yaw), 0, math.sin(yaw))
        n = (-d[2], 0, d[0])
        p0 = v_add((0, y, 0), v_mul(d, 6))
        p1 = v_add((0, y, 0), v_mul(d, 95))
        # a board 89 long, 22 tall, 3 thick, pointed tip
        for s in (1, -1):
            off = v_mul(n, 1.5 * s)
            pts = [v_add(p0, off), v_add(p1, off), v_add(v_add(p1, v_mul(d, 12)), v_add(off, (0, 11, 0))),
                   v_add(p1, v_add(off, (0, 22, 0))), v_add(p0, v_add(off, (0, 22, 0)))]
            mb.polygon(PAINT, pts, normal=v_mul(n, s), colours=[(c[0], c[1], c[2], 1)] * 5)
        mb.box(BRASS, (min(p0[0], p1[0]) - 1, y + 9, min(p0[2], p1[2]) - 2), (max(p0[0], p1[0]) + 1, y + 13,
                                                                               max(p0[2], p1[2]) + 2))
    return mb


def airship():
    """A dirigible: a long doped-canvas envelope with brass bands and fins, a
    timber gondola with portholes, and two propellers. Along +X, 4000 long."""
    mb = MeshBuilder("Airship")
    L, R = 4000.0, 520.0
    prof = []
    for k in range(25):
        t = k / 24
        x = -L / 2 + t * L
        r = R * math.sin(math.pi * t) ** 0.62 * (1.0 - 0.15 * t)
        prof.append((max(r, 0.0), x))
    prof[0] = (0.0, -L / 2)
    prof[-1] = (0.0, L / 2)
    mb.lathe(FABRIC, [(r, x) for r, x in prof], segments=36, axis="x",
             colour=(0.62, 0.52, 0.38, 1.0))
    for t in (0.2, 0.35, 0.5, 0.65, 0.8):
        r = R * math.sin(math.pi * t) ** 0.62 * (1.0 - 0.15 * t) + 3
        x = -L / 2 + t * L
        mb.lathe(BRASS, [(r, x - 12), (r, x + 12)], segments=36, axis="x")
    # Tail fins (four), copper framed fabric.
    for k in range(4):
        a = math.pi / 2 * k + math.pi / 4
        d = (0, math.cos(a), math.sin(a))
        base0 = -L / 2 + 250
        pts = [(base0, 0, 0), (base0 + 650, 0, 0), (base0 + 120, 0, 0)]
        p_root0 = v_add((base0 + 60, 0, 0), v_mul(d, R * 0.35))
        p_root1 = v_add((base0 + 700, 0, 0), v_mul(d, R * 0.62))
        p_tip1 = v_add((base0 + 380, 0, 0), v_mul(d, R * 1.05))
        p_tip0 = v_add((base0 - 60, 0, 0), v_mul(d, R * 0.95))
        quad = [p_root0, p_root1, p_tip1, p_tip0]
        mb.polygon(FABRIC, quad, colours=[(0.45, 0.2, 0.12, 1)] * 4)
        mb.polygon(FABRIC, quad[::-1], colours=[(0.45, 0.2, 0.12, 1)] * 4)
        mb.tube(COPPER, quad + [quad[0]], 6.0, segments=6, closed_ends=False)
    # Gondola: a long timber cabin under the middle, on brass struts.
    gy = -R - 160
    mb.box(WOOD, (-500, gy, -110), (500, gy + 150, 110))
    mb.lathe(BRASS, [(0, -620), (110, -500), (110, 500), (0, 620)], centre=(0, gy + 75, 0), segments=4, axis="x")
    mb.box(COPPER, (-520, gy + 150, -125), (520, gy + 165, 125))
    for x in range(-420, 421, 120):
        for z in (-111, 111):
            n = (0, 0, -1) if z < 0 else (0, 0, 1)
            gauge_like_port(mb, (x, gy + 90, z), n)
    for x in (-380, 0, 380):
        for z in (-100, 100):
            mb.tube(BRASS, [(x, gy + 160, z), (x * 0.9, -R * 0.93, z * 1.6)], 5.0, segments=6)
    # Propellers on outriggers.
    for z in (-260, 260):
        mb.tube(IRON, [(380, gy + 100, z * 0.4), (420, gy + 100, z)], 6.0, segments=8)
        mb.lathe(BRASS, [(0, 0), (25, 10), (25, 60), (0, 90)], centre=(470, gy + 100, z), segments=12, axis="x")
        for k in range(4):
            a = math.pi / 2 * k + 0.3
            tip = (548, gy + 100 + math.cos(a) * 140, z + math.sin(a) * 140)
            mb.polygon(WOOD, [(548, gy + 100, z), (548, gy + 100 + math.cos(a + 0.18) * 140,
                                                   z + math.sin(a + 0.18) * 140), tip])
            mb.polygon(WOOD, [(548, gy + 100, z), tip, (548, gy + 100 + math.cos(a + 0.18) * 140,
                                                        z + math.sin(a + 0.18) * 140)])
    return mb


def gauge_like_port(mb, centre, n):
    """A brass porthole with lit glass."""
    segs = 12
    up = (0.0, 1.0, 0.0)
    right = v_norm(v_cross(n, up))
    pts = [v_add(centre, v_add(v_mul(right, 22 * math.cos(2 * math.pi * k / segs)), v_mul(up, 22 * math.sin(2 * math.pi * k / segs))))
           for k in range(segs)]
    glass = [v_add(p, v_mul(n, 1)) for p in pts]
    mb.polygon(WINDOW_LIT, glass, normal=n, uvs=[(0.5 + 0.5 * math.cos(2 * math.pi * k / segs), 0.5 + 0.5 * math.sin(2 * math.pi * k / segs)) for k in range(segs)])
    for k in range(segs):
        a, b = pts[k], pts[(k + 1) % segs]
        oa = v_add(centre, v_mul(v_sub(a, centre), 1.3))
        ob = v_add(centre, v_mul(v_sub(b, centre), 1.3))
        mb.polygon(BRASS, [v_add(a, v_mul(n, 4)), v_add(oa, v_mul(n, 4)), v_add(ob, v_mul(n, 4)), v_add(b, v_mul(n, 4))],
                   normal=n)
