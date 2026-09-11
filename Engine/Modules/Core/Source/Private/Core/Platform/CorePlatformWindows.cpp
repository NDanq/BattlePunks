#include "Public/Core/CorePlatform.h"

/*
 * Windows implementation of the OS-mechanism seam.
 *
 * This is the ONLY file in EngineCore permitted to include <Windows.h> or call
 * a Win32 function. A port to another platform reimplements exactly this
 * surface (see CorePlatformApple.mm / the planned CorePlatformSDL.cpp) and
 * touches nothing else.
 *
 * Paths crossing the seam are UTF-8; every Win32 call below is the wide
 * variant with an explicit conversion, so paths containing non-ASCII survive.
 * The narrow -A entry points would silently mangle them under most codepages.
 */

#include <Windows.h>
#include <shlobj.h>

#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace EngineCore
{
namespace
{
    std::wstring Widen(const std::string& InUtf8)
    {
        if(InUtf8.empty()) return std::wstring();

        const int Needed = ::MultiByteToWideChar(CP_UTF8, 0, InUtf8.data(), int(InUtf8.size()), nullptr, 0);
        if(Needed <= 0) return std::wstring();

        std::wstring Out(size_t(Needed), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, InUtf8.data(), int(InUtf8.size()), Out.data(), Needed);
        return Out;
    }

    std::string Narrow(const std::wstring& InWide)
    {
        if(InWide.empty()) return std::string();

        const int Needed = ::WideCharToMultiByte(CP_UTF8, 0, InWide.data(), int(InWide.size()),
                                                 nullptr, 0, nullptr, nullptr);
        if(Needed <= 0) return std::string();

        std::string Out(size_t(Needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, InWide.data(), int(InWide.size()),
                              Out.data(), Needed, nullptr, nullptr);
        return Out;
    }

    HANDLE ToWin32(const FileHandle InHandle)
    {
        // INVALID_HANDLE_VALUE is (HANDLE)-1, not null, so nullptr is used as
        // the seam's "no handle" value and translated at the boundary.
        return InHandle ? reinterpret_cast<HANDLE>(InHandle) : INVALID_HANDLE_VALUE;
    }

    /*
     * appDLLGetLastError's backing store. Captured at the point of failure
     * rather than read on demand, because GetLastError() is clobbered by the
     * very next Win32 call -- including ones made inside this file.
     */
    thread_local std::string GDLLLastError;

    void CaptureDLLError(const DWORD InErr)
    {
        LPWSTR Buffer = nullptr;
        const DWORD Len = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                           FORMAT_MESSAGE_FROM_SYSTEM     |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                           nullptr, InErr, 0,
                                           reinterpret_cast<LPWSTR>(&Buffer), 0, nullptr);

        if(Len && Buffer)
        {
            // FormatMessage leaves a trailing CRLF on nearly every message.
            size_t Trimmed = Len;
            while(Trimmed > 0 && (Buffer[Trimmed - 1] == L'\r' || Buffer[Trimmed - 1] == L'\n'))
            {
                --Trimmed;
            }
            GDLLLastError = Narrow(std::wstring(Buffer, Trimmed));
        }
        else
        {
            GDLLLastError = "Win32 error " + std::to_string(InErr);
        }

        if(Buffer) ::LocalFree(Buffer);
    }
}

FileHandle appFileOpenRead(const std::string& InPath)
{
    const HANDLE H = ::CreateFileW(Widen(InPath).c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    return (H == INVALID_HANDLE_VALUE) ? nullptr : reinterpret_cast<FileHandle>(H);
}

FileHandle appFileOpenWrite(const std::string& InPath)
{
    const HANDLE H = ::CreateFileW(Widen(InPath).c_str(),
                                   GENERIC_WRITE,
                                   0,
                                   nullptr,
                                   CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    return (H == INVALID_HANDLE_VALUE) ? nullptr : reinterpret_cast<FileHandle>(H);
}

size_t appFileRead(const FileHandle InHandle, void* OutDst, const size_t InBytes)
{
    if(!InHandle || !OutDst || InBytes == 0) return 0;

    size_t Total = 0;
    uint8_t* Dst = static_cast<uint8_t*>(OutDst);

    // ReadFile is capped at a DWORD per call; loop so >4GB reads are not
    // silently truncated.
    while(Total < InBytes)
    {
        const size_t Remaining = InBytes - Total;
        const DWORD  Chunk     = DWORD((Remaining > 0xFFFFFFFFull) ? 0xFFFFFFFFull : Remaining);

        DWORD Got = 0;
        if(!::ReadFile(ToWin32(InHandle), Dst + Total, Chunk, &Got, nullptr) || Got == 0)
        {
            break;      // error, or clean end of file
        }
        Total += Got;
    }

    return Total;
}

size_t appFileWrite(const FileHandle InHandle, const void* InSrc, const size_t InBytes)
{
    if(!InHandle || !InSrc || InBytes == 0) return 0;

    size_t Total = 0;
    const uint8_t* Src = static_cast<const uint8_t*>(InSrc);

    while(Total < InBytes)
    {
        const size_t Remaining = InBytes - Total;
        const DWORD  Chunk     = DWORD((Remaining > 0xFFFFFFFFull) ? 0xFFFFFFFFull : Remaining);

        DWORD Put = 0;
        if(!::WriteFile(ToWin32(InHandle), Src + Total, Chunk, &Put, nullptr) || Put == 0)
        {
            break;
        }
        Total += Put;
    }

    return Total;
}

void appFileClose(const FileHandle InHandle)
{
    if(InHandle)
    {
        ::CloseHandle(ToWin32(InHandle));
    }
}

bool appFileReadAll(const std::string& InPath, std::vector<uint8_t>& OutBytes)
{
    OutBytes.clear();

    const FileHandle H = appFileOpenRead(InPath);
    if(!H) return false;

    LARGE_INTEGER FileSize{};
    if(!::GetFileSizeEx(ToWin32(H), &FileSize) || FileSize.QuadPart < 0)
    {
        appFileClose(H);
        return false;
    }

    if(FileSize.QuadPart == 0)
    {
        appFileClose(H);
        return true;        // empty file is a successful read of nothing
    }

    OutBytes.resize(size_t(FileSize.QuadPart));
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
    const DWORD Attribs = ::GetFileAttributesW(Widen(InPath).c_str());
    return Attribs != INVALID_FILE_ATTRIBUTES && !(Attribs & FILE_ATTRIBUTE_DIRECTORY);
}

bool appCreateDirectories(const std::string& InPath)
{
    if(InPath.empty()) return false;

    std::wstring Path = Widen(InPath);
    for(wchar_t& Ch : Path)
    {
        if(Ch == L'/') Ch = L'\\';
    }

    // Walk the path creating each missing component. SHCreateDirectoryExW
    // would do this in one call but rejects relative paths, which the pipeline
    // cache and the smoke tests both use.
    for(size_t Idx = 0; Idx <= Path.size(); ++Idx)
    {
        if(Idx != Path.size() && Path[Idx] != L'\\') continue;
        if(Idx == 0) continue;

        const std::wstring Partial = Path.substr(0, Idx);

        // Skip a bare drive specifier ("C:"), which is not creatable.
        if(Partial.size() == 2 && Partial[1] == L':') continue;

        if(!::CreateDirectoryW(Partial.c_str(), nullptr))
        {
            const DWORD Err = ::GetLastError();
            if(Err != ERROR_ALREADY_EXISTS)
            {
                return false;
            }
        }
    }

    return true;
}

std::string appGetAppDataDir(const std::string& InAppName)
{
    PWSTR RoamingPath = nullptr;
    if(FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &RoamingPath)))
    {
        if(RoamingPath) ::CoTaskMemFree(RoamingPath);
        return std::string();
    }

    std::string Result = Narrow(RoamingPath);
    ::CoTaskMemFree(RoamingPath);

    if(!InAppName.empty())
    {
        Result += "\\";
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

    WIN32_FILE_ATTRIBUTE_DATA Data{};
    if(!::GetFileAttributesExW(Widen(InPath).c_str(), GetFileExInfoStandard, &Data))
    {
        return false;
    }
    if(Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        return false;
    }

    // One call for both fields. Two getters would be two syscalls and, worse,
    // could observe the file mid-write between them -- the pair has to be
    // sampled atomically for it to work as a change signature.
    OutStamp.ModifiedTime = (uint64_t(Data.ftLastWriteTime.dwHighDateTime) << 32)
                          |  uint64_t(Data.ftLastWriteTime.dwLowDateTime);
    OutStamp.SizeBytes    = (uint64_t(Data.nFileSizeHigh) << 32) | uint64_t(Data.nFileSizeLow);

    // A stamp of 0 means "unavailable" to callers, and a real FILETIME is never
    // 0 for an existing file. Guard anyway so a bizarre filesystem cannot make
    // a successful stat look like a failed one.
    if(OutStamp.ModifiedTime == 0) OutStamp.ModifiedTime = 1;
    return true;
}

bool appFileCopy(const std::string& InSrcPath, const std::string& InDstPath, const bool bInOverwrite)
{
    // CopyFileW preserves the source's last-write time, which the module
    // manager relies on: it re-stats the SOURCE after copying to detect a build
    // that started mid-copy, and that comparison only means anything if the
    // copy did not perturb the thing being compared.
    return ::CopyFileW(Widen(InSrcPath).c_str(), Widen(InDstPath).c_str(), !bInOverwrite) != 0;
}

bool appFileDelete(const std::string& InPath)
{
    if(::DeleteFileW(Widen(InPath).c_str()))
    {
        return true;
    }
    // Already gone is the outcome the caller wanted.
    return ::GetLastError() == ERROR_FILE_NOT_FOUND || ::GetLastError() == ERROR_PATH_NOT_FOUND;
}

bool appFileCanOpenExclusive(const std::string& InPath)
{
    const HANDLE H = ::CreateFileW(Widen(InPath).c_str(),
                                   GENERIC_READ,
                                   0,                   // dwShareMode 0 -- the whole point
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if(H == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    ::CloseHandle(H);
    return true;
}

bool appDirectoryList(const std::string& InPath, std::vector<std::string>& OutNames)
{
    OutNames.clear();

    std::wstring Pattern = Widen(InPath);
    if(!Pattern.empty() && Pattern.back() != L'\\' && Pattern.back() != L'/')
    {
        Pattern += L'\\';
    }
    Pattern += L'*';

    WIN32_FIND_DATAW Found{};
    const HANDLE Search = ::FindFirstFileW(Pattern.c_str(), &Found);
    if(Search == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    do
    {
        const std::wstring Name(Found.cFileName);
        if(Name == L"." || Name == L"..") continue;
        OutNames.push_back(Narrow(Name));
    }
    while(::FindNextFileW(Search, &Found));

    ::FindClose(Search);
    return true;
}

bool appDirectoryDelete(const std::string& InPath)
{
    if(::RemoveDirectoryW(Widen(InPath).c_str()))
    {
        return true;
    }
    const DWORD Err = ::GetLastError();
    return Err == ERROR_FILE_NOT_FOUND || Err == ERROR_PATH_NOT_FOUND;
}

DLLHandle appDLLOpen(const std::string& InPath)
{
    /*
     * NO LOAD_WITH_ALTERED_SEARCH_PATH -- read before "fixing" this.
     *
     * A hot-reloaded module is loaded from a shadow copy in a subdirectory, but
     * it imports EngineCore.dll, which sits next to the EXE. The DEFAULT search
     * order includes the directory the application was loaded from, so the
     * import resolves. LOAD_WITH_ALTERED_SEARCH_PATH replaces that entry with
     * the shadow directory, where EngineCore.dll is not, and the load fails
     * with a message that says nothing about search paths.
     */
    const HMODULE Module = ::LoadLibraryW(Widen(InPath).c_str());
    if(!Module)
    {
        CaptureDLLError(::GetLastError());
        return nullptr;
    }

    GDLLLastError.clear();
    return reinterpret_cast<DLLHandle>(Module);
}

void* appDLLGetSymbol(const DLLHandle InHandle, const char* InSymbol)
{
    if(!InHandle || !InSymbol)
    {
        GDLLLastError = "appDLLGetSymbol called with a null handle or symbol";
        return nullptr;
    }

    // GetProcAddress returns FARPROC (a function pointer); the cast through
    // void* is the conversion the seam's opaque return type requires and is
    // well-defined on every platform this targets.
    FARPROC Proc = ::GetProcAddress(reinterpret_cast<HMODULE>(InHandle), InSymbol);
    if(!Proc)
    {
        CaptureDLLError(::GetLastError());
        return nullptr;
    }

    GDLLLastError.clear();
    return reinterpret_cast<void*>(Proc);
}

void appDLLClose(const DLLHandle InHandle)
{
    if(InHandle)
    {
        ::FreeLibrary(reinterpret_cast<HMODULE>(InHandle));
    }
}

std::string appDLLGetLastError()
{
    return GDLLLastError;
}

uint32_t appProcessID()
{
    return uint32_t(::GetCurrentProcessId());
}

ThreadID appThreadID()
{
    return ThreadID(::GetCurrentThreadId());
}

// === Thread attributes ======================================================

namespace
{
    /*
     * SetThreadDescription is Windows 10 1607 and newer, and resolving it at
     * runtime rather than linking it is not defensiveness for its own sake: an
     * import of a symbol the running OS does not export makes the PROCESS fail
     * to start, with a loader error box and no log line. A missing thread name
     * must degrade to the legacy path below, not to an unlaunchable engine.
     *
     * The function-local static is a CACHE, not a singleton -- it owns nothing
     * and has no destructor, so the static-ordering objection CoreConfig.h and
     * CoreModuleManager.h raise against Meyers singletons does not apply here.
     */
    using SetThreadDescriptionFn = HRESULT (WINAPI*)(HANDLE, PCWSTR);

    SetThreadDescriptionFn ResolveSetThreadDescription()
    {
        static const SetThreadDescriptionFn Resolved = []() -> SetThreadDescriptionFn
        {
            // GetModuleHandle, not LoadLibrary: kernel32 is already mapped into
            // every process, and this must not take a reference on it.
            const HMODULE Kernel32 = ::GetModuleHandleW(L"kernel32.dll");
            if(!Kernel32)
            {
                return nullptr;
            }

            // Through void* because GetProcAddress returns FARPROC, and casting
            // that straight to another function pointer type warns at /W4.
            return reinterpret_cast<SetThreadDescriptionFn>(
                       reinterpret_cast<void*>(::GetProcAddress(Kernel32, "SetThreadDescription")));
        }();

        return Resolved;
    }

    /*
     * The pre-1607 mechanism: raise a magic exception that only a debugger
     * looks at.
     *
     * ONLY AN ATTACHED DEBUGGER EVER SEES THIS. The name is not stored anywhere
     * the OS can report, so Task Manager, a profiler that attaches later, and a
     * crash dump all still show a bare thread id. That asymmetry is why
     * SetThreadDescription exists, and why this is the fallback rather than the
     * implementation.
     *
     * With no debugger attached nobody handles the exception and the __except
     * swallows it, which is what makes it safe to call unconditionally.
     */
    void LegacySetThreadName(const char* InName)
    {
        const DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push, 8)
        struct THREADNAME_INFO
        {
            DWORD  dwType;      // must be 0x1000
            LPCSTR szName;
            DWORD  dwThreadID;  // -1 means the calling thread
            DWORD  dwFlags;     // must be 0
        };
#pragma pack(pop)

        THREADNAME_INFO Info;
        Info.dwType     = 0x1000;
        Info.szName     = InName;
        Info.dwThreadID = DWORD(-1);
        Info.dwFlags    = 0;

        /* Nothing with a destructor may live in this scope -- __try/__except and
         * unwinding do not mix. THREADNAME_INFO is a POD and InName is borrowed,
         * so there is nothing to unwind. */
        __try
        {
            ::RaiseException(MS_VC_EXCEPTION, 0,
                             sizeof(Info) / sizeof(ULONG_PTR),
                             reinterpret_cast<const ULONG_PTR*>(&Info));
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

void appThreadSetName(const char* InName)
{
    if(!InName || !*InName)
    {
        return;
    }

    if(const SetThreadDescriptionFn Setter = ResolveSetThreadDescription())
    {
        const std::wstring Wide = Widen(std::string(InName));
        if(!Wide.empty())
        {
            // GetCurrentThread() is a pseudo-handle meaning "the calling
            // thread". It needs no close, and it is the right thing to pass.
            Setter(::GetCurrentThread(), Wide.c_str());
        }
    }

    /* Unconditionally, not as an else. The two mechanisms are read by different
     * tools -- SetThreadDescription by Task Manager, WPA and a dump; the
     * exception by a debugger attached right now -- and Visual Studio's thread
     * window still prefers the legacy name in some configurations. Naming twice
     * costs one swallowed exception per thread, once. */
    LegacySetThreadName(InName);
}

bool appThreadSetAffinity(const uint64_t InMask)
{
    if(InMask == 0)
    {
        return false;
    }

    /*
     * DWORD_PTR is 32 bits in a 32-bit process, so a mask naming hardware
     * thread 32 or above would be truncated to a DIFFERENT set of cores.
     * Refusing is the only honest answer: a caller who asked for cores 32-39
     * and silently got 0-7 has been actively misled.
     */
    if(sizeof(DWORD_PTR) < sizeof(uint64_t) && (InMask >> 32) != 0)
    {
        return false;
    }

    /*
     * Ignores processor groups, and therefore means "group 0" on a machine with
     * more than 64 logical processors. CorePlatform.h states that ceiling; group
     * 0 is what "no affinity available here" looks like on such a machine.
     */
    return ::SetThreadAffinityMask(::GetCurrentThread(), DWORD_PTR(InMask)) != 0;
}

bool appThreadSetPriority(const uint8_t InPriority)
{
    int Win = THREAD_PRIORITY_NORMAL;

    switch(InPriority)
    {
        /*
         * LOWEST, NOT THREAD_PRIORITY_IDLE. Idle runs only when nothing else in
         * the system wants a core, so a file watcher there can be starved
         * indefinitely on a busy machine and hot reload simply stops noticing
         * rebuilds. Background means "yield to the frame", not "never run".
         */
        case 0: Win = THREAD_PRIORITY_LOWEST;  break;

        case 1: Win = THREAD_PRIORITY_NORMAL;  break;

        /*
         * HIGHEST, NOT TIME_CRITICAL. Time-critical outranks much of the
         * kernel's own work: a spin at that priority does not merely cost this
         * process, it stops the desktop responding, and recovering can mean
         * power-cycling the machine. Nothing in an engine needs it.
         */
        case 2: Win = THREAD_PRIORITY_HIGHEST; break;

        default: return false;
    }

    return ::SetThreadPriority(::GetCurrentThread(), Win) != FALSE;
}

uint32_t appThreadHardwareCount()
{
    /*
     * ALL_PROCESSOR_GROUPS, so this reports every logical processor on a
     * >64-thread machine rather than only the 64 in the calling thread's group.
     *
     * DELIBERATELY INCONSISTENT with appThreadSetAffinity, which cannot name
     * those cores. The count is used for SIZING a pool, where being truthful is
     * right; affinity is used for PINNING, where being unable to name a core
     * means declining.
     */
    if(const DWORD Count = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))
    {
        return uint32_t(Count);
    }

    SYSTEM_INFO Info;
    ::GetSystemInfo(&Info);
    return Info.dwNumberOfProcessors ? uint32_t(Info.dwNumberOfProcessors) : 1u;
}

uint32_t appThreadPhysicalCoreCount()
{
    DWORD Bytes = 0;

    /* The sizing call is EXPECTED to fail with ERROR_INSUFFICIENT_BUFFER -- that
     * is how this API reports the length it needs. Any other failure is real. */
    if(::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &Bytes) == FALSE
       && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER
       && Bytes > 0)
    {
        /* The size goes through a named variable because
         * `std::vector<uint8_t> Buffer(size_t(Bytes));` is the most vexing
         * parse: `size_t(Bytes)` reads as a parameter declaration, so that line
         * declares a FUNCTION called Buffer and every use of it below fails with
         * an error that points nowhere near the cause. */
        const size_t          BufferBytes = Bytes;
        std::vector<uint8_t>  Buffer(BufferBytes);

        if(::GetLogicalProcessorInformationEx(
               RelationProcessorCore,
               reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Buffer.data()),
               &Bytes) != FALSE)
        {
            uint32_t Cores  = 0;
            DWORD    Offset = 0;

            /*
             * Variable-length records, walked by each record's own Size.
             *
             * THE BOUND IS THE RECORD HEADER, NOT sizeof THE UNION, and getting
             * that wrong costs exactly one core while looking entirely correct.
             * A RelationProcessorCore record is SMALLER than
             * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX), whose size is
             * dominated by its largest arm (the GROUP_AFFINITY array in the
             * package and cache relations). Requiring a whole union to remain in
             * the buffer therefore drops the FINAL record, and a 6-core part
             * reports 5 -- verified against Win32_Processor.NumberOfCores on an
             * i7-8700K, which is the only way this class of bug gets noticed.
             *
             * So the loop only requires the Relationship + Size prefix, which is
             * what has to be readable before Size can be trusted at all.
             */
            const DWORD PrefixBytes = DWORD(sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD));

            while(Offset + PrefixBytes <= Bytes)
            {
                const auto* Info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                                       Buffer.data() + Offset);

                /* A zero Size would spin forever and an oversized one would read
                 * past the buffer. Neither should ever happen; both stop the walk
                 * rather than trusting the OS to be well-formed. Subtracting
                 * rather than adding keeps the comparison free of overflow. */
                if(Info->Size == 0 || Info->Size > Bytes - Offset)
                {
                    break;
                }

                if(Info->Relationship == RelationProcessorCore)
                {
                    ++Cores;
                }

                Offset += Info->Size;
            }

            if(Cores > 0)
            {
                return Cores;
            }
        }
    }

    /* Degrading to the logical count over-reports on a hyperthreaded part, which
     * sizes a pool too large rather than down to a single thread. Per
     * CorePlatform.h, neither of these may ever return 0. */
    return appThreadHardwareCount();
}

// === Virtual memory =========================================================

size_t appMemPageSize()
{
    /*
     * dwPageSize, NOT dwAllocationGranularity.
     *
     * The two differ on Windows -- 4KB against 64KB -- and they answer
     * different questions. Granularity is the spacing VirtualAlloc will place
     * separate RESERVATIONS at; page size is the unit COMMIT operates in.
     * Callers here use this to round commit ranges, so it is the page size they
     * need. Rounding commits to 64KB instead would commit up to 60KB of
     * untouched memory per grow, which is precisely the waste reserve/commit
     * exists to avoid.
     */
    SYSTEM_INFO Info;
    ::GetSystemInfo(&Info);
    return size_t(Info.dwPageSize);
}

void* appMemReserve(const size_t InBytes)
{
    if(InBytes == 0)
    {
        return nullptr;
    }

    // MEM_RESERVE alone: address space, no pages, no backing store charged.
    // PAGE_NOACCESS because a reserved-but-uncommitted page is unreadable
    // anyway -- naming the protection here documents intent and costs nothing;
    // the real protection is set by the commit below.
    return ::VirtualAlloc(nullptr, InBytes, MEM_RESERVE, PAGE_NOACCESS);
}

bool appMemCommit(void* InBase, const size_t InBytes)
{
    if(!InBase || InBytes == 0)
    {
        return false;
    }

    /*
     * VirtualAlloc rounds the range out to page boundaries itself, so no
     * arithmetic is needed here to honour the header's "rounded up internally"
     * promise. It also succeeds on an already-committed range rather than
     * failing, which is what lets a pool commit greedily.
     */
    return ::VirtualAlloc(InBase, InBytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool appMemDecommit(void* InBase, const size_t InBytes)
{
    if(!InBase || InBytes == 0)
    {
        return false;
    }

    // MEM_DECOMMIT, never MEM_RELEASE: the address range must stay reserved so
    // that a later commit gets the same addresses back. MEM_RELEASE here would
    // hand the range to the next unrelated reservation in the process and turn
    // every pointer the pool handed out into a pointer at somebody else's data.
    return ::VirtualFree(InBase, InBytes, MEM_DECOMMIT) != FALSE;
}

void appMemRelease(void* InBase, const size_t /*InBytes*/)
{
    if(InBase)
    {
        // MEM_RELEASE requires a size of 0 and the exact base returned by the
        // reservation -- passing the real length is an error on Windows. The
        // parameter is still in the signature because munmap needs it; see the
        // PAIRING RULE in CorePlatform.h.
        ::VirtualFree(InBase, 0, MEM_RELEASE);
    }
}
}
