#include "Public/NimbleEngine/GameConsole.h"

#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

/*
 * The console's registry, its log capture, its line editing and its overlay:
 * a UI document built here rather than loaded, so every build has it with
 * nothing to cook. CONSOLE.md has the user's side.
 */

/* A shipping game turns it off; an .ini or the editor can turn it back on. */
NIMBLE_CONFIG_BOOL(GConsoleEnabled, "Console", "Enabled", true, nullptr);

/* A key name, as an input map binds (INPUT.md). Grave is the key under Escape: ` and ~. */
NIMBLE_CONFIG_STRING(GConsoleToggleKey, "Console", "ToggleKey", "Grave", nullptr);

namespace NimbleEngine
{
namespace
{
    constexpr size_t OUTPUT_MAX     = 512;   // lines kept
    constexpr size_t HISTORY_MAX    = 64;    // lines entered, kept
    constexpr int    VISIBLE_LINES  = 16;    // the overlay's output, above the prompt
    constexpr size_t LINE_CHARS_MAX = 240;   // longer lines are cut: the overlay does not wrap
    constexpr size_t COMPLETIONS_SHOWN = 32;

    const char* const DocumentPath = "Engine/Console";   // not a file: built by ConsoleDocument()
    const char* const LogKey       = "Console.Log";
    const char* const PromptKey    = "Console.Prompt";

    bool EqualsNoCase(const std::string& InA, const std::string& InB)
    {
        return InA.size() == InB.size()
            && std::equal(InA.begin(), InA.end(), InB.begin(), [](const char L, const char R)
               {
                   return std::tolower(static_cast<unsigned char>(L)) == std::tolower(static_cast<unsigned char>(R));
               });
    }

    bool StartsWithNoCase(const std::string& InText, const std::string& InPrefix)
    {
        return InText.size() >= InPrefix.size() && EqualsNoCase(InText.substr(0, InPrefix.size()), InPrefix);
    }

    std::string Trim(const std::string& InText)
    {
        const size_t First = InText.find_first_not_of(" \t\r\n");
        if(First == std::string::npos) return std::string();
        const size_t Last = InText.find_last_not_of(" \t\r\n");
        return InText.substr(First, Last - First + 1);
    }

    /* UTF-8: a continuation byte is 10xxxxxx. */
    bool IsContinuation(const char InByte) { return (static_cast<unsigned char>(InByte) & 0xC0u) == 0x80u; }

    // === The registry ===========================================================

    struct RegisteredCommand
    {
        ConsoleCommandHandle Handle = CONSOLE_COMMAND_INVALID;
        ConsoleCommandInfo   Info;
    };

    /* Function-local: registrars in other images run at their own load. */
    std::vector<RegisteredCommand>& Commands()
    {
        static std::vector<RegisteredCommand> Registry;
        return Registry;
    }

    /* The newest of a name wins, so a reloaded module's replaces the old. */
    const RegisteredCommand* FindRegistered(const std::string& InName)
    {
        const std::vector<RegisteredCommand>& All = Commands();
        for(auto Each = All.rbegin(); Each != All.rend(); ++Each)
        {
            if(EqualsNoCase(Each->Info.Name, InName)) return &*Each;
        }
        return nullptr;
    }

    // === The log ================================================================
    // Every message, from any thread, into a queue the game thread drains in
    // Update. The host's handler (the launcher's, the editor's) still gets it
    // first; the console chains in front of it while a console exists.

    std::mutex                GLogMutex;
    std::vector<std::string>  GLogPending;
    EngineCore::LogHandler    GPreviousHandler = nullptr;
    int                       GConsoles = 0;

    /* Set while the console prints its own lines: they go to the log but are
     * already in the output. */
    thread_local bool         GPrinting = false;

