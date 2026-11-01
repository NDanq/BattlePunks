"""CrashDummy's walk, armed walk, recoil and hit reactions, generated on its Rigify FK controls.

    blender -b ~/Documents/Blender/CrashDummy/CrashDummy.blend \
        --python Engine/Content/Characters/CrashDummy/Source/make_crashdummy_clips.py -- \
        [--save] [--export Engine/Content/Characters/CrashDummy/CrashDummy.glb]

Writes FB_Walk, UB_Walk, UB_Walk_Armed, UB_Fire and the four UB_Hit_* into the open .blend (--save
keeps them, --export writes the .glb the engine imports, with the settings the
original export used), and closes every looping clip's range on the frame that
repeats its first, so a loop no longer skips a frame as it wraps.

The walk is solved, not keyed by hand: each foot is planted on the ground for
58% of the cycle -- heel strike, flat, rolling off the ball -- and swings
through the rest; the legs reach it with two-bone IK onto the FK chain, so the
FK clip the engine plays keeps the feet where the solve put them. Over it the
pelvis bobs (lowest at the double support, highest over the planted foot),
sways toward the planted foot, rolls and twists; the chest counter-twists, the
arms swing a little behind the legs and the hands behind the arms, and the
head stays level.

UB_Fire is an additive clip (the anim state's Recoil layer plays it relative to
its first frame, the armed idle): the gun hand flips up and back, the chest
gives a little and settles in about a third of a second.

UB_Hit_Front, UB_Hit_Back, UB_Hit_Left and UB_Hit_Right are additive too (the
HitReact layer, masked at DEF-spine.001 so the legs keep walking): a hit from
the front throws the chest back, from behind folds it forward, from a side
twists that shoulder back and leans away; the head and arms whip a frame or
two behind the chest, and it all settles, with a small overshoot, in about
half a second. They are keyed at full (heavy) strength; the layer's HitWeight
scales them down for light hits.
"""
import math
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

FPS = 30
WALK_FRAMES = 30            # one cycle: two steps, a second
WALK_SPEED = 1.5            # m/s a planted foot moves back; the anim state's walk is at 150
DUTY = 0.58                 # of the cycle a foot is planted
FOOT_FLAT_Y = -0.33         # a planted foot's ankle, at heel strike, before the body moves on (-Y is forward)
HIP_LOW, HIP_HIGH = 0.98, 1.035  # the hip joints' height at the double support and over the planted foot
REST_HIP = 1.072
STRIKE_TOES_UP = math.radians(14.0)
TOE_OFF_HEEL_UP = math.radians(42.0)
SWING_LIFT = 0.10           # how far the ankle rises above its path, mid-swing
FIRE_FRAMES = 10
HIT_FRAMES = 18

rig = bpy.data.objects["CrashDummy_Rig"]
PB = rig.pose.bones


def update():
    bpy.context.view_layer.update()


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


def hermite(p0, v0, p1, v1, s, span):
    s2, s3 = s * s, s * s * s
    return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * span * v0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * span * v1


def rot_x(angle):
    return Matrix.Rotation(angle, 3, "X")


# ---------------------------------------------------------------------------------------------
# Poses
# ---------------------------------------------------------------------------------------------

def read_pose(action_name, frame):
    """Every control's basis at a frame of an action."""
    rig.animation_data.action = bpy.data.actions[action_name]
    bpy.context.scene.frame_set(frame)
    pose = {pb.name: (pb.location.copy(), pb.rotation_quaternion.copy(), pb.scale.copy()) for pb in PB}
    rig.animation_data.action = None
    return pose


def apply_pose(pose):
    for name, (loc, rot, scale) in pose.items():
        pb = PB[name]
        pb.location, pb.rotation_quaternion, pb.scale = loc, rot, scale


