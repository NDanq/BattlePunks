#include "Public/NimbleEngine/Input.h"

/*
 * Sandbox's controls: one input map, "Default", which every InputComponent
 * in the game reads (its Map field's default). Keys are names
 * (Engine/Source/INPUT.md); each line is also a config var, so
 * Config/DefaultEngine.ini can rebind it without a rebuild:
 *
 *     [Input_Default]
 *     Jump = K, Gamepad.South
 *
 * Who reads it:
 *   Marker      SandboxGame.cpp's Sandbox.DriveMarker system (MoveX, MoveY,
 *               Boost) and Scripts/JumpCounter.script.nmgraph (Jump)
 *   InputDemo   Scripts/InputDemo.lua (Jump, Boost, and raw keys)
 */
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Action("Jump",  "Space, Gamepad.South");
    Map.Action("Boost", "LeftShift, RightShift, Gamepad.RightShoulder");

    /* +X right and +Y up, as the view draws them (a stick's LeftY is +down, hence its sign). */
    Map.Axis("MoveX", "D, Right, -A, -Left, Gamepad.LeftX, Gamepad.DPadRight, -Gamepad.DPadLeft");
    Map.Axis("MoveY", "W, Up, -S, -Down, -Gamepad.LeftY, Gamepad.DPadUp, -Gamepad.DPadDown");
}
