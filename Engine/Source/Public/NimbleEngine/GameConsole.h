#ifndef NIMBLEENGINE_GAMECONSOLE_H
#define NIMBLEENGINE_GAMECONSOLE_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"

/*
 * The in-game console (CONSOLE.md): the toggle key ([Console] ToggleKey,
 * Grave -- the tilde key) drops it down over the game, in the launcher and in
 * Play-in-Editor alike. A line is a registered command ("help", "open
 * BakedLighting") or, failing that, a config var ("Shadows.Enabled 0"), as in
 * the editor's Console panel. It shows the engine's log as it arrives.
 *
 *   NIMBLE_CONSOLE_COMMAND   a command, at file scope in any module's .cpp
 *   GameConsole              one per GameSession (GameSession::GetConsole)
 *
 * Game thread only, apart from the log capture (any thread).
 */
namespace NimbleEngine
{
    class GameConsole;
    class GameSession;
    class InputState;

    /* What a command's function gets. */
    struct NE_API ConsoleCommandContext
    {
        GameConsole&                    Console;
        GameSession&                    Session;
        const std::vector<std::string>& Args;   // the words after the command's name; quotes group

        void Print(const std::string& InText) const;
        void PrintError(const std::string& InText) const;
    };

    using ConsoleCommandFn = void (*)(const ConsoleCommandContext& InContext);

    struct ConsoleCommandInfo
    {
        std::string      Name;    // what is typed; case-insensitive
        std::string      Usage;   // its arguments, "<level>"; empty if none
        std::string      Help;    // one line
        ConsoleCommandFn Fn = nullptr;
    };

    using ConsoleCommandHandle = uint32_t;
    constexpr ConsoleCommandHandle CONSOLE_COMMAND_INVALID = 0;

    /* Called by the macro. A name already registered is replaced until this
     * one is unregistered (a reloaded module's command takes over). */
    NE_API ConsoleCommandHandle RegisterConsoleCommand(const char* InName, const char* InUsage, const char* InHelp, ConsoleCommandFn InFn);
    NE_API void                 UnregisterConsoleCommand(ConsoleCommandHandle InHandle);

    /* False if none has InName. */
    NE_API bool FindConsoleCommand(const char* InName, ConsoleCommandInfo* OutInfo = nullptr);

    /* Every command, by name. */
    NE_API std::vector<ConsoleCommandInfo> GetConsoleCommands();

    /* Shell-style words: spaces split, "double quotes" group. */
    NE_API std::vector<std::string> SplitConsoleLine(const std::string& InLine);

    class NE_API GameConsole
    {
    public:
        explicit GameConsole(GameSession& InSession);
        ~GameConsole();   // closed, and the log no longer captured

        GameConsole(const GameConsole&)            = delete;
        GameConsole& operator=(const GameConsole&) = delete;

        /* Shown over everything, with the window's text input on; the game
         * keeps running under it. */
        void Open();
        void Close();
        void Toggle() { IsOpen() ? Close() : Open(); }
        bool IsOpen() const { return bOpen; }

        /*
         * The host's input, before the UI sees it (GameSession's Tick): the
         * toggle key opens and closes it, and Escape closes it. While open it
         * takes typed text and the editing keys -- Backspace, Delete, Left,
         * Right, Home, End, Up and Down through the history, PageUp and
         * PageDown through the output, Tab to complete, Return to run -- and
         * consumes everything, so neither the UI nor the game sees any of it.
         */
        void ProcessInput(InputState& InOutInput);

        /* InLine as if typed and entered: echoed, kept in the history, run.
         * False (the reason printed) if nothing handled it. */
        bool Execute(const std::string& InLine);

        void Print(const std::string& InText);        // and to the log
        void PrintError(const std::string& InText);
        void Clear();

        /* The log captured since the last call into the output, then what the
         * overlay shows. Once a frame, after the World, before the UI's Update. */
        void Update();

        /* Command names, then config var names, that start with InPrefix
         * (case-insensitive), sorted. */
        static std::vector<std::string> GetCompletions(const std::string& InPrefix);

        const std::vector<std::string>& GetOutput() const   { return Output; }
        const std::string&              GetInputLine() const { return Input; }

    private:
        void AddLine(const std::string& InLine);
        void Complete();
        void ShowHistory(int InIndex);
        void Refresh();

        GameSession& Session;
        bool         bOpen = false;
        bool         bDirty = true;   // the overlay is out of date

        /* (C4251 on these private members is harmless.) */
        std::vector<std::string> Output;     // oldest first, at most OUTPUT_MAX
        std::string              Input;      // the line being typed
        size_t                   Cursor = 0; // a byte offset into Input, on a character's first byte
        std::vector<std::string> History;    // oldest first
        int                      HistoryIndex = -1;   // into History while browsing; -1 the line being typed
        std::string              Draft;               // that line, kept while browsing
        int                      Scroll = 0;          // lines back from the newest
    };
}

/*
 * A console command, at file scope in one .cpp of any module; unregistered
 * with its module. The body's `Console` is the ConsoleCommandContext:
 *
 *     NIMBLE_CONSOLE_COMMAND(god, "[on|off]", "Toggle invulnerability")
 *     {
 *         Console.Print(Console.Args.empty() ? "god: toggled" : "god: " + Console.Args[0]);
 *     }
 */
#define NIMBLE_CONSOLE_COMMAND(CommandName, UsageText, HelpText)                                           \
    static void CommandName##_ConsoleCommandBody(const ::NimbleEngine::ConsoleCommandContext& Console);    \
    namespace                                                                                               \
    {                                                                                                       \
        struct CommandName##_ConsoleCommandRegistrar                                                        \
        {                                                                                                   \
            CommandName##_ConsoleCommandRegistrar()                                                         \
                : Handle(::NimbleEngine::RegisterConsoleCommand(#CommandName, UsageText, HelpText,          \
                                                                 &CommandName##_ConsoleCommandBody)) {}     \
            ~CommandName##_ConsoleCommandRegistrar() { ::NimbleEngine::UnregisterConsoleCommand(Handle); }  \
            ::NimbleEngine::ConsoleCommandHandle Handle;                                                    \
        };                                                                                                  \
        CommandName##_ConsoleCommandRegistrar G##CommandName##_ConsoleCommandRegistrar;                     \
    }                                                                                                       \
    static void CommandName##_ConsoleCommandBody(const ::NimbleEngine::ConsoleCommandContext& Console)

#endif //NIMBLEENGINE_GAMECONSOLE_H
