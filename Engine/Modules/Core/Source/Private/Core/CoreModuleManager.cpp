#include "Public/Core/CoreModuleManager.h"

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace EngineCore
{
namespace
{
    ModuleManager* GModuleManager = nullptr;

    /* Seconds on a steady clock. See the SetSettleSeconds comment for why this
     * is not driven by Tick's InDeltaSeconds. */
    double NowSeconds()
    {
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point Origin = Clock::now();
        return std::chrono::duration<double>(Clock::now() - Origin).count();
    }

    /* "" if the path has no directory component, which then means "here". */
    std::string DirectoryOf(const std::string& InPath)
    {
        const size_t Slash = InPath.find_last_of("/\\");
        return (Slash == std::string::npos) ? std::string(".") : InPath.substr(0, Slash);
    }

    std::string FileNameOf(const std::string& InPath)
    {
        const size_t Slash = InPath.find_last_of("/\\");
        return (Slash == std::string::npos) ? InPath : InPath.substr(Slash + 1);
    }

    /* Path with its extension replaced. Used only to find the sibling .pdb. */
    std::string WithExtension(const std::string& InPath, const std::string& InExtension)
    {
        const size_t Dot   = InPath.find_last_of('.');
        const size_t Slash = InPath.find_last_of("/\\");
        if(Dot == std::string::npos || (Slash != std::string::npos && Dot < Slash))
        {
            return InPath + InExtension;
        }
        return InPath.substr(0, Dot) + InExtension;
    }

    std::string Join(const std::string& InDir, const std::string& InLeaf)
    {
        if(InDir.empty()) return InLeaf;

        const char Last = InDir.back();
        if(Last == '/' || Last == '\\') return InDir + InLeaf;
        return InDir + "/" + InLeaf;
    }

    /* "<pid>-<gen>", zero-padded so a directory listing sorts sensibly and so
     * the pid prefix is trivially parseable by the sweep. */
    std::string GenerationDirName(const uint32_t InPid, const uint32_t InGeneration)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%u-%04u", InPid, InGeneration);
        return std::string(Buffer);
    }

    std::string HexOf(const uint32_t InValue)
    {
        char Buffer[16];
        std::snprintf(Buffer, sizeof(Buffer), "0x%08X", InValue);
        return std::string(Buffer);
    }
}

ModuleManager::ModuleManager(std::string InShadowRootOverride)
    : ShadowRootOverride(std::move(InShadowRootOverride))
{
}

ModuleManager::~ModuleManager()
{
    UnloadAll();

    // A manager that was installed as the process-wide one must not leave a
    // dangling pointer behind it. Only clear it if it is still us: a host that
    // already swapped in a different manager should keep the one it chose.
    if(GModuleManager == this)
    {
        GModuleManager = nullptr;
    }
}

ModuleManager::ModuleRecord* ModuleManager::Find(const std::string& InModuleName)
{
    for(ModuleRecord& Record : Records)
    {
        if(Record.Name == InModuleName) return &Record;
    }
    return nullptr;
}

const ModuleManager::ModuleRecord* ModuleManager::Find(const std::string& InModuleName) const
{
    for(const ModuleRecord& Record : Records)
    {
        if(Record.Name == InModuleName) return &Record;
    }
    return nullptr;
}

std::string ModuleManager::ShadowRootFor(const ModuleRecord& InRecord) const
{
    if(!ShadowRootOverride.empty())
    {
        return ShadowRootOverride;
    }

    /*
     * Rooted under the WATCHED library's own directory, not %TEMP%. On Windows
     * the loader searches the application's directory for imports, which under
     * a flat output layout is where EngineCore.dll sits; on Linux the modules
     * are built with an $ORIGIN/.. RPATH that resolves libEngineCore.so one
     * level up from here. A temp-directory root would break the Linux case
     * outright.
     */
    return Join(DirectoryOf(InRecord.SourcePath), "HotReload");
}

