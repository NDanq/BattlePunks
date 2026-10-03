#if WITH_EDITOR

#include "Private/Editor/Project/ProjectTemplate.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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
            QString Text = QString::fromUtf8(In.readAll());
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
}

#endif // WITH_EDITOR
