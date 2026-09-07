#include "Public/Core/CorePlatform.h"

/*
 * POSIX implementation of the OS-mechanism seam -- macOS and Linux.
 *
 * Deliberately plain POSIX/C++ rather than Objective-C, so one file covers
 * both platforms and neither needs a toolchain the other lacks. The only
 * platform divergence is where per-user data lives, which is a path
 * convention, not an API.
 *
 * COMPILE-UNVERIFIED: written on a Windows host, where it cannot be built.
 * Review it on first Mac/Linux configure.
 */

#include <cstdio>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <cerrno>
#include <dlfcn.h>
#include <dirent.h>

namespace EngineCore
{
namespace
{
    FILE* ToStdio(const FileHandle InHandle)
    {
        return static_cast<FILE*>(InHandle);
    }

    /*
     * appDLLGetLastError's backing store. Captured at the point of failure
     * because dlerror() is consume-on-read: whoever calls it first clears it.
     */
    thread_local std::string GDLLLastError;

    std::string HomeDir()
    {
        if(const char* Home = ::getenv("HOME"))
        {
            return std::string(Home);
        }
        return std::string();
    }
}

FileHandle appFileOpenRead(const std::string& InPath)
{
    // "rb" -- binary, always. Reading SPIR-V as text corrupts it.
    return std::fopen(InPath.c_str(), "rb");
}

FileHandle appFileOpenWrite(const std::string& InPath)
{
    return std::fopen(InPath.c_str(), "wb");
}

size_t appFileRead(const FileHandle InHandle, void* OutDst, const size_t InBytes)
{
    if(!InHandle || !OutDst || InBytes == 0) return 0;
    return std::fread(OutDst, 1, InBytes, ToStdio(InHandle));
}

size_t appFileWrite(const FileHandle InHandle, const void* InSrc, const size_t InBytes)
{
    if(!InHandle || !InSrc || InBytes == 0) return 0;
    return std::fwrite(InSrc, 1, InBytes, ToStdio(InHandle));
}

void appFileClose(const FileHandle InHandle)
{
    if(InHandle)
    {
        std::fclose(ToStdio(InHandle));
    }
}

bool appFileReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    OutBytes.clear();

    struct stat Info{};
    if(::stat(InPath.c_str(), &Info) != 0 || S_ISDIR(Info.st_mode))
    {
        return false;
    }

    const FileHandle H = appFileOpenRead(InPath);
    if(!H) return false;

    if(Info.st_size == 0)
    {
        appFileClose(H);
        return true;        // empty file is a successful read of nothing
    }

    OutBytes.resize(size_t(Info.st_size));
    const size_t Got = appFileRead(H, OutBytes.data(), OutBytes.size());
    appFileClose(H);

    if(Got != OutBytes.size())
    {
        OutBytes.clear();
        return false;
    }
    return true;
}

bool appFileExists(const std::string& InPath)
{
    struct stat Info{};
    return ::stat(InPath.c_str(), &Info) == 0 && !S_ISDIR(Info.st_mode);
}

bool appCreateDirectories(const std::string& InPath)
{
    if(InPath.empty()) return false;

    std::string Path = InPath;
    for(size_t Idx = 1; Idx <= Path.size(); ++Idx)
    {
        if(Idx != Path.size() && Path[Idx] != '/') continue;

        const std::string Partial = Path.substr(0, Idx);
        if(::mkdir(Partial.c_str(), 0755) != 0 && errno != EEXIST)
        {
            return false;
        }
    }

    return true;
}

std::string appGetAppDataDir(const std::string& InAppName)
{
    const std::string Home = HomeDir();
    if(Home.empty()) return std::string();

    std::string Result;
#if defined(__APPLE__)
    Result = Home + "/Library/Application Support";
#else
    if(const char* XdgData = ::getenv("XDG_DATA_HOME"))
    {
        Result = XdgData;
    }
    else
    {
        Result = Home + "/.local/share";
    }
#endif

    if(!InAppName.empty())
    {
        Result += "/";
        Result += InAppName;
    }

    if(!appCreateDirectories(Result))
    {
        return std::string();
    }

    return Result;
}

bool appFileStat(const std::string& InPath, FileStamp& OutStamp)
{
    OutStamp = FileStamp();

    struct stat St{};
    if(::stat(InPath.c_str(), &St) != 0)
    {
        return false;
    }
    if(S_ISDIR(St.st_mode))
    {
        return false;
    }

#if defined(__APPLE__)
    const uint64_t Seconds = uint64_t(St.st_mtimespec.tv_sec);
    const uint64_t Nanos   = uint64_t(St.st_mtimespec.tv_nsec);
#else
    const uint64_t Seconds = uint64_t(St.st_mtim.tv_sec);
    const uint64_t Nanos   = uint64_t(St.st_mtim.tv_nsec);
#endif

    OutStamp.ModifiedTime = Seconds * 1000000000ull + Nanos;
    OutStamp.SizeBytes    = uint64_t(St.st_size);

    // 0 is the seam's "unavailable" value, so a file stamped exactly at the
    // epoch must not read as a failed stat.
    if(OutStamp.ModifiedTime == 0) OutStamp.ModifiedTime = 1;
    return true;
}