def turn(name, axis, degrees):
    """Turns a control about an armature-space axis through its head."""
    pb = PB[name]
    head = pb.matrix.translation.copy()
    pb.matrix = Matrix.Translation(head) @ Matrix.Rotation(math.radians(degrees), 4, axis) @ Matrix.Translation(-head) @ pb.matrix
    update()


def turn_local(name, axis, degrees):
    pb = PB[name]
    pb.rotation_quaternion = pb.rotation_quaternion @ Quaternion(Vector(axis), math.radians(degrees))


def set_world_rotation(name, rotation):
    """Keeps the control where it is, turned to an armature-space rotation."""
    pb = PB[name]
    m = rotation.to_4x4()
    m.translation = pb.matrix.translation
    pb.matrix = m
    update()


REST = {pb.name: pb.bone.matrix_local.to_3x3() for pb in PB}


# ---------------------------------------------------------------------------------------------
# The feet
# ---------------------------------------------------------------------------------------------

ANKLE_REST = {s: PB["foot_fk." + s].bone.head_local.copy() for s in "LR"}
BALL_REST = {s: PB["toe_fk." + s].bone.head_local.copy() for s in "LR"}
HEEL_FROM_ANKLE = Vector((0.0, 0.045, -ANKLE_REST["L"].z))   # the heel's ground contact, behind the ankle


def pivot(point, centre, angle):
    """About the X axis through centre; a positive angle lifts what is behind (+Y) it."""
    return centre + rot_x(angle) @ (point - centre)


def planted(side, u):
    """The foot in stance, u from 0 (heel strike) to DUTY (toe off): ankle, foot pitch, toe pitch (world)."""
    ankle0 = ANKLE_REST[side].copy()
    ankle0.y = FOOT_FLAT_Y
    ankle0 += Vector((0.0, WALK_SPEED * u, 0.0))          # the ground carries it back
    heel = ankle0 + HEEL_FROM_ANKLE
    ball = ankle0 + (BALL_REST[side] - ANKLE_REST[side])
    heel_end, roll_start = 0.08, 0.36
    if u < heel_end:                                       # landing on the heel, toes coming down
        angle = -STRIKE_TOES_UP * (1.0 - smooth(u / heel_end))
        return pivot(ankle0, heel, angle), angle, angle
    if u < roll_start:                                     # flat
        return ankle0, 0.0, 0.0
    angle = TOE_OFF_HEEL_UP * smooth((u - roll_start) / (DUTY - roll_start)) ** 1.3   # rolling off the ball
    return pivot(ankle0, ball, angle), angle, 0.0


def foot(side, u):
    """Ankle position (armature space), foot pitch and toe pitch at a phase of the side's own cycle."""
    u %= 1.0
    if u < DUTY:
        return planted(side, u)
    span = (1.0 - DUTY) * WALK_FRAMES / FPS
    s = (u - DUTY) / (1.0 - DUTY)
    a0, p0, _ = planted(side, DUTY - 1e-6)
    a1, p1, _ = planted(side, 0.0)
    y = hermite(a0.y, WALK_SPEED, a1.y, 0.25 * WALK_SPEED, s, span)
    lift = SWING_LIFT * math.sin(math.pi * s ** 0.8)
    z = a0.z + (a1.z - a0.z) * smooth(s) + lift
    pitch = p0 + (p1 - p0) * smooth(min(1.0, s * 1.35))
    toe = pitch - p0 * (1.0 - smooth(s / 0.45))           # the toe stays bent off the ball, then relaxes
    return Vector((a0.x, y, z)), pitch, toe