    void CaptureLog(const EngineCore::LogVerbosity InVerbosity, const char* InMessage)
    {
        if(!GPrinting)
        {
            const char* Tag = InVerbosity == EngineCore::LogVerbosity::Warning ? "Warning: "
                            : InVerbosity >= EngineCore::LogVerbosity::Error   ? "Error: "
                                                                                : "";
            std::lock_guard<std::mutex> Lock(GLogMutex);
            if(GLogPending.size() < OUTPUT_MAX) { GLogPending.push_back(std::string(Tag) + InMessage); }
        }

        if(GPreviousHandler)
        {
            GPreviousHandler(InVerbosity, InMessage);
            return;
        }

        /* What CoreLog does with no handler installed: stderr, and Fatal aborts. */
        std::fprintf(stderr, "%s\n", InMessage);
        std::fflush(stderr);
        if(InVerbosity == EngineCore::LogVerbosity::Fatal) { std::abort(); }
    }

    void BeginLogCapture()
    {
        if(GConsoles++ > 0) return;
        GPreviousHandler = EngineCore::GetLogHandler();
        EngineCore::SetLogHandler(&CaptureLog);
    }

    void EndLogCapture()
    {
        if(--GConsoles > 0) return;
        /* Only if still ours: a host that installed its own since keeps it. */
        if(EngineCore::GetLogHandler() == &CaptureLog) { EngineCore::SetLogHandler(GPreviousHandler); }
        GPreviousHandler = nullptr;
        std::lock_guard<std::mutex> Lock(GLogMutex);
        GLogPending.clear();
    }

    // === The overlay ============================================================

    UI::UIWidgetDesc Widget(const char* InType, const int32_t InParent, std::vector<UI::UIPropertyText> InProperties)
    {
        UI::UIWidgetDesc Desc;
        Desc.Type       = InType;
        Desc.Parent     = InParent;
        Desc.Properties = std::move(InProperties);
        return Desc;
    }

