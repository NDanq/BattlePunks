#if WITH_EDITOR

#include "Private/Editor/Build/ContentCook.h"

#include <string>

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/NimbleEngine/WorldList.h"

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/ProjectSettingsDialog.h"
#include "Private/Editor/UI/UIAssets.h"

namespace
{
    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }

    qint64 FileBytes(const QString& InPath)
    {
        const QFileInfo Info(InPath);
        return Info.isFile() ? Info.size() : 0;
    }

    /* Every file under InFolder, recursively. */
    qint64 FolderBytes(const QString& InFolder)
    {
        qint64 Bytes = 0;
        QDirIterator It(InFolder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while(It.hasNext())
        {
            Bytes += FileBytes(It.next());
        }
        return Bytes;
    }

    /* The project's own files matching InPattern: not the cooked tree, not a
     * build folder, not outside the project. Runtime path -> disk path. */
    QList<QPair<QString, QString>> ProjectFiles(const QString& InRoot, const QString& InPattern)
    {
        QList<QPair<QString, QString>> Out;
        const QString Cooked = QFileInfo(EditorCook::GetCookedRoot(InRoot)).absoluteFilePath() + QLatin1Char('/');
        QDirIterator It(InRoot, { InPattern }, QDir::Files, QDirIterator::Subdirectories);
        while(It.hasNext())
        {
            const QString File    = It.next();
            const QString Runtime = EditorContent::ToRuntimePath(File);
            if(Runtime.isEmpty() || Runtime.startsWith(QStringLiteral("Engine/"))
               || QFileInfo(File).absoluteFilePath().startsWith(Cooked))
            {
                continue;
            }
            bool bIgnored = false;
            for(const QString& Part : QDir(InRoot).relativeFilePath(File).split(QLatin1Char('/')))
            {
                bIgnored |= EditorGraphs::IsIgnoredFolder(Part);
            }
            if(!bIgnored)
            {
                Out.append({ Runtime, File });
            }
        }
        return Out;
    }

    bool IsTrue(const QString& InText)
    {
        const QString T = InText.trimmed().toLower();
        return !(T == QStringLiteral("false") || T == QStringLiteral("0") || T == QStringLiteral("no") || T == QStringLiteral("off"));
    }
}

namespace EditorCook
{
    const char* GetTypeName(const EAssetType InType)
    {
        switch(InType)
        {
        case AT_Graphs:  return "Node graphs";
        case AT_Worlds:  return "Worlds";
        case AT_Prefabs: return "Prefabs";
        case AT_UI:      return "UI documents";
        default:         return "?";
        }
    }

    int CookStats::GetFailedCount() const
    {
        int Failed = 0;
        for(const TypeStats& Type : Types)
        {
            Failed += Type.Failed;
        }
        return Failed;
    }

    QString GetCookedRoot(const QString& InProjectRoot)
    {
        return QDir(InProjectRoot).filePath(QStringLiteral("Cooked"));
    }

    QString GetCookedPath(const QString& InProjectRoot, const QString& InRuntimePath)
    {
        return QDir(GetCookedRoot(InProjectRoot)).filePath(InRuntimePath);
    }

    QString FormatBytes(const qint64 InBytes)
    {
        if(InBytes < 1024)               { return QStringLiteral("%1 B").arg(InBytes); }
        if(InBytes < 1024 * 1024)        { return QStringLiteral("%1 KB").arg(double(InBytes) / 1024.0, 0, 'f', 1); }
        return QStringLiteral("%1 MB").arg(double(InBytes) / (1024.0 * 1024.0), 0, 'f', 2);
    }

