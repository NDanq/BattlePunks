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
    CORE_API bool        appFileExists (const std::string& InPath);

    CORE_API bool        appCreateDirectories(const std::string& InPath);

    /* Per-user writable data dir for AppName, created if absent. "" on failure. */
    CORE_API std::string appGetAppDataDir(const std::string& InAppName);

    // === Thread identity ====================================================
    // Replaces Qt::HANDLE + QThread::currentThreadId(). Opaque; only ever
    // compared for equality, never interpreted.

    using ThreadID = uint64_t;

    CORE_API ThreadID appThreadID();

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
