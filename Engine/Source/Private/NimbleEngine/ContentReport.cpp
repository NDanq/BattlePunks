#include "Public/NimbleEngine/NimbleEngine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/WorldList.h"
#if WITH_NETCORE
#include "Public/NetCore/NetSession.h"
#endif

/*
 * Application::ExecReport: what Collaboration's content worker learns of a
 * commit from the game's own server build (Modules/Editor/COLLABORATION.md, P4).
 *
 * THE REPORT, one line each, tab-separated, UTF-8:
 *
 *     module  <name>              the game module that loaded ("" if none)
 *     schema  <16 hex digits>     NetCore's ComputeSchemaHash (0 without NetCore)
 *     checked <count>             worlds and prefabs loaded
 *     world   <name> <file> <mode> <1|0> <why>   each [World_*]: its game mode,
 *                                 whether players there see each other (a
 *                                 NetGameMode whose avatar prefabs have a Net
 *                                 component), and if not, why
 *     problem <path>  <message>   <path> relative to the project, '/' separated
 *
 * A problem is a warning or error logged while loading a file (an unknown
 * component type, an unreadable entity file, a missing instance, ...),
 * blamed on the file the message names when that is one of the world's own,
 * else on the world; a file that doesn't load at all; or an asset path a
 * world, entity or prefab names that doesn't exist.
 */
namespace NimbleEngine
{
namespace
{
    namespace fs = std::filesystem;

    struct Problem
    {
        std::string Path;
        std::string Message;
    };

    /* What the loads logged, while one is under way. Main thread only. */
    std::vector<std::string>* GCaptured = nullptr;
    EngineCore::LogHandler    GPrevious = nullptr;

    void CaptureHandler(const EngineCore::LogVerbosity InVerbosity, const char* InMessage)
    {
        if(GCaptured && (InVerbosity == EngineCore::LogVerbosity::Warning || InVerbosity == EngineCore::LogVerbosity::Error))
        {
            GCaptured->push_back(InMessage ? InMessage : "");
        }
        if(GPrevious)
        {
            GPrevious(InVerbosity, InMessage);
        }
        else
        {
            std::fprintf(stderr, "%s\n", InMessage ? InMessage : "");
        }
    }

    std::string Generic(const fs::path& InPath)
    {
        return InPath.generic_u8string();
    }

    std::string Relative(const fs::path& InPath, const fs::path& InRoot)
    {
        std::error_code Error;
        const fs::path Rel = fs::relative(InPath, InRoot, Error);
        return Error || Rel.empty() ? Generic(InPath) : Generic(Rel);
    }

