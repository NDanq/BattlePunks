#include "Public/UI/UIAction.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>

#include "Public/UI/UILog.h"

namespace UI
{
    namespace
    {
        /* A cursor over an action list's text, for the parser below. */
        struct Cursor
        {
            const std::string& Text;
            size_t             At = 0;

            void SkipSpace()
            {
                while(At < Text.size() && std::isspace(static_cast<unsigned char>(Text[At]))) { ++At; }
            }
            bool AtEnd() { SkipSpace(); return At >= Text.size(); }
            bool Take(const char InChar)
            {
                SkipSpace();
                if(At < Text.size() && Text[At] == InChar) { ++At; return true; }
                return false;
            }
            /* [A-Za-z0-9_.]+, or "" if none here. */
            std::string Word()
            {
                SkipSpace();
                const size_t Start = At;
                while(At < Text.size() && (std::isalnum(static_cast<unsigned char>(Text[At])) || Text[At] == '_' || Text[At] == '.')) { ++At; }
                return Text.substr(Start, At - Start);
            }
        };

        bool IsDottedName(const std::string& InName)
        {
            if(InName.empty() || InName.front() == '.' || InName.back() == '.' || InName.find("..") != std::string::npos) return false;
            return !std::isdigit(static_cast<unsigned char>(InName.front()));
        }

        /* One argument at InCursor into OutAction's InIndex. */
        bool ParseArg(Cursor& InCursor, UIAction& OutAction, const int InIndex, std::string& OutError)
        {
            InCursor.SkipSpace();
            const std::string& Text = InCursor.Text;
            if(InCursor.At >= Text.size()) { OutError = "an argument is missing"; return false; }

            const char First = Text[InCursor.At];
            if(First == '\'' || First == '"')
            {
                const size_t Close = Text.find(First, InCursor.At + 1);
                if(Close == std::string::npos) { OutError = "a string is not closed"; return false; }
                OutAction.Args.Args[InIndex] = GameCore::EventArg::Make(Text.substr(InCursor.At + 1, Close - InCursor.At - 1).c_str());
                InCursor.At = Close + 1;
                return true;
            }
            if(First == '$')
            {
                ++InCursor.At;
                const std::string Token = InCursor.Word();
                if(Token == "self")       { OutAction.Tokens[InIndex] = UIAction::AT_Self; }
                else if(Token == "value") { OutAction.Tokens[InIndex] = UIAction::AT_Value; }
                else                      { OutError = "unknown token $" + Token; return false; }
                return true;
            }
            if(First == '-' || First == '+' || std::isdigit(static_cast<unsigned char>(First)) || First == '.')
            {
                char* End = nullptr;
                const double Value = std::strtod(Text.c_str() + InCursor.At, &End);
                if(End == Text.c_str() + InCursor.At) { OutError = "a bad number"; return false; }
                InCursor.At = size_t(End - Text.c_str());
                OutAction.Args.Args[InIndex] = GameCore::EventArg::Make(Value);
                return true;
            }
            const std::string Word = InCursor.Word();
            if(Word == "true" || Word == "false")
            {
                OutAction.Args.Args[InIndex] = GameCore::EventArg::Make(Word == "true");
                return true;
            }
            OutError = Word.empty() ? std::string("an argument is not a number, a 'string', true, false or a $token")
                                    : "'" + Word + "' is not an argument (strings are quoted)";
            return false;
        }

        struct NativeEntry
        {
            UIActionHandle Handle = UI_ACTION_INVALID;
            UINativeAction Fn     = nullptr;
            std::string    Tooltip;
        };

        struct NativeRegistry
        {
            std::mutex                          Lock;
            std::map<std::string, NativeEntry>  ByName;
            UIActionHandle                      NextHandle = 1;
        };

        NativeRegistry& GetNatives()
        {
            static NativeRegistry Instance;
            return Instance;
        }
    }