bool appFileCopy(const std::string& InSrcPath, const std::string& InDstPath, const bool bInOverwrite)
{
    if(!bInOverwrite && appFileExists(InDstPath))
    {
        return false;
    }

    // Built on the seam's own file functions rather than <filesystem>: the
    // seam exists precisely so the rest of the tree needs neither.
    std::vector<uint8_t> Bytes;
    if(!appFileReadAll(InSrcPath, Bytes))
    {
        return false;
    }

    const FileHandle Dst = appFileOpenWrite(InDstPath);
    if(!Dst)
    {
        return false;
    }

    bool bOk = true;
    if(!Bytes.empty())
    {
        bOk = appFileWrite(Dst, Bytes.data(), Bytes.size()) == Bytes.size();
    }
    appFileClose(Dst);

    if(!bOk)
    {
        appFileDelete(InDstPath);
        return false;
    }

    // Preserve the mode, so copying an executable or a shared object produces
    // something the loader will still accept.
    struct stat St{};
    if(::stat(InSrcPath.c_str(), &St) == 0)
    {
        ::chmod(InDstPath.c_str(), St.st_mode);
    }

    return true;
}

bool appFileDelete(const std::string& InPath)
{
    if(::unlink(InPath.c_str()) == 0)
    {
        return true;
    }
    return errno == ENOENT;      // already gone is the outcome the caller wanted
}

bool appFileCanOpenExclusive(const std::string& InPath)
{
    /*
     * See the PLATFORM ASYMMETRY note in CorePlatform.h. POSIX has no mandatory
     * locking, so there is nothing here to interlock against: a linker writing
     * this file right now does not prevent us opening it. This degenerates to
     * an existence check on purpose, and the settle window in
     * ModuleManager::Tick is what actually guards against a half-written
     * library on this platform.
     */
    return appFileExists(InPath);
}

bool appDirectoryList(const std::string& InPath, std::vector<std::string>& OutNames)
{
    OutNames.clear();

    DIR* Dir = ::opendir(InPath.c_str());
    if(!Dir)
    {
        return false;
    }

    while(const struct dirent* Entry = ::readdir(Dir))
    {
        const std::string Name(Entry->d_name);
        if(Name == "." || Name == "..") continue;
        OutNames.push_back(Name);
    }

    ::closedir(Dir);
    return true;
}

bool appDirectoryDelete(const std::string& InPath)
{
    if(::rmdir(InPath.c_str()) == 0)
    {
        return true;
    }
    return errno == ENOENT;
}

DLLHandle appDLLOpen(const std::string& InPath)
{
    /*
     * RTLD_NOW: an unresolved symbol fails here rather than ambushing the first
     * call into it, which matches Windows and is what makes the manager's
     * "reject and roll back" path able to see the failure at all.
     *
     * RTLD_LOCAL: two generations of the same module must never merge symbol
     * tables. The consequence -- cross-boundary dynamic_cast and typeid on
     * module-defined types become unreliable -- is fine, because the RELOAD
     * HAZARDS block in CoreModule.h forbids both anyway.
     */
    ::dlerror();        // clear any stale condition before the call

    void* Handle = ::dlopen(InPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if(!Handle)
    {
        // Captured immediately: dlerror() is consume-on-read, so deferring this
        // to appDLLGetLastError would reliably return nothing.
        const char* Err = ::dlerror();
        GDLLLastError = Err ? Err : "dlopen failed with no diagnostic";
        return nullptr;
    }

    GDLLLastError.clear();
    return DLLHandle(Handle);
}

void* appDLLGetSymbol(const DLLHandle InHandle, const char* InSymbol)
{
    if(!InHandle || !InSymbol)
    {
        GDLLLastError = "appDLLGetSymbol called with a null handle or symbol";
        return nullptr;
    }

    // A symbol may legitimately resolve to null, so the return value alone
    // cannot report failure -- clear, call, then ask.
    ::dlerror();
    void* Symbol = ::dlsym(InHandle, InSymbol);

    if(const char* Err = ::dlerror())
    {
        GDLLLastError = Err;
        return nullptr;
    }

    GDLLLastError.clear();
    return Symbol;
}

void appDLLClose(const DLLHandle InHandle)
{
    /*
     * KNOWN LINUX CAVEAT: dlclose is advisory. glibc may decline to unmap a
     * library that uses static TLS, in which case the old code stays resident
     * and a subsequent "reload" silently keeps running the previous version.
     * There is no reliable way to detect this from here. Windows has no
     * equivalent problem.
     */
    if(InHandle)
    {
        ::dlclose(InHandle);
    }
}

std::string appDLLGetLastError()
{
    return GDLLLastError;
}

uint32_t appProcessID()
{
    return uint32_t(::getpid());
}

ThreadID appThreadID()
{
#if defined(__APPLE__)
    uint64_t Id = 0;
    ::pthread_threadid_np(nullptr, &Id);
    return ThreadID(Id);
#else
    // pthread_t is an opaque scalar on glibc; reinterpreting it is the
    // conventional way to get a comparable id, and comparison is all this is
    // ever used for.
    return ThreadID(reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(::pthread_self())));
#endif
}
}
