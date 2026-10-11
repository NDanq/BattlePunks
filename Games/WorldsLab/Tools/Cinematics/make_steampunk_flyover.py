#!/usr/bin/env python3
"""The Sequencer's validation cinematic for SteampunkStreet.

Writes, idempotently:
  - three camera entities into the level (CineCam_Walk, CineCam_Orbit,
    CineCam_Street; inactive, so the player's camera stays the game's),
  - a "Cinematic" entity with a Sequence Player (Cinematic; started by the
    CinematicDirector graph, make_cinematic_director.py),
  - Content/Cinematics/SteampunkFlyover.nmseq:
      0-12 s  a walk down the front street at eye height, turning up the
              cross street toward the back street (head bob, a look up)
      12-22 s cut: an orbit round the airship, above the roofs
      22-32 s cut: low on the cross street, a dutch angle, pulling back and
              widening from a tight lens on the airship down the street
    with a fade in and out, an event at each cut, and markers.

Run from the repository root:  python Games/WorldsLab/Tools/Cinematics/make_steampunk_flyover.py
"""
import math
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
WORLD = GAME / "Content" / "Levels" / "Steampunk" / "SteampunkStreet.nmworld"
FOLDER = WORLD.with_suffix("")
SEQUENCE = GAME / "Content" / "Cinematics" / "SteampunkFlyover.nmseq"
RUNTIME = "Content/Cinematics/SteampunkFlyover.nmseq"

FPS = 30
LENGTH = 32 * FPS
AIRSHIP_GUID = "f61971ac8064258ec8747f05d97da7ff"
AIRSHIP = (-1200.0, 5200.0, 3400.0)

CAMERAS = {
    "CineCam_Walk":   "5e9c0a1d00000000000000000000c001",
    "CineCam_Orbit":  "5e9c0a1d00000000000000000000c002",
    "CineCam_Street": "5e9c0a1d00000000000000000000c003",
}
PLAYER_GUID = "5e9c0a1d00000000000000000000c0aa"


def camera_entity(name, guid, fov):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<Entity Guid="{guid}" Name="{name}">
  <Component Type="Camera" Version="2">
    <ZNear Value="5" />
    <ZFar Value="40000" />
    <Active Value="false" />
    <Projection Value="1" />
    <FovY Value="{fov}" />
  </Component>
  <Component Type="Transform" Version="1">
    <Position X="0" Y="300" Z="0" />
    <Rotation X="0" Y="0" Z="0" />
    <Scale X="1" Y="1" Z="1" />
  </Component>
</Entity>
"""


def player_entity():
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<Entity Guid="{PLAYER_GUID}" Name="Cinematic">
  <Component Type="Sequence Player" Version="1">
    <Sequence Value="{RUNTIME}" />
    <PlayRate Value="1" />
    <AutoPlay Value="false" />
    <Loop Value="false" />
    <RestoreState Value="false" />
    <DestroyWhenFinished Value="false" />
    <Cinematic Value="true" />
    <Skippable Value="true" />
  </Component>
  <Component Type="Transform" Version="1">
    <Position X="0" Y="0" Z="0" />
    <Rotation X="0" Y="0" Z="0" />
    <Scale X="1" Y="1" Z="1" />
  </Component>
</Entity>
"""


def write_level():
    manifest = WORLD.read_text()
    for name, guid in CAMERAS.items():
        (FOLDER / f"{guid}.nmentity").write_text(camera_entity(name, guid, 60))
    (FOLDER / f"{PLAYER_GUID}.nmentity").write_text(player_entity())
    for guid in list(CAMERAS.values()) + [PLAYER_GUID]:
        line = f'  <Entity Guid="{guid}" />'
        if line not in manifest:
            manifest = manifest.replace("</World>", line + "\n</World>")
    WORLD.write_text(manifest)


# --- The camera work ---------------------------------------------------------------

def look_at(cam, target):
    """Yaw (about Y, 0 along +Z, +90 along +X) and pitch (positive looks down)."""
    dx, dy, dz = target[0] - cam[0], target[1] - cam[1], target[2] - cam[2]
    yaw = math.degrees(math.atan2(dx, dz))
    pitch = -math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    return yaw, pitch


def unwrap(angles):
    out = [angles[0]]
    for a in angles[1:]:
        while a - out[-1] > 180: a -= 360
        while a - out[-1] < -180: a += 360
        out.append(a)
    return out


def smooth(t):
    return t * t * (3 - 2 * t)


