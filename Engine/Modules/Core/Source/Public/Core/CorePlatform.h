#ifndef ENGINECORE_COREPLATFORM_H
#define ENGINECORE_COREPLATFORM_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "Public/Core/CoreAPI.h"

/*
 * The OS-mechanism seam.
 *
 * No file in EngineCore or GDI -- core or backend -- may call a raw OS API or
 * a thread-identity primitive directly. Everything OS-specific goes through an
 * app* function declared here and implemented in exactly one file per
 * platform (CorePlatformWindows.cpp, CorePlatformApple.mm, and later
 * CorePlatformSDL.cpp). If the seam is complete, porting to a new platform is
 * that one file and nothing else.
 *
 * Portable std primitives -- std::mutex, std::deque, std::string -- are NOT
 * OS-specific and stay as they are. Only genuinely platform-divergent
 * mechanisms cross this line.
 *
 * Layering note: this is the OS-mechanism seam. IFileProvider
 * (CoreFileProvider.h) is the separate RESOURCE-POLICY seam -- where an asset
 * is looked up (Qt qrc, SDL RWops, plain filesystem). FilesystemProvider is
 * implemented on top of the functions here.
 *
 * All paths are UTF-8.
 */

namespace EngineCore
{
    // === File I/O ===========================================================
    // Opened in BINARY mode, always. Reading SPIR-V as text corrupts it on
    // Windows, and that failure surfaces as an unreadable shader module rather
    // than anything that points at the read.

    using FileHandle = void*;

    CORE_API FileHandle  appFileOpenRead (const std::string& InPath);   // nullptr on failure
    CORE_API FileHandle  appFileOpenWrite(const std::string& InPath);   // nullptr on failure
    CORE_API size_t      appFileRead (FileHandle InHandle, void* OutDst, size_t InBytes);
    CORE_API size_t      appFileWrite(FileHandle InHandle, const void* InSrc, size_t InBytes);
    CORE_API void        appFileClose(FileHandle InHandle);

