#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/UI/UIAction.h"
#include "Public/UI/UISystem.h"

/*
 * What every game's UI has (Modules/UI/README.md): the "UI" input map its
 * menus navigate with, and the native actions a document can name without
 * a game writing any -- OnClick="Native:Game.Quit".
 */

/* Menu navigation. Rebindable like any map: [Input.UI] Accept=... in config. */
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

    /* The document an action names by its first argument, or its own. */
    std::string PathOf(const UI::UIActionContext& InContext)
    {
        if(InContext.Args->Count > 0 && InContext.Args->Args[0].Type == GameCore::EventArg::ET_String)
        {
            return InContext.Args->Args[0].String;
        }
        return InContext.Document ? InContext.Document->GetPath() : std::string();
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

UI_DEFINE_ACTIONS(UI)
{
    Actions.Add("Open", [](const UI::UIActionContext& InContext)
    {
        if(InContext.System && InContext.Args->Count > 0) { InContext.System->Open(PathOf(InContext)); }
    }, "Opens the document named ('Content/UI/Options.nmui')");

    Actions.Add("Close", [](const UI::UIActionContext& InContext)
    {
        if(InContext.System) { InContext.System->Close(PathOf(InContext)); }
    }, "Closes the document named, or this one");

    Actions.Add("Toggle", [](const UI::UIActionContext& InContext)
    {
        const std::string Path = PathOf(InContext);
        if(!InContext.System || Path.empty()) return;
        if(InContext.System->IsOpen(Path)) { InContext.System->Close(Path); }
        else                               { InContext.System->Open(Path); }
    }, "Opens the document named, or closes it if open");

    Actions.Add("SetValue", [](const UI::UIActionContext& InContext)
    {
        const GameCore::EventArgs& Args = *InContext.Args;
        if(!InContext.System || Args.Count < 2 || Args.Args[0].Type != GameCore::EventArg::ET_String) return;
        const GameCore::EventArg& Value = Args.Args[1];
        UI::UIValue Set;
        switch(Value.Type)
        {
            case GameCore::EventArg::ET_Number: Set = UI::UIValue::Number(Value.Number); break;
            case GameCore::EventArg::ET_Bool:   Set = UI::UIValue::Bool(Value.Bool); break;
            case GameCore::EventArg::ET_String: Set = UI::UIValue::String(Value.String); break;
            default: return;
        }
        InContext.System->SetValue(Args.Args[0].String, Set);
    }, "Sets a data store value: UI.SetValue('Key', value)");
}
