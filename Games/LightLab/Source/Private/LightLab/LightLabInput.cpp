#include "Public/NimbleEngine/Input.h"

/*
 * LightLab's controls: one input map, "Default", read by the Player entity's
 * InputComponent. Keys are names (Engine/Source/INPUT.md); each line is also
 * a config var, so Config/DefaultEngine.ini can rebind it without a rebuild:
 *
 *     [Input_Default]
 *     ToggleShadows = G
 *
 * Who reads it:
 *   Player   LightLabGame.cpp's LightLab.Fly system (Move*, Look*, Boost)
 *            and LightLabGameMode (CycleTechnique, ToggleShadows, ToggleBakedLighting,
 *            ResetLevel, Pause, CaptureMouse); in PhysicsLab, LightLabPhysicsMode
 *            (Throw, ResetLevel, EnterVehicle, Pause, CaptureMouse) and its
 *            LightLab.Drive system (MoveForward, MoveRight, Handbrake)
 *
 * The pause menu (Content/UI/PauseMenu.nmui) is navigated with the engine's
 * own "UI" map (arrows, Return, Escape; the D-pad, South, East) while it is open.
 */
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Action("Boost",          "LeftShift, RightShift, Gamepad.LeftStick");
    Map.Action("CycleTechnique", "T, Gamepad.North");
    Map.Action("ToggleShadows",  "F, Gamepad.West");
    Map.Action("ToggleBakedLighting", "L, Gamepad.East");
    Map.Action("Pause",          "Escape, Gamepad.Start");
    Map.Action("CaptureMouse",   "Mouse.Left");

    /* Every level: back to how it loaded. */
    Map.Action("ResetLevel",     "R, Gamepad.Back");

    /* PhysicsLab (LightLabPhysics.cpp): a click, once the mouse is held, throws a ball;
     * C gets in and out of the buggy, which drives on the Move axes, Space its handbrake. */
    Map.Action("Throw",          "Mouse.Left, Gamepad.RightTrigger");
    Map.Action("EnterVehicle",   "C, Gamepad.North");
    Map.Action("Handbrake",      "Space, Gamepad.South");

    /* Along the view, across it, and along world up (+Y). */
    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis("MoveUp",      "E, Space, -Q, -LeftCtrl, Gamepad.RightShoulder, -Gamepad.LeftShoulder");

    /* Degrees this frame. Mouse axes are not clamped, so a pixel of motion is
     * 0.15 degrees; a full stick is 2.5 degrees a frame. Both read +Y down (the
     * window's and the stick's own sense), so down looks down. */
    Map.Axis("LookX", "Mouse.DeltaX*0.15, Gamepad.RightX*2.5");
    Map.Axis("LookY", "Mouse.DeltaY*0.15, Gamepad.RightY*2.5");
}