bool ModuleManager::RegisterModule(const std::string& InModuleName, const std::string& InDLLPath, const bool bInWatch)
{
    if(InModuleName.empty() || InDLLPath.empty())
    {
        CORE_LOG_ERROR("ModuleManager::RegisterModule requires a name and a path");
        return false;
    }

    if(Find(InModuleName))
    {
        CORE_LOG_ERROR("ModuleManager: module '" + InModuleName + "' is already registered");
        return false;
    }

    ModuleRecord Record;
    Record.Name       = InModuleName;
    Record.SourcePath = InDLLPath;
    Record.bWatch     = bInWatch;
    Records.push_back(std::move(Record));

    if(!bSwept)
    {
        bSwept = true;
        SweepShadowDirectory();
    }

    return true;
}

bool ModuleManager::UnregisterModule(const std::string& InModuleName)
{
    for(size_t Index = 0; Index < Records.size(); ++Index)
    {
        if(Records[Index].Name != InModuleName) continue;

        UnloadModule(InModuleName);
        Records.erase(Records.begin() + long(Index));
        return true;
    }
    return false;
}

void ModuleManager::SetWatchEnabled(const std::string& InModuleName, const bool bInEnabled)
{
    if(ModuleRecord* Record = Find(InModuleName))
    {
        Record->bWatch = bInEnabled;
    }
}

bool ModuleManager::LoadFromShadow(ModuleRecord& InRecord, const std::string& InShadowPath)
{
    const DLLHandle Handle = appDLLOpen(InShadowPath);
    if(!Handle)
    {
        CORE_LOG_ERROR("ModuleManager: could not open '" + InShadowPath + "': " + appDLLGetLastError());
        return false;
    }

    /*
     * The version is checked BEFORE anything is created and before any virtual
     * is called -- see the ORDER IS LOAD-BEARING note in CoreModule.h. The name
     * comes from the free function precisely so a rejected DLL can still be
     * named in the log.
     */
    const ModuleGetABIVersionFn GetABIVersion =
        reinterpret_cast<ModuleGetABIVersionFn>(appDLLGetSymbol(Handle, MODULE_SYMBOL_ABI_VERSION));

    if(!GetABIVersion)
    {
        CORE_LOG_ERROR("ModuleManager: '" + InRecord.Name + "' exports no " +
                       MODULE_SYMBOL_ABI_VERSION + " -- not a Nimble module");
        appDLLClose(Handle);
        return false;
    }

    const uint32_t Reported = GetABIVersion();
    if(Reported != NIMBLE_MODULE_ABI_VERSION)
    {
        std::string Reported_Name;
        if(const ModuleGetNameFn GetName =
               reinterpret_cast<ModuleGetNameFn>(appDLLGetSymbol(Handle, MODULE_SYMBOL_NAME)))
        {
            if(const char* Name = GetName()) Reported_Name = Name;
        }

        // Hex, so the configuration flags in the high bits are readable at a
        // glance -- that is usually what differs, not the revision.
        CORE_LOG_ERROR("ModuleManager: refusing '" + InRecord.Name + "'" +
                       (Reported_Name.empty() ? std::string() : " (reports itself as '" + Reported_Name + "')") +
                       " -- ABI " + HexOf(Reported) + ", host expects " + HexOf(NIMBLE_MODULE_ABI_VERSION));
        appDLLClose(Handle);
        return false;
    }

    const ModuleCreateFn Create =
        reinterpret_cast<ModuleCreateFn>(appDLLGetSymbol(Handle, MODULE_SYMBOL_CREATE));
    const ModuleDestroyFn Destroy =
        reinterpret_cast<ModuleDestroyFn>(appDLLGetSymbol(Handle, MODULE_SYMBOL_DESTROY));

    if(!Create || !Destroy)
    {
        CORE_LOG_ERROR("ModuleManager: '" + InRecord.Name + "' is missing " +
                       MODULE_SYMBOL_CREATE + " or " + MODULE_SYMBOL_DESTROY);
        appDLLClose(Handle);
        return false;
    }

    // Snapshot the two registries Core owns, so a module that installs itself
    // into one and forgets to remove itself can be named at shutdown rather
    // than discovered as a crash in the host's next log call.
    const LogHandler   HandlerBefore  = GetLogHandler();
    IFileProvider*     ProviderBefore = GetFileProvider();

    IModule* Instance = Create();
    if(!Instance)
    {
        CORE_LOG_ERROR("ModuleManager: " + std::string(MODULE_SYMBOL_CREATE) +
                       " returned null for '" + InRecord.Name + "'");
        appDLLClose(Handle);
        return false;
    }

    // IsEmpty rather than "did we ever load", so InPrev == nullptr reliably
    // means a cold start rather than "reloaded, but the old build wrote
    // nothing".
    const ModuleReloadState* Previous = InRecord.State.IsEmpty() ? nullptr : &InRecord.State;

    if(!Instance->OnStartup(Previous))
    {
        CORE_LOG_ERROR("ModuleManager: '" + InRecord.Name + "' refused to start up");
        Destroy(Instance);
        appDLLClose(Handle);
        return false;
    }

    InRecord.Handle           = Handle;
    InRecord.Instance         = Instance;
    InRecord.Destroy          = Destroy;
    InRecord.ShadowPath       = InShadowPath;
    InRecord.ShadowDir        = DirectoryOf(InShadowPath);
    InRecord.ModuleState      = EModuleState::MS_Loaded;
    InRecord.LogHandlerAtLoad = HandlerBefore;
    InRecord.ProviderAtLoad   = ProviderBefore;

    return true;
}

