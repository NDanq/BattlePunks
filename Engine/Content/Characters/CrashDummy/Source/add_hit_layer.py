"""Adds the Hit trigger, HitDir and HitWeight and the additive HitReact layer to CrashDummy.nmanimstate (once).

    python3 Engine/Content/Characters/CrashDummy/Source/add_hit_layer.py

HitReact plays one of the UB_Hit_* clips over the body from DEF-spine.001 up,
relative to its first frame (the armed idle), so the jolt lands on whatever
the body is doing and the legs keep walking. HitDir picks the clip -- 0 front,
1 back, 2 left, 3 right: the side the hit came from -- and HitWeight scales
the layer (the clips are keyed heavy; a light hit sets about 0.6). Each
direction has two states that take turns, so a hit during a reaction starts
it again from the other with a short crossfade.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import nmanimstate  # noqa: E402

FLOAT, TRIGGER = 0, 2
TRIGGERED, EQUAL = 6, 2
PATH = os.path.join(os.path.dirname(__file__), "..", "CrashDummy.nmanimstate")
CLIP = "Engine/Content/Characters/CrashDummy/CrashDummy_UB_Hit_{}.nmanim"
DIRECTIONS = ("Front", "Back", "Left", "Right")

state = nmanimstate.load(PATH)
if any(layer["Name"] == "HitReact" for layer in state["Layers"]):
    sys.exit("CrashDummy.nmanimstate already has a HitReact layer")
state["Parameters"] += [
    {"Name": "Hit", "Type": TRIGGER, "Default": 0.0},
    {"Name": "HitDir", "Type": FLOAT, "Default": 0.0},
    {"Name": "HitWeight", "Type": FLOAT, "Default": 1.0},
]

states = [{"Name": "Rest", "Samples": []}]
transitions = []
for i, direction in enumerate(DIRECTIONS):
    jolt = {"Samples": [{"Clip": CLIP.format(direction), "Position": 0.0}], "Loop": 1}
    first = len(states)
    states += [dict(jolt, Name=direction), dict(jolt, Name=direction + "2")]
    hit = [{"Parameter": "Hit", "Compare": TRIGGERED, "Value": 0.0}, {"Parameter": "HitDir", "Compare": EQUAL, "Value": float(i)}]
    # From any state but itself: in the first, the hit goes to the second, and back.
    transitions += [
        {"From": -1, "To": first, "Duration": 0.05, "Conditions": hit},
        {"From": -1, "To": first + 1, "Duration": 0.05, "Conditions": hit},
    ]
for s in range(1, len(states)):
    transitions.append({"From": s, "To": 0, "Duration": 0.1, "AtEnd": True})

state["Layers"].append({
    "Name": "HitReact", "Blend": 1, "AdditiveBase": 0, "Weight": 1.0, "WeightParameter": "HitWeight",
    "MaskBones": ["DEF-spine.001"], "EntryState": 0, "States": states, "Transitions": transitions,
})
nmanimstate.save(PATH, state)
print("added HitReact")