def solve_leg(side, ankle, pitch, toe_pitch):
    thigh, shin = PB["thigh_fk." + side], PB["shin_fk." + side]
    hip = thigh.matrix.translation.copy()
    l1, l2 = thigh.length, shin.length
    to = ankle - hip
    d = min(to.length, (l1 + l2) * 0.9995)
    direction = to.normalized()
    pole = Vector((0.0, -1.0, 0.0))                        # knees forward
    pole = (pole - direction * pole.dot(direction)).normalized()
    a = (l1 * l1 - l2 * l2 + d * d) / (2.0 * d)
    knee = hip + direction * a + pole * math.sqrt(max(l1 * l1 - a * a, 0.0))
    end = hip + direction * d

    def frame(y_axis, rest):
        x_axis = y_axis.cross(pole).normalized()
        if x_axis.dot(rest.col[0]) < 0.0:
            x_axis = -x_axis
        z_axis = x_axis.cross(y_axis)
        return Matrix((x_axis, y_axis, z_axis)).transposed()

    set_world_rotation(thigh.name, frame((knee - hip).normalized(), REST[thigh.name]))
    set_world_rotation(shin.name, frame((end - knee).normalized(), REST[shin.name]))
    set_world_rotation("foot_fk." + side, rot_x(pitch) @ REST["foot_fk." + side])
    set_world_rotation("toe_fk." + side, rot_x(toe_pitch) @ REST["toe_fk." + side])


# ---------------------------------------------------------------------------------------------
# The body over the feet
# ---------------------------------------------------------------------------------------------

def body(u):
    """Pelvis, spine, head and the legs, for phase u of the cycle (0: the left heel strikes)."""
    c, s = math.cos(2 * math.pi * u), math.sin(2 * math.pi * u)
    bob = 0.5 - 0.5 * math.cos(4 * math.pi * (u - 0.03))     # 0 at the double support, 1 over the planted foot
    torso = PB["torso"]
    torso.location = Vector((0.010 * s, 0.0, (HIP_LOW - REST_HIP) + (HIP_HIGH - HIP_LOW) * bob))
    update()
    turn("hips", "Z", -6.0 * c)          # the hip of the leg in front, forward
    turn("hips", "Y", -4.0 * s)          # the swinging leg's side drops
    turn("hips", "X", 2.0)
    turn("chest", "X", 3.0 + 1.0 * bob)  # a slight lean, more as it rises onto the leg
    return c, s, bob


def stabilise_head(u, bob):
    lag = math.cos(4 * math.pi * (u - 0.10))
    set_world_rotation("neck", Matrix.Rotation(math.radians(3.0 + 0.6 * lag), 3, "X") @ REST["neck"])
    set_world_rotation("head", Matrix.Rotation(math.radians(5.0 + 1.2 * lag), 3, "X") @ REST["head"])


def legs(u):
    for side, offset in (("L", 0.0), ("R", 0.5)):
        solve_leg(side, *foot(side, u + offset))


def swing_arms(u):
    """Opposite to the legs, two frames behind them; elbows bend as an arm comes forward, hands trail."""
    for side, sign in (("L", 1.0), ("R", -1.0)):
        arm = sign * math.cos(2 * math.pi * (u - 0.07))      # 1: back, -1: forward
        hand = sign * math.cos(2 * math.pi * (u - 0.16))
        turn("upper_arm_fk." + side, "X", 8.0 + 16.0 * arm)
        turn_local("forearm_fk." + side, (1, 0, 0), 9.0 + 12.0 * max(0.0, -arm) + 3.0 * max(0.0, arm))
        turn_local("hand_fk." + side, (1, 0, 0), 7.0 * hand)
        update()


def counter_twist(c, s, u, amount):
    turn("chest", "Z", amount * 5.0 * math.cos(2 * math.pi * (u - 0.04)))
    turn("chest", "Y", amount * 2.5 * s)


# ---------------------------------------------------------------------------------------------
# Writing actions
# ---------------------------------------------------------------------------------------------

def new_action(name, first, last, cyclic):
    old = bpy.data.actions.get(name)
    if old:
        old.name = name + "_replaced"
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    action.use_frame_range = True
    action.frame_start, action.frame_end = first, last
    action.use_cyclic = cyclic
    rig.animation_data.action = action
    return action, old