    bool UIActionList::Parse(const std::string& InSource, std::string* OutError)
    {
        Source = InSource;
        Actions.clear();

        Cursor C{ InSource };
        std::string Error;
        std::vector<UIAction> Parsed;
        while(!C.AtEnd())
        {
            UIAction Action;
            const std::string Kind = C.Word();
            if(Kind == "Native")     { Action.Kind = EUIActionKind::Native; }
            else if(Kind == "Lua")   { Action.Kind = EUIActionKind::Lua; }
            else if(Kind == "Event") { Action.Kind = EUIActionKind::Event; }
            else if(Kind == "Graph") { Action.Kind = EUIActionKind::Graph; }
            else
            {
                Error = Kind.empty() ? std::string("expected Native:, Lua:, Event: or Graph:")
                                     : "'" + Kind + "' is not Native, Lua, Event or Graph";
                break;
            }
            if(!C.Take(':')) { Error = "expected ':' after " + Kind; break; }

            Action.Target = C.Word();
            if(!IsDottedName(Action.Target)) { Error = "'" + Action.Target + "' is not a name"; break; }
            if(Action.Kind == EUIActionKind::Graph)
            {
                /* A handler of the graph: one name, given the value and sender itself. */
                if(Action.Target.find('.') != std::string::npos) { Error = "a Graph: handler is one name ('" + Action.Target + "')"; break; }
                if(C.Take('(') && !C.Take(')')) { Error = "a Graph: handler takes no arguments (it is given $value and $self)"; break; }
            }

            if(C.Take('('))
            {
                if(!C.Take(')'))
                {
                    int Count = 0;
                    bool bOk = true;
                    do
                    {
                        if(Count >= 4) { Error = "more than four arguments"; bOk = false; break; }
                        if(!ParseArg(C, Action, Count, Error)) { bOk = false; break; }
                        ++Count;
                    }
                    while(C.Take(','));
                    if(!bOk) break;
                    if(!C.Take(')')) { Error = "expected ')'"; break; }
                    Action.Args.Count = uint8_t(Count);
                }
            }
            Parsed.push_back(std::move(Action));

            if(C.AtEnd()) break;
            if(!C.Take(';')) { Error = "expected ';' between actions"; break; }
        }

        if(!Error.empty())
        {
            if(OutError) { *OutError = Error; }
            return false;
        }
        Actions = std::move(Parsed);
        return true;
    }

    UIActionHandle RegisterUIAction(const char* InName, const UINativeAction InFn, const char* InTooltip)
    {
        if(!InName || !InFn || !IsDottedName(InName) || std::string(InName).find('.') == std::string::npos)
        {
            UI_LOG_ERROR(std::string("native action '") + (InName ? InName : "") + "': a name is Group.Name")
            return UI_ACTION_INVALID;
        }
        NativeRegistry& R = GetNatives();
        std::lock_guard<std::mutex> Guard(R.Lock);
        if(R.ByName.count(InName))
        {
            UI_LOG_ERROR(std::string("native action '") + InName + "' is already registered; ignoring the second")
            return UI_ACTION_INVALID;
        }
        NativeEntry& Made = R.ByName[InName];
        Made.Handle  = R.NextHandle++;
        Made.Fn      = InFn;
        Made.Tooltip = InTooltip ? InTooltip : "";
        return Made.Handle;
    }

    void UnregisterUIAction(const UIActionHandle InHandle)
    {
        if(InHandle == UI_ACTION_INVALID) return;
        NativeRegistry& R = GetNatives();
        std::lock_guard<std::mutex> Guard(R.Lock);
        for(auto It = R.ByName.begin(); It != R.ByName.end(); ++It)
        {
            if(It->second.Handle == InHandle)
            {
                R.ByName.erase(It);
                return;
            }
        }
    }

    UINativeAction FindUIAction(const char* InName)
    {
        if(!InName) return nullptr;
        NativeRegistry& R = GetNatives();
        std::lock_guard<std::mutex> Guard(R.Lock);
        const auto Found = R.ByName.find(InName);
        return Found != R.ByName.end() ? Found->second.Fn : nullptr;
    }

    std::vector<UIActionInfo> GetUIActions()
    {
        NativeRegistry& R = GetNatives();
        std::lock_guard<std::mutex> Guard(R.Lock);
        std::vector<UIActionInfo> Out;
        for(const auto& Each : R.ByName) { Out.push_back({ Each.first, Each.second.Tooltip }); }
        return Out;
    }
}
