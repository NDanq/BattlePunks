#include "Public/NimbleEngine/Plugin.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <set>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePack.h"
#include "Public/Core/CorePlatform.h"

/* The plugin host: Plugin.h says what it does, NimblePlugins.cmake what the
 * build leaves for it to read. */

namespace NimbleEngine
{
    /* The host's way into a PluginContext, which plugins see without it. */
    struct PluginHostAccess
    {
        static void Set(PluginContext& InContext, const std::string& InName, const std::string& InVersion,
                        std::shared_ptr<EngineCore::IFileProvider> InFiles)
        {
            InContext.PluginName = InName;
            InContext.Version    = InVersion;
            InContext.Files      = std::move(InFiles);
        }
    };

    namespace
    {
        struct LoadedLibrary
        {
            std::string           File;
            EngineCore::DLLHandle Handle  = nullptr;
            IPlugin*              Plugin  = nullptr;
            PluginDestroyFn       Destroy = nullptr;
            bool                  bStarted = false;
        };

        struct PluginRecord
        {
            std::string                                Name;
            std::string                                Version;
            std::string                                SourceDir;
            std::vector<std::string>                   ModuleFiles;
            std::shared_ptr<EngineCore::IFileProvider> Files;     // the plugin folder: descriptor, Config/, Content/
            std::unique_ptr<PluginContext>             Context;
            std::vector<LoadedLibrary>                 Libraries;
        };

        struct WorldHookRecord
        {
            std::string     Plugin;
            PluginWorldHook Hook;
        };

        struct PluginHostState
        {
            std::string                  RuntimeDir;
            std::vector<PluginRecord>    Records;   // load order
            std::vector<WorldHookRecord> Hooks;     // registration order
            std::vector<std::string>     MountedLayers;
        };

        PluginHostState& Host()
        {
            static PluginHostState State;
            return State;
        }

        std::string Trim(const std::string& InText)
        {
            const size_t First = InText.find_first_not_of(" \t\r\n");
            if(First == std::string::npos) { return std::string(); }
            const size_t Last = InText.find_last_not_of(" \t\r\n");
            return InText.substr(First, Last - First + 1);
        }

        std::string JoinPath(const std::string& InDir, const std::string& InName)
        {
            if(InDir.empty()) { return InName; }
            const char Last = InDir.back();
            return (Last == '/' || Last == '\\') ? InDir + InName : InDir + "/" + InName;
        }

        bool IsDirectory(const std::string& InPath)
        {
            std::error_code Error;
            return !InPath.empty() && std::filesystem::is_directory(std::filesystem::u8path(InPath), Error);
        }

        /* The manifest's [Plugin_<Name>] sections, in file order. */
        std::vector<PluginRecord> ParseManifest(const std::string& InText)
        {
            std::vector<PluginRecord> Result;
            size_t Begin = 0;
            while(Begin <= InText.size())
            {
                size_t End = InText.find('\n', Begin);
                if(End == std::string::npos) { End = InText.size(); }
                const std::string Line = Trim(InText.substr(Begin, End - Begin));
                Begin = End + 1;

                if(Line.empty() || Line[0] == ';' || Line[0] == '#') { continue; }
                if(Line.front() == '[' && Line.back() == ']')
                {
                    const std::string Section = Line.substr(1, Line.size() - 2);
                    static const std::string Prefix = "Plugin_";
                    if(Section.compare(0, Prefix.size(), Prefix) == 0 && Section.size() > Prefix.size())
                    {
                        Result.emplace_back();
                        Result.back().Name = Section.substr(Prefix.size());
                    }
                    continue;
                }
                const size_t Eq = Line.find('=');
                if(Eq == std::string::npos || Result.empty()) { continue; }
                const std::string Key   = Trim(Line.substr(0, Eq));
                const std::string Value = Trim(Line.substr(Eq + 1));
                PluginRecord& Record = Result.back();
                if(Key == "Version")     { Record.Version = Value; }
                else if(Key == "Source") { Record.SourceDir = Value; }
                else if(Key == "Modules")
                {
                    size_t From = 0;
                    while(From <= Value.size())
                    {
                        size_t Comma = Value.find(',', From);
                        if(Comma == std::string::npos) { Comma = Value.size(); }
                        const std::string File = Trim(Value.substr(From, Comma - From));
                        if(!File.empty()) { Record.ModuleFiles.push_back(File); }
                        From = Comma + 1;
                    }
                }
            }
            return Result;
        }

