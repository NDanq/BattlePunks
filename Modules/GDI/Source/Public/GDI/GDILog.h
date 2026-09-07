#ifndef CORECLIENT_GDILOG_H
#define CORECLIENT_GDILOG_H

#include <string>

#include "Public/Core/CoreLog.h"

/*
 * GDI's logging macros. Thin wrappers over EngineCore's, existing for one
 * reason: argument conversion.
 *
 * WHY THIS SHIM EXISTS
 *
 * The 131 existing call sites pass whatever they had to hand, and many passed
 * a QString built by concatenation. EngineCore's LogDispatch takes const char*
 * / std::string and, correctly, knows nothing about QString. Without an
 * adapter the choice would be to edit all 131 sites now, and then edit them
 * ALL AGAIN when QString became std::string.
 *
 * The overload set below absorbed that, and the QString overload has since
 * been removed -- every call site now binds to std::string or const char*.
 * The indirection stays because it is also where GDINum and GDIHex live.
 *
 * NAMING HAZARD: <wingdi.h> defines GDI_ERROR as 0xFFFFFFFFL and <Windows.h>
 * reaches nearly every OpenGL translation unit here. The error macro is
 * therefore GDI_LOG_ERROR. Never define anything spelled GDI_ERROR.
 */

namespace GDILogDetail
{
    inline std::string ToLogString(const char* InText)
    {
        return InText ? std::string(InText) : std::string();
    }

    inline const std::string& ToLogString(const std::string& InText)
    {
        return InText;
    }

    /*
     * Replaces QString::number in log messages.
     *
     * Templated and cast through long long so it also accepts the enum types
     * the backends log -- VkResult in particular, which std::to_string has no
     * overload for. Every call site passes an integer or an enum; a float
     * would truncate, so do not extend it to one silently.
     *
     * Concatenation needs no further help: `"failed: " + GDINum(R)` binds to
     * operator+(const char*, const std::string&), and each further `+ " ..."`
     * then has a std::string on the left.
     */
    template<typename T>
    inline std::string GDINum(T InValue)
    {
        return std::to_string(static_cast<long long>(InValue));
    }

    /*
     * Lowercase hex, no prefix -- replaces QString::arg(v, 0, 16) in the
     * handle-logging lines the Vulkan backend emits under QT_DEBUG.
     */
    inline std::string GDIHexStr(unsigned long long InValue)
    {
        static const char* Digits = "0123456789abcdef";
        if(InValue == 0) return std::string("0");

        std::string Out;
        while(InValue != 0)
        {
            Out.insert(Out.begin(), Digits[InValue & 0xFull]);
            InValue >>= 4;
        }
        return Out;
    }
}

/*
 * The QString overload that used to sit here is gone with the last QString in
 * GDI. Call sites bind to the std::string / const char* overloads above, which
 * is what they were written to fall back to.
 */

#define GDIHex(Value)          (::GDILogDetail::GDIHexStr(static_cast<unsigned long long>(Value)))

#define GDI_LOG(Message)       CORE_LOG(::GDILogDetail::ToLogString(Message))
#define GDI_LOG_WARN(Message)  CORE_LOG_WARN(::GDILogDetail::ToLogString(Message))
#define GDI_LOG_ERROR(Message) CORE_LOG_ERROR(::GDILogDetail::ToLogString(Message))

/*
 * Shaped like the CHECK() it replaces -- a bare if, not a do/while -- because
 * several existing call sites omit the trailing semicolon (GDIRenderTarget.h
 * among them). Do not tighten this without auditing all of them.
 */
#define GDI_CHECK(Condition, Message)                                          \
    if( !(Condition) )                                                         \
    {                                                                          \
        ::EngineCore::LogDispatch(::EngineCore::LogVerbosity::Fatal,            \
                                  ::GDILogDetail::ToLogString(Message));       \
    }

#endif //CORECLIENT_GDILOG_H
