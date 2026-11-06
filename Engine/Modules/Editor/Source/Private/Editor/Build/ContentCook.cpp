#if WITH_EDITOR

#include "Private/Editor/Build/ContentCook.h"

#include <algorithm>
#include <string>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/NimbleEngine/ShaderCook.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Sequencer/SequenceFile.h"
#if WITH_AICORE
#include "Public/AICore/BehaviorTreeFile.h"
#include "Public/AICore/NavMesh.h"
#endif

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Particles/ParticleAssets.h"
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

    /* The terrains (and their foliage) a text world's entities name -- a Terrain
     * component's Terrain and Foliage fields, in <World>/<guid>.nmentity --
     * whose file is not in the project. */
    QStringList FindMissingTerrains(const QString& InRoot, const QString& InWorldFile)
    {
        QStringList Missing;
        const QFileInfo Info(InWorldFile);
        const QString Folder = QDir(Info.absolutePath()).filePath(Info.completeBaseName());
        static const QRegularExpression TerrainField(
            QStringLiteral("<Component Type=\"Terrain\"[^>]*>\\s*<Terrain Value=\"([^\"]*)\""));
        QDirIterator It(Folder, { QStringLiteral("*.nmentity") }, QDir::Files);
        while(It.hasNext())
        {
            QFile Entity(It.next());
            if(!Entity.open(QIODevice::ReadOnly | QIODevice::Text)) { continue; }
            const QString Text = QString::fromUtf8(Entity.readAll());
            /* And its foliage, a field of the same component. */
            static const QRegularExpression FoliageField(QStringLiteral("<Foliage Value=\"([^\"]*)\""));
            for(const QRegularExpression* Field : { &TerrainField, &FoliageField })
            {
                if(Field == &FoliageField && !Text.contains(QStringLiteral("<Component Type=\"Terrain\""))) { continue; }
                QRegularExpressionMatchIterator Found = Field->globalMatch(Text);
                while(Found.hasNext())
                {
                    const QString Path = Found.next().captured(1);
                    if(!Path.isEmpty() && !QFileInfo::exists(QDir(InRoot).filePath(Path)) && !Missing.contains(Path)) { Missing << Path; }
                }
            }
        }
        return Missing;
    }

    /* The project's own files matching InPattern: not the cooked tree, not a
     * build folder or Saved/, not outside the project. Runtime path -> disk path. */
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
        case AT_Shaders: return "Shaders";
        case AT_Worlds:  return "Worlds";
        case AT_Prefabs: return "Prefabs";
        case AT_UI:      return "UI documents";
        case AT_Sequences: return "Sequences";
        case AT_NavMeshes: return "Navmeshes";
        case AT_BehaviorTrees: return "Behavior trees";
        default:         return "?";
        }
    }

    bool IsSized(const EAssetType InType)
    {
        return InType != AT_Graphs && InType != AT_Shaders;
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

    QString GetSavedRoot(const QString& InProjectRoot)
    {
        return QDir(InProjectRoot).filePath(QStringLiteral("Saved"));
    }

    QString GetCookedRoot(const QString& InProjectRoot)
    {
        return QDir(GetSavedRoot(InProjectRoot)).filePath(QStringLiteral("Cooked"));
    }

    QString GetLogsDir(const QString& InProjectRoot)
    {
        return QDir(GetSavedRoot(InProjectRoot)).filePath(QStringLiteral("Logs"));
    }

    QString NewLogPath(const QString& InProjectRoot, const QString& InKind, const int InKeep)
    {
        const QDir Logs(GetLogsDir(InProjectRoot));
        QDir().mkpath(Logs.absolutePath());

        /* The stamp sorts by name: oldest first. */
        QStringList Older = Logs.entryList({ InKind + QStringLiteral("-*.log") }, QDir::Files, QDir::Name);
        while(!Older.isEmpty() && Older.size() > std::max(InKeep - 1, 0))
        {
            QFile::remove(Logs.filePath(Older.takeFirst()));
        }
        return Logs.filePath(InKind + QLatin1Char('-') + QDateTime::currentDateTime().toString(QStringLiteral("yyyy.MM.dd-HH.mm.ss"))
                             + QStringLiteral(".log"));
    }

    QString GetCookerPath()
    {
        return QString::fromStdString(NimbleEngine::ShaderCook::FindCooker(QCoreApplication::applicationDirPath().toStdString()));
    }

    QString GetEngineDir()
    {
        const QString Content = EditorContent::GetEngineContentRoot();
        return Content.isEmpty() ? QString() : QFileInfo(Content).absolutePath();   // Engine/Content -> Engine
    }

    QStringList GetCookerArgs(const QString& InProjectRoot, const bool bShipping)
    {
        QStringList Args{ QStringLiteral("shaders"), QStringLiteral("-project"), InProjectRoot, QStringLiteral("-editor") };
        const QString Engine = GetEngineDir();
        if(!Engine.isEmpty())
        {
            Args << QStringLiteral("-engine") << Engine;
        }
        const EngineCore::ConfigVar* ModuleVar = EngineCore::ConfigFindVar("Game", "Module");
        if(ModuleVar && ModuleVar->GetString()[0] != '\0')
        {
            Args << QStringLiteral("-module") << QString::fromUtf8(ModuleVar->GetString());
        }
        if(bShipping)
        {
            Args << QStringLiteral("-shipping");
        }
        return Args;
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

    QString LogStamp()
    {
        return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz "));
    }

    QString LogHeader(const QString& InKind)
    {
        return QStringLiteral("%1 log, started %2").arg(InKind, QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }

    CookStats CookProject(const QString& InProjectRoot, const std::function<void(const QString&)>& InProgress, const bool bShipping)
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

        // --- Node graphs: their saved code is what the shader cook reads -------
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

        // --- Shaders: NimbleCook, the engine's and the project's ----------------
        Step.start();
        {
            const QString Cook = GetCookerPath();
            Progress(QStringLiteral("Cooking shaders (NimbleCook)"));
            if(Cook.isEmpty())
            {
                ++Stats.Types[AT_Shaders].Failed;
                Stats.Failures << QStringLiteral("no NimbleCook beside the editor: build the NimbleCook target");
            }
            else
            {
                QProcess Process;
                Process.setProcessChannelMode(QProcess::MergedChannels);
                Process.start(Cook, GetCookerArgs(Root, bShipping));
                const bool bRan = Process.waitForFinished(-1) && Process.exitStatus() == QProcess::NormalExit;

                /* "NimbleCook: 2 cooked, 1 up to date, 0 failed", and a
                 * "NimbleCook: FAILED <what>" line per failure. */
                static const QRegularExpression Summary(QStringLiteral("^NimbleCook: (\\d+) cooked, (\\d+) up to date, (\\d+) failed"));
                const QString Output = QString::fromLocal8Bit(Process.readAll());
                bool bSummarized = false;
                for(const QString& Line : Output.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts))
                {
                    CORE_LOG("[Cook] " + ToStd(Line))
                    const QRegularExpressionMatch Match = Summary.match(Line);
                    if(Match.hasMatch())
                    {
                        Stats.Types[AT_Shaders].Cooked = Match.captured(1).toInt() + Match.captured(2).toInt();
                        Stats.Types[AT_Shaders].Failed = Match.captured(3).toInt();
                        bSummarized = true;
                    }
                    else if(Line.startsWith(QStringLiteral("NimbleCook: FAILED ")))
                    {
                        Stats.Failures << QStringLiteral("Shaders: %1 did not cook; the Console's [Cook] lines say why")
                                              .arg(Line.mid(int(sizeof("NimbleCook: FAILED ")) - 1));
                    }
                }
                if(!bRan || !bSummarized || (Process.exitCode() != 0 && Stats.Types[AT_Shaders].Failed == 0))
                {
                    ++Stats.Types[AT_Shaders].Failed;
                    Stats.Failures << QStringLiteral("NimbleCook did not finish (%1); the Console's [Cook] lines say why")
                                          .arg(bRan ? QStringLiteral("exit code %1").arg(Process.exitCode()) : Process.errorString());
                }

                /* Beside the editor too, so it draws with them without a build. */
                const int Copied = NimbleEngine::ShaderCook::RestageCookedShaders(
                    QCoreApplication::applicationDirPath().toStdString(), GetEngineDir().toStdString(), Root.toStdString(), true);
                if(Copied > 0)
                {
                    CORE_LOG("[Cook] " + std::to_string(Copied) + " cooked shader file(s) copied beside the editor")
                }
            }
        }
        Stats.Types[AT_Shaders].Seconds = double(Step.elapsed()) / 1000.0;

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
                /* Its terrains' heightfields ship beside it, as they are: each must exist. */
                const QStringList MissingTerrains = FindMissingTerrains(Root, Source);
                if(!MissingTerrains.isEmpty())
                {
                    ++Stats.Types[AT_Worlds].Failed;
                    Stats.Failures << QStringLiteral("%1: its terrain or foliage file does not exist: %2").arg(File, MissingTerrains.join(QStringLiteral(", ")));
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

        // --- Sequences ---------------------------------------------------------
        Step.start();
        {
            const QList<QPair<QString, QString>> Sequences = ProjectFiles(Root, QStringLiteral("*.nmseq"));
            Progress(QStringLiteral("Cooking %1 sequence(s)").arg(Sequences.size()));
            for(const QPair<QString, QString>& Sequence : Sequences)
            {
                const QString Out = GetCookedPath(Root, Sequence.first);
                std::string   Error;

                /* What it plays must ship with it; anything else wrong is a warning. */
                Sequencer::SequenceAsset Asset;
                QFile SourceFile(Sequence.second);
                const QByteArray Bytes = SourceFile.open(QIODevice::ReadOnly) ? SourceFile.readAll() : QByteArray();
                if(Sequencer::ReadSequence(std::vector<uint8_t>(Bytes.begin(), Bytes.end()), Asset, &Error))
                {
                    QStringList Missing;
                    for(const std::string& Referenced : Asset.GetReferencedAssets())
                    {
                        if(!QFileInfo::exists(EditorParticles::ToProjectPath(QString::fromStdString(Referenced))))
                        {
                            Missing << QString::fromStdString(Referenced);
                        }
                    }
                    for(const std::string& Problem : Asset.Validate())
                    {
                        CORE_LOG_WARN("[Cook] " + ToStd(Sequence.first) + ": " + Problem)
                    }
                    if(!Missing.isEmpty())
                    {
                        ++Stats.Types[AT_Sequences].Failed;
                        Stats.Failures << QStringLiteral("%1: plays what does not exist: %2").arg(Sequence.first, Missing.join(QStringLiteral(", ")));
                        continue;
                    }
                }
                if(Sequencer::CookSequenceFile(ToStd(Sequence.second), ToStd(Out), &Error))
                {
                    ++Stats.Types[AT_Sequences].Cooked;
                    Stats.Types[AT_Sequences].BytesIn  += FileBytes(Sequence.second);
                    Stats.Types[AT_Sequences].BytesOut += FileBytes(Out);
                }
                else
                {
                    ++Stats.Types[AT_Sequences].Failed;
                    Stats.Failures << QStringLiteral("%1: %2").arg(Sequence.first, QString::fromStdString(Error));
                }
            }
        }
        Stats.Types[AT_Sequences].Seconds = double(Step.elapsed()) / 1000.0;

#if WITH_AICORE
        // --- Navmeshes ---------------------------------------------------------
        /* Already binary (NavMesh::Save): loaded to prove it reads, then copied. */
        Step.start();
        {
            const QList<QPair<QString, QString>> Meshes = ProjectFiles(Root, QStringLiteral("*.nmnav"));
            Progress(QStringLiteral("Cooking %1 navmesh(es)").arg(Meshes.size()));
            for(const QPair<QString, QString>& Mesh : Meshes)
            {
                const QString Out = GetCookedPath(Root, Mesh.first);
                QFile SourceFile(Mesh.second);
                const QByteArray Bytes = SourceFile.open(QIODevice::ReadOnly) ? SourceFile.readAll() : QByteArray();
                std::string Error;
                AICore::NavMesh Check;
                if(Bytes.isEmpty() || !Check.Load(std::vector<uint8_t>(Bytes.begin(), Bytes.end()), &Error))
                {
                    ++Stats.Types[AT_NavMeshes].Failed;
                    Stats.Failures << QStringLiteral("%1: %2").arg(Mesh.first, Error.empty() ? QStringLiteral("could not be read") : QString::fromStdString(Error));
                    continue;
                }
                QDir().mkpath(QFileInfo(Out).absolutePath());
                QFile::remove(Out);
                if(QFile::copy(Mesh.second, Out))
                {
                    ++Stats.Types[AT_NavMeshes].Cooked;
                    Stats.Types[AT_NavMeshes].BytesIn  += FileBytes(Mesh.second);
                    Stats.Types[AT_NavMeshes].BytesOut += FileBytes(Out);
                }
                else
                {
                    ++Stats.Types[AT_NavMeshes].Failed;
                    Stats.Failures << QStringLiteral("%1: could not be written").arg(Mesh.first);
                }
            }
        }
        Stats.Types[AT_NavMeshes].Seconds = double(Step.elapsed()) / 1000.0;

        // --- Behavior trees ----------------------------------------------------
        /* A tree naming a type no module registered fails here, not in the
         * shipped game: the game module is loaded while the editor cooks. */
        Step.start();
        {
            const QList<QPair<QString, QString>> Trees = ProjectFiles(Root, QStringLiteral("*.nmbt"));
            Progress(QStringLiteral("Cooking %1 behavior tree(s)").arg(Trees.size()));
            for(const QPair<QString, QString>& Tree : Trees)
            {
                const QString Out = GetCookedPath(Root, Tree.first);
                QDir().mkpath(QFileInfo(Out).absolutePath());
                std::string Error;
                if(AICore::CookBehaviorTreeFile(ToStd(Tree.second), ToStd(Out), &Error))
                {
                    ++Stats.Types[AT_BehaviorTrees].Cooked;
                    Stats.Types[AT_BehaviorTrees].BytesIn  += FileBytes(Tree.second);
                    Stats.Types[AT_BehaviorTrees].BytesOut += FileBytes(Out);
                }
                else
                {
                    ++Stats.Types[AT_BehaviorTrees].Failed;
                    Stats.Failures << QStringLiteral("%1: %2").arg(Tree.first, QString::fromStdString(Error));
                }
            }
        }
        Stats.Types[AT_BehaviorTrees].Seconds = double(Step.elapsed()) / 1000.0;
#endif

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
            const bool bSized = IsSized(EAssetType(Type));
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
