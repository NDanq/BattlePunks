"""
Demo Lab's level layouts: the data make_levels.py builds the grey-box worlds
from, kept apart so the art pass can dress the same rooms later (Tools/Art
reads this module for room rectangles, spawn points and interactable spots).

Units are centimetres. +X is east, +Z is north, +Y is up; a yaw of 0 faces
north (+Z) and 90 faces east (+X), as the engine's yaw = atan2(x, z).

  Rect rooms     (x0, z0, x1, z1): walls stand inside the rectangle, so two
                 rooms that share an edge get two walls side by side.
  Doors          per side ('N', 'S', 'E', 'W'): (position along the side, width).
                 N/S doors give an X, E/W doors a Z.
  Octagons       a centre, an apothem (centre to the inside of a wall) and
                 doors per side by compass name ('N', 'NE', ... 'NW').
  Rotated rooms  a centre, a size, a yaw, doors by side as (offset, width) in
                 the room's own frame.
  Points         (x, y, z, yaw): spawns, interactables, cameras' targets.
  Routes         [(x, z), ...]: the AutoPlay test's waypoints ("AP_<route>_<n>").
"""

WALL_THICKNESS = 30
DOOR_HEIGHT = 330

# =============================================================================
# MainMenu: the lab's facade at dusk, seen from the plaza. The menu world is
# L1's own ground (its terrain, plaza, facade and cliff, dressed by
# dress_l1.dress_menu), so its coordinates are L1's: the blast door at
# (3700, 1200, 6985). 'ground' and 'facade' are the grey-box stand-ins.
# =============================================================================
MENU = {
    'ground': (1000, 4500, 6500, 8000),
    'facade': [   # (name, centre, size)
        ('Facade_Cliff',   (3700, 2700, 7400), (9000, 3000, 600)),
        ('Facade_Front',   (3700, 1900, 7000), (3200, 1400, 100)),
        ('Facade_Door',    (3700, 1500, 6960), (600, 600, 40)),
        ('Plaza',          (3700, 1190, 6200), (5000, 20, 3200)),
    ],
    # the camera's slow dolly across the plaza: (frame, position, target)
    'dolly': [(0, (2300, 1420, 5050), (3700, 1700, 6985)),
              (600, (3700, 1330, 5350), (3700, 1800, 6985)),
              (1200, (5100, 1420, 5050), (3700, 1700, 6985))],
}

# =============================================================================
# L1 Foothills: the dock (south-west), the beach, the switchback road climbing
# north-east (three hairpins: the second is the overlook over the sea and the
# forest), the gatehouse barrier across the road, the meadow and the signal
# mast east, the plaza under the cliff and the blast door in it. The ground is
# a terrain (dress_l1.py); 'road' is the one Spline the road, its lamps, rails
# and fence posts come from: (x, y, z) control points, the road's surface
# height at each (the terrain is cut and filled to it).
# =============================================================================
L1 = {
    'ground': (-1200, -2500, 6500, 8000),
    'sea': (-9000, -6000, -1200, 4000),
    'sea_level': -120,
    'pier': (-4400, -1650, -1200, -1350),               # deck top y = 0
    'road': [(-1150, 5, -1490), (-500, 25, -1330), (500, 55, -1120), (1700, 110, -760), (2900, 200, -150),
             (3650, 270, 380), (3800, 300, 800), (3400, 330, 1150), (2300, 390, 1330), (1200, 450, 1480),
             (550, 495, 1800), (750, 530, 2280), (1700, 590, 2560), (2700, 640, 2650), (3400, 660, 2660),
             (4300, 690, 2720), (5000, 760, 3100), (5200, 850, 3650), (4800, 950, 4200), (4100, 1060, 4750),
             (3750, 1150, 5250), (3700, 1200, 5700)],
    'road_width': 600,
    # the grey box's road terraces (kept for make_levels' stand-ins; the dressing removes them)
    'leg1': {'x0': 1200, 'x1': 1800, 'z0': -600, 'z1': 2400, 'steps': 10, 'y0': 0, 'y1': 300},
    'landing': (200, 2400, 4000, 3000, 300),            # x0, z0, x1, z1, top
    'leg2': {'x0': 3400, 'x1': 4000, 'z0': 3000, 'z1': 5400, 'steps': 8, 'y0': 300, 'y1': 600},
    'plaza': (2300, 5450, 5100, 6950, 1200),             # x0, z0, x1, z1, top
    'cliff': (1000, 7000, 6500, 7600, 2600),             # x0, z0, x1, z1, height (stand-in)
    'gatehouse': (3060, 2890, 3625, 3310, 575),          # the kit's footprint (yaw 0, centre 3300, 3100)
    'gatehouse_centre': (3300, 3100),
    'overlook': (250, 1850),
    'forest': (-1000, 3500, 800, 6500),                  # grey-box spruce stand-ins
    'mast': (6300, 3700),
    'points': {
        'Entry_Dock':      (-2600, 0, -1500, 90),
        'Crank_Gatehouse': (3055, 810, 3100, 90),        # the kit's crank wheel on the gatehouse's west wall
        'Gate':            (3060, 650, 2650, 90),        # the barrier across the road (yaw 90: along Z)
        'BlastDoor':       (3700, 1200, 6985, 0),
        'Trigger_AtGate':  (2500, 700, 2620, 0),
        'Trigger_Overlook': (450, 600, 1850, 0),
        'Trigger_Plaza':   (3700, 1300, 5800, 0),
        'Trigger_BlastDoor': (3700, 1300, 6500, 0),
    },
    'routes': {
        'L1_Road1': [(-1800, -1500), (-1150, -1490), (-710, -1380), (-270, -1280), (170, -1200), (610, -1090), (1040, -980),
                     (1470, -850), (1890, -680), (2290, -490), (2690, -270), (3080, -40), (3460, 200), (3750, 540),
                     (3710, 950), (3330, 1180), (2890, 1270), (2440, 1310), (1990, 1370), (1540, 1410), (1110, 1510),
                     (690, 1670), (580, 2050), (890, 2370), (1320, 2490), (1760, 2570), (2210, 2620), (2660, 2650),
                     (2850, 2700), (2960, 3000)],
        'L1_Road2': [(2850, 2700), (3110, 2660), (3560, 2660), (4010, 2670), (4450, 2770), (4850, 2970), (5130, 3310),
                     (5170, 3740), (4900, 4100), (4560, 4390), (4200, 4660), (3900, 4990), (3720, 5400), (3700, 5700),
                     (3700, 6450)],
    },
}