        /* Where a plugin's files are read from: its source folder (editor,
         * -uncooked), its pack (a packaged build) or what the build staged. */
        std::shared_ptr<EngineCore::IFileProvider> OpenPluginFiles(const PluginRecord& InRecord, const std::string& InRuntimeDir,
                                                                   const bool bInUseSource, std::string& OutFrom)
        {
            if(bInUseSource && IsDirectory(InRecord.SourceDir))
            {
                OutFrom = InRecord.SourceDir;
                return std::make_shared<EngineCore::FilesystemProvider>(InRecord.SourceDir);
            }

            const std::string PackPath = JoinPath(InRuntimeDir, "Paks/Plugins/" + InRecord.Name + ".nmpak");
            if(EngineCore::appFileExists(PackPath))
            {
                std::vector<uint8_t> Bytes;
                std::string Why;
                std::shared_ptr<EngineCore::PackFileProvider> Pack =
                    EngineCore::appFileReadAll(PackPath, Bytes) ? EngineCore::PackFileProvider::Load(std::move(Bytes), Why) : nullptr;
                if(Pack)
                {
                    OutFrom = PackPath;
                    return Pack;
                }
                CORE_LOG_ERROR("[Plugins] " + InRecord.Name + ": " + PackPath + " is not a readable pack"
                               + (Why.empty() ? std::string() : " (" + Why + ")"))
            }

            OutFrom = JoinPath(InRuntimeDir, "Plugins/" + InRecord.Name);
            return std::make_shared<EngineCore::FilesystemProvider>(OutFrom);
        }

        /*
         * Under every plugin's layer: answers nothing, but a "<Name>:/" path
         * that reaches it with no enabled plugin of that name is a project
         * referring to a plugin it does not have -- an error, once per name.
         * A known name reaching it is just a missing file, which the asker
         * reports as it would any other.
         */
        class StrictPrefixGuard : public EngineCore::IFileProvider
        {
        public:
            explicit StrictPrefixGuard(std::set<std::string> InKnown) : Known(std::move(InKnown)) {}

            bool ReadAll(const std::string& InPath, std::vector<uint8_t>&) override { Check(InPath); return false; }
            bool Exists (const std::string& InPath) override                       { Check(InPath); return false; }
            bool ReadRange(const std::string& InPath, uint64_t, uint64_t, std::vector<uint8_t>&) override
            {
                Check(InPath);
                return false;
            }

        private:
            void Check(const std::string& InPath)
            {
                const std::string Name = GetPluginPrefix(InPath);
                if(Name.empty() || Known.count(Name) != 0) { return; }
                std::lock_guard<std::mutex> Lock(Mutex);   // the render thread reads assets too
                if(Reported.insert(Name).second)
                {
                    CORE_LOG_ERROR("[Plugins] '" + InPath + "' names plugin " + Name + ", which is not enabled: add "
                                   + Name + "=true to [Plugins] in Config/DefaultEngine.ini (Project Settings > Plugins)")
                }
            }

            const std::set<std::string> Known;
            std::mutex                  Mutex;
            std::set<std::string>       Reported;
        };

        constexpr const char* GUARD_LAYER = "Plugin:*";

