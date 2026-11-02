#if WITH_EDITOR

#include "Private/Editor/Project/ProjectTemplate.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace EditorProject
{
    namespace
    {
        const QString NAME_TOKEN       = QStringLiteral("@PROJECT_NAME@");
        const QString ENGINE_DIR_TOKEN = QStringLiteral("@ENGINE_DIR@");
        const QString FILE_NAME_TOKEN  = QStringLiteral("__PROJECT__");

        QString Clean(const QString& InPath)
        {
            return QDir::fromNativeSeparators(QDir(InPath).absolutePath());
        }
    }

    QString GetGameTemplateDir()
    {
        static const QString Dir = []()
        {
#ifdef NIMBLEEDITOR_TEMPLATES_DIR
            const QString Built = QDir(QString::fromUtf8(NIMBLEEDITOR_TEMPLATES_DIR)).filePath(QStringLiteral("Game"));
            if(QFileInfo(Built).isDir()) { return Clean(Built); }
#endif
            const QString Staged = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Engine/Templates/Game"));
            return QFileInfo(Staged).isDir() ? Clean(Staged) : QString();
        }();
        return Dir;
    }

    QString GetEngineDir()
    {
        const QString Template = GetGameTemplateDir();
        if(Template.isEmpty()) { return QString(); }
        QDir Engine(Template);   // Engine/Templates/Game
        Engine.cdUp();
        Engine.cdUp();
        return QFileInfo(Engine.filePath(QStringLiteral("CMakeLists.txt"))).isFile() ? Clean(Engine.path()) : QString();
    }

    QString GetDefaultGamesDir()
    {
        const QString Engine = GetEngineDir();
        if(Engine.isEmpty()) { return QString(); }
        return Clean(QDir(Engine).filePath(QStringLiteral("../Games")));
    }

    QString ValidateName(const QString& InName)
    {
        static const QRegularExpression Identifier(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
        if(InName.isEmpty())                    { return QStringLiteral("Enter a name"); }
        if(!Identifier.match(InName).hasMatch()) { return QStringLiteral("Letters, digits and _ only, starting with a letter"); }
        if(InName.size() > 48)                  { return QStringLiteral("At most 48 characters"); }
        for(const char* Reserved : { "Engine", "NimbleEngine", "NimbleEditor", "Games", "Templates" })
        {
            if(InName.compare(QLatin1String(Reserved), Qt::CaseInsensitive) == 0)
            {
                return QStringLiteral("\"%1\" is used by the engine").arg(InName);
            }
        }
        return QString();
    }

    QString EngineDirForCMake(const QString& InProjectDir, const QString& InEngineDir)
    {
        const QString Relative = QDir(InProjectDir).relativeFilePath(InEngineDir);
        if(Relative.isEmpty() || QDir::isAbsolutePath(Relative))
        {
            return Clean(InEngineDir);   // another drive
        }
        return QStringLiteral("${CMAKE_CURRENT_SOURCE_DIR}/") + QDir::fromNativeSeparators(Relative);
    }

    bool CreateProject(const QString& InTemplateDir, const QString& InEngineDir, const QString& InParentDir,
                       const QString& InName, QString& OutProjectDir, QString& OutError, QStringList* OutFiles)
    {
        OutProjectDir.clear();
        OutError = ValidateName(InName);
        if(!OutError.isEmpty()) { return false; }
        if(InTemplateDir.isEmpty() || !QFileInfo(InTemplateDir).isDir())
        {
            OutError = QStringLiteral("The project template is missing: %1").arg(InTemplateDir);
            return false;
        }
        if(InParentDir.isEmpty())
        {
            OutError = QStringLiteral("Choose a location");
            return false;
        }

        const QString ProjectDir = Clean(QDir(InParentDir).filePath(InName));
        if(QFileInfo::exists(ProjectDir))
        {
            OutError = QStringLiteral("%1 already exists").arg(QDir::toNativeSeparators(ProjectDir));
            return false;
        }
        if(!QDir().mkpath(ProjectDir))
        {
            OutError = QStringLiteral("Could not create %1").arg(QDir::toNativeSeparators(ProjectDir));
            return false;
        }

        const QString EngineDir = EngineDirForCMake(ProjectDir, InEngineDir);
        const QDir    Template(InTemplateDir);
        QStringList   Written;

        QDirIterator It(InTemplateDir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while(It.hasNext())
        {
            const QString Source   = It.next();
            const QString Relative = QString(Template.relativeFilePath(Source)).replace(FILE_NAME_TOKEN, InName);
            const QString Target   = QDir(ProjectDir).filePath(Relative);

            QFile In(Source);
            if(!In.open(QIODevice::ReadOnly))
            {
                OutError = QStringLiteral("Could not read %1").arg(QDir::toNativeSeparators(Source));
                break;
            }
            /* LF whatever the checkout has (core.autocrlf), as NewProject.cmake writes. */
            QString Text = QString::fromUtf8(In.readAll());
            Text.remove(QLatin1Char('\r'));
            Text.replace(NAME_TOKEN, InName);
            Text.replace(ENGINE_DIR_TOKEN, EngineDir);

            QDir().mkpath(QFileInfo(Target).absolutePath());
            QFile Out(Target);
            if(!Out.open(QIODevice::WriteOnly) || Out.write(Text.toUtf8()) < 0)
            {
                OutError = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(Target));
                break;
            }
            Written << Relative;
        }

        if(!OutError.isEmpty())
        {
            QDir(ProjectDir).removeRecursively();   // it did not exist before
            return false;
        }

        Written.sort(Qt::CaseInsensitive);
        if(OutFiles) { *OutFiles = Written; }
        OutProjectDir = ProjectDir;
        return true;
    }

    QString GetExamplesDir()
    {
        const QString Engine = GetEngineDir();
        if(Engine.isEmpty()) { return QString(); }
        const QString Dir = QDir(Engine).filePath(QStringLiteral("Examples"));
        return QFileInfo(QDir(Dir).filePath(QStringLiteral("Examples.json"))).isFile() ? Dir : QString();
    }

    QString GetNewProjectScript()
    {
        const QString Engine = GetEngineDir();
        if(Engine.isEmpty()) { return QString(); }
        const QString Script = QDir(Engine).filePath(QStringLiteral("Binaries/NewProject.cmake"));
        return QFileInfo(Script).isFile() ? Script : QString();
    }

    QList<ExampleInfo> ListExamples(const QString& InExamplesDir, QString* OutError)
    {
        QList<ExampleInfo> Examples;
        if(InExamplesDir.isEmpty()) { return Examples; }
        const QString Path = QDir(InExamplesDir).filePath(QStringLiteral("Examples.json"));
        QFile File(Path);
        if(!File.open(QIODevice::ReadOnly))
        {
            if(OutError) { *OutError = QStringLiteral("Could not read %1").arg(QDir::toNativeSeparators(Path)); }
            return Examples;
        }
        QJsonParseError Parse;
        const QJsonObject Index = QJsonDocument::fromJson(File.readAll(), &Parse).object();
        if(Parse.error != QJsonParseError::NoError || Index.value(QStringLiteral("Schema")).toInt() != 1)
        {
            if(OutError) { *OutError = QStringLiteral("%1 is not an example index this editor reads").arg(QDir::toNativeSeparators(Path)); }
            return Examples;
        }
        for(const QJsonValue& Value : Index.value(QStringLiteral("Examples")).toArray())
        {
            const QJsonObject Entry = Value.toObject();
            ExampleInfo Info;
            Info.Name          = Entry.value(QStringLiteral("Name")).toString();
            Info.Title         = Entry.value(QStringLiteral("Title")).toString(Info.Name);
            Info.Description   = Entry.value(QStringLiteral("Description")).toString();
            Info.EngineVersion = Entry.value(QStringLiteral("EngineVersion")).toString();
            Info.UnpackedBytes = qint64(Entry.value(QStringLiteral("UnpackedBytes")).toDouble());
            Info.Files         = Entry.value(QStringLiteral("Files")).toInt();
            for(const QJsonValue& Part : Entry.value(QStringLiteral("Parts")).toArray())
            {
                Info.PackedBytes += qint64(Part.toObject().value(QStringLiteral("Bytes")).toDouble());
            }
            if(ValidateName(Info.Name).isEmpty()) { Examples << Info; }
        }
        std::sort(Examples.begin(), Examples.end(), [](const ExampleInfo& A, const ExampleInfo& B) { return A.Name < B.Name; });
        return Examples;
    }

    QStringList GetExampleSteps(const bool bInBuild)
    {
        return bInBuild ? QStringList{ QStringLiteral("unpack"), QStringLiteral("configure"), QStringLiteral("build"), QStringLiteral("cook") }
                        : QStringList{ QStringLiteral("unpack") };
    }

    QStringList ExampleScriptArgs(const QString& InScript, const QString& InExamplesDir, const QString& InExample,
                                  const QString& InParentDir, const QString& InName, const QStringList& InSteps)
    {
        return { QStringLiteral("-DEXAMPLE=") + InExample,
                 QStringLiteral("-DNAME=") + InName,
                 QStringLiteral("-DLOCATION=") + QDir::fromNativeSeparators(InParentDir),
                 QStringLiteral("-DEXAMPLES_DIR=") + QDir::fromNativeSeparators(InExamplesDir),
                 QStringLiteral("-DSTEPS=") + InSteps.join(QLatin1Char(';')),
                 QStringLiteral("-P"), InScript };
    }

    bool ParseExampleStep(const QString& InLine, int& OutIndex, int& OutCount, QString& OutWhat)
    {
        static const QRegularExpression Step(QStringLiteral("NewProject: \\[(\\d+)/(\\d+)\\] (.*)$"));
        const QRegularExpressionMatch Match = Step.match(InLine);
        if(!Match.hasMatch()) { return false; }
        OutIndex = Match.captured(1).toInt();
        OutCount = Match.captured(2).toInt();
        OutWhat  = Match.captured(3).trimmed();
        return true;
    }
}

#endif // WITH_EDITOR