def key_all(frame, located):
    for pb in PB:
        if pb.name.startswith(("MCH-", "ORG-", "DEF-", "VIS_")):
            continue
        pb.keyframe_insert("rotation_quaternion", frame=frame, group=pb.name)
        if pb.name in located:
            pb.keyframe_insert("location", frame=frame, group=pb.name)


def finish(action, old):
    rig.animation_data.action = None
    for fcurve in fcurves(action):
        for k in fcurve.keyframe_points:
            k.interpolation = "LINEAR"
    if old:
        bpy.data.actions.remove(old)


def fcurves(action):
    found = list(getattr(action, "fcurves", []) or [])
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for bag in strip.channelbags:
                found += list(bag.fcurves)
    return found


def make_walk(name, upper_base, armed):
    idle = read_pose("FB_Idle", 1)
    upper = read_pose(upper_base, 1)
    action, old = new_action(name, 1, WALK_FRAMES + 1, True)
    rig.animation_data.action = None
    for frame in range(1, WALK_FRAMES + 2):
        u = ((frame - 1) % WALK_FRAMES) / WALK_FRAMES
        apply_pose(idle)
        apply_pose({k: v for k, v in upper.items() if not k.startswith(("thigh", "shin", "foot", "toe", "torso", "hips"))})
        update()
        c, s, bob = body(u)
        if armed:
            counter_twist(c, s, u, 0.35)
            sway = math.sin(4 * math.pi * (u - 0.12))   # the gun settles a little after each step
            turn_local("upper_arm_fk.R", (1, 0, 0), 1.2 * sway)
            turn_local("forearm_fk.R", (1, 0, 0), 1.6 * sway)
            turn_local("hand_fk.R", (1, 0, 0), -1.0 * math.sin(4 * math.pi * (u - 0.2)))
            update()
        else:
            counter_twist(c, s, u, 1.0)
            swing_arms(u)
        stabilise_head(u, bob)
        legs(u)
        rig.animation_data.action = action
        key_all(frame, {"torso"})
        rig.animation_data.action = None
    finish(action, old)


def make_fire():
    """Additive: the armed idle's first frame, then the kick."""
    base = read_pose("UB_Idle_Armed", 1)
    kick = [0.0, 0.8, 1.0, 0.72, 0.42, 0.18, 0.02, -0.06, -0.03, 0.0]
    action, old = new_action("UB_Fire", 1, FIRE_FRAMES, False)
    rig.animation_data.action = None
    for frame in range(1, FIRE_FRAMES + 1):
        k = kick[frame - 1]
        late = kick[max(0, frame - 2)]                  # the body answers a frame after the hand
        apply_pose(base)
        update()
        turn("chest", "X", -2.0 * late)                 # pushed back
        turn("chest", "Z", 2.0 * late)                  # the gun shoulder back
        turn_local("upper_arm_fk.R", (1, 0, 0), -4.0 * k)
        turn_local("forearm_fk.R", (1, 0, 0), 9.0 * k)
        turn_local("hand_fk.R", (1, 0, 0), 12.0 * k)
        turn_local("shoulder.R", (1, 0, 0), 2.0 * late)
        update()
        set_world_rotation("head", Matrix.Rotation(math.radians(-1.5 * late), 3, "X") @ PB["head"].matrix.to_3x3())
        rig.animation_data.action = action
        key_all(frame, {"torso"})
        rig.animation_data.action = None
    finish(action, old)


# Per direction: chest pitch (+ folds forward), chest twist (+ turns the left shoulder back), chest lean
# (+ tips the top toward the left), and how far the arms are flung (+ forward).
HITS = {
    "Front": dict(pitch=-15.0, twist=3.0, lean=0.0, fling=-1.0),
    "Back": dict(pitch=13.0, twist=-2.0, lean=0.0, fling=1.0),
    "Left": dict(pitch=-4.0, twist=16.0, lean=-7.0, fling=0.0),
    "Right": dict(pitch=-4.0, twist=-16.0, lean=7.0, fling=0.0),
}
HIT_ENVELOPE = [0.0, 0.62, 1.0, 0.94, 0.78, 0.6, 0.44, 0.31, 0.2, 0.12, 0.05, 0.0, -0.05, -0.07, -0.05, -0.03, -0.01, 0.0]


