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
 *            Pause, CaptureMouse)
 *
 * and a second map, "Fox", read by the SteerableFox prefab's InputComponent
 * (Content/Prefabs/SteerableFox.script.nmgraph). Its keys are clear of
 * Default's, so the camera and the fox steer at once.
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

/* The steerable fox: U walks on (hold Y to run), H and K turn, J sits once it
 * has stopped, I looks round (its head layer). The pad's D-pad and South. */
NIMBLE_DEFINE_INPUT_MAP(Fox)
{
    Map.Action("FoxSit", "J, Gamepad.DPadDown");

    /* Run and Look are axes, 0 or 1 held, so the graph blends with them. */
    Map.Axis("FoxForward", "U, Gamepad.DPadUp");
    Map.Axis("FoxTurn",    "K, -H, Gamepad.DPadRight, -Gamepad.DPadLeft");
    Map.Axis("FoxRun",     "Y, Gamepad.South");
    Map.Axis("FoxLook",    "I");
}
