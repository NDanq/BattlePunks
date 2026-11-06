#ifndef NIMBLEEDITOR_EDITORCONSOLE_H
#define NIMBLEEDITOR_EDITORCONSOLE_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QString>
#include <QStringList>

/*
 * The Console panel's command interpreter. Lines split on spaces (quotes
 * group); the first word picks a registered command, else the line goes to
 * Core's ExecConfigCommand ("Render.VSync false").
 *
 * Replies go through Print/PrintError into EditorLog. The constructor
 * registers help, echo, clear, cvars and reset; EditorMainWindow adds scene
 * commands. GUI thread only.
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

    /* Echoes and runs InLine. False (reason printed) if nothing handled it. */
    bool Execute(const QString& InLine);

    void Print(const QString& InText) const;
    void PrintError(const QString& InText) const;

    /* Command names, then config var names. Rebuilt per call. */
    QStringList GetCompletions() const;

    const std::vector<Command>& GetCommands() const { return Commands; }

private:
    const Command* Find(const QString& InName) const;

    std::vector<Command> Commands;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCONSOLE_H