def make_hit(direction):
    """Additive: the armed idle's first frame, the jolt, the settle."""
    h = HITS[direction]
    base = read_pose("UB_Idle_Armed", 1)
    action, old = new_action("UB_Hit_" + direction, 1, HIT_FRAMES, False)
    rig.animation_data.action = None
    for frame in range(1, HIT_FRAMES + 1):
        e = HIT_ENVELOPE[frame - 1]
        late = HIT_ENVELOPE[max(0, frame - 2)]          # the head and arms answer a frame behind the chest
        later = HIT_ENVELOPE[max(0, frame - 3)]
        apply_pose(base)
        update()
        turn("chest", "X", h["pitch"] * e)
        turn("chest", "Z", h["twist"] * e)
        turn("chest", "Y", h["lean"] * e)
        # The head lags the chest, then catches up past it.
        turn_local("neck", (1, 0, 0), -0.35 * h["pitch"] * e + 0.25 * h["pitch"] * later)
        turn_local("head", (0, 0, 1), -0.3 * h["twist"] * late)
        turn_local("shoulder.L", (1, 0, 0), 4.0 * late)
        turn_local("shoulder.R", (1, 0, 0), 4.0 * late)
        # The off arm flies loose; the gun arm only dips and comes back.
        turn_local("upper_arm_fk.L", (1, 0, 0), 14.0 * h["fling"] * later + 6.0 * late)
        turn_local("forearm_fk.L", (1, 0, 0), 14.0 * later)
        turn_local("hand_fk.L", (1, 0, 0), -10.0 * later)
        turn_local("upper_arm_fk.R", (1, 0, 0), 4.0 * h["fling"] * later)
        turn_local("forearm_fk.R", (1, 0, 0), -7.0 * late)
        turn_local("hand_fk.R", (1, 0, 0), -6.0 * later)
        update()
        rig.animation_data.action = action
        key_all(frame, {"torso"})
        rig.animation_data.action = None
    finish(action, old)


def close_loops():
    """A looping clip's last key repeats its first: the range ends on it, so the loop does not skip a frame."""
    for name in ("FB_Idle", "FB_Run", "FB_Fall", "UB_Idle", "UB_Idle_Armed", "UB_Run", "UB_Run_Armed"):
        action = bpy.data.actions[name]
        last = max(k.co[0] for fc in fcurves(action) for k in fc.keyframe_points)
        action.use_frame_range = True
        action.frame_end = last


def export(path):
    for obj in bpy.data.objects:
        obj.select_set(obj.name in ("CrashDummy_Body", "CrashDummy_Rig"))
    rig.animation_data.action = None
    for pb in PB:
        pb.location, pb.rotation_quaternion, pb.scale = (0, 0, 0), (1, 0, 0, 0), (1, 1, 1)
    bpy.ops.export_scene.gltf(filepath=path, use_selection=True, export_def_bones=True, export_animation_mode="ACTIONS",
                              export_force_sampling=True, export_optimize_animation_size=False, export_apply=False,
                              export_yup=True)


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    rest_action = rig.animation_data.action
    make_walk("FB_Walk", "FB_Idle", armed=False)
    make_walk("UB_Walk", "FB_Idle", armed=False)
    make_walk("UB_Walk_Armed", "UB_Idle_Armed", armed=True)
    make_fire()
    for direction in HITS:
        make_hit(direction)
    close_loops()
    rig.animation_data.action = rest_action
    if "--save" in args:
        bpy.ops.wm.save_mainfile()
    if "--export" in args:
        export(args[args.index("--export") + 1])
        rig.animation_data.action = rest_action


main()
