#if WITH_EDITOR

#include "Public/Editor/EditorConsole.h"

#include <algorithm>
#include <string>

#include <QProcess>

#include "Public/Core/CoreConfig.h"
#include "Public/Editor/EditorLog.h"

namespace
{
    const char* TypeName(const EngineCore::EConfigType InType)
    {
        switch(InType)
        {
            case EngineCore::EConfigType::CT_Bool:   return "bool";
            case EngineCore::EConfigType::CT_Int:    return "int";
            case EngineCore::EConfigType::CT_Float:  return "float";
            case EngineCore::EConfigType::CT_String: return "string";
        }
        return "?";
    }
}

EditorConsole::EditorConsole()
{
    Register(QStringLiteral("help"), QStringLiteral("[command]"),
             QStringLiteral("List the commands, or describe one"),
             [this](const QStringList& InArgs)
    {
        if(!InArgs.isEmpty())
        {
            const Command* Found = Find(InArgs.front());
            if(!Found)
            {
                PrintError(QStringLiteral("No command '%1'").arg(InArgs.front()));
                return;
            }
            Print(QStringLiteral("%1 %2\n  %3").arg(Found->Name, Found->Usage, Found->Help).trimmed());
            return;
        }

        std::vector<const Command*> Sorted;
        int Width = 0;
        for(const Command& Each : Commands)
        {
            Sorted.push_back(&Each);
            Width = std::max(Width, int((Each.Name + QLatin1Char(' ') + Each.Usage).size()));
        }
        std::sort(Sorted.begin(), Sorted.end(), [](const Command* L, const Command* R) { return L->Name < R->Name; });

        QStringList Lines;
        for(const Command* Each : Sorted)
        {
            const QString Signature = (Each->Name + QLatin1Char(' ') + Each->Usage).trimmed();
            Lines << QStringLiteral("  %1  %2").arg(Signature, -Width).arg(Each->Help);
        }
        Lines << QString()
              << QStringLiteral("Anything else is a config var: 'Section.Name' prints it, 'Section.Name value' sets it.");
        Print(Lines.join(QLatin1Char('\n')));
    });

    Register(QStringLiteral("echo"), QStringLiteral("<text>"), QStringLiteral("Print the text"),
             [this](const QStringList& InArgs) { Print(InArgs.join(QLatin1Char(' '))); });

    Register(QStringLiteral("clear"), QString(), QStringLiteral("Empty the console"),
             [](const QStringList&)
    {
        if(EditorLog* Log = EditorLog::Get())
        {
            Log->Clear();
        }
    });

    Register(QStringLiteral("cvars"), QStringLiteral("[filter]"),
             QStringLiteral("List config vars and their values; * marks one changed from its default"),
             [this](const QStringList& InArgs)
    {
        const QString Filter = InArgs.isEmpty() ? QString() : InArgs.front();

        QStringList Lines;
        for(EngineCore::ConfigVar* Var = EngineCore::ConfigGetFirst(); Var; Var = Var->GetNext())
        {
            const QString Name = QString::fromStdString(Var->GetQualifiedName());
            if(!Filter.isEmpty() && !Name.contains(Filter, Qt::CaseInsensitive))
            {
                continue;
            }
            Lines << QStringLiteral("%1 %2 = %3  (%4)")
                     .arg(Var->IsOverridden() ? QLatin1Char('*') : QLatin1Char(' '))
                     .arg(Name, QString::fromStdString(Var->ToString()), QString::fromLatin1(TypeName(Var->GetType())));
        }
        if(Lines.isEmpty())
        {
            Print(Filter.isEmpty() ? QStringLiteral("No config vars") : QStringLiteral("No config var matches '%1'").arg(Filter));
            return;
        }
        /* Registry order is reverse registration and not stable across builds
         * (CoreConfig.h), so sort for display. */
        Lines.sort(Qt::CaseInsensitive);
        Lines << QStringLiteral("%1 of %2").arg(Lines.size()).arg(EngineCore::ConfigGetVarCount());
        Print(Lines.join(QLatin1Char('\n')));
    });

    Register(QStringLiteral("reset"), QStringLiteral("<Section.Name>"),
             QStringLiteral("Put a config var back to its compiled default"),
             [this](const QStringList& InArgs)
    {
        if(InArgs.size() != 1)
        {
            PrintError(QStringLiteral("Usage: reset <Section.Name>"));
            return;
        }
        bool bAmbiguous = false;
        EngineCore::ConfigVar* Var = EngineCore::ConfigFindVarByPath(InArgs.front().toStdString(), &bAmbiguous);
        if(!Var || bAmbiguous)
        {
            PrintError(Var ? QStringLiteral("'%1' is ambiguous; give the section").arg(InArgs.front())
                           : QStringLiteral("Unknown config variable '%1'").arg(InArgs.front()));
            return;
        }
        Var->ResetToDefault();
        Print(QString::fromStdString(Var->GetQualifiedName() + " = " + Var->ToString()));
    });
}