bool ModuleManager::LoadGeneration(ModuleRecord& InRecord)
{
    FileStamp SourceStamp;
    if(!appFileStat(InRecord.SourcePath, SourceStamp))
    {
        CORE_LOG_ERROR("ModuleManager: '" + InRecord.Name + "' has no file at " + InRecord.SourcePath);
        return false;
    }

    // Guard 2 of 3 (see the settle window in Tick and the re-verify below): on
    // Windows link.exe holds its output with no sharing at all, so a failure
    // here is a hard "a build is writing this right now".
    if(!appFileCanOpenExclusive(InRecord.SourcePath))
    {
        CORE_LOG("ModuleManager: '" + InRecord.Name + "' is being written; deferring");
        return false;
    }

    const std::string Root = ShadowRootFor(InRecord);
    if(!appCreateDirectories(Root))
    {
        CORE_LOG_ERROR("ModuleManager: could not create shadow root " + Root);
        return false;
    }

    // A fresh directory per generation, because Windows will not let the
    // previous copy be overwritten while it is (or was recently) mapped.
    const std::string GenerationDir =
        Join(Root, GenerationDirName(appProcessID(), ++InRecord.Generation));

    if(!appCreateDirectories(GenerationDir))
    {
        CORE_LOG_ERROR("ModuleManager: could not create shadow directory " + GenerationDir);
        return false;
    }

    const std::string FileName   = FileNameOf(InRecord.SourcePath);
    const std::string ShadowPath = Join(GenerationDir, FileName);

    if(!appFileCopy(InRecord.SourcePath, ShadowPath, true))
    {
        CORE_LOG_ERROR("ModuleManager: could not shadow-copy " + InRecord.SourcePath + " -> " + ShadowPath);
        DeleteGenerationDirectory(GenerationDir, InRecord.Name);
        return false;
    }

    /*
     * Copy the sibling .pdb under its ORIGINAL filename.
     *
     * Modules are linked with /PDBALTPATH:%_PDB%, so the DLL records only the
     * bare filename of its symbols rather than an absolute path. The debugger
     * resolves that relative to the loaded module's own directory, finds this
     * copy, and never opens -- and so never locks -- the original. Without it,
     * hot reload just trades a locked DLL for a locked PDB and the next build
     * dies with LNK1201 instead of LNK1104. The per-generation DIRECTORY is
     * what makes this work, since the recorded filename is fixed.
     */
    const std::string SourcePdb = WithExtension(InRecord.SourcePath, ".pdb");
    if(appFileExists(SourcePdb))
    {
        appFileCopy(SourcePdb, Join(GenerationDir, FileNameOf(SourcePdb)), true);
    }

    // Guard 3 of 3: a build could have started between the exclusive-open probe
    // and the copy, which would leave us holding a torn image that opens fine
    // and misbehaves later.
    FileStamp AfterStamp;
    if(!appFileStat(InRecord.SourcePath, AfterStamp) || AfterStamp != SourceStamp)
    {
        CORE_LOG("ModuleManager: '" + InRecord.Name + "' changed while being copied; deferring");
        DeleteGenerationDirectory(GenerationDir, InRecord.Name);
        return false;
    }

    if(!LoadFromShadow(InRecord, ShadowPath))
    {
        DeleteGenerationDirectory(GenerationDir, InRecord.Name);
        return false;
    }

    InRecord.LoadedStamp  = SourceStamp;
    InRecord.PendingStamp = FileStamp();
    return true;
}

