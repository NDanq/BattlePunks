"""Adds the Fire trigger and the additive Recoil layer to CrashDummy.nmanimstate (once).

    python3 Engine/Content/Characters/CrashDummy/Source/add_recoil_layer.py

Recoil plays UB_Fire over the upper body, relative to its first frame (the armed
idle), so the kick lands on whatever the arms are doing. Fire and Fire2 take
turns: a shot during the kick starts it again from the other.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import nmanimstate  # noqa: E402

TRIGGER, ATEND = 2, True
PATH = os.path.join(os.path.dirname(__file__), "..", "CrashDummy.nmanimstate")
CLIP = "Engine/Content/Characters/CrashDummy/CrashDummy_UB_Fire.nmanim"

state = nmanimstate.load(PATH)
if any(layer["Name"] == "Recoil" for layer in state["Layers"]):
    sys.exit("CrashDummy.nmanimstate already has a Recoil layer")
state["Parameters"].append({"Name": "Fire", "Type": TRIGGER, "Default": 0.0})

fired = [{"Parameter": "Fire", "Compare": 6, "Value": 0.0}]
kick = {"Samples": [{"Clip": CLIP, "Position": 0.0}], "Loop": 1}
state["Layers"].append({
    "Name": "Recoil", "Blend": 1, "AdditiveBase": 0, "Weight": 1.0,
    "MaskBones": ["DEF-spine.002"], "EntryState": 0,
    "States": [{"Name": "Rest", "Samples": []}, dict(kick, Name="Fire"), dict(kick, Name="Fire2")],
    "Transitions": [
        {"From": 0, "To": 1, "Duration": 0.0, "Conditions": fired},
        {"From": 1, "To": 2, "Duration": 0.03, "Conditions": fired},
        {"From": 2, "To": 1, "Duration": 0.03, "Conditions": fired},
        {"From": 1, "To": 0, "Duration": 0.05, "AtEnd": ATEND},
        {"From": 2, "To": 0, "Duration": 0.05, "AtEnd": ATEND},
    ],
})
nmanimstate.save(PATH, state)
print("added Recoil")
