#include "Public/NimbleEngine/Input.h"

/*
 * The player's map (TDD §11). Look axes are degrees this frame, +down
 * (CharacterMoverGuide.md "The input map"); Config/DefaultGame.ini's
 * [Input_DemoLabPlayer] overrides any binding here, and Project Settings >
 * Input edits that.
 */
NIMBLE_DEFINE_INPUT_MAP(DemoLabPlayer) {
    Map.Axis("MoveForward", "W, -S, -Gamepad.LeftY");
    Map.Axis("MoveRight", "D, -A, Gamepad.LeftX");
    Map.Axis("LookX", "Mouse.DeltaX*0.15, Gamepad.RightX*2.5");
    Map.Axis("LookY", "Mouse.DeltaY*0.15, Gamepad.RightY*2.5");
    Map.Action("Run", "LeftShift, Gamepad.LeftStick");
    Map.Action("Jump", "Space, Gamepad.South");
    Map.Action("Crouch", "C, LeftCtrl, Gamepad.East");
    Map.Action("Aim", "Mouse.Right, Gamepad.LeftTrigger");
    Map.Action("Fire", "Mouse.Left, Gamepad.RightTrigger");
    Map.Action("Reload", "R, Gamepad.West");
    Map.Action("Throw", "G, Gamepad.RightShoulder");
    Map.Action("Melee", "V, Gamepad.RightStick");
    Map.Action("Interact", "E, Gamepad.North");
    Map.Action("NextWeapon", "Q, Gamepad.DPadRight");
    Map.Action("PrevWeapon", "Gamepad.DPadLeft");
    Map.Action("Slot1", "1");
    Map.Action("Slot2", "2");
    Map.Action("Slot3", "3");
    Map.Action("Holster", "H, Gamepad.DPadDown");
    Map.Action("Pause", "Escape, Gamepad.Start");
    Map.Action("TechOverlay", "F1, Gamepad.Back");
}