        void StartLibrary(PluginRecord& InRecord, LoadedLibrary& InLibrary, bool& bInOutOk)
        {
            const std::filesystem::path Path = std::filesystem::absolute(
                std::filesystem::u8path(JoinPath(Host().RuntimeDir, InLibrary.File)));
            const auto PathU8 = Path.u8string();
            const std::string PathText(PathU8.begin(), PathU8.end());

            InLibrary.Handle = EngineCore::appDLLOpen(PathText);
            if(!InLibrary.Handle)
            {
                CORE_LOG_ERROR("[Plugins] " + InRecord.Name + ": could not open " + PathText + ": " + EngineCore::appDLLGetLastError())
                bInOutOk = false;
                return;
            }

            /* The version before anything else: a stale library's IPlugin is
             * exactly what cannot be trusted (CoreModule.h's load gate). */
            const auto GetVersion = reinterpret_cast<PluginGetABIVersionFn>(
                EngineCore::appDLLGetSymbol(InLibrary.Handle, PLUGIN_SYMBOL_ABI_VERSION));
            const auto Create = reinterpret_cast<PluginCreateFn>(
                EngineCore::appDLLGetSymbol(InLibrary.Handle, PLUGIN_SYMBOL_CREATE));
            InLibrary.Destroy = reinterpret_cast<PluginDestroyFn>(
                EngineCore::appDLLGetSymbol(InLibrary.Handle, PLUGIN_SYMBOL_DESTROY));
            if(!GetVersion || !Create || !InLibrary.Destroy)
            {
                CORE_LOG_ERROR("[Plugins] " + InRecord.Name + ": " + InLibrary.File
                               + " has no NIMBLE_IMPLEMENT_PLUGIN; its components and scripts are registered, but it is not started")
                InLibrary.Destroy = nullptr;
                return;   // kept open: what it registered as it loaded points into it
            }
            const uint32_t Version = GetVersion();
            if(Version != NIMBLE_PLUGIN_ABI_VERSION)
            {
                CORE_LOG_ERROR("[Plugins] " + InRecord.Name + ": " + InLibrary.File + " was built for plugin ABI "
                               + std::to_string(Version) + ", this build is " + std::to_string(NIMBLE_PLUGIN_ABI_VERSION)
                               + ": rebuild it with this project")
                InLibrary.Destroy = nullptr;
                bInOutOk = false;
                return;
            }

            InLibrary.Plugin = Create();
            if(!InLibrary.Plugin)
            {
                bInOutOk = false;
                return;
            }
            if(!InLibrary.Plugin->OnStartup(*InRecord.Context))
            {
                CORE_LOG_ERROR("[Plugins] " + InRecord.Name + ": " + InLibrary.File + " refused to start")
                InLibrary.Destroy(InLibrary.Plugin);
                InLibrary.Plugin = nullptr;
                bInOutOk = false;
                return;
            }
            InLibrary.bStarted = true;
        }

        void RemoveHooksOf(const std::string& InPlugin)
        {
            std::vector<WorldHookRecord>& Hooks = Host().Hooks;
            Hooks.erase(std::remove_if(Hooks.begin(), Hooks.end(),
                                       [&InPlugin](const WorldHookRecord& InHook) { return InHook.Plugin == InPlugin; }),
                        Hooks.end());
        }
    }

    // === PluginContext =======================================================

    void PluginContext::AddWorldHook(const PluginWorldHook& InHook)
    {
        if(!InHook.Attach && !InHook.Detach)
        {
            return;
        }
        Host().Hooks.push_back({ PluginName, InHook });
    }

    bool PluginContext::ReadPluginFile(const std::string& InPath, std::vector<uint8_t>& OutBytes) const
    {
        return Files && Files->ReadAll(InPath, OutBytes);
    }

    // === PrefixFileProvider ==================================================

    PrefixFileProvider::PrefixFileProvider(std::string InName, std::shared_ptr<EngineCore::IFileProvider> InInner)
        : Prefix(std::move(InName) + ":/"), Inner(std::move(InInner))
    {
    }

    bool PrefixFileProvider::Resolve(const std::string& InPath, std::string& OutInner) const
    {
        if(InPath.size() <= Prefix.size() || InPath.compare(0, Prefix.size(), Prefix) != 0)
        {
            return false;
        }
        OutInner = "Content/" + InPath.substr(Prefix.size());
        return true;
    }

    bool PrefixFileProvider::ReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
    {
        std::string InnerPath;
        return Inner && Resolve(InPath, InnerPath) && Inner->ReadAll(InnerPath, OutBytes);
    }

    bool PrefixFileProvider::Exists(const std::string& InPath)
    {
        std::string InnerPath;
        return Inner && Resolve(InPath, InnerPath) && Inner->Exists(InnerPath);
    }

