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