    /* A panel across the top: VISIBLE_LINES of output over the prompt. Over
     * every other document; takes no input of its own (ProcessInput does). */
    std::shared_ptr<const UI::UIDocumentAsset> ConsoleDocument()
    {
        static const std::shared_ptr<const UI::UIDocumentAsset> Asset = []
        {
            auto New = std::make_shared<UI::UIDocumentAsset>();
            New->Settings.InputMode = UI::EUIInputMode::GameOnly;
            New->Settings.Layer     = 100000;
            New->Widgets.push_back(Widget("Canvas", -1, { { "Name", "Root" } }));
            New->Widgets.push_back(Widget("Border", 0, { { "Name", "Panel" }, { "Anchors", "0,0,1,0" }, { "AutoSize", "true" },
                                                         { "Padding", "24,14,24,16" },
                                                         { "Background.Tint", "#05080DE6" },
                                                         { "Background.OutlineColor", "#D98A2F99" },
                                                         { "Background.OutlineThickness", "2" } }));
            New->Widgets.push_back(Widget("VerticalBox", 1, { { "Name", "Content" } }));
            New->Widgets.push_back(Widget("Text", 2, { { "Name", "Log" }, { "Text", std::string("{Bind:") + LogKey + "}" },
                                                       { "FontSize", "22" }, { "Color", "#D8DEE6FF" } }));
            New->Widgets.push_back(Widget("Text", 2, { { "Name", "Prompt" }, { "Text", std::string("{Bind:") + PromptKey + "}" },
                                                       { "FontSize", "24" }, { "Color", "#FFD9A0FF" }, { "Margin", "0,10,0,0" } }));
            return std::shared_ptr<const UI::UIDocumentAsset>(std::move(New));
        }();
        return Asset;
    }
}

// === The registry ===============================================================

ConsoleCommandHandle RegisterConsoleCommand(const char* InName, const char* InUsage, const char* InHelp, const ConsoleCommandFn InFn)
{
    static ConsoleCommandHandle NextHandle = 1;
    if(!InName || !*InName || !InFn)
    {
        CORE_LOG_ERROR("RegisterConsoleCommand: a command needs a name and a function")
        return CONSOLE_COMMAND_INVALID;
    }

    RegisteredCommand New;
    New.Handle     = NextHandle++;
    New.Info.Name  = InName;
    New.Info.Usage = InUsage ? InUsage : "";
    New.Info.Help  = InHelp ? InHelp : "";
    New.Info.Fn    = InFn;
    Commands().push_back(std::move(New));
    return Commands().back().Handle;
}

void UnregisterConsoleCommand(const ConsoleCommandHandle InHandle)
{
    std::vector<RegisteredCommand>& All = Commands();
    All.erase(std::remove_if(All.begin(), All.end(), [InHandle](const RegisteredCommand& Each) { return Each.Handle == InHandle; }),
              All.end());
}

bool FindConsoleCommand(const char* InName, ConsoleCommandInfo* OutInfo)
{
    const RegisteredCommand* Found = InName ? FindRegistered(InName) : nullptr;
    if(Found && OutInfo) { *OutInfo = Found->Info; }
    return Found != nullptr;
}

std::vector<ConsoleCommandInfo> GetConsoleCommands()
{
    std::vector<ConsoleCommandInfo> Out;
    for(const RegisteredCommand& Each : Commands())
    {
        if(FindRegistered(Each.Info.Name) == &Each) { Out.push_back(Each.Info); }   // a replaced one once
    }
    std::sort(Out.begin(), Out.end(), [](const ConsoleCommandInfo& L, const ConsoleCommandInfo& R) { return L.Name < R.Name; });
    return Out;
}

std::vector<std::string> SplitConsoleLine(const std::string& InLine)
{
    std::vector<std::string> Words;
    std::string Word;
    bool bInWord = false, bQuoted = false;
    for(const char Each : InLine)
    {
        if(Each == '"')
        {
            bQuoted = !bQuoted;
            bInWord = true;   // "" is an empty word
        }
        else if(!bQuoted && (Each == ' ' || Each == '\t'))
        {
            if(bInWord) { Words.push_back(Word); Word.clear(); bInWord = false; }
        }
        else
        {
            Word += Each;
            bInWord = true;
        }
    }
    if(bInWord) { Words.push_back(Word); }
    return Words;
}

void ConsoleCommandContext::Print(const std::string& InText) const      { Console.Print(InText); }
void ConsoleCommandContext::PrintError(const std::string& InText) const { Console.PrintError(InText); }

// === The console ================================================================

GameConsole::GameConsole(GameSession& InSession)
    : Session(InSession)
{
    BeginLogCapture();
}

GameConsole::~GameConsole()
{
    Close();
    EndLogCapture();
}

void GameConsole::Open()
{
    if(bOpen || !GConsoleEnabled.GetBool()) return;
    bOpen  = true;
    bDirty = true;
    Scroll = 0;
    Session.GetHost().SetTextInputActive(true);
    Refresh();   // the values before the document's first bind
    Session.GetUI().OpenAsset(ConsoleDocument(), DocumentPath, true);
}

void GameConsole::Close()
{
    if(!bOpen) return;
    bOpen = false;
    Session.GetHost().SetTextInputActive(false);
    Session.GetUI().Close(DocumentPath);
}

void GameConsole::ProcessInput(InputState& InOutInput)
{
    if(!GConsoleEnabled.GetBool())
    {
        Close();
        return;
    }

    const InputKey ToggleKey = FindInputKey(GConsoleToggleKey.GetString());
    if(InOutInput.WasPressed(ToggleKey, INPUT_PLAYER_DEVICES))
    {
        Toggle();
        InOutInput.Consume(ToggleKey);
        if(!bOpen)
        {
            InOutInput.ConsumeKeyboard();   // and the ` it typed
            return;
        }
    }
    if(!bOpen) return;

    static const InputKey Escape    = FindInputKey("Escape");
    static const InputKey Return    = FindInputKey("Return");
    static const InputKey Enter     = FindInputKey("KP_Enter");
    static const InputKey Backspace = FindInputKey("Backspace");
    static const InputKey Delete    = FindInputKey("Delete");
    static const InputKey Left      = FindInputKey("Left");
    static const InputKey Right     = FindInputKey("Right");
    static const InputKey Home      = FindInputKey("Home");
    static const InputKey End       = FindInputKey("End");
    static const InputKey Up        = FindInputKey("Up");
    static const InputKey Down      = FindInputKey("Down");
    static const InputKey PageUp    = FindInputKey("PageUp");
    static const InputKey PageDown  = FindInputKey("PageDown");
    static const InputKey Tab       = FindInputKey("Tab");

    const auto Hits = [&InOutInput](const InputKey InKey) { return InOutInput.GetPressesAndRepeats(InKey, INPUT_PLAYER_DEVICES); };

    if(InOutInput.WasPressed(Escape, INPUT_PLAYER_DEVICES))
    {
        Close();
        InOutInput.ConsumeAll();
        return;
    }

    /* Typed text at the cursor; not the toggle key's own character. */
    std::string Typed;
    for(const char* Each = InOutInput.GetText(); *Each; ++Each)
    {
        const unsigned char Byte = static_cast<unsigned char>(*Each);
        if(Byte < 0x20 || Byte == 0x7F || *Each == '`' || *Each == '~') continue;
        Typed += *Each;
    }
    if(!Typed.empty())
    {
        Input.insert(Cursor, Typed);
        Cursor += Typed.size();
        HistoryIndex = -1;
        bDirty = true;
    }

    for(int Each = Hits(Backspace); Each > 0 && Cursor > 0; --Each)
    {
        size_t Start = Cursor - 1;
        while(Start > 0 && IsContinuation(Input[Start])) --Start;
        Input.erase(Start, Cursor - Start);
        Cursor = Start;
        bDirty = true;
    }
    for(int Each = Hits(Delete); Each > 0 && Cursor < Input.size(); --Each)
    {
        size_t Stop = Cursor + 1;
        while(Stop < Input.size() && IsContinuation(Input[Stop])) ++Stop;
        Input.erase(Cursor, Stop - Cursor);
        bDirty = true;
    }
    for(int Each = Hits(Left); Each > 0 && Cursor > 0; --Each)
    {
        do { --Cursor; } while(Cursor > 0 && IsContinuation(Input[Cursor]));
        bDirty = true;
    }
    for(int Each = Hits(Right); Each > 0 && Cursor < Input.size(); --Each)
    {
        do { ++Cursor; } while(Cursor < Input.size() && IsContinuation(Input[Cursor]));
        bDirty = true;
    }
    if(Hits(Home)) { Cursor = 0;            bDirty = true; }
    if(Hits(End))  { Cursor = Input.size(); bDirty = true; }

    for(int Each = Hits(Up); Each > 0 && !History.empty(); --Each)
    {
        ShowHistory(HistoryIndex < 0 ? int(History.size()) - 1 : std::max(HistoryIndex - 1, 0));
    }
    for(int Each = Hits(Down); Each > 0 && HistoryIndex >= 0; --Each)
    {
        ShowHistory(HistoryIndex + 1 < int(History.size()) ? HistoryIndex + 1 : -1);
    }

    if(const int Pages = Hits(PageUp))
    {
        Scroll = std::min(Scroll + Pages * VISIBLE_LINES / 2, std::max(int(Output.size()) - VISIBLE_LINES, 0));
        bDirty = true;
    }
    if(const int Pages = Hits(PageDown))
    {
        Scroll = std::max(Scroll - Pages * VISIBLE_LINES / 2, 0);
        bDirty = true;
    }
    if(InOutInput.WasPressed(Tab, INPUT_PLAYER_DEVICES)) { Complete(); }

    if(InOutInput.WasPressed(Return, INPUT_PLAYER_DEVICES) || InOutInput.WasPressed(Enter, INPUT_PLAYER_DEVICES))
    {
        const std::string Line = Input;
        Input.clear();
        Cursor       = 0;
        HistoryIndex = -1;
        Scroll       = 0;
        bDirty       = true;
        Execute(Line);
    }

    /* The console has the input: none of it reaches the UI or the game. */
    InOutInput.ConsumeAll();
}

bool GameConsole::Execute(const std::string& InLine)
{
    const std::string Line = Trim(InLine);
    if(Line.empty()) return false;

    AddLine("> " + Line);
    if(History.empty() || History.back() != Line)
    {
        History.push_back(Line);
        if(History.size() > HISTORY_MAX) { History.erase(History.begin()); }
    }

    std::vector<std::string> Words = SplitConsoleLine(Line);
    if(Words.empty()) return false;

    if(const RegisteredCommand* Found = FindRegistered(Words.front()))
    {
        const ConsoleCommandFn Fn = Found->Info.Fn;   // the command may register or unregister others
        Words.erase(Words.begin());
        const ConsoleCommandContext Context{ *this, Session, Words };
        Fn(Context);
        return true;
    }

    std::string Response;
    if(EngineCore::ExecConfigCommand(Line, Response))
    {
        Print(Response);
        return true;
    }

    const bool bUnknown = Response.rfind("Unknown config variable", 0) == 0;
    PrintError(bUnknown ? "Unknown command or config var '" + Words.front() + "' -- 'help' lists commands, 'cvars' lists vars"
                        : Response);
    return false;
}

void GameConsole::Print(const std::string& InText)
{
    AddLine(InText);
    GPrinting = true;
    CORE_LOG("[Console] " + InText)
    GPrinting = false;
}

void GameConsole::PrintError(const std::string& InText)
{
    AddLine("Error: " + InText);
    GPrinting = true;
    CORE_LOG_WARN("[Console] " + InText)
    GPrinting = false;
}

void GameConsole::Clear()
{
    Output.clear();
    Scroll = 0;
    bDirty = true;
}

void GameConsole::AddLine(const std::string& InLine)
{
    /* One entry per line, so scrolling counts what is shown. */
    const size_t End = !InLine.empty() && InLine.back() == '\n' ? InLine.size() - 1 : InLine.size();
    size_t Start = 0;
    while(Start <= End)
    {
        size_t Stop = InLine.find('\n', Start);
        if(Stop == std::string::npos || Stop > End) Stop = End;
        std::string Piece = InLine.substr(Start, Stop - Start);
        if(Piece.size() > LINE_CHARS_MAX)
        {
            size_t Cut = LINE_CHARS_MAX;
            while(Cut > 0 && IsContinuation(Piece[Cut])) --Cut;
            Piece = Piece.substr(0, Cut) + "...";
        }
        Output.push_back(std::move(Piece));
        if(Scroll > 0) { ++Scroll; }   // scrolled back: stay on the same lines
        Start = Stop + 1;
    }
    if(Output.size() > OUTPUT_MAX) { Output.erase(Output.begin(), Output.begin() + ptrdiff_t(Output.size() - OUTPUT_MAX)); }
    Scroll = std::min(Scroll, std::max(int(Output.size()) - VISIBLE_LINES, 0));
    bDirty = true;
}

void GameConsole::ShowHistory(const int InIndex)
{
    if(HistoryIndex < 0) { Draft = Input; }
    HistoryIndex = InIndex;
    Input  = InIndex < 0 ? Draft : History[size_t(InIndex)];
    Cursor = Input.size();
    bDirty = true;
}

std::vector<std::string> GameConsole::GetCompletions(const std::string& InPrefix)
{
    std::vector<std::string> Out;
    for(const ConsoleCommandInfo& Each : GetConsoleCommands())
    {
        if(StartsWithNoCase(Each.Name, InPrefix)) Out.push_back(Each.Name);
    }
    std::vector<std::string> Vars;
    for(EngineCore::ConfigVar* Var = EngineCore::ConfigGetFirst(); Var; Var = Var->GetNext())
    {
        const std::string Name = Var->GetQualifiedName();
        if(StartsWithNoCase(Name, InPrefix)) Vars.push_back(Name);
    }
    std::sort(Vars.begin(), Vars.end());
    Out.insert(Out.end(), Vars.begin(), Vars.end());
    return Out;
}

void GameConsole::Complete()
{
    /* The first word only: a command's or a var's name. */
    if(Input.find(' ') != std::string::npos) return;

    const std::vector<std::string> Matches = GetCompletions(Input);
    if(Matches.empty()) return;
    if(Matches.size() == 1)
    {
        Input = Matches.front() + " ";
    }
    else
    {
        /* As far as they agree, and the choices. */
        std::string Common = Matches.front();
        for(const std::string& Each : Matches)
        {
            size_t Length = 0;
            while(Length < Common.size() && Length < Each.size()
                  && std::tolower(static_cast<unsigned char>(Common[Length])) == std::tolower(static_cast<unsigned char>(Each[Length])))
            {
                ++Length;
            }
            Common.resize(Length);
        }
        if(Common.size() > Input.size()) { Input = Common; }

        std::string Choices;
        for(size_t Each = 0; Each < Matches.size() && Each < COMPLETIONS_SHOWN; ++Each)
        {
            Choices += (Each ? "  " : "") + Matches[Each];
        }
        if(Matches.size() > COMPLETIONS_SHOWN) { Choices += "  (" + std::to_string(Matches.size() - COMPLETIONS_SHOWN) + " more)"; }
        AddLine(Choices);
    }
    Cursor = Input.size();
    bDirty = true;
}

void GameConsole::Update()
{
    std::vector<std::string> Arrived;
    {
        std::lock_guard<std::mutex> Lock(GLogMutex);
        Arrived.swap(GLogPending);
    }
    for(const std::string& Each : Arrived) { AddLine(Each); }

    if(bOpen && bDirty) { Refresh(); }
}

void GameConsole::Refresh()
{
    /* Always VISIBLE_LINES, blank at the top, so the panel keeps its height. */
    const int Last  = int(Output.size()) - Scroll;
    const int First = Last - VISIBLE_LINES;
    std::string Log;
    for(int Each = First; Each < Last; ++Each)
    {
        if(Each > First) Log += '\n';
        if(Each >= 0) Log += Output[size_t(Each)];
    }
    if(Scroll > 0) { Log += "\n-- " + std::to_string(Scroll) + " more below (PageDown) --"; }
    else           { Log += '\n'; }

    UI::UISystem& UI = Session.GetUI();
    UI.SetValue(LogKey, UI::UIValue::String(Log));
    UI.SetValue(PromptKey, UI::UIValue::String("> " + Input.substr(0, Cursor) + "|" + Input.substr(Cursor)));
    bDirty = false;
}

// === The engine's commands ======================================================
}

