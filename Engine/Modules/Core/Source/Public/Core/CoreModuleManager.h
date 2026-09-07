#ifndef ENGINECORE_COREMODULEMANAGER_H
#define ENGINECORE_COREMODULEMANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreModule.h"
#include "Public/Core/CorePlatform.h"

/*
 * The host side of hot reload: owns the loaded libraries, watches their files,
 * and swaps them.
 *
 * CoreModule.h is the module author's half of this contract and carries the
 * RELOAD HAZARDS block. Read that first -- everything here assumes a module
 * obeys it, and none of it can be enforced from this side.
 *
 * WHY A CLASS RATHER THAN THE G-STATIC IDIOM
 *
 * CoreLog.cpp and CoreFileProvider.cpp put a G-prefixed pointer in an anonymous
 * namespace behind free functions, and that is the right shape for a seam with
 * no owned state -- a function pointer, a non-owning interface pointer.
 * ModuleManager owns a vector of records, open OS handles, directories on disk
 * and reload buffers; a file-scope object would be a different thing wearing
 * the same clothes.
 *
 * A Meyers singleton would be specifically wrong: its destructor runs during
 * static destruction, so it might close a module AFTER the host's atexit
 * handlers and after other translation units' globals are gone. Unload ordering
 * relative to host teardown is exactly the hazard hot reload exists to control,
 * and a function-local static hands that ordering to the linker. The host must
 * be able to write Manager.UnloadAll() at a point of its choosing in main().
 *
 * The locator pair at the bottom still exists, so subsystems can reach "the"
 * manager the same way they reach "the" file provider -- non-owning raw
 * pointer, no locking, set by the host. That is the part of the idiom that does
 * apply.
 *
 * THREADING: none. Every call on one manager -- Tick, Load, Unload, Reload --
 * must happen on a single thread, and it must be the thread OnTick is expected
 * to run on. Same rule as GetFileProvider.
 */

namespace EngineCore
{
    class CORE_API ModuleManager
    {
    public:
        /* Shadow copies default to "<directory of the watched library>/HotReload".
         * That directory has to be writable -- true for a build tree and for
         * Binaries/, false for an installed read-only application directory,
         * which is what the explicit constructor is for. */
        ModuleManager() = default;
        explicit ModuleManager(std::string InShadowRootOverride);

        /* Unloads everything still loaded, in reverse registration order. */
        ~ModuleManager();

        ModuleManager(const ModuleManager&)            = delete;
        ModuleManager& operator=(const ModuleManager&) = delete;

        // === Registration ===================================================

        /* InDLLPath is the file that gets WATCHED, not the one that gets
         * loaded: the manager loads a shadow copy of it so that a rebuild can
         * overwrite this path while the module is live. False if the name is
         * already registered. */
        bool RegisterModule(const std::string& InModuleName, const std::string& InDLLPath, bool bInWatch = true);

        /* Unloads first if loaded. */
        bool UnregisterModule(const std::string& InModuleName);

        void SetWatchEnabled(const std::string& InModuleName, bool bInEnabled);

        // === Lifetime =======================================================

        bool LoadModule  (const std::string& InModuleName);
        bool UnloadModule(const std::string& InModuleName);

        /* The explicit request, for an editor hotkey. Ignores watch state, the
         * settle window, and the failed-stamp retry gate -- a person asking
         * again is a signal, unlike a file that has not changed. */
        bool ReloadModule(const std::string& InModuleName);

        void UnloadAll();

        // === The pump =======================================================

        /*
         * Polls every watched module, reloads those whose file changed AND
         * settled, then calls OnTick on everything loaded. Returns the number
         * of modules successfully reloaded during this call.
         *
         * Reloads happen strictly before any OnTick, so no module is ticked in
         * the same call in which another was swapped underneath it.
         */
        int Tick(float InDeltaSeconds);

        // === Query ==========================================================

        EModuleState GetModuleState(const std::string& InModuleName) const;

        /* HAZARD: valid only until the next Tick, Reload or Unload. Never cache
         * it in a member -- see DANGLING VTABLES in CoreModule.h. */
        IModule* GetModule(const std::string& InModuleName) const;