    CookStats CookProject(const QString& InProjectRoot, const std::function<void(const QString&)>& InProgress)
    {
        CookStats Stats;
        const QString Root = InProjectRoot.isEmpty() ? EditorGraphs::GetProjectRoot() : InProjectRoot;
        const auto Progress = [&InProgress](const QString& InText)
        {
            CORE_LOG("[Cook] " + ToStd(InText))
            if(InProgress) { InProgress(InText); }
        };

        QElapsedTimer Total;
        Total.start();
        QElapsedTimer Step;

        // --- Node graphs: their saved code is what the build cooks -------------
        Step.start();
        Progress(QStringLiteral("Regenerating node graphs"));
        {
            QStringList Lines;
            const int Failed = EditorGraphs::RegenerateAll(Lines);
            Stats.Types[AT_Graphs].Failed = Failed;
            Stats.Types[AT_Graphs].Cooked = int(Lines.size()) - Failed;
            for(const QString& Line : Lines)
            {
                CORE_LOG("[Cook] " + ToStd(Line))
            }
            if(Failed > 0)
            {
                Stats.Failures << QStringLiteral("%1 node graph(s) did not regenerate; their lines in the Console say why").arg(Failed);
            }
        }
        Stats.Types[AT_Graphs].Seconds = double(Step.elapsed()) / 1000.0;

        // --- Worlds: the list's, Cook=true -----------------------------------
        Step.start();
        {
            ConfigIniFile Config;
            const QString ConfigPath = ProjectSettingsDialog::GetProjectConfigPath();
            if(!Config.Load(ConfigPath))
            {
                Stats.Failures << QStringLiteral("%1: %2").arg(ConfigPath, Config.GetError());
                ++Stats.Types[AT_Worlds].Failed;
            }
            const QString Prefix = QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX);
            for(const ConfigIniFile::Section& Section : Config.GetSections(Prefix))
            {
                const QString Name = Section.Name.mid(Prefix.size());
                QString File;
                bool    bCook = true;
                for(const QPair<QString, QString>& Key : Section.Keys)
                {
                    if(Key.first.compare(QStringLiteral("File"), Qt::CaseInsensitive) == 0) { File = Key.second.trimmed(); }
                    if(Key.first.compare(QStringLiteral("Cook"), Qt::CaseInsensitive) == 0) { bCook = IsTrue(Key.second); }
                }
                if(File.isEmpty())
                {
                    ++Stats.Types[AT_Worlds].Failed;
                    Stats.Failures << QStringLiteral("[%1] has no File").arg(Section.Name);
                    continue;
                }

                const QString Out = GetCookedPath(Root, File);
                if(!bCook)
                {
                    Stats.Excluded << Name;
                    if(QFile::exists(Out) && QFile::remove(Out))
                    {
                        ++Stats.Types[AT_Worlds].Removed;
                        Progress(QStringLiteral("World %1 is not cooked; removed its old cooked copy").arg(Name));
                    }
                    continue;
                }

                Progress(QStringLiteral("Cooking world %1 (%2)").arg(Name, File));
                const QString Source = QDir(Root).filePath(File);
                if(!QFileInfo::exists(Source))
                {
                    ++Stats.Types[AT_Worlds].Failed;
                    Stats.Failures << QStringLiteral("%1: the world file does not exist").arg(File);
                    continue;
                }
                if(GameCore::CookWorldFile(ToStd(Source).c_str(), ToStd(Out).c_str()))
                {
                    ++Stats.Types[AT_Worlds].Cooked;
                    Stats.Worlds << Name;
                    /* A text world is its manifest and the folder of the same name beside it. */
                    const QFileInfo Info(Source);
                    Stats.Types[AT_Worlds].BytesIn  += FileBytes(Source) + FolderBytes(QDir(Info.absolutePath()).filePath(Info.completeBaseName()));
                    Stats.Types[AT_Worlds].BytesOut += FileBytes(Out);
                }
                else
                {
                    ++Stats.Types[AT_Worlds].Failed;
                    Stats.Failures << QStringLiteral("%1: could not cook (the Console's [GameCore] lines say why)").arg(File);
                }
            }
        }
        Stats.Types[AT_Worlds].Seconds = double(Step.elapsed()) / 1000.0;

        // --- Prefabs -----------------------------------------------------------
        Step.start();
        {
            const QList<QPair<QString, QString>> Prefabs = ProjectFiles(Root, QStringLiteral("*.nmprefab"));
            Progress(QStringLiteral("Cooking %1 prefab(s)").arg(Prefabs.size()));
            for(const QPair<QString, QString>& Prefab : Prefabs)
            {
                const QString Out = GetCookedPath(Root, Prefab.first);
                QDir().mkpath(QFileInfo(Out).absolutePath());   // CookPrefabFile does not
                if(GameCore::CookPrefabFile(ToStd(Prefab.second).c_str(), ToStd(Out).c_str()))
                {
                    ++Stats.Types[AT_Prefabs].Cooked;
                    Stats.Types[AT_Prefabs].BytesIn  += FileBytes(Prefab.second);
                    Stats.Types[AT_Prefabs].BytesOut += FileBytes(Out);
                }
                else
                {
                    ++Stats.Types[AT_Prefabs].Failed;
                    Stats.Failures << QStringLiteral("%1: could not cook (the Console's [GameCore] lines say why)").arg(Prefab.first);
                }
            }
        }
        Stats.Types[AT_Prefabs].Seconds = double(Step.elapsed()) / 1000.0;

