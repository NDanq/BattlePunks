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

namespace EngineCore
{
namespace
{
    FILE* ToStdio(const FileHandle InHandle)
    {
        return static_cast<FILE*>(InHandle);
    }

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