bool ModuleManager::LoadModule(const std::string& InModuleName)
{
    ModuleRecord* Record = Find(InModuleName);
    if(!Record)
    {
        CORE_LOG_ERROR("ModuleManager: '" + InModuleName + "' is not registered");
        return false;
    }

    if(Record->ModuleState == EModuleState::MS_Loaded)
    {
        return true;
    }

    if(!LoadGeneration(*Record))
    {
        Record->ModuleState = EModuleState::MS_Failed;
        return false;
    }

    CORE_LOG("ModuleManager: loaded '" + InModuleName + "' (" +
             std::string(Record->Instance->GetVersionString()) + ")");
    return true;
}

void ModuleManager::TeardownInstance(ModuleRecord& InRecord)
{
    if(InRecord.Instance)
    {
        InRecord.Instance->OnShutdown(InRecord.State);

        /*
         * The manager cannot enforce the DANGLING CALLBACKS rule, but it can
         * detect the two registries Core owns. A module that installed itself
         * here and did not restore the previous value is about to leave a
         * function pointer aimed at memory that is seconds away from being
         * unmapped -- and the crash would surface in the HOST's next log call,
         * nowhere near the module. Warn while the name is still known.
         */
        if(GetLogHandler() != InRecord.LogHandlerAtLoad)
        {
            CORE_LOG_WARN("ModuleManager: '" + InRecord.Name +
                          "' left the log handler pointing into its own image; restoring");
            SetLogHandler(InRecord.LogHandlerAtLoad);
        }
        if(GetFileProvider() != InRecord.ProviderAtLoad)
        {
            CORE_LOG_WARN("ModuleManager: '" + InRecord.Name +
                          "' left the file provider pointing into its own image; restoring");
            SetFileProvider(InRecord.ProviderAtLoad);
        }

        if(InRecord.Destroy)
        {
            InRecord.Destroy(InRecord.Instance);
        }
        InRecord.Instance = nullptr;
    }

    InRecord.Destroy = nullptr;

    if(InRecord.Handle)
    {
        appDLLClose(InRecord.Handle);
        InRecord.Handle = nullptr;
    }
}

void ModuleManager::DeleteGenerationDirectory(const std::string& InDir, const std::string& InModuleName) const
{
    if(InDir.empty()) return;

    std::vector<std::string> Names;
    if(appDirectoryList(InDir, Names))
    {
        for(const std::string& Name : Names)
        {
            appFileDelete(Join(InDir, Name));
        }
    }

    /*
     * Best effort, and at Log verbosity rather than Error on purpose: a
     * debugger commonly still holds generation N's .pdb open while generation
     * N+1 is loading, so the first attempt failing is the normal case, not a
     * fault. SweepShadowDirectory reclaims whatever is left on the next run.
     */
    if(!appDirectoryDelete(InDir))
    {
        CORE_LOG("ModuleManager: could not remove shadow directory " + InDir +
                 " for '" + InModuleName + "' (still in use; it will be swept later)");
    }
}