    bool PrefixFileProvider::ReadRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize,
                                       std::vector<uint8_t>& OutBytes)
    {
        std::string InnerPath;
        return Inner && Resolve(InPath, InnerPath) && Inner->ReadRange(InnerPath, InOffset, InSize, OutBytes);
    }

    std::string GetPluginPrefix(const std::string& InPath)
    {
        const size_t Colon = InPath.find(':');
        if(Colon < 2 || Colon == std::string::npos || InPath.compare(Colon, 2, ":/") != 0)
        {
            return std::string();
        }
        const auto IsLetter = [](const char InChar) { return (InChar >= 'A' && InChar <= 'Z') || (InChar >= 'a' && InChar <= 'z'); };
        const auto IsDigit  = [](const char InChar) { return InChar >= '0' && InChar <= '9'; };
        if(!IsLetter(InPath[0]))
        {
            return std::string();
        }
        for(size_t Index = 1; Index < Colon; ++Index)
        {
            if(!IsLetter(InPath[Index]) && !IsDigit(InPath[Index]))
            {
                return std::string();
            }
        }
        return InPath.substr(0, Colon);
    }

    // === Plugins =============================================================

    namespace Plugins
    {
        int Discover(const std::string& InRuntimeDir, const bool bInUseSource)
        {
            PluginHostState& State = Host();
            if(!State.Records.empty())
            {
                if(std::any_of(State.Records.begin(), State.Records.end(), [](const PluginRecord& InRecord)
                               { return !InRecord.Libraries.empty(); }))
                {
                    CORE_LOG_ERROR("[Plugins] Discover called with plugin libraries open; Close() first")
                    return int(State.Records.size());
                }
                State.Records.clear();
            }
            State.RuntimeDir = InRuntimeDir;

            const std::string ManifestPath = JoinPath(InRuntimeDir, "Plugins/Plugins.manifest");
            std::vector<uint8_t> Bytes;
            if(!EngineCore::appFileReadAll(ManifestPath, Bytes))
            {
                return 0;   // a build without plugins, or one from before them
            }
            State.Records = ParseManifest(std::string(Bytes.begin(), Bytes.end()));

            for(PluginRecord& Record : State.Records)
            {
                std::string From;
                Record.Files   = OpenPluginFiles(Record, InRuntimeDir, bInUseSource, From);
                Record.Context = std::make_unique<PluginContext>();
                PluginHostAccess::Set(*Record.Context, Record.Name, Record.Version, Record.Files);

                /* Its defaults before the project's file, which then wins
                 * key by key; a var its library declares claims the value
                 * when it loads (CoreConfig.h). */
                std::vector<uint8_t> ConfigBytes;
                if(Record.Files->ReadAll("Config/Default" + Record.Name + ".ini", ConfigBytes))
                {
                    EngineCore::LoadConfigText(std::string(ConfigBytes.begin(), ConfigBytes.end()));
                }
                CORE_LOG("[Plugins] " + Record.Name + " " + Record.Version + " from " + From)
            }
            return int(State.Records.size());
        }

        void MountContent(EngineCore::IFileProvider& InAssets)
        {
            PluginHostState& State = Host();
            std::set<std::string> Known;
            for(const PluginRecord& Record : State.Records)
            {
                Known.insert(Record.Name);
            }

            /* The guard first, so every plugin's layer is asked before it. */
            if(InAssets.MountLayer(GUARD_LAYER, std::make_shared<StrictPrefixGuard>(Known)))
            {
                State.MountedLayers.push_back(GUARD_LAYER);
            }
            for(const PluginRecord& Record : State.Records)
            {
                const std::string Layer = "Plugin:" + Record.Name;
                if(InAssets.MountLayer(Layer, std::make_shared<PrefixFileProvider>(Record.Name, Record.Files)))
                {
                    State.MountedLayers.push_back(Layer);
                }
                else
                {
                    CORE_LOG_ERROR("[Plugins] the asset provider takes no layers: " + Record.Name + "'s content is unreachable")
                }
            }
        }

        void UnmountContent(EngineCore::IFileProvider& InAssets)
        {
            for(const std::string& Layer : Host().MountedLayers)
            {
                InAssets.UnmountLayer(Layer);
            }
            Host().MountedLayers.clear();
        }

        bool LoadModules()
        {
            bool bOk = true;
            for(PluginRecord& Record : Host().Records)
            {
                if(!Record.Libraries.empty())
                {
                    continue;   // already loaded
                }
                for(const std::string& File : Record.ModuleFiles)
                {
                    Record.Libraries.push_back(LoadedLibrary{});
                    Record.Libraries.back().File = File;
                    StartLibrary(Record, Record.Libraries.back(), bOk);
                }
                /* A library that refused leaves nothing registered by hand. */
                const bool bRefused = std::any_of(Record.Libraries.begin(), Record.Libraries.end(),
                                                  [](const LoadedLibrary& InLibrary) { return InLibrary.Handle && !InLibrary.Plugin && InLibrary.Destroy; });
                if(bRefused)
                {
                    RemoveHooksOf(Record.Name);
                }
            }
            return bOk;
        }

        void ShutdownModules()
        {
            std::vector<PluginRecord>& Records = Host().Records;
            for(auto RecordIt = Records.rbegin(); RecordIt != Records.rend(); ++RecordIt)
            {
                for(auto LibraryIt = RecordIt->Libraries.rbegin(); LibraryIt != RecordIt->Libraries.rend(); ++LibraryIt)
                {
                    if(LibraryIt->Plugin)
                    {
                        if(LibraryIt->bStarted)
                        {
                            LibraryIt->Plugin->OnShutdown(*RecordIt->Context);
                        }
                        LibraryIt->Destroy(LibraryIt->Plugin);
                        LibraryIt->Plugin   = nullptr;
                        LibraryIt->bStarted = false;
                    }
                }
                RemoveHooksOf(RecordIt->Name);
            }
        }

        void Close()
        {
            ShutdownModules();
            std::vector<PluginRecord>& Records = Host().Records;
            for(auto RecordIt = Records.rbegin(); RecordIt != Records.rend(); ++RecordIt)
            {
                for(auto LibraryIt = RecordIt->Libraries.rbegin(); LibraryIt != RecordIt->Libraries.rend(); ++LibraryIt)
                {
                    if(LibraryIt->Handle)
                    {
                        EngineCore::appDLLClose(LibraryIt->Handle);
                        LibraryIt->Handle = nullptr;
                    }
                }
            }
            Records.clear();
            Host().Hooks.clear();
        }

        bool WarnConfigMismatch()
        {
            struct Visit
            {
                static void Entry(const char* InSection, const char* InKey, const char* InValue, void* InUserData)
                {
                    std::string Section(InSection);
                    std::transform(Section.begin(), Section.end(), Section.begin(), [](unsigned char C) { return char(std::tolower(C)); });
                    if(Section != "plugins") { return; }

                    std::string Value(InValue);
                    std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char C) { return char(std::tolower(C)); });
                    const bool bWanted = Value == "true" || Value == "1" || Value == "yes" || Value == "on";

                    std::string Key(InKey);
                    std::transform(Key.begin(), Key.end(), Key.begin(), [](unsigned char C) { return char(std::tolower(C)); });
                    bool bBuilt = false;
                    for(const PluginRecord& Record : Host().Records)
                    {
                        std::string Name = Record.Name;
                        std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char C) { return char(std::tolower(C)); });
                        bBuilt = bBuilt || Name == Key;
                    }
                    if(bWanted == bBuilt) { return; }
                    CORE_LOG_WARN(std::string("[Plugins] ") + InKey + "=" + InValue + ", but this build was configured "
                                  + (bBuilt ? "with" : "without") + " it; rebuild the game to apply the change -- its next "
                                  "build re-configures from the project's config")
                    *static_cast<bool*>(InUserData) = true;
                }
            };
            bool bWarned = false;
            EngineCore::ConfigForEachEntry("Plugins", &Visit::Entry, &bWarned);
            return bWarned;
        }

        size_t GetCount()
        {
            return Host().Records.size();
        }

        const std::string& GetNameAt(const size_t InIndex)
        {
            static const std::string None;
            return InIndex < Host().Records.size() ? Host().Records[InIndex].Name : None;
        }

        bool IsLoaded(const std::string& InName)
        {
            for(const PluginRecord& Record : Host().Records)
            {
                if(Record.Name == InName)
                {
                    return std::any_of(Record.Libraries.begin(), Record.Libraries.end(),
                                       [](const LoadedLibrary& InLibrary) { return InLibrary.bStarted; });
                }
            }
            return false;
        }

        void AttachWorld(GameCore::World& InWorld)
        {
            for(const WorldHookRecord& Each : Host().Hooks)
            {
                if(Each.Hook.Attach)
                {
                    Each.Hook.Attach(InWorld, Each.Hook.UserData);
                }
            }
        }

        void DetachWorld(GameCore::World& InWorld)
        {
            const std::vector<WorldHookRecord>& Hooks = Host().Hooks;
            for(auto It = Hooks.rbegin(); It != Hooks.rend(); ++It)
            {
                if(It->Hook.Detach)
                {
                    It->Hook.Detach(InWorld, It->Hook.UserData);
                }
            }
        }
    }
}