NIMBLE_CONSOLE_COMMAND(help, "[command]", "List the commands, or describe one")
{
    if(!Console.Args.empty())
    {
        NimbleEngine::ConsoleCommandInfo Found;
        if(!NimbleEngine::FindConsoleCommand(Console.Args.front().c_str(), &Found))
        {
            Console.PrintError("No command '" + Console.Args.front() + "'");
            return;
        }
        Console.Print(Found.Name + (Found.Usage.empty() ? "" : " " + Found.Usage) + "\n  " + Found.Help);
        return;
    }

    /* No columns: the overlay's font is proportional. */
    std::string Text;
    for(const auto& Each : NimbleEngine::GetConsoleCommands())
    {
        Text += "  " + (Each.Usage.empty() ? Each.Name : Each.Name + " " + Each.Usage) + "  -  " + Each.Help + "\n";
    }
    Text += "Anything else is a config var: 'Section.Name' prints it, 'Section.Name value' sets it. Tab completes.";
    Console.Print(Text);
}

NIMBLE_CONSOLE_COMMAND(echo, "<text>", "Print the text")
{
    std::string Text;
    for(const std::string& Each : Console.Args) { Text += (Text.empty() ? "" : " ") + Each; }
    Console.Print(Text);
}

NIMBLE_CONSOLE_COMMAND(clear, "", "Empty the console")
{
    Console.Console.Clear();
}

