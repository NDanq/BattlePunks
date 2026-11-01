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
# MainMenu: the lab's facade at dusk, set into the cliff above the plaza.
# =============================================================================
MENU = {
    'ground': (-6000, -4000, 6000, 8000),
    'facade': [   # (name, centre, size)
        ('Facade_Cliff',   (0, 1500, 3200), (9000, 3000, 600)),
        ('Facade_Front',   (0, 700, 2850), (3200, 1400, 100)),
        ('Facade_Door',    (0, 300, 2790), (600, 600, 40)),
        ('Facade_TowerW',  (-1900, 1100, 2700), (500, 2200, 500)),
        ('Facade_TowerE',  (1900, 1100, 2700), (500, 2200, 500)),
        ('Facade_Dome',    (0, 1700, 2600), (1400, 500, 1000)),
        ('Facade_Stack1',  (-1200, 2000, 3000), (160, 1600, 160)),
        ('Facade_Stack2',  (1200, 2200, 3000), (160, 2000, 160)),
        ('Plaza',          (0, -10, 1200), (5000, 20, 3200)),
    ],
    # the camera's slow dolly: (frame, position, target)
    'dolly': [(0, (-1800, 260, -1400), (0, 700, 2800)),
              (600, (0, 200, -900), (0, 800, 2800)),
              (1200, (1800, 260, -1400), (0, 700, 2800))],
}

# =============================================================================
# L1 Foothills: the dock (south-west), the beach, the switchback road climbing
# north-east, the gatehouse on the landing, the overlook, the plaza and the
# blast door in the cliff.
# =============================================================================
L1 = {
    'ground': (-1200, -2500, 6500, 8000),
    'sea': (-9000, -6000, -1200, 4000),
    'pier': (-2800, -1700, -1200, -1300),
    # the road's first leg: terraces 30 cm high, north from the beach
    'leg1': {'x0': 1200, 'x1': 1800, 'z0': -600, 'z1': 2400, 'steps': 10, 'y0': 0, 'y1': 300},
    'landing': (200, 2400, 4000, 3000, 300),            # x0, z0, x1, z1, top
    'leg2': {'x0': 3400, 'x1': 4000, 'z0': 3000, 'z1': 5400, 'steps': 8, 'y0': 300, 'y1': 600},
    'plaza': (2400, 5400, 5000, 7000, 600),
    'cliff': (1000, 7000, 6500, 7600, 2600),             # x0, z0, x1, z1, height
    'gatehouse': (2950, 3000, 3350, 3350, 700),
    'forest': (-1000, 3500, 800, 6500),                  # spruce stand-ins
    'mast': (5800, 3500),
    'points': {
        'Entry_Dock':      (-2600, 0, -1500, 90),
        'Crank_Gatehouse': (3200, 400, 2930, 0),
        'Gate':            (3700, 300, 3150, 0),
        'BlastDoor':       (3700, 600, 6985, 0),
        'Trigger_AtGate':  (3000, 300, 2700, 0),
        'Trigger_Overlook': (600, 300, 2700, 0),
        'Trigger_Plaza':   (3700, 600, 5700, 0),
        'Trigger_BlastDoor': (3700, 600, 6500, 0),
    },
    'routes': {
        'L1_Road1': [(-1800, -1500), (-500, -1250), (1000, -900), (1500, -800), (1500, 800), (1500, 2200),
                     (1500, 2700), (2600, 2720), (3050, 2780)],
        'L1_Road2': [(3700, 2800), (3700, 3400), (3700, 5000), (3700, 5700), (3700, 6450)],
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
    'ring': {'centre': (0, 0), 'apothem': 1800, 'height': 700, 'doors': {'NW': 300, 'S': 300}},
    'drum': {'centre': (0, 0), 'apothem': 600, 'height': 700, 'doors': {'S': 240}},
    'elevator': {'distance': 2215, 'angle': 315, 'size': (800, 800), 'height': 450, 'doors': {'S': [(0, 300)]}},
    'rooms': {
        'Tunnel':     (-200, -4400, 200, -1800, 400, {'N': [(0, 300)], 'S': [(0, 300)]}),
        'PumpHall':   (-400, -6400, 3400, -4400, 700, {'N': [(0, 300)], 'E': [(-5400, 300)]}),
        'FreightLift': (3400, -5800, 4200, -5000, 500, {'W': [(-5400, 300)]}),
    },
    'pumps': [(800, -5900), (1700, -5900), (2600, -5900)],
    'points': {
        'Entry_Elevator':  (-1566, 0, 1566, 135),
        'Intel':           (0, 110, 0, 0),
        'Locker':          (-600, 120, -1500, 180),
        'AmmoCrate_Ring':  (600, 30, -1500, 0),
        'AmmoCrate_Pump':  (-200, 30, -6200, 0),
        'Beacon_Start':    (1200, 120, -4800, 0),
        'Beacon_Fire':     (1650, 120, -4800, 0),
        'FreightPanel':    (4050, 120, -5400, 90),
        'Trigger_PumpHall': (0, 0, -4700, 0),
    },
    'spawns': {
        'W1': [(1300, 500), (-1300, 600), (900, 1250), (-900, 1300)],
        'W2': [(3000, -6000), (2900, -4800), (200, -6100), (1500, -6200)],
    },
    'routes': {
        'L3_Vault':   [(-1450, 1450), (-1150, 1150), (-1300, -100), (-850, -850), (0, -1150), (0, -750), (0, -300)],
        'L3_Locker':  [(0, -750), (-300, -1200), (-550, -1380)],
        'L3_Fight1':  [(0, -1200)],
        'L3_Tunnel':  [(0, -1300), (0, -2000), (0, -3500), (0, -4650), (900, -4650)],
        'L3_Fight2':  [(1400, -5000)],
        'L3_Freight': [(2000, -5000), (3000, -5400), (3550, -5400), (3850, -5400)],
    },
}
