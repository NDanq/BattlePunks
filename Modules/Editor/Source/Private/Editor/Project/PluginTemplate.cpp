#if WITH_EDITOR

#include "Private/Editor/Project/PluginTemplate.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/Project/ProjectTemplate.h"

/* nimble_create_plugin (Engine/cmake/NimblePluginTemplate.cmake) in C++: the
 * same steps in the same order, the same patterns. Keep them in step. */

namespace EditorPlugins
{
    namespace
    {
        const QString FILE_NAME_TOKEN = QStringLiteral("__PLUGIN__");

        /* Every file below InDir, relative, '/'-separated. */
        QStringList ListFiles(const QString& InDir)
        {
            QStringList Files;
            QDirIterator It(InDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while(It.hasNext()) { Files << QDir(InDir).relativeFilePath(It.next()); }
            return Files;
        }

        /* The @IF_<InOption>@ lines: kept less the marker, or dropped whole. */
        void ApplyOption(QString& InOutText, const QString& InOption, const bool bInOn)
        {
            const QString Marker = QStringLiteral("@IF_%1@").arg(InOption);
            if(bInOn)
            {
                InOutText.replace(QRegularExpression(QStringLiteral("(^|\n)([ \t]*)") + QRegularExpression::escape(Marker)),
                                  QStringLiteral("\\1\\2"));
                return;
            }
            const QRegularExpression Line(QStringLiteral("(^|\n)[ \t]*") + QRegularExpression::escape(Marker) + QStringLiteral("[^\n]*\n"));
            while(Line.match(InOutText).hasMatch())
            {
                InOutText.replace(Line, QStringLiteral("\\1"));
            }
        }
    }

    QString GetPluginTemplateDir()
    {
        const QString Engine = EditorProject::GetEngineDir();
        if(Engine.isEmpty()) { return QString(); }
        const QString Dir = QDir(Engine).filePath(QStringLiteral("Templates/Plugin"));
        return QFileInfo(Dir).isDir() ? QDir::fromNativeSeparators(QDir(Dir).absolutePath()) : QString();
    }

