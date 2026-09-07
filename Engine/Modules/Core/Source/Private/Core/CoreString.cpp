#include "Public/Core/CoreString.h"

#include <cctype>

namespace EngineCore
{
bool StartsWith(const std::string& InText, const std::string& InPrefix)
{
    if(InPrefix.size() > InText.size()) return false;
    return InText.compare(0, InPrefix.size(), InPrefix) == 0;
}

bool EndsWith(const std::string& InText, const std::string& InSuffix)
{
    if(InSuffix.size() > InText.size()) return false;
    return InText.compare(InText.size() - InSuffix.size(), InSuffix.size(), InSuffix) == 0;
}

bool Contains(const std::string& InText, const std::string& InNeedle)
{
    return InText.find(InNeedle) != std::string::npos;
}

std::vector<std::string> Split(const std::string& InText, const char InSeparator)
{
    std::vector<std::string> Out;

    // KeepEmptyParts, matching QString::split's default. An empty input still
    // yields one (empty) part, exactly as Qt does.
    size_t Start = 0;
    while(true)
    {
        const size_t Pos = InText.find(InSeparator, Start);
        if(Pos == std::string::npos)
        {
            Out.push_back(InText.substr(Start));
            break;
        }
        Out.push_back(InText.substr(Start, Pos - Start));
        Start = Pos + 1;
    }

    return Out;
}

std::string Trim(const std::string& InText)
{
    size_t First = 0;
    while(First < InText.size() && std::isspace(static_cast<unsigned char>(InText[First]))) ++First;

    size_t Last = InText.size();
    while(Last > First && std::isspace(static_cast<unsigned char>(InText[Last - 1]))) --Last;

    return InText.substr(First, Last - First);
}
}