class Track:
    def __init__(self):
        self.channels = {}

    def key(self, channel, frame, value, interp=None):
        self.channels.setdefault(channel, []).append((frame, value, interp))

    def xml(self, indent="    "):
        out = [f'{indent}<Track type="Transform">']
        for channel in ("Position.X", "Position.Y", "Position.Z", "Rotation.X", "Rotation.Y", "Rotation.Z"):
            keys = self.channels.get(channel)
            if not keys:
                continue
            out.append(f'{indent}  <Channel name="{channel}">')
            for frame, value, interp in sorted(keys):
                extra = f' interp="{interp}"' if interp else ""
                out.append(f'{indent}    <Key f="{frame}" v="{value:.3f}"{extra}/>')
            out.append(f'{indent}  </Channel>')
        out.append(f'{indent}</Track>')
        return "\n".join(out)


def walk_shot(start, end):
    """Down the front street from the west, turning north up the cross street."""
    t = Track()
    # (seconds, x, z, yaw, pitch)
    path = [
        (0.0, -3200, -1610, 90, 1),
        (3.0, -2150, -1610, 89, 0),
        (6.0, -1050, -1600, 86, -1),
        (7.6, -420, -1560, 66, -3),
        (8.8, -90, -1250, 30, -6),
        (10.2, 30, -500, 6, -12),
        (12.0, 70, 350, -12, -24),
    ]
    for sec, x, z, yaw, pitch in path:
        f = start + round(sec * FPS)
        t.key("Position.X", f, x)
        t.key("Position.Z", f, z)
        t.key("Rotation.Y", f, yaw)
        t.key("Rotation.X", f, pitch)
    # Footsteps: the head bobs twice a second, and sways a little.
    step = 8
    for k, f in enumerate(range(start, end + 1, step)):
        t.key("Position.Y", f, 168.0 + (4.0 if k % 2 else -1.0))
        t.key("Rotation.Z", f, 0.6 if k % 4 == 1 else (-0.6 if k % 4 == 3 else 0.0))
    return t


def orbit_shot(start, end):
    """Round the airship, above the roofs, looking at it."""
    t = Track()
    radius, lift = 3600.0, 1100.0
    a0, a1 = math.radians(150), math.radians(-70)
    frames = list(range(start, end + 1, 15))
    yaws, pitches = [], []
    for f in frames:
        u = (f - start) / (end - start)
        a = a0 + (a1 - a0) * u
        cam = (AIRSHIP[0] + radius * math.sin(a), AIRSHIP[1] + lift - 300 * u, AIRSHIP[2] + radius * math.cos(a))
        yaw, pitch = look_at(cam, AIRSHIP)
        yaws.append(yaw)
        pitches.append(pitch)
        t.key("Position.X", f, cam[0])
        t.key("Position.Y", f, cam[1])
        t.key("Position.Z", f, cam[2])
    for f, yaw, pitch in zip(frames, unwrap(yaws), pitches):
        t.key("Rotation.Y", f, yaw)
        t.key("Rotation.X", f, pitch)
    return t


def street_shot(start, end):
    """Low on the cross street, rolled, pulling back and up, held on the airship."""
    t = Track()
    a, b = (90.0, 120.0, -400.0), (220.0, 900.0, -3500.0)
    frames = list(range(start, end + 1, 15))
    yaws = []
    for f in frames:
        u = smooth((f - start) / (end - start))
        cam = tuple(a[i] + (b[i] - a[i]) * u for i in range(3))
        yaw, pitch = look_at(cam, AIRSHIP)
        yaws.append(yaw)
        t.key("Position.X", f, cam[0])
        t.key("Position.Y", f, cam[1])
        t.key("Position.Z", f, cam[2])
        t.key("Rotation.X", f, pitch)
        t.key("Rotation.Z", f, 9.0 - 3.0 * u)   # the dutch angle, easing as it widens
    for f, yaw in zip(frames, unwrap(yaws)):
        t.key("Rotation.Y", f, yaw)
    return t