# =============================================================================
# L2 The Laboratory: airlock south, foyer, the round atrium in the centre,
# the materials workshop west, the conservatory east (behind glass), the
# archive north-west, the turbine hall north (the freight lift at its west
# end) and the elevator north-east.
# =============================================================================
L2 = {
    # Heights: 590 is the kit wall (500) scaled so its doorway clears the 330 door gaps; the
    # atrium's walls stop there too and the glass dome sits on them (dress_l2_laboratory.py).
    'rooms': {   # name: (x0, z0, x1, z1, height, doors)
        'Airlock':      (-300, -3000, 300, -2400, 590, {'N': [(0, 240)]}),
        'Foyer':        (-900, -2400, 900, -1200, 590, {'S': [(0, 240)], 'N': [(0, 300)]}),
        'Workshop':     (-3200, -900, -1200, 900, 590, {'E': [(0, 300)], 'N': [(-2200, 240)]}),
        'Archive':      (-3200, 900, -1200, 2600, 590, {'S': [(-2200, 240)], 'N': [(-2200, 300)]}),
        'TurbineHall':  (-3200, 2600, 2800, 4200, 700, {'S': [(-2200, 300), (1900, 300)]}),
        'Elevator':     (1200, 1000, 2800, 2600, 590, {'N': [(1900, 300)]}),
        'Conservatory': (1200, -1000, 3200, 1000, 590, {}),
    },
    'atrium': {'centre': (0, 0), 'apothem': 1200, 'height': 590, 'doors': {'S': 300, 'W': 300}, 'glass': ['E']},
    'orrery': (0, 0, 250, 300),                          # centre x, z, radius, height
    # the gantry: steps up the orrery's west side to a platform under the dome
    'gantry': {'x0': -1000, 'z0': 450, 'z1': 750, 'steps': 15, 'run': 80, 'rise': 30,
               'platform': (200, 400, 700, 800)},
    'turbines': [(-600, 3150), (400, 3150), (1100, 3150)],
    'benches': [(-2600, 500), (-2600, -760), (-1800, -760)],   # workshop benches (centres); 1 and 2 against the south wall
    'samples': ['Anisotropic', 'ClearCoat', 'Cloth', 'Subsurface', 'Translucent', 'Volume'],
    'points': {
        'Entry_Airlock':     (0, 0, -2800, 0),
        'Entry_FreightLift': (-2900, 0, 3400, 90),
        'Breaker':           (-850, 120, -1800, 270),
        'Keycard':           (-2600, 95, 480, 0),
        'KeycardReader':     (-2000, 120, 855, 0),
        'Valve1':            (-1000, 100, 4120, 0),
        'Valve2':            (0, 100, 4120, 0),
        'Valve3':            (1000, 100, 4120, 0),
        'ElevatorPanel':     (1900, 120, 1080, 180),
        'RoofHatch':         (450, 620, 600, 0),
        'AmmoCrate_Turbine': (-2950, 30, 2950, 0),
        'AmmoCrate_Archive': (-3000, 30, 1800, 0),
        'AmmoCrate_Atrium':  (-900, 30, -600, 0),
    },
    'spawns': {
        'W3':         [(1500, 3800), (2400, 3000), (500, 3900), (2000, 3700)],
        'Stragglers': [(-2800, -500), (-1600, 500)],
        'W4':         [(900, -500), (900, 500), (400, 900), (400, -900)],
    },
    'routes': {
        'L2_Breaker':   [(0, -2700), (0, -2100), (-600, -1800)],
        'L2_Workshop':  [(-400, -1800), (0, -1550), (0, -1000), (-600, -200), (-1000, 0), (-1450, 0), (-2300, 200),
                         (-2600, 330)],
        'L2_Reader':    [(-2300, 600), (-2050, 720)],
        'L2_Valve1':    [(-2200, 700), (-2200, 1200), (-2200, 2300), (-2200, 2950), (-1000, 3650), (-1000, 3950)],
        'L2_Valve2':    [(0, 3950)],
        'L2_Valve3':    [(1000, 3950)],
        'L2_Elevator':  [(1500, 3650), (1900, 3000), (1900, 2300), (1900, 1450)],
        'L2A_Fight3':   [(-2400, 3400)],
        'L2A_Archive':  [(-2200, 3000), (-2200, 2300), (-2200, 1500)],
        'L2A_Fight4':   [(-2200, 1500)],
        'L2A_Atrium':   [(-2200, 1300), (-2200, 600), (-2000, 0), (-1450, 0), (-700, -300)],
        'L2A_Fight5':   [(-600, -300)],
        'L2A_Gantry':   [(-1100, 600), (-960, 600), (250, 600), (450, 600)],
    },
}