        /* An optional module-specific entry point, by exported name. Same
         * lifetime hazard as GetModule. */
        void* GetModuleSymbol(const std::string& InModuleName, const char* InSymbol) const;

        /* The blob as it currently stands. Non-null even while the module is
         * unloaded -- that is the point of it being host-owned. */
        const ModuleReloadState* GetReloadState(const std::string& InModuleName) const;

        uint32_t GetReloadCount(const std::string& InModuleName) const;

        /* The version string the live instance reports, or "" if not loaded.
         * The cheapest way for a host to see that a reload actually took. */
        std::string GetModuleVersionString(const std::string& InModuleName) const;

        // === Iteration ======================================================

        size_t             GetModuleCount() const;
        const std::string& GetModuleNameAt(size_t InIndex) const;

        // === Tuning =========================================================

        /*
         * A changed file must present the same (mtime, size) for this long
         * before it is copied, which is the only mid-build guard that works on
         * POSIX. Defaults to 0.25s. Tests set it to 0.
         *
         * Measured against a steady clock, NOT against the InDeltaSeconds
         * handed to Tick: a settle window driven by frame delta stops working
         * the instant the host stalls, and a big rebuild is exactly what stalls
         * it.
         */
        void  SetSettleSeconds(float InSeconds);
        float GetSettleSeconds() const;

        /* Deletes shadow directories left behind by OTHER, dead processes.
         * Called automatically by the first RegisterModule. */
        void SweepShadowDirectory();

    private:
        struct ModuleRecord
        {
            std::string Name;
            std::string SourcePath;        // the watched file
            std::string ShadowDir;         // current generation's directory
            std::string ShadowPath;        // the copy actually loaded

            DLLHandle       Handle   = nullptr;
            IModule*        Instance = nullptr;
            ModuleDestroyFn Destroy  = nullptr;

            ModuleReloadState State;

            FileStamp LoadedStamp;         // SourcePath as of the current load
            FileStamp PendingStamp;        // a candidate seen but not yet settled
            FileStamp LastFailedStamp;     // do not retry this exact build
            double    PendingSinceSeconds = 0.0;

            /* The two registries Core owns, sampled just before OnStartup so
             * that a module which installs itself into one and forgets to
             * restore it can be named at shutdown -- see DANGLING CALLBACKS in
             * CoreModule.h -- instead of surfacing as a crash in the host's
             * next log call. */
            LogHandler     LogHandlerAtLoad = nullptr;
            IFileProvider* ProviderAtLoad   = nullptr;

            uint32_t     Generation  = 0;
            uint32_t     ReloadCount = 0;
            EModuleState ModuleState = EModuleState::MS_Unloaded;
            bool         bWatch      = true;
        };

        ModuleRecord*       Find(const std::string& InModuleName);
        const ModuleRecord* Find(const std::string& InModuleName) const;

        std::string ShadowRootFor(const ModuleRecord& InRecord) const;

        /* Opens a fresh generation of InRecord from its source. Leaves the
         * record untouched on failure, so a caller can roll back. */
        bool LoadGeneration(ModuleRecord& InRecord);

        /* Reopens a previously proven shadow copy. The rollback path. */
        bool LoadFromShadow(ModuleRecord& InRecord, const std::string& InShadowPath);

        /* OnShutdown + Destroy + close. Does NOT touch the shadow directory,
         * because the rollback path needs it to survive. */
        void TeardownInstance(ModuleRecord& InRecord);

        void DeleteGenerationDirectory(const std::string& InDir, const std::string& InModuleName) const;

        bool DoReload(ModuleRecord& InRecord);

        std::vector<ModuleRecord> Records;
        std::string               ShadowRootOverride;
        float                     SettleSeconds = 0.25f;
        bool                      bSwept        = false;
    };

    /* Process-wide manager. Not owned -- exactly like SetFileProvider. */
    CORE_API void           SetModuleManager(ModuleManager* InManager);
    CORE_API ModuleManager* GetModuleManager();
}

#endif //ENGINECORE_COREMODULEMANAGER_H
