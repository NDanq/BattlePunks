#include "Private/Collab/CollabProcess.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <cstdlib>
    #include <cstring>
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <mach-o/dyld.h>
    #endif
    extern char** environ;
#endif

namespace Collab
{
#if defined(_WIN32)
namespace
{
    std::wstring Widen(const std::string& InText)
    {
        if(InText.empty())
        {
            return std::wstring();
        }
        const int Length = MultiByteToWideChar(CP_UTF8, 0, InText.data(), int(InText.size()), nullptr, 0);
        std::wstring Out(size_t(Length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, InText.data(), int(InText.size()), Out.data(), Length);
        return Out;
    }

    std::string Narrow(const std::wstring& InText)
    {
        if(InText.empty())
        {
            return std::string();
        }
        const int Length = WideCharToMultiByte(CP_UTF8, 0, InText.data(), int(InText.size()), nullptr, 0, nullptr, nullptr);
        std::string Out(size_t(Length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, InText.data(), int(InText.size()), Out.data(), Length, nullptr, nullptr);
        return Out;
    }

    /* One argument, quoted as CommandLineToArgvW reads it back. */
    void AppendQuoted(std::wstring& InOutLine, const std::wstring& InArg)
    {
        if(!InArg.empty() && InArg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        {
            InOutLine += InArg;
            return;
        }
        InOutLine += L'"';
        for(size_t Index = 0;; ++Index)
        {
            size_t Slashes = 0;
            while(Index < InArg.size() && InArg[Index] == L'\\')
            {
                ++Index;
                ++Slashes;
            }
            if(Index == InArg.size())
            {
                InOutLine.append(Slashes * 2, L'\\');
                break;
            }
            if(InArg[Index] == L'"')
            {
                InOutLine.append(Slashes * 2 + 1, L'\\');
                InOutLine += L'"';
            }
            else
            {
                InOutLine.append(Slashes, L'\\');
                InOutLine += InArg[Index];
            }
        }
        InOutLine += L'"';
    }

    std::wstring CommandLine(const std::vector<std::string>& InArgs)
    {
        std::wstring Line;
        for(const std::string& Each : InArgs)
        {
            if(!Line.empty())
            {
                Line += L' ';
            }
            AppendQuoted(Line, Widen(Each));
        }
        return Line;
    }

    /* This process's environment with InExtra on top, as CreateProcessW's block. */
    std::wstring EnvironmentBlock(const std::vector<std::string>& InExtra)
    {
        std::wstring Block;
        if(LPWCH Current = GetEnvironmentStringsW())
        {
            for(LPWCH Entry = Current; *Entry; Entry += wcslen(Entry) + 1)
            {
                const std::wstring Line(Entry);
                bool bOverridden = false;
                for(const std::string& Extra : InExtra)
                {
                    const std::wstring Wide = Widen(Extra);
                    const size_t Equals = Wide.find(L'=');
                    if(Equals != std::wstring::npos && _wcsnicmp(Line.c_str(), Wide.c_str(), Equals + 1) == 0)
                    {
                        bOverridden = true;
                    }
                }
                if(!bOverridden)
                {
                    Block += Line;
                    Block += L'\0';
                }
            }
            FreeEnvironmentStringsW(Current);
        }
        for(const std::string& Extra : InExtra)
        {
            Block += Widen(Extra);
            Block += L'\0';
        }
        Block += L'\0';
        return Block;
    }
}

ProcessResult RunProcess(const std::vector<std::string>& InArgs, const std::string& InWorkingDir,
                         const std::vector<std::string>& InEnvironment)
{
    ProcessResult Result;
    if(InArgs.empty())
    {
        return Result;
    }

    SECURITY_ATTRIBUTES Inherit = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE ReadEnd = nullptr, WriteEnd = nullptr;
    if(!CreatePipe(&ReadEnd, &WriteEnd, &Inherit, 0))
    {
        return Result;
    }
    SetHandleInformation(ReadEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE NullIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inherit, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW Startup = {};
    Startup.cb         = sizeof(Startup);
    Startup.dwFlags    = STARTF_USESTDHANDLES;
    Startup.hStdInput  = NullIn;
    Startup.hStdOutput = WriteEnd;
    Startup.hStdError  = WriteEnd;
    PROCESS_INFORMATION Info = {};

    std::wstring Line = CommandLine(InArgs);
    std::wstring Environment = EnvironmentBlock(InEnvironment);
    const std::wstring Dir = Widen(InWorkingDir);
    const BOOL bCreated = CreateProcessW(nullptr, Line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                         Environment.data(), Dir.empty() ? nullptr : Dir.c_str(), &Startup, &Info);
    CloseHandle(WriteEnd);
    if(NullIn != INVALID_HANDLE_VALUE)
    {
        CloseHandle(NullIn);
    }
    if(!bCreated)
    {
        CloseHandle(ReadEnd);
        return Result;
    }
    Result.bStarted = true;

    char Buffer[4096];
    DWORD Read = 0;
    while(ReadFile(ReadEnd, Buffer, sizeof(Buffer), &Read, nullptr) && Read > 0)
    {
        Result.Output.append(Buffer, Read);
    }
    CloseHandle(ReadEnd);

    WaitForSingleObject(Info.hProcess, INFINITE);
    DWORD Exit = 1;
    GetExitCodeProcess(Info.hProcess, &Exit);
    Result.ExitCode = int(Exit);
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);
    return Result;
}

ChildProcess SpawnProcess(const std::vector<std::string>& InArgs, const std::string& InWorkingDir)
{
    ChildProcess Child;
    if(InArgs.empty())
    {
        return Child;
    }
    STARTUPINFOW Startup = {};
    Startup.cb = sizeof(Startup);
    PROCESS_INFORMATION Info = {};
    std::wstring Line = CommandLine(InArgs);
    const std::wstring Dir = Widen(InWorkingDir);
    if(CreateProcessW(nullptr, Line.data(), nullptr, nullptr, FALSE, 0, nullptr, Dir.empty() ? nullptr : Dir.c_str(), &Startup, &Info))
    {
        CloseHandle(Info.hThread);
        Child.Handle = uintptr_t(Info.hProcess);
    }
    return Child;
}

bool IsProcessRunning(const ChildProcess& InChild)
{
    return InChild.IsValid() && WaitForSingleObject(HANDLE(InChild.Handle), 0) == WAIT_TIMEOUT;
}

void KillProcess(ChildProcess& InOutChild)
{
    if(!InOutChild.IsValid())
    {
        return;
    }
    const HANDLE Process = HANDLE(InOutChild.Handle);
    if(WaitForSingleObject(Process, 0) == WAIT_TIMEOUT)
    {
        TerminateProcess(Process, 1);
        WaitForSingleObject(Process, 5000);
    }
    CloseHandle(Process);
    InOutChild.Handle = 0;
}

std::string GetExecutableDir()
{
    std::wstring Path(MAX_PATH, L'\0');
    for(;;)
    {
        const DWORD Length = GetModuleFileNameW(nullptr, Path.data(), DWORD(Path.size()));
        if(Length == 0)
        {
            return std::string();
        }
        if(Length < Path.size())
        {
            Path.resize(Length);
            break;
        }
        Path.resize(Path.size() * 2);
    }
    const size_t Slash = Path.find_last_of(L"\\/");
    return Narrow(Slash == std::wstring::npos ? std::wstring() : Path.substr(0, Slash));
}

#else // POSIX

namespace
{
    std::vector<char*> Argv(const std::vector<std::string>& InArgs)
    {
        std::vector<char*> Out;
        for(const std::string& Each : InArgs)
        {
            Out.push_back(const_cast<char*>(Each.c_str()));
        }
        Out.push_back(nullptr);
        return Out;
    }
}

ProcessResult RunProcess(const std::vector<std::string>& InArgs, const std::string& InWorkingDir,
                         const std::vector<std::string>& InEnvironment)
{
    ProcessResult Result;
    if(InArgs.empty())
    {
        return Result;
    }
    int Pipe[2];
    if(pipe(Pipe) != 0)
    {
        return Result;
    }
    const pid_t Pid = fork();
    if(Pid < 0)
    {
        close(Pipe[0]);
        close(Pipe[1]);
        return Result;
    }
    if(Pid == 0)
    {
        dup2(Pipe[1], STDOUT_FILENO);
        dup2(Pipe[1], STDERR_FILENO);
        close(Pipe[0]);
        close(Pipe[1]);
        const int Null = open("/dev/null", O_RDONLY);
        if(Null >= 0)
        {
            dup2(Null, STDIN_FILENO);
        }
        if(!InWorkingDir.empty() && chdir(InWorkingDir.c_str()) != 0)
        {
            _exit(127);
        }
        for(const std::string& Each : InEnvironment)
        {
            putenv(const_cast<char*>(Each.c_str()));
        }
        std::vector<char*> Args = Argv(InArgs);
        execvp(Args[0], Args.data());
        _exit(127);
    }
    close(Pipe[1]);
    Result.bStarted = true;
    char Buffer[4096];
    ssize_t Read = 0;
    while((Read = read(Pipe[0], Buffer, sizeof(Buffer))) > 0 || (Read < 0 && errno == EINTR))
    {
        if(Read > 0)
        {
            Result.Output.append(Buffer, size_t(Read));
        }
    }
    close(Pipe[0]);
    int Status = 0;
    while(waitpid(Pid, &Status, 0) < 0 && errno == EINTR) {}
    Result.ExitCode = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
    if(Result.ExitCode == 127)
    {
        Result.bStarted = false;   // exec failed: not found
    }
    return Result;
}

ChildProcess SpawnProcess(const std::vector<std::string>& InArgs, const std::string& InWorkingDir)
{
    ChildProcess Child;
    if(InArgs.empty())
    {
        return Child;
    }
    const pid_t Pid = fork();
    if(Pid == 0)
    {
        if(!InWorkingDir.empty() && chdir(InWorkingDir.c_str()) != 0)
        {
            _exit(127);
        }
        std::vector<char*> Args = Argv(InArgs);
        execvp(Args[0], Args.data());
        _exit(127);
    }
    if(Pid > 0)
    {
        Child.Handle = uintptr_t(Pid);
    }
    return Child;
}

bool IsProcessRunning(const ChildProcess& InChild)
{
    if(!InChild.IsValid())
    {
        return false;
    }
    int Status = 0;
    return waitpid(pid_t(InChild.Handle), &Status, WNOHANG) == 0;
}

void KillProcess(ChildProcess& InOutChild)
{
    if(!InOutChild.IsValid())
    {
        return;
    }
    const pid_t Pid = pid_t(InOutChild.Handle);
    int Status = 0;
    if(waitpid(Pid, &Status, WNOHANG) == 0)
    {
        kill(Pid, SIGTERM);
        waitpid(Pid, &Status, 0);
    }
    InOutChild.Handle = 0;
}

std::string GetExecutableDir()
{
    char Path[4096] = {};
#if defined(__APPLE__)
    uint32_t Size = sizeof(Path);
    if(_NSGetExecutablePath(Path, &Size) != 0)
    {
        return std::string();
    }
#else
    const ssize_t Length = readlink("/proc/self/exe", Path, sizeof(Path) - 1);
    if(Length <= 0)
    {
        return std::string();
    }
    Path[Length] = '\0';
#endif
    std::string Full(Path);
    const size_t Slash = Full.find_last_of('/');
    return Slash == std::string::npos ? std::string() : Full.substr(0, Slash);
}

#endif
}
