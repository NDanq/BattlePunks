#include "Public/UI/UIAction.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidgets.h"

/*
 * The UI.* native actions every document can name: opening and closing
 * documents, switching a Switcher's page, setting data store values, and
 * playing a document's animations.
 * They need only the UISystem, so they work in a game, a test and the
 * editor's preview alike (where they are logged, not run).
 */
namespace
{
    std::string StringArg(const UI::UIActionContext& InContext, const int InIndex)
    {
        if(InContext.Args->Count <= InIndex) return std::string();
        const GameCore::EventArg& Arg = InContext.Args->Args[InIndex];
        if(Arg.Type == GameCore::EventArg::ET_String) return Arg.String;
        if(Arg.Type == GameCore::EventArg::ET_Number) return UI::FormatFloat(float(Arg.Number));
        return std::string();
    }

    /* The document an action names by its first argument, or its own. */
    std::string PathOf(const UI::UIActionContext& InContext)
    {
        const std::string Named = StringArg(InContext, 0);
        if(!Named.empty()) return Named;
        return InContext.Document ? InContext.Document->GetPath() : std::string();
    }
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

    Actions.Add("ShowPage", [](const UI::UIActionContext& InContext)
    {
        /* ('Switcher', 'Page') in this document: a page by name, or by number. */
        UI::UIDocumentInstance* Document = InContext.Document;
        UI::UIWidget* Widget = Document ? Document->FindWidget(StringArg(InContext, 0)) : nullptr;
        if(auto* Switcher = dynamic_cast<UI::UISwitcher*>(Widget)) { Switcher->ShowPage(StringArg(InContext, 1)); }
    }, "Shows a Switcher's page: UI.ShowPage('Pages', 'Graphics') or UI.ShowPage('Pages', 1)");

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

    Actions.Add("PlayAnimation", [](const UI::UIActionContext& InContext)
    {
        /* ('Name') in this document, or ('Name', 'Content/UI/Other.nmui'). */
        const std::string Name  = StringArg(InContext, 0);
        const std::string Other = StringArg(InContext, 1);
        UI::UIDocumentInstance* Document = InContext.Document;
        if(!Other.empty() && InContext.System) { Document = InContext.System->Find(Other); }
        if(Document && !Name.empty()) { Document->PlayAnimation(Name); }
    }, "Plays an animation: UI.PlayAnimation('Pulse'), or in another document UI.PlayAnimation('Pulse', 'Content/UI/HUD.nmui')");

    Actions.Add("StopAnimation", [](const UI::UIActionContext& InContext)
    {
        const std::string Name  = StringArg(InContext, 0);
        const std::string Other = StringArg(InContext, 1);
        UI::UIDocumentInstance* Document = InContext.Document;
        if(!Other.empty() && InContext.System) { Document = InContext.System->Find(Other); }
        if(Document) { Document->StopAnimation(Name); }
    }, "Stops an animation where it is: UI.StopAnimation('Pulse')");
}