bool ModuleManager::DoReload(ModuleRecord& InRecord)
{
    // Remembered, NOT deleted: this is what the rollback path reopens.
    const std::string OldShadowDir  = InRecord.ShadowDir;
    const std::string OldShadowPath = InRecord.ShadowPath;

    TeardownInstance(InRecord);

    if(LoadGeneration(InRecord))
    {
        ++InRecord.ReloadCount;
        InRecord.LastFailedStamp = FileStamp();
        DeleteGenerationDirectory(OldShadowDir, InRecord.Name);

        CORE_LOG("ModuleManager: reloaded '" + InRecord.Name + "' (" +
                 std::string(InRecord.Instance->GetVersionString()) + ")");
        return true;
    }

    /*
     * ROLLBACK.
     *
     * The alternative -- leaving the module unloaded -- means an editor drops
     * the gameplay module because somebody saved a typo, and cannot get it back
     * without restarting. That is worse than not having hot reload at all.
     *
     * The old code is re-fed the blob IT wrote moments ago, which is why
     * OnStartup is documented as having to tolerate its own OnShutdown output.
     */
    FileStamp FailedStamp;
    if(appFileStat(InRecord.SourcePath, FailedStamp))
    {
        // The retry gate. Without it a broken build spins copy -> open ->
        // reject -> roll back once per tick, forever, filling the log and the
        // disk. It releases as soon as the stamp changes again.
        InRecord.LastFailedStamp = FailedStamp;
    }

    if(!OldShadowPath.empty() && LoadFromShadow(InRecord, OldShadowPath))
    {
        CORE_LOG_WARN("ModuleManager: '" + InRecord.Name +
                      "' failed to reload; rolled back to the previous build (" +
                      std::string(InRecord.Instance->GetVersionString()) + ")");
        return false;
    }

    CORE_LOG_ERROR("ModuleManager: '" + InRecord.Name +
                   "' failed to reload and could not be rolled back; it is now unloaded");
    InRecord.ModuleState = EModuleState::MS_Failed;
    return false;
}

bool ModuleManager::ReloadModule(const std::string& InModuleName)
{
    ModuleRecord* Record = Find(InModuleName);
    if(!Record)
    {
        CORE_LOG_ERROR("ModuleManager: '" + InModuleName + "' is not registered");
        return false;
    }

    // An explicit request clears the retry gate: a person asking again is a
    // signal in a way an unchanged file is not.
    Record->LastFailedStamp = FileStamp();

    if(Record->ModuleState != EModuleState::MS_Loaded)
    {
        return LoadModule(InModuleName);
    }

    return DoReload(*Record);
}

bool ModuleManager::UnloadModule(const std::string& InModuleName)
{
    ModuleRecord* Record = Find(InModuleName);
    if(!Record) return false;

    if(Record->ModuleState != EModuleState::MS_Loaded && !Record->Handle)
    {
        Record->ModuleState = EModuleState::MS_Unloaded;
        return true;
    }

    TeardownInstance(*Record);
    DeleteGenerationDirectory(Record->ShadowDir, Record->Name);

    Record->ShadowDir    = std::string();
    Record->ShadowPath   = std::string();
    Record->LoadedStamp  = FileStamp();
    Record->PendingStamp = FileStamp();
    Record->ModuleState  = EModuleState::MS_Unloaded;

    CORE_LOG("ModuleManager: unloaded '" + InModuleName + "'");
    return true;
}

void ModuleManager::UnloadAll()
{
    // Reverse registration order, on the assumption that later modules may have
    // been built on earlier ones. Nothing enforces that dependency, but the
    // ordering costs nothing and is the one that can be right.
    for(size_t Index = Records.size(); Index > 0; --Index)
    {
        UnloadModule(Records[Index - 1].Name);
    }
}

int ModuleManager::Tick(const float InDeltaSeconds)
{
    int Reloaded = 0;
    const double Now = NowSeconds();

    for(ModuleRecord& Record : Records)
    {
        if(!Record.bWatch) continue;
        if(Record.ModuleState != EModuleState::MS_Loaded &&
           Record.ModuleState != EModuleState::MS_Failed) continue;

        FileStamp Stamp;
        if(!appFileStat(Record.SourcePath, Stamp))
        {
            // Vanished mid-build. Wait for it to come back rather than tearing
            // down a module that is working perfectly well.
            continue;
        }

        if(Stamp == Record.LoadedStamp)
        {
            Record.PendingStamp = FileStamp();
            continue;
        }

        if(Stamp == Record.LastFailedStamp)
        {
            continue;       // the retry gate; ReloadModule ignores it
        }

        // Guard 1 of 3, and the only one that works on POSIX: the same
        // (mtime, size) has to be observed twice, SettleSeconds apart.
        if(Stamp != Record.PendingStamp)
        {
            Record.PendingStamp        = Stamp;
            Record.PendingSinceSeconds = Now;
            continue;
        }

        if(Now - Record.PendingSinceSeconds < double(SettleSeconds))
        {
            continue;
        }

        if(!appFileCanOpenExclusive(Record.SourcePath))
        {
            // A build just started, so the stamp we settled on is already
            // stale. Restart the clock rather than merely deferring.
            Record.PendingSinceSeconds = Now;
            continue;
        }

        if(DoReload(Record))
        {
            ++Reloaded;
        }
    }

    // Strictly after every reload, so no module is ticked in the same call in
    // which another was swapped underneath it.
    for(ModuleRecord& Record : Records)
    {
        if(Record.ModuleState == EModuleState::MS_Loaded && Record.Instance)
        {
            Record.Instance->OnTick(InDeltaSeconds);
        }
    }

    return Reloaded;
}