NIMBLE_CONSOLE_COMMAND(cvars, "[filter]", "List config vars and their values; * marks one changed from its default")
{
    const std::string Filter = Console.Args.empty() ? std::string() : Console.Args.front();
    const auto Lower = [](std::string InText)
    {
        for(char& Each : InText) { Each = char(std::tolower(static_cast<unsigned char>(Each))); }
        return InText;
    };

    std::vector<std::string> Lines;
    for(EngineCore::ConfigVar* Var = EngineCore::ConfigGetFirst(); Var; Var = Var->GetNext())
    {
        const std::string Name = Var->GetQualifiedName();
        if(!Filter.empty() && Lower(Name).find(Lower(Filter)) == std::string::npos) continue;
        Lines.push_back(std::string(Var->IsOverridden() ? "*" : " ") + " " + Name + " = " + Var->ToString());
    }
    if(Lines.empty())
    {
        Console.Print(Filter.empty() ? "No config vars" : "No config var matches '" + Filter + "'");
        return;
    }
    /* Registry order is reverse registration (CoreConfig.h): sort for display. */
    std::sort(Lines.begin(), Lines.end(), [&Lower](const std::string& L, const std::string& R) { return Lower(L.substr(2)) < Lower(R.substr(2)); });
    std::string Text;
    for(const std::string& Each : Lines) { Text += Each + "\n"; }
    Text += std::to_string(Lines.size()) + " of " + std::to_string(EngineCore::ConfigGetVarCount());
    Console.Print(Text);
}

