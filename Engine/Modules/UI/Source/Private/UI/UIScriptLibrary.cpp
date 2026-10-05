#include <algorithm>

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidgets.h"

/*
 * Lua's UI library: documents opened and closed, widgets changed by name,
 * and the data store bindings read. On the session's UISystem
 * (SetScriptUISystem); every call is a no-op without one.
 *
 *     UI.Open("Content/UI/Pause.nmui")
 *     UI.SetText("Content/UI/HUD.nmui", "Ammo", "12 / 30")
 *     UI.SetNumber("Player0.Health", 0.4)
 */
namespace
{
    UI::UIWidget* FindWidget(GameCore::ScriptCall& Call, UI::UIDocumentInstance*& OutDocument)
    {
        OutDocument = nullptr;
        UI::UISystem* System = UI::GetScriptUISystem();
        if(!System) return nullptr;
        OutDocument = System->Find(std::string(Call.GetString(1)));
        return OutDocument ? OutDocument->FindWidget(Call.GetString(2)) : nullptr;
    }

    /* Widget InName's property InProperty, from Lua's third argument as text. */
    void SetFromText(GameCore::ScriptCall& Call, const char* InProperty, const std::string& InText)
    {
        UI::UIDocumentInstance* Document = nullptr;
        UI::UIWidget* Widget = FindWidget(Call, Document);
        Call.PushBool(Widget && Document->SetPropertyText(*Widget, InProperty, InText));
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(UI)
{
    // === Documents ===========================================================

    Lib.Function("Open", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        const std::string Path = Call.GetString(1);
        const bool bPersistent = Call.HasArg(2) && Call.GetBool(2);
        Call.PushInteger(System ? int64_t(System->Open(Path, bPersistent)) : 0);
    }, "Path:string, Persistent:bool -> Handle:integer").Tooltip("Opens a .nmui over the others (0 if it does not load)");

    Lib.Function("Close", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        Call.PushBool(System && System->Close(std::string(Call.GetString(1))));
    }, "Path:string -> Closed:bool");

    Lib.Pure("IsOpen", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        Call.PushBool(System && System->IsOpen(Call.GetString(1)));
    }, "Path:string -> Open:bool");

    // === Widgets =============================================================

    Lib.Function("SetText", [](GameCore::ScriptCall& Call)
    {
        SetFromText(Call, "Text", Call.GetString(3));
    }, "Path:string, Widget:string, Text:string -> Done:bool").Tooltip("A Text widget's text, found by its Name");

    Lib.Function("SetVisible", [](GameCore::ScriptCall& Call)
    {
        SetFromText(Call, "Visibility", Call.GetBool(3) ? "Visible" : "Collapsed");
    }, "Path:string, Widget:string, Visible:bool -> Done:bool").Tooltip("Visible, or Collapsed (taking no space)");

    Lib.Function("SetEnabled", [](GameCore::ScriptCall& Call)
    {
        SetFromText(Call, "IsEnabled", Call.GetBool(3) ? "true" : "false");
    }, "Path:string, Widget:string, Enabled:bool -> Done:bool");

    Lib.Function("SetProperty", [](GameCore::ScriptCall& Call)
    {
        const std::string Property = Call.GetString(3);
        SetFromText(Call, Property.c_str(), Call.GetString(4));
    }, "Path:string, Widget:string, Property:string, Value:string -> Done:bool").Tooltip("Any property, as XML writes it");

    Lib.Function("SetFocus", [](GameCore::ScriptCall& Call)
    {
        UI::UIDocumentInstance* Document = nullptr;
        UI::UIWidget* Widget = FindWidget(Call, Document);
        Call.PushBool(Widget && Document->SetFocus(int32_t(Widget->GetIndex())));
    }, "Path:string, Widget:string -> Done:bool");

    Lib.Function("ShowPage", [](GameCore::ScriptCall& Call)
    {
        UI::UIDocumentInstance* Document = nullptr;
        UI::UIWidget* Widget = FindWidget(Call, Document);
        auto* Switcher = dynamic_cast<UI::UISwitcher*>(Widget);
        Call.PushBool(Switcher && Switcher->ShowPage(Call.GetString(3)));
    }, "Path:string, Switcher:string, Page:string -> Done:bool").Tooltip("A Switcher's page, by its Name or its number (0 first)");

    Lib.Function("PlayAnimation", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        UI::UIDocumentInstance* Document = System ? System->Find(std::string(Call.GetString(1))) : nullptr;
        Call.PushBool(Document && Document->PlayAnimation(Call.GetString(2)));
    }, "Path:string, Animation:string -> Playing:bool").Tooltip("Reserved: animations do not play yet (false)");

    // === Data store ==========================================================

    Lib.Function("SetNumber", [](GameCore::ScriptCall& Call)
    {
        if(UI::UISystem* System = UI::GetScriptUISystem()) { System->SetValue(Call.GetString(1), UI::UIValue::Number(Call.GetNumber(2))); }
    }, "Key:string, Value:number").Tooltip("A value {Bind:Key} shows");

    Lib.Function("SetString", [](GameCore::ScriptCall& Call)
    {
        if(UI::UISystem* System = UI::GetScriptUISystem()) { System->SetValue(Call.GetString(1), UI::UIValue::String(Call.GetString(2))); }
    }, "Key:string, Value:string");

    Lib.Function("SetBool", [](GameCore::ScriptCall& Call)
    {
        if(UI::UISystem* System = UI::GetScriptUISystem()) { System->SetValue(Call.GetString(1), UI::UIValue::Bool(Call.GetBool(2))); }
    }, "Key:string, Value:bool");

    Lib.Pure("GetNumber", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        const UI::UIValue* Value = System ? System->GetValue(Call.GetString(1)) : nullptr;
        Call.PushNumber(Value ? Value->AsNumber() : 0.0);
    }, "Key:string -> Value:number");

    Lib.Function("SetCount", [](GameCore::ScriptCall& Call)
    {
        /* A ListView's length: List.Count. Its items are List.<i>.<Field>. */
        if(UI::UISystem* System = UI::GetScriptUISystem())
        {
            System->SetValue(std::string(Call.GetString(1)) + ".Count", UI::UIValue::Number(std::max(Call.GetNumber(2), 0.0)));
        }
    }, "List:string, Count:integer").Tooltip("A ListView's item count: List.Count");

    Lib.Pure("GetBool", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        const UI::UIValue* Value = System ? System->GetValue(Call.GetString(1)) : nullptr;
        Call.PushBool(Value && (Value->Type == UI::EUIValueType::Bool ? Value->B : Value->AsNumber() != 0.0));
    }, "Key:string -> Value:bool");

    Lib.Pure("GetString", [](GameCore::ScriptCall& Call)
    {
        UI::UISystem* System = UI::GetScriptUISystem();
        const UI::UIValue* Value = System ? System->GetValue(Call.GetString(1)) : nullptr;
        Call.PushString(Value ? Value->AsString().c_str() : "");
    }, "Key:string -> Value:string");
}
