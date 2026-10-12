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
#include <sys/mman.h>
#include <sys/resource.h>   // setpriority -- the only SCHED_OTHER priority Linux has
#include <unistd.h>
#include <pthread.h>
#include <sched.h>          // cpu_set_t, sched_get_priority_min/max
#include <signal.h>         // kill(pid, 0): is it running
#include <cerrno>
#include <dlfcn.h>
#include <dirent.h>

#if defined(__APPLE__)
    #include <sys/sysctl.h> // hw.physicalcpu
#endif

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

bool appFileReadRange(const std::string& InPath, const uint64_t InOffset, const uint64_t InSize, std::vector<uint8_t>& OutBytes)
{
    OutBytes.clear();
    const FileHandle H = appFileOpenRead(InPath);
    if(!H) return false;
    if(::fseeko(ToStdio(H), off_t(InOffset), SEEK_SET) != 0)
    {
        appFileClose(H);
        return false;
    }
    OutBytes.resize(size_t(InSize));
    const size_t Got = InSize == 0 ? 0 : appFileRead(H, OutBytes.data(), OutBytes.size());
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

bool appProcessIsRunning(const uint32_t InPid)
{
    /* Signal 0 checks without sending; EPERM means it exists, someone else's. */
    return ::kill(pid_t(InPid), 0) == 0 || errno == EPERM;
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

// === Thread attributes ======================================================

void appThreadSetName(const char* InName)
{
    if(!InName || !*InName)
    {
        return;
    }

    /*
     * TRUNCATED TO 15 CHARACTERS PLUS THE NULL, because Linux rejects the whole
     * call with ERANGE if the name is longer -- so an over-long name does not
     * merely get clipped, it produces a thread with NO name at all. Truncating
     * here makes the failure impossible instead of silent. macOS allows longer
     * names and is truncated identically, so a thread has the same name on both
     * platforms and a log line from one matches a capture from the other.
     */
    char Truncated[16];
    size_t Len = 0;
    while(Len < sizeof(Truncated) - 1 && InName[Len] != '\0')
    {
        Truncated[Len] = InName[Len];
        ++Len;
    }
    Truncated[Len] = '\0';

#if defined(__APPLE__)
    /* Apple's takes no thread argument: it can only ever name the caller. That
     * restriction is the reason CorePlatform.h specifies this seam as
     * calling-thread-only for everyone. */
    ::pthread_setname_np(Truncated);
#else
    ::pthread_setname_np(::pthread_self(), Truncated);
#endif
}

bool appThreadSetAffinity(const uint64_t InMask)
{
    if(InMask == 0)
    {
        return false;
    }

#if defined(__linux__)
    /* cpu_set_t and pthread_setaffinity_np need _GNU_SOURCE, which g++ and
     * clang++ define for themselves when compiling C++ against libstdc++. No
     * CMake flag is required; if that ever stops being true, this is where it
     * breaks and the fix is a target_compile_definitions on the platform file. */
    cpu_set_t Set;
    CPU_ZERO(&Set);

    for(int Bit = 0; Bit < 64; ++Bit)
    {
        if(InMask & (uint64_t(1) << Bit))
        {
            /* CPU_SETSIZE is 1024 on glibc, so a 64-bit mask cannot overflow the
             * set. The 64-thread ceiling here is CorePlatform.h's, not the
             * platform's -- Linux could express more and this cannot say it. */
            CPU_SET(Bit, &Set);
        }
    }

    return ::pthread_setaffinity_np(::pthread_self(), sizeof(Set), &Set) == 0;
#else
    /*
     * macOS HAS NO THREAD AFFINITY API. thread_policy_set with
     * THREAD_AFFINITY_POLICY expresses "these threads share a cache", not "run
     * on core N", and it is a no-op on Apple silicon entirely. Returning false
     * is the honest answer; pretending otherwise would have a caller believe it
     * had pinned something.
     */
    (void)InMask;
    return false;
#endif
}

bool appThreadSetPriority(const uint8_t InPriority)
{
    if(InPriority > 2)
    {
        return false;
    }

#if defined(__APPLE__)
    /*
     * macOS honours a real priority range within SCHED_OTHER, so the ladder maps
     * onto min/normal/max directly.
     */
    int          Policy = 0;
    sched_param  Param{};
    if(::pthread_getschedparam(::pthread_self(), &Policy, &Param) != 0)
    {
        return false;
    }

    const int Min = ::sched_get_priority_min(Policy);
    const int Max = ::sched_get_priority_max(Policy);
    if(Min < 0 || Max < 0)
    {
        return false;
    }

    switch(InPriority)
    {
        case 0: Param.sched_priority = Min;                 break;
        case 1: Param.sched_priority = Min + (Max - Min) / 2; break;
        case 2: Param.sched_priority = Max;                 break;
        default: return false;
    }

    return ::pthread_setschedparam(::pthread_self(), Policy, &Param) == 0;
#else
    /*
     * LINUX IS NOT SYMMETRIC AND CANNOT BE MADE SO.
     *
     * Under SCHED_OTHER -- which is what every thread in an engine runs unless
     * someone has gone out of their way -- sched_get_priority_min and _max both
     * return 0. There is exactly one valid value, so pthread_setschedparam
     * cannot express this ladder at all. What Linux actually exposes for
     * SCHED_OTHER is the nice value.
     *
     * So:
     *   Background -> nice +5 via setpriority, which on Linux (and ONLY on
     *                 Linux) applies to the calling THREAD when who is 0,
     *                 because a thread is a process to that syscall.
     *   Normal     -> nothing to do, and reporting success is correct: the
     *                 thread already has the priority that was asked for.
     *   High       -> refused. Lowering nice needs CAP_SYS_NICE or a raised
     *                 RLIMIT_NICE, which a game will not have, and a real-time
     *                 policy (SCHED_FIFO) is a much larger decision than a
     *                 priority hint -- a spinning SCHED_FIFO thread can lock up
     *                 a machine that has no other runnable core.
     *
     * Returning false for High is the point of the bool. A caller that must have
     * it can see that it did not get it.
     */
    switch(InPriority)
    {
        case 0:
            errno = 0;
            if(::setpriority(PRIO_PROCESS, 0, 5) != 0 && errno != 0)
            {
                return false;
            }
            return true;

        case 1:
            return true;

        default:
            return false;
    }
#endif
}

uint32_t appThreadHardwareCount()
{
    /* _SC_NPROCESSORS_ONLN, not _CONF: online counts the processors usable right
     * now, configured counts the ones that physically exist including any the
     * kernel has taken offline. A pool sized to offline cores oversubscribes the
     * ones that remain. */
    const long Count = ::sysconf(_SC_NPROCESSORS_ONLN);
    return Count > 0 ? uint32_t(Count) : 1u;
}

namespace
{
#if !defined(__APPLE__)
    /*
     * Reads a whole small file. NOT appFileReadAll, deliberately: that one sizes
     * its buffer from stat(), and EVERY FILE IN sysfs REPORTS st_size 0 -- so it
     * would return an empty success for all of these and the core count would
     * silently degrade to the logical count. Reading until EOF is the only thing
     * that works on a synthetic filesystem.
     */
    bool ReadSmallTextFile(const char* InPath, std::string& OutText)
    {
        OutText.clear();

        FILE* F = ::fopen(InPath, "rb");
        if(!F)
        {
            return false;
        }

        char   Buffer[256];
        size_t Got = 0;
        while((Got = ::fread(Buffer, 1, sizeof(Buffer), F)) > 0)
        {
            OutText.append(Buffer, Got);
        }

        ::fclose(F);
        return true;
    }
#endif
}

uint32_t appThreadPhysicalCoreCount()
{
#if defined(__APPLE__)
    /* Apple just answers the question. */
    int    Cores = 0;
    size_t Size  = sizeof(Cores);
    if(::sysctlbyname("hw.physicalcpu", &Cores, &Size, nullptr, 0) == 0 && Cores > 0)
    {
        return uint32_t(Cores);
    }
    return appThreadHardwareCount();
#else
    /*
     * Linux has no syscall for this, so it is counted from sysfs topology.
     *
     * THE TRICK: every logical processor on one physical core reports an
     * IDENTICAL thread_siblings_list, so the number of DISTINCT lists is the
     * number of physical cores. That is more robust than counting distinct
     * core_id values, which are only unique within a package and would
     * under-count a two-socket machine.
     *
     * /proc/cpuinfo is the other common approach and is worse: its format
     * differs by architecture, and on some it omits the core id entirely.
     *
     * Linear scan over a vector of strings to deduplicate, because Core scans
     * linearly everywhere (there is no std::map in this module) and this runs
     * once at startup, not in a frame.
     */
    const uint32_t Logical = appThreadHardwareCount();

    std::vector<std::string> Distinct;
    Distinct.reserve(Logical);

    for(uint32_t Cpu = 0; Cpu < Logical; ++Cpu)
    {
        char Path[128];
        ::snprintf(Path, sizeof(Path),
                   "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", Cpu);

        std::string List;
        if(!ReadSmallTextFile(Path, List) || List.empty())
        {
            /* Topology unavailable -- a container with a restricted /sys, or an
             * offline cpu leaving a hole in the numbering. Give up on the whole
             * computation rather than return a count derived from a partial
             * walk, which would be confidently wrong. */
            return Logical;
        }

        bool bSeen = false;
        for(const std::string& Existing : Distinct)
        {
            if(Existing == List)
            {
                bSeen = true;
                break;
            }
        }

        if(!bSeen)
        {
            Distinct.push_back(List);
        }
    }

    return Distinct.empty() ? Logical : uint32_t(Distinct.size());
#endif
}

// === Virtual memory =========================================================

namespace
{
    size_t PageSizeCached()
    {
        // sysconf is a syscall on some libcs and this is called on every
        // commit; caching it is safe because the page size cannot change
        // during a process's lifetime.
        static const size_t Cached = size_t(::sysconf(_SC_PAGESIZE));
        return Cached;
    }

    /* Rounds [InBase, InBase+InBytes) OUT to whole pages: base down, end up.
     * mprotect and madvise both require a page-aligned base and reject
     * anything else with EINVAL, which is the difference from Windows -- there
     * VirtualAlloc does this rounding itself. */
    void PageAlignRange(void*& InOutBase, size_t& InOutBytes)
    {
        const size_t    Page  = PageSizeCached();
        const uintptr_t Start = reinterpret_cast<uintptr_t>(InOutBase);
        const uintptr_t End   = Start + InOutBytes;

        const uintptr_t AlignedStart = Start & ~uintptr_t(Page - 1);
        const uintptr_t AlignedEnd   = (End + Page - 1) & ~uintptr_t(Page - 1);

        InOutBase  = reinterpret_cast<void*>(AlignedStart);
        InOutBytes = size_t(AlignedEnd - AlignedStart);
    }
}

size_t appMemPageSize()
{
    return PageSizeCached();
}

void* appMemReserve(const size_t InBytes)
{
    if(InBytes == 0)
    {
        return nullptr;
    }

    /*
     * PROT_NONE is what makes this a reservation rather than an allocation.
     * With MAP_NORESERVE it also keeps the kernel from charging the range
     * against the commit limit on Linux; on macOS the flag is accepted and
     * ignored, and PROT_NONE alone already prevents any page from being
     * backed.
     *
     * MAP_ANONYMOUS | MAP_PRIVATE with fd -1 is the portable "just give me
     * address space" spelling. Some older systems spell the flag MAP_ANON;
     * both have meant the same thing on Linux and macOS for many years.
     */
    void* Base = ::mmap(nullptr, InBytes, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    // mmap signals failure with MAP_FAILED ((void*)-1), NOT nullptr. Returning
    // it unchecked would hand callers a pointer that passes an `if(Base)` test
    // and faults on first use.
    return (Base == MAP_FAILED) ? nullptr : Base;
}

bool appMemCommit(void* InBase, const size_t InBytes)
{
    if(!InBase || InBytes == 0)
    {
        return false;
    }

    void*  Base  = InBase;
    size_t Bytes = InBytes;
    PageAlignRange(Base, Bytes);

    // Committing on POSIX is granting access to a range that is already mapped
    // PROT_NONE: the kernel faults pages in on first touch, zero-filled. There
    // is no separate "commit" call, and re-mprotecting an already-readable
    // range is a legal no-op -- which is the same greedy-commit property the
    // Windows implementation has.
    return ::mprotect(Base, Bytes, PROT_READ | PROT_WRITE) == 0;
}

bool appMemDecommit(void* InBase, const size_t InBytes)
{
    if(!InBase || InBytes == 0)
    {
        return false;
    }

    void*  Base  = InBase;
    size_t Bytes = InBytes;
    PageAlignRange(Base, Bytes);

    /*
     * MADV_DONTNEED first, then mprotect back to PROT_NONE.
     *
     * ORDER MATTERS AND SO DOES DOING BOTH. mprotect alone makes the range
     * inaccessible but leaves the physical pages charged to the process, so a
     * pool that decommits to release memory would release nothing. madvise
     * alone drops the pages but leaves the range writable, so a stale pointer
     * silently faults a fresh zero page in instead of crashing.
     *
     * PLATFORM ASYMMETRY -- read before relying on the "contents are lost"
     * half of the contract. On Linux MADV_DONTNEED guarantees a subsequent
     * read returns zero. On macOS it is advisory and the kernel MAY keep the
     * pages, so contents may survive. That is exactly why CorePlatform.h
     * specifies the contents as LOST rather than as ZERO: relying on either
     * outcome is a bug, and only Linux would catch it.
     */
    ::madvise(Base, Bytes, MADV_DONTNEED);
    return ::mprotect(Base, Bytes, PROT_NONE) == 0;
}

void appMemRelease(void* InBase, const size_t InBytes)
{
    if(InBase && InBytes > 0)
    {
        // munmap needs the length -- this is the half of the seam's PAIRING
        // RULE that Windows does not care about and POSIX cannot do without.
        ::munmap(InBase, InBytes);
    }
}
}