NIMBLE_CONSOLE_COMMAND(reset, "<Section.Name>", "Put a config var back to its compiled default")
{
    if(Console.Args.size() != 1)
    {
        Console.PrintError("Usage: reset <Section.Name>");
        return;
    }
    bool bAmbiguous = false;
    EngineCore::ConfigVar* Var = EngineCore::ConfigFindVarByPath(Console.Args.front(), &bAmbiguous);
    if(!Var || bAmbiguous)
    {
        Console.PrintError(Var ? "'" + Console.Args.front() + "' is ambiguous; give the section"
                               : "Unknown config variable '" + Console.Args.front() + "'");
        return;
    }
    Var->ResetToDefault();
    Console.Print(Var->GetQualifiedName() + " = " + Var->ToString());
}

NIMBLE_CONSOLE_COMMAND(open, "<level>", "Load a level by its registered name, as [Game] StartLevel")
{
    if(Console.Args.size() != 1)
    {
        Console.PrintError("Usage: open <level>");
        return;
    }
    NimbleEngine::ILevel* Level = NimbleEngine::FindLevelByName(Console.Args.front().c_str());
    if(!Level)
    {
        Console.PrintError("No level named '" + Console.Args.front() + "'");
        return;
    }
    Console.Print("Opening " + Console.Args.front());
    Console.Session.RequestLevel(*Level);
}

