#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/UI/UIAction.h"
#include "Public/UI/UISystem.h"

/*
 * What every game's UI has (Modules/UI/README.md): the "UI" input map its
 * menus navigate with, and the Game.* native actions a document can name
 * without a game writing any -- OnClick="Native:Game.Quit". The UI.* ones
 * are the UI module's own (UIBuiltinActions.cpp).
 */

/* Menu navigation. Rebindable like any map: [Input_UI] Accept=... in config. */
NIMBLE_DEFINE_INPUT_MAP(UI)
{
    Map.Action("Up",     "Up, Gamepad.DPadUp");
    Map.Action("Down",   "Down, Gamepad.DPadDown");
    Map.Action("Left",   "Left, Gamepad.DPadLeft");
    Map.Action("Right",  "Right, Gamepad.DPadRight");
    Map.Action("Accept", "Return, Space, Gamepad.South");
    Map.Action("Back",   "Escape, Gamepad.East");
    Map.Action("Next",   "Tab");
}

namespace
{
    NimbleEngine::GameSession* SessionOf(const UI::UIActionContext& InContext)
    {
        return static_cast<NimbleEngine::GameSession*>(InContext.HostData);
    }
}

UI_DEFINE_ACTIONS(Game)
{
    Actions.Add("Quit", [](const UI::UIActionContext& InContext)
    {
        if(NimbleEngine::GameSession* Session = SessionOf(InContext)) { Session->RequestQuit(); }
    }, "Ends the game");

    Actions.Add("Pause", [](const UI::UIActionContext& InContext)
    {
        if(NimbleEngine::GameSession* Session = SessionOf(InContext)) { Session->SetPaused(true); }
    }, "Stops the World; the UI keeps running");

    Actions.Add("Resume", [](const UI::UIActionContext& InContext)
    {
        if(NimbleEngine::GameSession* Session = SessionOf(InContext)) { Session->SetPaused(false); }
    });

    Actions.Add("TogglePause", [](const UI::UIActionContext& InContext)
    {
        if(NimbleEngine::GameSession* Session = SessionOf(InContext)) { Session->SetPaused(!Session->IsPaused()); }
    });
}
