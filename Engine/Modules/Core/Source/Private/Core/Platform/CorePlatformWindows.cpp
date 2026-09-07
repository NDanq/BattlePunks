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

ThreadID appThreadID()
{
    return ThreadID(::GetCurrentThreadId());
}
}