def write_sequence():
    walk, orbit, street = (0, 359), (360, 659), (660, LENGTH)
    lines = [f'<?xml version="1.0"?>', f'<Sequence version="1" fps="{FPS}" length="{LENGTH}">']
    shots = [("walk", "CineCam_Walk", walk_shot(*walk), 62),
             ("orbit", "CineCam_Orbit", orbit_shot(*orbit), 55),
             ("street", "CineCam_Street", street_shot(*street), None)]
    for bid, name, track, fov in shots:
        lines.append(f'  <Binding id="{bid}" kind="possess" guid="{CAMERAS[name]}" name="{name}">')
        lines.append(track.xml())
        lines.append('    <Track type="Float" component="Camera" field="FovY">')
        if fov is not None:
            lines.append(f'      <Key f="0" v="{fov}"/>')
        else:
            # The zoom out: a long lens on the airship widening to the street.
            lines.append(f'      <Key f="{street[0]}" v="22" interp="CubicInOut"/>')
            lines.append(f'      <Key f="{street[1]}" v="72"/>')
        lines.append('    </Track>')
        lines.append('  </Binding>')
    # The airship's engines, heard in 3D from where it is once it is in shot.
    lines.append(f'  <Binding id="airship" kind="possess" guid="{AIRSHIP_GUID}" name="Airship">')
    lines.append('    <Track type="Audio" name="Engines">')
    lines.append(f'      <Section start="{orbit[0] - 30}" end="{LENGTH}" sound="Content/Sounds/EngineLoop.nmsound" bus="Ambient" spatial="3D" blendIn="45" blendOut="30"/>')
    lines.append('    </Track>')
    lines.append('  </Binding>')
    lines.append('  <Track type="CameraCut" name="Shots">')
    lines.append(f'    <Section start="{walk[0]}" end="{walk[1] + 1}" camera="walk"/>')
    lines.append(f'    <Section start="{orbit[0]}" end="{orbit[1] + 1}" camera="orbit"/>')
    lines.append(f'    <Section start="{street[0]}" end="{street[1]}" camera="street"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="Fade">')
    lines.append('    <Key f="0" v="1" interp="QuadOut"/>')
    lines.append('    <Key f="24" v="0" interp="Constant"/>')
    lines.append(f'    <Key f="{LENGTH - 30}" v="0" interp="QuadIn"/>')
    lines.append(f'    <Key f="{LENGTH}" v="1"/>')
    lines.append('  </Track>')
    # Post process: the orbit graded with the Courtyard's stack, faded in and out;
    # the street shot focused on the airship (depth of field), the street near
    # the lens soft. Max blur 0 before the cut keeps the other shots sharp.
    lines.append('  <Track type="PostProcess" name="Orbit grade">')
    lines.append(f'    <Section start="{orbit[0]}" end="{orbit[1] + 1}" stack="Content/PostProcess/Courtyard/Courtyard.pp.nmgraph" blendIn="60" blendOut="60"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="PostProcessSetting" setting="DofMaxBlur">')
    lines.append(f'    <Key f="0" v="0" interp="Constant"/>')
    lines.append(f'    <Key f="{street[0]}" v="14"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="PostProcessSetting" setting="DofFocusRange">')
    lines.append('    <Key f="0" v="900"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="PostProcessSetting" setting="DofFocusDistance">')
    a, b = (90.0, 120.0, -400.0), (220.0, 900.0, -3500.0)
    for f in range(street[0], street[1] + 1, 30):
        u = smooth((f - street[0]) / (street[1] - street[0]))
        cam = tuple(a[i] + (b[i] - a[i]) * u for i in range(3))
        lines.append(f'    <Key f="{f}" v="{math.dist(cam, AIRSHIP):.1f}"/>')
    lines.append('  </Track>')
    # Footsteps along the walk (2D, a master track), the guide's lines, and the
    # ambience ducked as the street shot pulls back.
    lines.append('  <Track type="Audio" name="Footsteps">')
    for f in range(walk[0] + 30, walk[1] - 20, 21):
        lines.append(f'    <Section start="{f}" end="{f + 12}" sound="Content/Sounds/Footstep.nmsound" volume="0.6"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="Subtitle" name="Guide">')
    lines.append(f'    <Section start="{walk[0] + 40}" end="{walk[0] + 170}" speaker="Guide" text="The old quarter. Mind the steam vents."/>')
    lines.append(f'    <Section start="{orbit[0] + 30}" end="{orbit[0] + 180}" speaker="Guide" text="And there she is: the Brass Leviathan."/>')
    lines.append(f'    <Section start="{street[0] + 20}" end="{street[0] + 150}" text="(engines thrum overhead)"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="BusVolume" bus="Ambient">')
    lines.append(f'    <Key f="{street[0]}" v="1" interp="CubicInOut"/>')
    lines.append(f'    <Key f="{LENGTH}" v="0.35"/>')
    lines.append('  </Track>')
    lines.append('  <Track type="Event" name="Shot log">')
    for (bid, *_), (s, _) in zip(shots, (walk, orbit, street)):
        lines.append(f'    <Key f="{s}" event="Flyover.Shot" arg0="{bid}" arg1="@{bid}"/>')
    lines.append(f'    <Key f="{LENGTH}" event="Flyover.Done" arg0="@airship" fireOnSkip="true"/>')
    lines.append('  </Track>')
    lines.append(f'  <Marker f="{walk[0]}" name="Walk"/>')
    lines.append(f'  <Marker f="{orbit[0]}" name="Orbit"/>')
    lines.append(f'  <Marker f="{street[0]}" name="Street"/>')
    lines.append('</Sequence>')
    SEQUENCE.parent.mkdir(parents=True, exist_ok=True)
    SEQUENCE.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    write_level()
    write_sequence()
    print(f"wrote {SEQUENCE.relative_to(GAME)} and {len(CAMERAS) + 1} entities into {WORLD.relative_to(GAME)}")
