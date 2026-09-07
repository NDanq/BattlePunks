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
}

#endif //ENGINECORE_COREPLATFORM_H