    QStringList GetStarters(const QString& InTemplateDir)
    {
        QStringList Starters;
        for(const QString& Each : QDir(InTemplateDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            if(Each != QStringLiteral("Common")) { Starters << Each; }
        }
        return Starters;
    }

    QString DescribeStarter(const QString& InStarter)
    {
        if(InStarter == QStringLiteral("Empty"))
        {
            return QStringLiteral("A runtime module and its IPlugin, starting and stopping. Nothing else: add what you need.");
        }
        if(InStarter == QStringLiteral("Component"))
        {
            return QStringLiteral("A component (saved, scripted, in Details), the system that steps it on every level, "
                                  "a script library and a config default -- like Spinner.");
        }
        if(InStarter == QStringLiteral("RenderPass"))
        {
            return QStringLiteral("A custom render pass on every level and its shader, cooked by NimbleCook -- like "
                                  "TimeOfDay's sky. Starts as a sepia tint over the finished picture.");
        }
        return QStringLiteral("A starter in Engine/Templates/Plugin.");
    }

    QString ReadEngineVersion(const QString& InEngineDir)
    {
        QFile File(QDir(InEngineDir).filePath(QStringLiteral("CMakeLists.txt")));
        if(!File.open(QIODevice::ReadOnly)) { return QString(); }
        static const QRegularExpression Project(QStringLiteral("(^|\n)project\\(NimbleEngine VERSION ([0-9.]+)"));
        const QRegularExpressionMatch Match = Project.match(QString::fromUtf8(File.readAll()));
        return Match.hasMatch() ? Match.captured(2) : QString();
    }

    QStringList GetReservedNames(const QString& InEngineDir)
    {
        QStringList Names = { QStringLiteral("Engine"), QStringLiteral("EngineCore"), QStringLiteral("NimbleEngine"),
                              QStringLiteral("NimbleEditor"), QStringLiteral("NimbleCook"), QStringLiteral("NimblePak"),
                              QStringLiteral("Games"), QStringLiteral("Templates"), QStringLiteral("Plugins") };
        if(!InEngineDir.isEmpty())
        {
            Names << QDir(QDir(InEngineDir).filePath(QStringLiteral("Modules"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        }
        return Names;
    }

    QString ValidateCategory(const QString& InCategory)
    {
        static const QRegularExpression Allowed(QStringLiteral("^[A-Za-z0-9 ]+$"));
        return InCategory.isEmpty() || Allowed.match(InCategory).hasMatch()
               ? QString() : QStringLiteral("Letters, digits and spaces only");
    }

    bool CreatePlugin(const QString& InTemplateDir, const QString& InDestDir, const NewPluginOptions& InOptions,
                      const QStringList& InReserved, QString& OutPluginDir, QString& OutError, QStringList* OutFiles)
    {
        static const QRegularExpression NameRule(QStringLiteral("^[A-Za-z][A-Za-z0-9]+$"));
        const QString& Name = InOptions.Name;
        if(!NameRule.match(Name).hasMatch())
        {
            OutError = QStringLiteral("'%1': a letter, then letters or digits only, two characters at least").arg(Name);
            return false;
        }
        for(const QString& Taken : InReserved)
        {
            if(Taken.compare(Name, Qt::CaseInsensitive) == 0)
            {
                OutError = QStringLiteral("'%1' is taken: a plugin, an engine module or a build target already has that name").arg(Name);
                return false;
            }
        }
        const QString Category = InOptions.Category.isEmpty() ? QStringLiteral("Gameplay") : InOptions.Category;
        if(!ValidateCategory(Category).isEmpty())
        {
            OutError = QStringLiteral("category '%1': letters, digits and spaces only").arg(Category);
            return false;
        }
        QString Description = InOptions.Description;
        Description.replace(QRegularExpression(QStringLiteral("[\r\n]+")), QStringLiteral(" "));
        Description = Description.trimmed();
        if(Description.isEmpty()) { Description = QStringLiteral("A NimbleEngine plugin"); }

        const QDir Template(InTemplateDir);
        const QString CommonDir  = Template.filePath(QStringLiteral("Common"));
        const QString StarterDir = Template.filePath(InOptions.Starter);
        if(InOptions.Starter.isEmpty() || InOptions.Starter == QStringLiteral("Common")
           || !QFileInfo(StarterDir).isDir() || !QFileInfo(CommonDir).isDir())
        {
            OutError = QStringLiteral("no starter '%1' in %2").arg(InOptions.Starter, InTemplateDir);
            return false;
        }
        OutPluginDir = QDir::fromNativeSeparators(QDir(InDestDir).absoluteFilePath(Name));
        if(QFileInfo::exists(OutPluginDir))
        {
            OutError = QStringLiteral("%1 already exists").arg(QDir::toNativeSeparators(OutPluginDir));
            return false;
        }

        /* Common's files, then the starter's over them. */
        const QStringList Own = ListFiles(StarterDir);
        QStringList All = ListFiles(CommonDir) + Own;
        All.removeDuplicates();
        All.sort();
        const QSet<QString> OwnSet(Own.begin(), Own.end());

        struct Pending
        {
            QString    Path;
            QByteArray Bytes;
        };
        QList<Pending> Writes;
        for(const QString& Rel : All)
        {
            const bool bFromCommon = !OwnSet.contains(Rel);
            if(!InOptions.bTest && Rel.startsWith(QStringLiteral("Tests/"))) { continue; }
            if(!InOptions.bEditor && Rel.startsWith(QStringLiteral("Source/__PLUGIN__Editor/"))) { continue; }
            if(!InOptions.bContent && bFromCommon && Rel.startsWith(QStringLiteral("Content/"))) { continue; }

            QFile Source(QDir(bFromCommon ? CommonDir : StarterDir).filePath(Rel));
            if(!Source.open(QIODevice::ReadOnly))
            {
                OutError = QStringLiteral("cannot read %1").arg(Source.fileName());
                return false;
            }
            QString Text = QString::fromUtf8(Source.readAll());
            Text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

            ApplyOption(Text, QStringLiteral("TEST"),    InOptions.bTest);
            ApplyOption(Text, QStringLiteral("EDITOR"),  InOptions.bEditor);
            ApplyOption(Text, QStringLiteral("CONTENT"), InOptions.bContent);

            Text.replace(QStringLiteral("@PLUGIN_NAME@"),        Name);
            Text.replace(QStringLiteral("@PLUGIN_UPPER@"),       Name.toUpper());
            Text.replace(QStringLiteral("@PLUGIN_CATEGORY@"),    Category);
            Text.replace(QStringLiteral("@PLUGIN_DESCRIPTION@"), Description);
            Text.replace(QStringLiteral("@PLUGIN_STARTER@"),     InOptions.Starter);
            Text.replace(QStringLiteral("@ENGINE_VERSION@"),     InOptions.EngineVersion);

            QString Out = Rel;
            Out.replace(FILE_NAME_TOKEN, Name);
            Writes.append({ Out, Text.toUtf8() });
        }

        /* All or nothing: a half-made plugin is removed. */
        const auto Undo = [&OutPluginDir]() { QDir(OutPluginDir).removeRecursively(); };
        QStringList Written;
        for(const Pending& Each : Writes)
        {
            const QString Path = QDir(OutPluginDir).filePath(Each.Path);
            QDir().mkpath(QFileInfo(Path).absolutePath());
            QFile File(Path);
            if(!File.open(QIODevice::WriteOnly | QIODevice::Truncate) || File.write(Each.Bytes) != Each.Bytes.size())
            {
                OutError = QStringLiteral("cannot write %1").arg(QDir::toNativeSeparators(Path));
                Undo();
                return false;
            }
            Written << Each.Path;
        }
        if(OutFiles) { *OutFiles = Written; }
        return true;
    }

    bool SetPluginEnabled(const QString& InConfigPath, const QString& InName, const bool bInEnabled, QString& OutError)
    {
        ConfigIniFile File;
        if(!File.Load(InConfigPath))
        {
            OutError = File.GetError();
            return false;
        }
        File.Set(QStringLiteral("Plugins"), InName, bInEnabled ? QStringLiteral("true") : QStringLiteral("false"));
        if(!File.Save(InConfigPath))
        {
            OutError = File.GetError();
            return false;
        }
        return true;
    }
}

#endif // WITH_EDITOR