EModuleState ModuleManager::GetModuleState(const std::string& InModuleName) const
{
    const ModuleRecord* Record = Find(InModuleName);
    return Record ? Record->ModuleState : EModuleState::MS_Unregistered;
}

IModule* ModuleManager::GetModule(const std::string& InModuleName) const
{
    const ModuleRecord* Record = Find(InModuleName);
    return Record ? Record->Instance : nullptr;
}

void* ModuleManager::GetModuleSymbol(const std::string& InModuleName, const char* InSymbol) const
{
    const ModuleRecord* Record = Find(InModuleName);
    if(!Record || !Record->Handle) return nullptr;

    return appDLLGetSymbol(Record->Handle, InSymbol);
}

const ModuleReloadState* ModuleManager::GetReloadState(const std::string& InModuleName) const
{
    const ModuleRecord* Record = Find(InModuleName);
    return Record ? &Record->State : nullptr;
}

uint32_t ModuleManager::GetReloadCount(const std::string& InModuleName) const
{
    const ModuleRecord* Record = Find(InModuleName);
    return Record ? Record->ReloadCount : 0;
}

std::string ModuleManager::GetModuleVersionString(const std::string& InModuleName) const
{
    const ModuleRecord* Record = Find(InModuleName);
    if(!Record || !Record->Instance) return std::string();

    const char* Version = Record->Instance->GetVersionString();
    return Version ? std::string(Version) : std::string();
}

size_t ModuleManager::GetModuleCount() const
{
    return Records.size();
}

const std::string& ModuleManager::GetModuleNameAt(const size_t InIndex) const
{
    static const std::string Empty;
    return (InIndex < Records.size()) ? Records[InIndex].Name : Empty;
}

void ModuleManager::SetSettleSeconds(const float InSeconds)
{
    SettleSeconds = (InSeconds < 0.0f) ? 0.0f : InSeconds;
}

float ModuleManager::GetSettleSeconds() const
{
    return SettleSeconds;
}

void ModuleManager::SweepShadowDirectory()
{
    const uint32_t SelfPid = appProcessID();

    for(const ModuleRecord& Record : Records)
    {
        const std::string Root = ShadowRootFor(Record);

        std::vector<std::string> Names;
        if(!appDirectoryList(Root, Names)) continue;

        for(const std::string& Name : Names)
        {
            /*
             * Reclaim only directories belonging to a process that is not us.
             * A concurrently running editor's working set is off limits, and an
             * entry whose name does not parse is left alone rather than
             * guessed at -- this function deletes things, so it errs toward
             * doing nothing.
             */
            const size_t Dash = Name.find('-');
            if(Dash == std::string::npos || Dash == 0) continue;

            bool bAllDigits = true;
            for(size_t Index = 0; Index < Dash; ++Index)
            {
                if(Name[Index] < '0' || Name[Index] > '9') { bAllDigits = false; break; }
            }
            if(!bAllDigits) continue;

            const unsigned long Pid = std::strtoul(Name.substr(0, Dash).c_str(), nullptr, 10);
            if(uint32_t(Pid) == SelfPid) continue;

            DeleteGenerationDirectory(Join(Root, Name), Record.Name);
        }
    }
}

void SetModuleManager(ModuleManager* InManager)
{
    GModuleManager = InManager;
}

ModuleManager* GetModuleManager()
{
    return GModuleManager;
}
}
