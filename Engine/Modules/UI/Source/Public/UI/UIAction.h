#ifndef UI_UIACTION_H
#define UI_UIACTION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/GameCore/EventBus.h"
#include "Public/UI/UIAPI.h"

/*
 * What a widget does when it is clicked (or hovered, pressed, ...): a list of
 * actions written as text, parsed once when the document loads.
 *
 *     OnClick="Native:Game.Quit"
 *     OnClick="Lua:MainMenu.Play"
 *     OnClick="Lua:Menu.Select('Arena', 2); Event:Menu.Changed($self)"
 *
 * Native   a C++ function registered with UI_DEFINE_ACTIONS ("Group.Name")
 * Lua      a Lua function by global path (ScriptEngine::CallFunction)
 * Event    an EventBus event of the session's World (Events.On in Lua)
 * Graph    a handler of the document's UI graph (its Graph setting):
 *          Graph:Resume_OnClick calls self:Resume_OnClick(value, sender) on
 *          the document's own instance of it. No arguments.
 *
 * Up to four arguments: numbers, 'strings' (or "strings"), true, false, and
 * two tokens -- $self, the widget's name, and $value, its value (a slider's,
 * a check box's 1 or 0; 0 for a button). They arrive as GameCore::EventArgs.
 *
 * Actions are queued and run after the input that fired them (UISystem), so
 * one that closes its own document is safe.
 */
namespace UI
{
    class UISystem;
    class UIDocumentInstance;
    class UIWidget;

    enum class EUIActionKind : uint8_t { Native, Lua, Event, Graph };

    struct UIAction
    {
        enum EArgToken : uint8_t { AT_Literal = 0, AT_Self, AT_Value };

        EUIActionKind      Kind = EUIActionKind::Native;
        std::string        Target;            // "Game.Quit", "MainMenu.Play", "Menu.Changed"
        GameCore::EventArgs Args;             // literals; tokens filled in when run
        EArgToken          Tokens[4] = { AT_Literal, AT_Literal, AT_Literal, AT_Literal };
    };

    /* A property's actions, as written and as parsed. */
    struct UI_API UIActionList
    {
        std::string           Source;
        std::vector<UIAction> Actions;

        bool IsEmpty() const { return Actions.empty(); }

        /* Replaces this list with InSource's. False, with OutError, on bad text:
         * the list is then empty but Source keeps the text, so an editor shows
         * what was typed. "" is an empty list. */
        bool Parse(const std::string& InSource, std::string* OutError = nullptr);

        bool operator==(const UIActionList& InOther) const { return Source == InOther.Source; }
    };

    // === Native actions =======================================================

    /* What a native action is called with. */
    struct UIActionContext
    {
        UISystem*                  System   = nullptr;
        UIDocumentInstance*        Document = nullptr;   // null for an action run without one
        UIWidget*                  Sender   = nullptr;   // null for a document's own (OnBack)
        const GameCore::EventArgs* Args     = nullptr;   // never null when called
        void*                      HostData = nullptr;   // UISystem::SetHostData: the game session
    };

    using UINativeAction = void (*)(const UIActionContext& InContext);

    using UIActionHandle = uint32_t;
    constexpr UIActionHandle UI_ACTION_INVALID = 0;

    /* Registers InFn as InName ("Group.Name"). UI_ACTION_INVALID, logged, if
     * the name is taken or malformed. */
    UI_API UIActionHandle RegisterUIAction(const char* InName, UINativeAction InFn, const char* InTooltip = nullptr);

    /* A no-op for UI_ACTION_INVALID or one already gone. */
    UI_API void UnregisterUIAction(UIActionHandle InHandle);

    /* Null if InName is not registered (now). */
    UI_API UINativeAction FindUIAction(const char* InName);

    /* The registered actions in name order, for tools (the editor's action picker). */
    struct UIActionInfo
    {
        std::string Name;
        std::string Tooltip;
    };
    UI_API std::vector<UIActionInfo> GetUIActions();

    /* What `Actions` is inside UI_DEFINE_ACTIONS. */
    class UIActionBinder
    {
    public:
        explicit UIActionBinder(const char* InGroup) : Group(InGroup) {}

        UIActionBinder& Add(const char* InName, const UINativeAction InFn, const char* InTooltip = nullptr)
        {
            const UIActionHandle Handle = RegisterUIAction((std::string(Group) + "." + InName).c_str(), InFn, InTooltip);
            if(Handle != UI_ACTION_INVALID) { Handles.push_back(Handle); }
            return *this;
        }

        std::vector<UIActionHandle> Handles;

    private:
        const char* Group;
    };
}

/*
 * Native actions "Group.<Name>", registered while this image is loaded --
 * a game module's go away when it unloads, as GAMECORE_DEFINE_SCRIPT_LIBRARY's do:
 *
 *     UI_DEFINE_ACTIONS(LightLab)
 *     {
 *         Actions.Add("Resume", &ResumeGame, "Closes the pause menu and resumes");
 *     }
 */
#define UI_DEFINE_ACTIONS(Group)                                                              \
    static void Group##_UIActionsBody(::UI::UIActionBinder& Actions);                         \
    namespace                                                                                 \
    {                                                                                         \
        struct Group##_UIActionsRegistrar                                                     \
        {                                                                                     \
            Group##_UIActionsRegistrar() : Binder(#Group) { Group##_UIActionsBody(Binder); }  \
            ~Group##_UIActionsRegistrar()                                                     \
            {                                                                                 \
                for(const ::UI::UIActionHandle Each : Binder.Handles)                         \
                {                                                                             \
                    ::UI::UnregisterUIAction(Each);                                           \
                }                                                                             \
            }                                                                                 \
            ::UI::UIActionBinder Binder;                                                      \
        };                                                                                    \
        Group##_UIActionsRegistrar G##Group##_UIActionsRegistrar;                             \
    }                                                                                         \
    static void Group##_UIActionsBody(::UI::UIActionBinder& Actions)

#endif //UI_UIACTION_H