void EditorConsole::Register(const QString& InName, const QString& InUsage, const QString& InHelp, CommandFn InFn)
{
    Command New{ InName, InUsage, InHelp, std::move(InFn) };
    for(Command& Existing : Commands)
    {
        if(Existing.Name.compare(InName, Qt::CaseInsensitive) == 0)
        {
            Existing = std::move(New);
            return;
        }
    }
    Commands.push_back(std::move(New));
}

const EditorConsole::Command* EditorConsole::Find(const QString& InName) const
{
    for(const Command& Each : Commands)
    {
        if(Each.Name.compare(InName, Qt::CaseInsensitive) == 0)
        {
            return &Each;
        }
    }
    return nullptr;
}

bool EditorConsole::Execute(const QString& InLine)
{
    const QString Line = InLine.trimmed();
    if(Line.isEmpty())
    {
        return false;
    }

    if(EditorLog* Log = EditorLog::Get())
    {
        Log->Append(EditorLog::EKind::Input, Line);
    }

    /* Shell-style words: "a b" is one argument, as ExecConfigCommand expects
     * of a string value with a space in it. */
    QStringList Words = QProcess::splitCommand(Line);
    if(Words.isEmpty())
    {
        return false;
    }

    if(const Command* Found = Find(Words.front()))
    {
        Words.removeFirst();
        Found->Fn(Words);
        return true;
    }

    std::string Response;
    const bool bConfig = EngineCore::ExecConfigCommand(Line.toStdString(), Response);
    if(bConfig)
    {
        Print(QString::fromStdString(Response));
        return true;
    }

    /* "Unknown config variable 'x'" alone reads as if only config vars were
     * possible; say that it is not a command either. */
    const bool bUnknown = Response.rfind("Unknown config variable", 0) == 0;
    PrintError(bUnknown ? QStringLiteral("Unknown command or config var '%1' -- 'help' lists commands, 'cvars' lists vars")
                          .arg(Words.front())
                        : QString::fromStdString(Response));
    return false;
}

void EditorConsole::Print(const QString& InText) const
{
    if(EditorLog* Log = EditorLog::Get())
    {
        Log->Append(EditorLog::EKind::Output, InText);
    }
}

void EditorConsole::PrintError(const QString& InText) const
{
    if(EditorLog* Log = EditorLog::Get())
    {
        Log->Append(EditorLog::EKind::OutputError, InText);
    }
}

QStringList EditorConsole::GetCompletions() const
{
    QStringList Names;
    for(const Command& Each : Commands)
    {
        Names << Each.Name;
    }
    Names.sort(Qt::CaseInsensitive);

    QStringList Vars;
    for(EngineCore::ConfigVar* Var = EngineCore::ConfigGetFirst(); Var; Var = Var->GetNext())
    {
        Vars << QString::fromStdString(Var->GetQualifiedName());
    }
    Vars.sort(Qt::CaseInsensitive);

    return Names + Vars;
}

#endif // WITH_EDITOR