    std::string ReadText(const fs::path& InPath)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(Generic(InPath), Bytes))
        {
            return std::string();
        }
        return std::string(Bytes.begin(), Bytes.end());
    }

    std::string OneLine(std::string InText)
    {
        std::replace(InText.begin(), InText.end(), '\t', ' ');
        std::replace(InText.begin(), InText.end(), '\n', ' ');
        std::replace(InText.begin(), InText.end(), '\r', ' ');
        return InText;
    }

    /* The quoted path in InMessage ('...'), if it is a file under InFolder. */
    std::string BlamedFile(const std::string& InMessage, const fs::path& InFolder, const fs::path& InRoot)
    {
        static const std::regex Quoted("'([^']+\\.nm[a-z]+)'");
        static const std::regex Guid("entity ([0-9a-f]{32})");
        std::smatch Match;
        fs::path File;
        if(std::regex_search(InMessage, Match, Quoted))
        {
            File = fs::u8path(Match[1].str());
        }
        else if(std::regex_search(InMessage, Match, Guid))
        {
            /* "entity <guid>: ...": its own file, beside the world. */
            File = InFolder / fs::u8path(Match[1].str() + ".nmentity");
        }
        else
        {
            return std::string();
        }
        if(File.is_relative())
        {
            File = InFolder / File;
        }
        std::error_code Error;
        if(!fs::exists(File, Error))
        {
            return std::string();
        }
        return Relative(File, InRoot);
    }

    /* Asset paths the file names ("Content/Props/Crate.nmmesh") that don't exist. */
    void CheckAssetPaths(const fs::path& InFile, const fs::path& InProject, const fs::path& InEngine, std::vector<Problem>& OutProblems)
    {
        static const std::regex Named("[\">]([A-Za-z0-9_ ./\\-]+\\.nm[a-z]+)[\"<]");
        const std::string Text = ReadText(InFile);
        for(auto It = std::sregex_iterator(Text.begin(), Text.end(), Named); It != std::sregex_iterator(); ++It)
        {
            const std::string Name = (*It)[1].str();
            if(Name.find("..") != std::string::npos)
            {
                continue;
            }
            fs::path Resolved;
            if(Name.rfind("Engine/", 0) == 0)
            {
                if(InEngine.empty())
                {
                    continue;
                }
                Resolved = InEngine.parent_path() / fs::u8path(Name);
            }
            else
            {
                Resolved = InProject / fs::u8path(Name);
            }
            std::error_code Error;
            if(!fs::exists(Resolved, Error))
            {
                OutProblems.push_back({ Relative(InFile, InProject), "names " + Name + ", which doesn't exist" });
            }
        }
    }

    /*
     * Whether players in InWorld see each other: its mode is a NetGameMode,
     * and every avatar prefab it can spawn -- the mode's own, else each Player
     * Start's, else [Avatar] DefaultPrefab -- has a Net component (else it
     * isn't replicated, and its client can't drive it). OutWhy says why not.
     */
    bool CheckWorldNetworked(const WorldEntry& InWorld, const fs::path& InProject, std::string& OutWhy)
    {
        if(!IsNetworkedGameMode(InWorld.GameMode.c_str()))
        {
            OutWhy = InWorld.GameMode.empty() ? std::string("it has no game mode")
                                              : "its mode, " + InWorld.GameMode + ", isn't a NetGameMode";
            return false;
        }

        std::vector<std::string> Prefabs;
        const std::string ModePrefab = GetGameModeAvatarPrefab(InWorld.GameMode.c_str());
        if(!ModePrefab.empty())
        {
            Prefabs.push_back(ModePrefab);
        }
        else
        {
            /* The level's Player Starts; one that names none takes the default. */
            bool bNeedsDefault = true;
            GameCore::World Scratch;
            if(GameCore::LoadWorldFile(Generic(InProject / fs::u8path(InWorld.File)).c_str(), Scratch))
            {
                Scratch.Each<PlayerStartComponent>([&](const GameCore::Entity&, PlayerStartComponent& InStart)
                {
                    if(!InStart.bEnabled)
                    {
                        return;
                    }
                    if(InStart.AvatarPrefab[0] != '\0')
                    {
                        Prefabs.push_back(InStart.AvatarPrefab);
                        bNeedsDefault = false;
                    }
                    else
                    {
                        bNeedsDefault = true;
                    }
                });
            }
            if(bNeedsDefault)
            {
                const EngineCore::ConfigVar* Default = EngineCore::ConfigFindVar("Avatar", "DefaultPrefab");
                if(Default && Default->GetString()[0] != '\0')
                {
                    Prefabs.push_back(Default->GetString());
                }
            }
        }
        std::sort(Prefabs.begin(), Prefabs.end());
        Prefabs.erase(std::unique(Prefabs.begin(), Prefabs.end()), Prefabs.end());
        if(Prefabs.empty())
        {
            OutWhy = "it names no avatar prefab (its Player Start, its mode, or [Avatar] DefaultPrefab)";
            return false;
        }

        for(const std::string& Prefab : Prefabs)
        {
            GameCore::World Scratch;
            const GameCore::Entity Root = Scratch.SpawnPrefab(Prefab.c_str()).GetRoot();
            if(!Root.IsValid() || !Root.IsAlive())
            {
                OutWhy = "its avatar prefab, " + Prefab + ", doesn't load";
                return false;
            }
            if(!Root.GetComponent<GameCore::NetComponent>())
            {
                OutWhy = "its avatar prefab, " + Prefab + ", has no Net component";
                return false;
            }
        }
        return true;
    }

    bool EndsWith(const std::string& InText, const char* InSuffix)
    {
        const size_t Length = std::char_traits<char>::length(InSuffix);
        return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
    }
}

