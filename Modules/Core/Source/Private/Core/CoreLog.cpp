#include "Public/Core/CoreLog.h"

#include <cstdio>
#include <cstdlib>

namespace EngineCore
{
namespace
{
    LogHandler GLogHandler = nullptr;

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
}