        // --- UI documents --------------------------------------------------------
        Step.start();
        {
            Progress(QStringLiteral("Cooking UI documents"));
            int     Failed = 0;
            QString Errors;
            Stats.Types[AT_UI].Cooked = EditorUI::CookAll(Root, GetCookedRoot(Root), Failed, Errors);
            Stats.Types[AT_UI].Failed = Failed;
            for(const QString& Line : Errors.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            {
                Stats.Failures << Line;
            }
            for(const QPair<QString, QString>& Document : ProjectFiles(Root, QStringLiteral("*.nmui")))
            {
                Stats.Types[AT_UI].BytesIn  += FileBytes(Document.second);
                Stats.Types[AT_UI].BytesOut += FileBytes(GetCookedPath(Root, Document.first));
            }
        }
        Stats.Types[AT_UI].Seconds = double(Step.elapsed()) / 1000.0;

        Stats.Seconds = double(Total.elapsed()) / 1000.0;
        for(const QString& Line : DescribeStats(Stats))
        {
            if(Stats.Succeeded()) { CORE_LOG("[Cook] " + ToStd(Line)) }
            else                  { CORE_LOG_WARN("[Cook] " + ToStd(Line)) }
        }
        for(const QString& Failure : Stats.Failures)
        {
            CORE_LOG_ERROR("[Cook] " + ToStd(Failure))
        }
        return Stats;
    }

    QStringList DescribeStats(const CookStats& InStats)
    {
        QStringList Lines;
        Lines << QStringLiteral("%1 %2 %3 %4 %5 %6")
                 .arg(QStringLiteral("Type"), -14).arg(QStringLiteral("Cooked"), 7).arg(QStringLiteral("Failed"), 7)
                 .arg(QStringLiteral("In"), 10).arg(QStringLiteral("Out"), 10).arg(QStringLiteral("Time"), 8);
        int    Cooked = 0, Failed = 0;
        qint64 In = 0, Out = 0;
        for(int Type = 0; Type < AT_Count; ++Type)
        {
            const TypeStats& T = InStats.Types[Type];
            const bool bSized = Type != AT_Graphs;
            Lines << QStringLiteral("%1 %2 %3 %4 %5 %6")
                     .arg(QString::fromLatin1(GetTypeName(EAssetType(Type))), -14).arg(T.Cooked, 7).arg(T.Failed, 7)
                     .arg(bSized ? FormatBytes(T.BytesIn) : QStringLiteral("-"), 10)
                     .arg(bSized ? FormatBytes(T.BytesOut) : QStringLiteral("-"), 10)
                     .arg(QStringLiteral("%1 s").arg(T.Seconds, 0, 'f', 2), 8);
            Cooked += T.Cooked;
            Failed += T.Failed;
            In     += T.BytesIn;
            Out    += T.BytesOut;
        }
        Lines << QStringLiteral("%1 %2 %3 %4 %5 %6")
                 .arg(QStringLiteral("Total"), -14).arg(Cooked, 7).arg(Failed, 7)
                 .arg(FormatBytes(In), 10).arg(FormatBytes(Out), 10)
                 .arg(QStringLiteral("%1 s").arg(InStats.Seconds, 0, 'f', 2), 8);
        Lines << QStringLiteral("Worlds cooked: %1").arg(InStats.Worlds.isEmpty() ? QStringLiteral("none") : InStats.Worlds.join(QStringLiteral(", ")));
        if(!InStats.Excluded.isEmpty())
        {
            Lines << QStringLiteral("Worlds excluded: %1").arg(InStats.Excluded.join(QStringLiteral(", ")));
        }
        return Lines;
    }
}

#endif // WITH_EDITOR
