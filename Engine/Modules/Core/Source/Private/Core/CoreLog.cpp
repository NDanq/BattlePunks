#include "Public/Core/CoreLog.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <vector>

namespace EngineCore
{
namespace
{
    LogHandler GLogHandler = nullptr;

    /* The run log. Leaked, as CoreConfig's entries are: a module's static
     * destructor may still log during shutdown. */
    std::mutex&  RunLogMutex() { static std::mutex* M = new std::mutex(); return *M; }
    std::FILE*   GRunLog = nullptr;
    std::string& RunLogPath()  { static std::string* P = new std::string(); return *P; }

    std::tm LocalNow(std::chrono::milliseconds* OutMillis = nullptr)
    {
        const auto Now = std::chrono::system_clock::now();
        const std::time_t Seconds = std::chrono::system_clock::to_time_t(Now);
        if(OutMillis)
        {
            *OutMillis = std::chrono::duration_cast<std::chrono::milliseconds>(Now.time_since_epoch()) % 1000;
        }
        std::tm Local{};
#if defined(_WIN32)
        localtime_s(&Local, &Seconds);
#else
        localtime_r(&Seconds, &Local);
#endif
        return Local;
    }

    const char* VerbosityTag(const LogVerbosity Verbosity)
    {
        switch(Verbosity)
        {
            case LogVerbosity::Log:     return "LOG";
            case LogVerbosity::Warning: return "WARN";
            case LogVerbosity::Error:   return "ERROR";
            case LogVerbosity::Fatal:   return "FATAL";
        }
        return "LOG";
    }

    /*
     * Used only when the host installed nothing. Fatal aborts here rather than
     * returning: see the contract note in CoreLog.h -- a silent default would
     * erase every existing assert.
     */
    void DefaultHandler(const LogVerbosity Verbosity, const char* Message)
    {
        std::fprintf(stderr, "[%s] %s\n", VerbosityTag(Verbosity), Message ? Message : "");
        std::fflush(stderr);

        if(Verbosity == LogVerbosity::Fatal)
        {
            std::abort();
        }
    }
}

void SetLogHandler(LogHandler InHandler)
{
    GLogHandler = InHandler;
}

LogHandler GetLogHandler()
{
    return GLogHandler;
}

void LogDispatch(const LogVerbosity Verbosity, const char* Message)
{
    {
        std::lock_guard<std::mutex> Lock(RunLogMutex());
        if(GRunLog)
        {
            std::chrono::milliseconds Millis{};
            const std::tm Local = LocalNow(&Millis);
            std::fprintf(GRunLog, "%02d:%02d:%02d.%03d [%s] %s\n", Local.tm_hour, Local.tm_min, Local.tm_sec,
                         int(Millis.count()), VerbosityTag(Verbosity), Message ? Message : "");
            std::fflush(GRunLog);
        }
    }

    if(GLogHandler)
    {
        // Returns even for Fatal -- deliberate, matches the historical
        // QCoreApplication::exit(-1) behaviour. See CoreLog.h.
        GLogHandler(Verbosity, Message ? Message : "");
        return;
    }

    DefaultHandler(Verbosity, Message);
}

void LogDispatch(const LogVerbosity Verbosity, const std::string& Message)
{
    LogDispatch(Verbosity, Message.c_str());
}

bool OpenRunLog(const std::string& InDirectory, const std::string& InName, const int InKeep)
{
    namespace fs = std::filesystem;
    CloseRunLog();

    std::error_code Error;
    const fs::path Directory = fs::u8path(InDirectory);
    fs::create_directories(Directory, Error);

    const std::tm Local = LocalNow();
    char Stamp[32];
    std::snprintf(Stamp, sizeof(Stamp), "%04d.%02d.%02d-%02d.%02d.%02d", Local.tm_year + 1900, Local.tm_mon + 1,
                  Local.tm_mday, Local.tm_hour, Local.tm_min, Local.tm_sec);
    const fs::path File = Directory / fs::u8path(InName + "-" + Stamp + ".log");
    const auto FileU8 = File.u8string();
    const std::string FilePath(FileU8.begin(), FileU8.end());

    /* Oldest first by name (the stamp sorts), so the newest InKeep stay. */
    std::vector<fs::path> Older;
    for(const fs::directory_entry& Entry : fs::directory_iterator(Directory, Error))
    {
        const auto NameU8 = Entry.path().filename().u8string();
        const std::string Name(NameU8.begin(), NameU8.end());
        if(Entry.is_regular_file(Error) && Name.size() > InName.size() + 5 && Name.compare(0, InName.size() + 1, InName + "-") == 0
           && Name.compare(Name.size() - 4, 4, ".log") == 0)
        {
            Older.push_back(Entry.path());
        }
    }
    std::sort(Older.begin(), Older.end());
    const size_t Keep = size_t(std::max(InKeep - 1, 0));   // the new one is the last kept
    for(size_t Index = 0; Older.size() > Keep && Index < Older.size() - Keep; ++Index)
    {
        fs::remove(Older[Index], Error);
    }

#if defined(_WIN32)
    std::FILE* Opened = _wfopen(File.c_str(), L"wb");
#else
    std::FILE* Opened = std::fopen(FilePath.c_str(), "wb");
#endif
    if(!Opened)
    {
        return false;
    }
    std::lock_guard<std::mutex> Lock(RunLogMutex());
    GRunLog = Opened;
    RunLogPath() = FilePath;
    return true;
}

void CloseRunLog()
{
    std::lock_guard<std::mutex> Lock(RunLogMutex());
    if(GRunLog)
    {
        std::fclose(GRunLog);
        GRunLog = nullptr;
    }
    RunLogPath().clear();
}

const std::string& GetRunLogPath()
{
    return RunLogPath();
}
}