int Application::ExecReport(const char* InDefaultModule, const ContentReportOptions& InOptions)
{
    using namespace EngineCore;

    Threads::SetMainThread();
    Memory::SetCollectorThread();

    GameModuleHost Game;
    GameModuleName = InDefaultModule ? InDefaultModule : "";

    std::vector<Problem> Problems;
    std::string ModuleName;
    std::string WorldLines;
    uint64_t    SchemaHash = 0;
    int         Checked = 0;
    bool        bWritten = false;

    if(Initialize() && Game.Load(GameModuleName))
    {
        ModuleName = Game.GetModuleName();
#if WITH_NETCORE
        SchemaHash = NetCore::ComputeSchemaHash();
#endif
        const fs::path Project = fs::u8path(InOptions.ProjectDir);
        const fs::path Engine  = InOptions.EngineDir.empty() ? fs::path() : fs::u8path(InOptions.EngineDir);

        /* Each listed world: whether players in it see each other (Play Together warns). */
        for(const WorldEntry& Each : ReadWorldList())
        {
            std::string Why;
            const bool bNetworked = CheckWorldNetworked(Each, Project, Why);
            WorldLines += "world\t" + OneLine(Each.Name) + "\t" + OneLine(Each.File) + "\t" + OneLine(Each.GameMode) + "\t"
                        + (bNetworked ? "1" : "0") + "\t" + OneLine(Why) + "\n";
        }

        /* Every world and prefab, in a stable order. */
        std::vector<fs::path> Files;
        std::error_code WalkError;
        for(fs::recursive_directory_iterator It(Project / "Content", WalkError), End; !WalkError && It != End; It.increment(WalkError))
        {
            if(!It->is_regular_file())
            {
                continue;
            }
            const std::string Name = Generic(It->path().filename());
            if(EndsWith(Name, ".nmworld") || EndsWith(Name, ".nmprefab"))
            {
                Files.push_back(It->path());
            }
        }
        std::sort(Files.begin(), Files.end());

        GPrevious = GetLogHandler();
        SetLogHandler(&CaptureHandler);
        for(const fs::path& File : Files)
        {
            std::vector<std::string> Messages;
            GCaptured = &Messages;
            const std::string Path = Generic(File);
            const bool bWorld = EndsWith(Path, ".nmworld");
            bool bLoaded = false;
            if(bWorld)
            {
                GameCore::World Scratch;
                bLoaded = GameCore::LoadWorldFile(Path.c_str(), Scratch);
            }
            else
            {
                GameCore::PrefabAsset Asset;
                bLoaded = GameCore::LoadPrefabFile(Path.c_str(), Asset);
            }
            GCaptured = nullptr;
            ++Checked;

            const std::string Own = Relative(File, Project);
            if(!bLoaded)
            {
                Problems.push_back({ Own, "doesn't load" });
            }
            /* A world's entity and instance files sit in a folder named after it. */
            const fs::path Folder = File.parent_path() / File.stem();
            /* Messages name files by absolute path; the report by the project's. */
            const std::string ProjectPrefix = Generic(Project) + "/";
            for(std::string Message : Messages)
            {
                const std::string Blamed = BlamedFile(Message, Folder, Project);
                for(size_t At = Message.find(ProjectPrefix); At != std::string::npos; At = Message.find(ProjectPrefix, At))
                {
                    Message.erase(At, ProjectPrefix.size());
                }
                Problems.push_back({ Blamed.empty() ? Own : Blamed, Message });
            }

            CheckAssetPaths(File, Project, Engine, Problems);
            if(bWorld)
            {
                std::error_code FolderError;
                for(fs::directory_iterator It(Folder, FolderError), End; !FolderError && It != End; It.increment(FolderError))
                {
                    const std::string Name = Generic(It->path().filename());
                    if(EndsWith(Name, ".nmentity") || EndsWith(Name, ".nmprefabinst"))
                    {
                        CheckAssetPaths(It->path(), Project, Engine, Problems);
                    }
                }
            }
        }
        SetLogHandler(GPrevious);
        GPrevious = nullptr;
    }
    else
    {
        CORE_LOG_ERROR("Content report: the game module didn't load")
    }

    /* Written whatever happened: an empty module line says it didn't load. */
    if(FileHandle Out = appFileOpenWrite(InOptions.ReportPath))
    {
        char Hex[17] = {};
        std::snprintf(Hex, sizeof(Hex), "%016llx", static_cast<unsigned long long>(SchemaHash));
        std::string Text = "module\t" + ModuleName + "\nschema\t" + Hex + "\nchecked\t" + std::to_string(Checked) + "\n";
        Text += WorldLines;
        for(const Problem& Each : Problems)
        {
            Text += "problem\t" + OneLine(Each.Path) + "\t" + OneLine(Each.Message) + "\n";
        }
        bWritten = appFileWrite(Out, Text.data(), Text.size()) == Text.size();
        appFileClose(Out);
    }
    if(!bWritten)
    {
        CORE_LOG_ERROR("Content report: could not write " + InOptions.ReportPath)
    }
    CORE_LOG("Content report: " + std::to_string(Checked) + " file(s), " + std::to_string(Problems.size()) + " problem(s)")

    Game.Unload();
    Shutdown();
    CloseRunLog();
    return bWritten && !ModuleName.empty() ? 0 : 1;
}
}