# =============================================================================
# L3 The Core: the elevator (north-west), the ring gallery around the
# reactor drum with the vault inside, the maintenance tunnel south to the
# coolant pump hall (south-east) and the freight lift east of it.
# =============================================================================
L3 = {
    'ring': {'centre': (0, 0), 'apothem': 1800, 'height': 2800, 'doors': {'NW': 300, 'S': 300}},
    # the shaft (dress_l3.py): the elevator arrives high on the NW wall; a bridge runs in to the
    # helix catwalk round the reactor, which spirals clockwise down to the south bridge over the
    # pit (between the drum and the gallery floor) and into the vault
    'arrival_y': 800,
    'pit': {'apothem': 900, 'depth': 1500, 'bridge': 300},
    'spiral': {'r_in': 700, 'r_out': 850, 'start': 321, 'walk': 9, 'drop': 800 / 9},
    'drum': {'centre': (0, 0), 'apothem': 600, 'height': 700, 'doors': {'S': 240}},
    'elevator': {'distance': 2215, 'angle': 315, 'size': (800, 800), 'height': 450, 'doors': {'S': [(0, 300)]}},
    'rooms': {
        'Tunnel':     (-200, -4400, 200, -1800, 400, {'N': [(0, 300)], 'S': [(0, 300)]}),
        'PumpHall':   (-400, -6400, 3400, -4400, 700, {'N': [(0, 300)], 'E': [(-5400, 300)]}),
        'FreightLift': (3400, -5800, 4200, -5000, 500, {'W': [(-5400, 300)]}),
    },
    'pumps': [(800, -5900), (1700, -5900), (2600, -5900)],
    'vents': [25, 335, 65, 295],   # compass angles of the W1 vents in the shaft wall
    'points': {
        'Entry_Elevator':  (-1566, 800, 1566, 135),
        'Intel':           (0, 120, 0, 0),
        'Locker':          (-600, 120, -1500, 180),
        'AmmoCrate_Ring':  (600, 30, -1500, 0),
        'AmmoCrate_Pump':  (-200, 30, -6200, 0),
        'Beacon_Start':    (1200, 120, -4800, 0),
        'Beacon_Fire':     (1650, 120, -4800, 0),
        'FreightPanel':    (4050, 120, -5400, 90),
        'Trigger_PumpHall': (0, 0, -4700, 0),
    },
    'spawns': {
        'W1': [(655, 1405), (-655, 1405), (1405, 655), (-1405, 655)],   # in front of the vents (C5 blows them open)
        'W2': [(3100, -6050), (2900, -4800), (200, -6100), (1250, -6250)],
    },
    'routes': {
        'L3_Vault':   [(-1450, 1450), (-1150, 1150), (-560, 560), (-361, 686), (-71, 772), (230, 740), (496, 596), (686, 361), (772, 71), (740, -230), (596, -496), (361, -686), (150, -780), (0, -760), (0, -300)],   # the bridge, the helix down round the reactor, the south bridge
        'L3_Locker':  [(0, -750), (0, -1000), (-300, -1200), (-550, -1380)],
        'L3_Fight1':  [(0, -1200)],
        'L3_Tunnel':  [(0, -1300), (0, -2000), (0, -3500), (0, -4650), (900, -4650)],
        'L3_Fight2':  [(1400, -5000)],
        'L3_Freight': [(2000, -5000), (3000, -5400), (3550, -5400), (3850, -5400)],
    },
}
