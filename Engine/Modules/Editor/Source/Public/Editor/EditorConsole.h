#ifndef NIMBLEEDITOR_EDITORCONSOLE_H
#define NIMBLEEDITOR_EDITORCONSOLE_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QString>
#include <QStringList>

/*
 * The editor's console commands: what the Console panel runs a typed line
 * through.
 *
 * A line is split into words (double quotes group, as on a shell command
 * line), and the first word is looked up among the registered commands. A line
 * that names no command goes to Core's ExecConfigCommand, so every config var
 * in the process -- the editor's, the renderer's, any module's -- can be read
 * ("Render.VSync") or set ("Render.VSync false") with nothing registered here.
 * That order is the one CoreConfig.h asks a host console for: its own verbs
 * first, then the config grammar.
 *
 * Commands print through Print/PrintError, which append to EditorLog as the
 * command's reply, so a reply sits under the line that asked for it.
 *
 * The built-ins that need nothing but the console are registered by the
 * constructor (help, echo, clear, cvars, reset); EditorMainWindow adds the ones
 * that act on the scene. GUI thread only, like the config registry it reaches.
 */
class EditorConsole
{
public:
    using CommandFn = std::function<void(const QStringList& InArgs)>;

    struct Command
    {
        QString   Name;
        QString   Usage;   // the arguments, "<name> [value]"; empty if none
        QString   Help;    // one line
        CommandFn Fn;
    };

    EditorConsole();

    /* Replaces an existing command of the same name (case-insensitive). */
    void Register(const QString& InName, const QString& InUsage, const QString& InHelp, CommandFn InFn);

    /* Echoes InLine, then runs it. False if nothing ran it, or it failed to
     * parse as a config command; the reason has been printed either way. */
    bool Execute(const QString& InLine);

    void Print(const QString& InText) const;
    void PrintError(const QString& InText) const;

    /* Command names, then every config var's Section.Name -- what the input
     * line completes against. Rebuilt per call: a module loaded since the last
     * call may have registered new vars. */
    QStringList GetCompletions() const;

    const std::vector<Command>& GetCommands() const { return Commands; }

private:
    const Command* Find(const QString& InName) const;

    std::vector<Command> Commands;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCONSOLE_H