/* As the editor's console spells it: "stat unit" toggles [Stats] Unit, "stat
 * frame" [Stats] Frame, "stat physics" [Stats] Physics, "stat audio" [Stats]
 * Audio, "stat streaming" [Stats] Streaming; "stat none" hides them all. */
NIMBLE_CONSOLE_COMMAND(stat, "<unit|frame|physics|audio|streaming|none>", "Show thread times, GPU times, the physics simulation, the level's sound or texture streaming over the game")
{
    static const char* const Names[] = { "unit", "frame", "physics", "audio", "streaming" };
    static const char* const Vars[]  = { "Unit", "Frame", "Physics", "Audio", "Streaming" };
    std::string Which = Console.Args.size() == 1 ? Console.Args.front() : std::string();
    for(char& Each : Which) { Each = char(std::tolower(static_cast<unsigned char>(Each))); }

    if(Which == "none")
    {
        for(const char* Var : Vars)
        {
            if(EngineCore::ConfigVar* Found = EngineCore::ConfigFindVar("Stats", Var)) { Found->SetBool(false); }
        }
        Console.Print("stats hidden");
        return;
    }
    for(size_t Index = 0; Index < std::size(Names); ++Index)
    {
        if(Which != Names[Index]) { continue; }
        EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Stats", Vars[Index]);
        if(!Var) { break; }
        Var->SetBool(!Var->GetBool());
        Console.Print("stat " + Which + ": " + (Var->GetBool() ? "on" : "off"));
        return;
    }
    Console.PrintError("Usage: stat unit | frame | physics | audio | streaming | none");
}

NIMBLE_CONSOLE_COMMAND(pause, "", "Pause or resume the World")
{
    Console.Session.SetPaused(!Console.Session.IsPaused());
    Console.Print(Console.Session.IsPaused() ? "Paused" : "Resumed");
}

NIMBLE_CONSOLE_COMMAND(quit, "", "End the game")
{
    Console.Session.RequestQuit();
}
