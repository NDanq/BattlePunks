#ifndef ENGINECORE_CORESTRING_H
#define ENGINECORE_CORESTRING_H

#include <string>
#include <vector>

#include "Public/Core/CoreAPI.h"

/*
 * The handful of string operations QString provided that std::string does not.
 *
 * Deliberately small: these exist because the GLSL preprocessor and a few
 * backend lookups used QString::split / contains / startsWith, and nothing
 * more. This is not the beginning of a string class -- if something needs more
 * than this, it probably wants a real parser rather than another helper.
 */

namespace EngineCore
{
    CORE_API bool StartsWith(const std::string& InText, const std::string& InPrefix);
    CORE_API bool EndsWith  (const std::string& InText, const std::string& InSuffix);
    CORE_API bool Contains  (const std::string& InText, const std::string& InNeedle);

    /*
     * Splits on a single character.
     *
     * Matches QString::split(QChar) with its default KeepEmptyParts: "a,,b"
     * yields three parts, and a trailing separator yields a trailing empty
     * part. The GLSL preprocessor splits source on newlines and relies on
     * blank lines surviving, so dropping empty parts would silently reflow
     * shader source.
     */
    CORE_API std::vector<std::string> Split(const std::string& InText, char InSeparator);

    /* Whitespace from both ends. */
    CORE_API std::string Trim(const std::string& InText);
}

#endif //ENGINECORE_CORESTRING_H
