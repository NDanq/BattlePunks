/*
 * NimbleEngineInputTests -- the input path from InputState to scripts:
 * key names, frame edges, input maps (and their config overrides), the
 * InputComponent's resolve at the start of TP_PrePhysics, ahead of scripts,
 * and the "Input" script library. No window: events go straight into the
 * state, as the router's callbacks would put them.
 */

#include <cmath>
#include <cstdio>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/InputRouter.h"

using namespace NimbleEngine;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

NIMBLE_DEFINE_INPUT_MAP(TestMap)
{
    Map.Action("Jump",  "Space, Gamepad.South");
    Map.Action("Fire",  "Mouse.Left, LeftCtrl");
    Map.Axis  ("MoveX", "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis  ("LookX", "Mouse.DeltaX*0.5");
}

namespace
{
    InputKey Key(const char* InName) { return FindInputKey(InName); }

    void TestNames()
    {
        std::printf("[names]\n");
        Expect(Key("Space") != INPUT_KEY_INVALID && std::string(GetInputKeyName(Key("space"))) == "Space",
               "a key is found by name, in any case, and named back");
        Expect(std::string(GetInputKeyName(Key("Mouse.Left"))) == "Mouse.Left", "mouse buttons have names");
        Expect(std::string(GetInputKeyName(Key("gamepad.south"))) == "Gamepad.South", "gamepad buttons have names");
        Expect(IsAnalogInputKey(Key("Gamepad.LeftX")) && IsAnalogInputKey(Key("Mouse.DeltaX")) && !IsAnalogInputKey(Key("W")),
               "sticks and mouse motion are analog, keys are not");
        Expect(Key("NoSuchKey") == INPUT_KEY_INVALID && Key("") == INPUT_KEY_INVALID, "an unknown name is no key");
    }

    void TestEdges()
    {
        std::printf("[edges]\n");
        InputState S;
        S.Press(Key("W"));
        Expect(S.IsDown("W") && S.WasPressed("W") && !S.WasReleased("W"), "a press is down and pressed");
        S.Press(Key("W"));
        S.BeginFrame(1);
        Expect(S.IsDown("W") && !S.WasPressed("W"), "the next frame it is held, not pressed; a repeat is no press");

        S.Press(Key("E"));
        S.Release(Key("E"));
        Expect(!S.IsDown("E") && S.WasPressed("E") && S.WasReleased("E"), "a tap inside one frame is pressed and released");

        S.MoveMouse(10, 20, 3, -2);
        S.MoveMouse(12, 21, 2, 1);
        Expect(S.GetMouseDelta().X == 5 && S.GetMouseDelta().Y == -1 && S.GetMousePosition().X == 12,
               "mouse motion sums over the frame; the position is the last");
        Expect(S.GetValue("Mouse.DeltaX") == 5.0f, "and reads as the Mouse.DeltaX axis");

        S.SetPadConnected(1, true);
        S.Press(Key("Gamepad.South"), 1);
        S.SetAnalog(Key("Gamepad.LeftX"), 0.75f, 1);
        Expect(S.IsDown("Gamepad.South", 1) && !S.IsDown("Gamepad.South", 0), "a pad button is its player's");
        Expect(S.GetValue("Gamepad.LeftX", 1) == 0.75f && S.IsPadConnected(1), "and so is a stick");
        S.SetPadConnected(1, false);
        Expect(!S.IsDown("Gamepad.South", 1) && S.GetValue("Gamepad.LeftX", 1) == 0.0f, "a disconnected pad lets go");

        S.ReleaseAll();
        Expect(!S.IsDown("W") && S.WasReleased("W"), "focus loss releases what was held, with the edge");
        S.BeginFrame(2);
        Expect(S.GetMouseDelta().X == 0 && !S.WasReleased("W"), "a new frame clears edges and motion");
    }

    void TestRouterLatch()
    {
        std::printf("[router]\n");
        InputRouter Router;   // no window: fed through GetPending, as the editor's fallback is
        Router.GetPending().Press(Key("Space"));
        Expect(!Router.GetState().IsDown("Space"), "pending input is not this frame's until latched");
        Router.Latch(7);
        Expect(Router.GetState().WasPressed("Space") && Router.GetState().GetFrameNumber() == 7, "Latch publishes it");
        Router.Latch(8);
        Expect(Router.GetState().IsDown("Space") && !Router.GetState().WasPressed("Space"), "and the next latch keeps it held");
    }

    void TestComponents()
    {
        std::printf("[components]\n");
        Expect(FindInputAction("TestMap", "Fire") == 1 && FindInputAxis("TestMap", "LookX") == 1, "a map's entries are indexed in order");

        GameCore::World W;
        GameCore::ScriptEngine Scripts;
        Scripts.Initialize();
        Scripts.BindWorld(&W);

        InputState S;
        SetScriptInputState(&S);
        W.RegisterSystem("Test.Input", GameCore::TP_PrePhysics,
                         [](GameCore::World& InWorld, float, void* InState) { ResolveInputComponents(InWorld, *static_cast<InputState*>(InState)); },
                         &S, GameCore::SO_BeforeScripts);

        /* A PrePhysics script records what it sees: it must be this frame's. */
        const char* const Script =
            "return { TickPhase = TickPhase.PrePhysics,"
            "  OnTick = function(self, dt)"
            "    local input = self.Entity:GetComponent('Input')"
            "    Seen = { jump = input:WasPressed('Jump'), held = input:IsDown('Jump'), x = input:GetAxis('MoveX'),"
            "             look = input:GetAxis('LookX'), raw = Input.IsKeyDown('Space') }"
            "  end }";
        const std::string Path = "InputProbe.lua";
        if(FILE* File = std::fopen(Path.c_str(), "wb")) { std::fputs(Script, File); std::fclose(File); }

        GameCore::Entity Player = W.CreateEntity("Player");
        InputComponent Input;
        std::snprintf(Input.Map, sizeof(Input.Map), "TestMap");
        Player.AddComponent(Input);
        Player.SetScript(Path.c_str());
        W.Tick(0.016f);   // starts the script

        S.BeginFrame(1);
        S.Press(Key("Space"));
        S.Press(Key("D"));
        S.MoveMouse(0, 0, 30, 0);
        W.Tick(0.016f);
        Expect(Scripts.RunString("assert(Seen.jump == true and Seen.held == true and Seen.x == 1 and Seen.look == 15 and Seen.raw == true)",
                                 "seen-1"),
               "a TP_PrePhysics script sees this frame's press, axis, unclamped mouse axis and raw key");

        S.BeginFrame(2);
        S.Press(Key("Gamepad.South"));   // a second key for a held action
        S.Press(Key("A"));
        W.Tick(0.016f);
        Expect(Scripts.RunString("assert(Seen.jump == false and Seen.held == true and Seen.x == 0)", "seen-2"),
               "a held action is not pressed again by a second key; opposite keys cancel");

        S.BeginFrame(3);
        S.Release(Key("Space"));
        S.Release(Key("Gamepad.South"));
        S.Release(Key("D"));
        S.Release(Key("A"));
        S.Press(Key("Mouse.Left"));
        S.Release(Key("Mouse.Left"));
        W.Tick(0.016f);
        const InputComponent* C = Player.GetComponent<InputComponent>();
        Expect(C && !C->IsDown("Jump") && C->WasReleased("Jump"), "letting go releases the action");
        Expect(C && C->WasPressed("Fire") && C->WasReleased("Fire") && !C->IsDown("Fire"), "a tap of a bound key is pressed and released");

        /* Rebinding through config: an .ini line, or the console. */
        EngineCore::LoadConfigText("[Input_TestMap]\nJump = K\n");
        S.BeginFrame(4);
        S.Press(Key("K"));
        W.Tick(0.016f);
        Expect(C && C->WasPressed("Jump"), "[Input_TestMap] Jump = K rebinds Jump");
        S.BeginFrame(5);
        S.Release(Key("K"));
        S.Press(Key("Space"));
        W.Tick(0.016f);
        Expect(C && !C->IsDown("Jump"), "and Space no longer jumps");
        EngineCore::LoadConfigText("[Input_TestMap]\nJump = Space, Gamepad.South\n");

        /* Disabled, everything reads up. */
        Player.GetComponent<InputComponent>()->bEnabled = 0;
        S.BeginFrame(6);
        W.Tick(0.016f);
        Expect(C && !C->IsDown("Jump") && C->GetAxis("MoveX") == 0.0f, "a disabled component reads nothing");

        Expect(Scripts.GetErrorCount() == 0, "the probe script raised no Lua errors");
        SetScriptInputState(nullptr);
        Scripts.UnbindWorld();
        Scripts.Shutdown();
        std::remove(Path.c_str());
    }
}

int main()
{
    std::printf("NimbleEngineInputTests\n");
    TestNames();
    TestEdges();
    TestRouterLatch();
    TestComponents();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