    CORE_API bool        appFileReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes);
    /* InSize bytes from InOffset; false (OutBytes cleared) if the file is
     * shorter than that or cannot be read. */
    CORE_API bool        appFileReadRange(const std::string& InPath, uint64_t InOffset, uint64_t InSize, std::vector<uint8_t>& OutBytes);
    CORE_API bool        appFileExists (const std::string& InPath);

    CORE_API bool        appCreateDirectories(const std::string& InPath);

    /* Per-user writable data dir for AppName, created if absent. "" on failure. */
    CORE_API std::string appGetAppDataDir(const std::string& InAppName);

    // === Thread identity ====================================================
    // Replaces Qt::HANDLE + QThread::currentThreadId(). Opaque; only ever
    // compared for equality, never interpreted.

    using ThreadID = uint64_t;

    CORE_API ThreadID appThreadID();

    // === Thread attributes ==================================================
    // The four thread mechanisms that are genuinely platform-divergent, and
    // therefore the only four that belong behind this seam.
    //
    // CREATION AND JOINING ARE NOT HERE, DELIBERATELY. std::thread is a
    // portable std primitive and falls under the rule at the top of this file
    // -- "Portable std primitives are NOT OS-specific and stay as they are".
    // CoreThread.h's Thread class is built on std::thread and calls these for
    // the parts std::thread cannot express. A CreateThread/JoinThread pair here
    // would add a seam function per platform to reimplement something the
    // standard library already ports correctly.
    //
    // Nor is sleep or yield: std::this_thread::sleep_for and yield() are
    // portable, and CoreThread.h wraps them without coming through here.

    /*
     * Names the CALLING thread for the debugger and the profiler.
     *
     * Not a cosmetic feature. A job system's worth of identically-shaped
     * worker threads is unreadable in a debugger's thread list without this,
     * and a capture from a profiler that shows "Thread 24188" eight times tells
     * you nothing about which stall belongs to which pool.
     *
     * CALLING-THREAD-ONLY ON PURPOSE, even though Windows can name another
     * thread and pthreads (mostly) cannot: pthread_setname_np's signature
     * differs per platform and Apple's takes no thread argument at all, so
     * "name yourself on entry" is the only contract all three can honour. The
     * Thread class calls this as the first statement of its trampoline.
     *
     * TRUNCATED, SILENTLY. Linux caps the name at 16 bytes including the null,
     * so a name is a short tag ("Job/3", "Render"), not a sentence. The
     * implementations truncate rather than fail, because a truncated name is
     * strictly better than no name and this must never be worth error-checking
     * at a call site.
     */
    CORE_API void appThreadSetName(const char* InName);

    /*
     * Restricts the CALLING thread to the hardware threads set in InMask,
     * where bit N is hardware thread N. False if the platform refused, or on
     * any platform with no affinity mechanism to speak of.
     *
     * A 64-BIT MASK AND THEREFORE A 64-HARDWARE-THREAD CEILING. Windows'
     * processor groups and Linux' arbitrarily-sized cpu_set_t can both express
     * more; a machine that large gets no affinity rather than a wrong one, and
     * that is an acceptable trade for a mask that fits in a register and an API
     * that is the same shape everywhere.
     *
     * USE THIS SPARINGLY. Pinning workers looks like an optimization and is
     * usually a pessimization: it forbids the OS scheduler from moving work off
     * a core that is busy with something outside this process. It exists for
     * measurement and for the one case that genuinely needs it -- keeping an
     * audio or a render thread off the core the OS has decided to park.
     */
    CORE_API bool appThreadSetAffinity(uint64_t InMask);

    /*
     * Scheduling priority for the CALLING thread, from the three-value ladder
     * in CoreThread.h. False if the platform refused.
     *
     * WHY THREE VALUES AND NOT A NUMBER: Windows takes a symbolic class from
     * THREAD_PRIORITY_IDLE to TIME_CRITICAL, POSIX takes an integer whose valid
     * range depends on the scheduling policy, and on Linux raising a thread's
     * priority above normal needs privileges an engine will not have. A
     * portable integer would be a number that means something different on
     * every platform. Three rungs map onto all of them, and the middle one
     * costs nothing to honour.
     *
     * 0 = Background, 1 = Normal, 2 = High. The enum lives in CoreThread.h
     * rather than here because this seam is about mechanism and that ladder is
     * policy; the cast happens in exactly one place, Thread::Start.
     */
    CORE_API bool appThreadSetPriority(uint8_t InPriority);

    /*
     * Hardware thread count (logical processors), and physical core count.
     *
     * WHY NOT std::thread::hardware_concurrency: it is permitted to return 0,
     * it is only a hint, and it cannot distinguish the two numbers at all.
     * Both matter for sizing a worker pool. On a hyperthreaded part the logical
     * count double-counts execution resources, so a pool sized to it can be
     * 2x oversubscribed for compute-bound work while being right for
     * latency-bound work -- the caller cannot make that choice without both
     * figures.
     *
     * Both return 1 rather than 0 when the platform will not say, so that
     * "count - 1" at a call site cannot underflow into a pool of 4 billion
     * workers.
     */
    CORE_API uint32_t appThreadHardwareCount();
    CORE_API uint32_t appThreadPhysicalCoreCount();

    // === File metadata ======================================================
    // A change-detection stamp, NOT a date. Opaque and platform-defined --
    // 100ns ticks since 1601 on Windows, nanoseconds since the epoch on POSIX.
    // Only ever compared for EQUALITY, never ordered and never converted to a
    // calendar time, for the same reason ThreadID is only ever compared: the
    // two platforms disagree about both origin and resolution.
    //
    // WHY EQUALITY AND NOT `>`: a developer reverting a bad build restores an
    // OLDER binary, and that must count as a change. And filesystems that still
    // report whole-second mtimes cannot distinguish two rebuilds inside one
    // second -- which is why SizeBytes travels with the stamp and the PAIR is
    // the change signature, not the time alone.

    using FileTime = uint64_t;      // 0 means "unavailable"

    /* Header-only and inline, so deliberately NOT CORE_API -- see CoreAPI.h. */
    struct FileStamp
    {
        FileTime ModifiedTime = 0;
        uint64_t SizeBytes    = 0;

        bool IsValid() const { return ModifiedTime != 0; }

        bool operator==(const FileStamp& InOther) const
        {
            return ModifiedTime == InOther.ModifiedTime && SizeBytes == InOther.SizeBytes;
        }
        bool operator!=(const FileStamp& InOther) const { return !(*this == InOther); }
    };

    CORE_API bool appFileStat  (const std::string& InPath, FileStamp& OutStamp);
    CORE_API bool appFileCopy  (const std::string& InSrcPath, const std::string& InDstPath, bool bInOverwrite);
    CORE_API bool appFileDelete(const std::string& InPath);

    /*
     * True if the file can be opened with NO sharing at all.
     *
     * This is the mid-build interlock: link.exe holds its output file with
     * dwShareMode 0 while writing, so a false here means "a build is producing
     * this file right now, do not copy it".
     *
     * PLATFORM ASYMMETRY -- read before relying on it: POSIX has no mandatory
     * locking, so the POSIX implementation degenerates to "the file exists and
     * is readable" and returns true for a half-written file. The settle window
     * in ModuleManager::Tick is what carries the weight there; this is a
     * Windows hardening measure, not the primary guard.
     */
    CORE_API bool appFileCanOpenExclusive(const std::string& InPath);

    /* Non-recursive. Names only -- no "." or "..", no path prefix. */
    CORE_API bool appDirectoryList(const std::string& InPath, std::vector<std::string>& OutNames);

    /* Non-recursive, and deliberately so: a recursive delete behind the seam is
     * a foot-gun aimed at whatever path a caller gets wrong. Fails if the
     * directory is not empty. */
    CORE_API bool appDirectoryDelete(const std::string& InPath);

    // === Dynamic libraries ==================================================
    // The mechanism behind CoreModuleManager. Opaque; a DLLHandle is never
    // dereferenced, only handed back to appDLLGetSymbol/appDLLClose.

    using DLLHandle = void*;

    CORE_API DLLHandle appDLLOpen     (const std::string& InPath);   // nullptr on failure
    CORE_API void*     appDLLGetSymbol(DLLHandle InHandle, const char* InSymbol);
    CORE_API void      appDLLClose    (DLLHandle InHandle);

    /*
     * Diagnostic text for the LAST failing appDLL* call ON THIS THREAD.
     *
     * The underlying mechanisms are both consume-on-read and easily clobbered
     * -- dlerror() clears itself, and GetLastError() is overwritten by the next
     * Win32 call, including ones inside this seam. So the implementations
     * capture the message into a thread_local AT THE POINT OF FAILURE and this
     * just returns it. That makes the contract identical on both platforms and
     * removes the "you must call it immediately" trap.
     *
     * Empty string if the last appDLL* call on this thread succeeded.
     */
    CORE_API std::string appDLLGetLastError();

    // === Process identity ===================================================
    // Used to name per-process shadow-copy directories, so that two engine
    // instances -- an editor and a test run -- do not fight over the same paths
    // and so that orphan cleanup can tell its own directories from a live
    // process's.

    CORE_API uint32_t appProcessID();

    /* Is a process with InPid running? True when it cannot be told (no
     * permission to look): callers use it to leave a live process's files
     * alone, so they err toward keeping them. A reused id reads as running. */
    CORE_API bool appProcessIsRunning(uint32_t InPid);

    // === Virtual memory =====================================================
    // Reserve/commit, as distinct from malloc. This exists for the pool
    // allocators in CoreMemoryPool.h, which want a large CONTIGUOUS address
    // range whose pointers stay valid as the pool grows, without paying for
    // physical pages that are never touched.
    //
    // malloc cannot express that. A 256MB malloc either commits 256MB or is a
    // lie the OS tells until first touch, depending on the platform, and a
    // realloc to grow it is free to MOVE the block -- which would invalidate
    // every pointer a pool has already handed out. Reserving the address range
    // up front and committing into it as the pool grows is the only way to get
    // "grows on demand" and "pointers never move" at the same time.
    //
    // WHY THIS IS BEHIND THE SEAM AND _aligned_malloc IS NOT
    //
    // VirtualAlloc and mmap are OS APIs and fall squarely under the rule at the
    // top of this file. _aligned_malloc vs std::aligned_alloc is CRT
    // divergence, not OS divergence -- CoreModule.cpp already calls
    // std::realloc directly -- so that one stays as an #if inside
    // CoreMemory.cpp rather than growing a seam function here.
    //
    // SIZES ARE ROUNDED UP INTERNALLY to page granularity, so a caller never
    // has to know the page size to use these correctly; appMemPageSize() is
    // for pools that want to CHOOSE a size, not for pools that must.
    //
    // PAIRING RULE: appMemRelease takes the same base AND the same size that
    // were handed to appMemReserve. Windows' VirtualFree(MEM_RELEASE) ignores
    // the length and POSIX' munmap requires it, so the seam takes the union of
    // what the two need. Releasing a sub-range of a reservation is not
    // expressible here and must not be attempted.

    CORE_API size_t appMemPageSize();

    /* Address space only -- no physical pages, no readable memory. nullptr on failure. */
    CORE_API void*  appMemReserve(size_t InBytes);

    /* Backs [InBase, InBase+InBytes) with pages and makes it readable/writable.
     * The range must lie inside one reservation. Committing an already-committed
     * range is legal and is a no-op, which is what lets a pool commit greedily
     * without tracking page boundaries itself. Newly committed pages read as
     * zero on both platforms. */
    CORE_API bool   appMemCommit(void* InBase, size_t InBytes);

    /* Returns the physical pages, keeping the address range reserved. The
     * contents are LOST -- a subsequent commit of the same range reads as zero,
     * it does not restore what was there. */
    CORE_API bool   appMemDecommit(void* InBase, size_t InBytes);

    CORE_API void   appMemRelease(void* InBase, size_t InBytes);
}

#endif //ENGINECORE_COREPLATFORM_H
