#ifndef ENGINECORE_CORELOG_H
#define ENGINECORE_CORELOG_H

#include <string>

#include "Public/Core/CoreAPI.h"

/*
 * Logging seam. EngineCore and everything layered on it (GDI, the backends)
 * log through these macros; the host application decides where the text goes
 * by installing a handler. Odessa installs one in main() that forwards to
 * OLogger + qDebug; an SDL host would install one that writes to stderr.
 *
 * NAMING HAZARD: <wingdi.h> defines GDI_ERROR as 0xFFFFFFFFL, and <Windows.h>
 * reaches nearly every OpenGL translation unit. Never define a macro whose
 * name starts with GDI_ERROR -- that is why the error macro is CORE_LOG_ERROR.
 */

namespace EngineCore
{
    enum class LogVerbosity : unsigned char
    {
        Log = 0,
        Warning,
        Error,
        Fatal
    };

    using LogHandler = void (*)(LogVerbosity Verbosity, const char* Message);

    /* Install the host's sink. Passing nullptr restores the default handler. */
    CORE_API void        SetLogHandler(LogHandler InHandler);
    CORE_API LogHandler  GetLogHandler();

    /*
     * Route one message.
     *
     * FATAL SEMANTICS -- read before changing:
     *
     *   With a handler installed, LogDispatch does exactly what the handler
     *   does and then RETURNS, even for Fatal. That is deliberate: Odessa's
     *   historical CHECK() called QCoreApplication::exit(-1), which only asks
     *   the event loop to quit and lets the failing function run on to
     *   completion. A great deal of existing code implicitly relies on that,
     *   so aborting here instead would change behaviour at every one of the
     *   existing assert sites.
     *
     *   With NO handler installed, Fatal writes to stderr and calls abort().
     *   It must not be a silent no-op -- a default that swallowed asserts
     *   would make every existing fatal check disappear the moment a host
     *   forgot to install a sink, which is precisely the failure this seam
     *   exists to prevent.
     */
    CORE_API void LogDispatch(LogVerbosity Verbosity, const char* Message);
    CORE_API void LogDispatch(LogVerbosity Verbosity, const std::string& Message);
}

#define CORE_LOG(Message)       ::EngineCore::LogDispatch(::EngineCore::LogVerbosity::Log,     Message);
#define CORE_LOG_WARN(Message)  ::EngineCore::LogDispatch(::EngineCore::LogVerbosity::Warning, Message);
#define CORE_LOG_ERROR(Message) ::EngineCore::LogDispatch(::EngineCore::LogVerbosity::Error,   Message);

/*
 * Shaped like the CHECK() it replaces -- a bare if, not a do/while -- so that
 * the ~30 existing call sites that omit the trailing semicolon keep compiling
 * unchanged. Do not "fix" this into a do/while without auditing all of them.
 */
#define CORE_CHECK(Condition, Message)                                        \
    if( !(Condition) )                                                        \
    {                                                                         \
        ::EngineCore::LogDispatch(::EngineCore::LogVerbosity::Fatal, Message); \
    }

#endif //ENGINECORE_CORELOG_H
