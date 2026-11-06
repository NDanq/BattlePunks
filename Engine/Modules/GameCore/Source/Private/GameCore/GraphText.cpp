#include "Public/GameCore/GraphText.h"

/* GraphText.h: reading a node graph's generated code without NodeGraph. */

namespace GameCore
{
    bool ExtractGraphGeneratedCode(const std::string& InGraphText, const char* InExtension, std::string& OutCode)
    {
        const size_t Section = InGraphText.find("<gen_code");
        if(Section == std::string::npos)
        {
            return false;
        }
        const std::string Opening = std::string("<Output Extension=\"") + InExtension + "\"";
        const size_t Output = InGraphText.find(Opening, Section);
        if(Output == std::string::npos)
        {
            return false;
        }
        size_t At = InGraphText.find('>', Output + Opening.size());
        if(At == std::string::npos)
        {
            return false;
        }
        if(InGraphText[At - 1] == '/')
        {
            OutCode.clear();   // <Output .../>: empty
            return true;
        }

        /* One CDATA section, or several where the text held "]]>". */
        static const char CDATA_OPEN[]  = "<![CDATA[";
        static const char CDATA_CLOSE[] = "]]>";
        static const char OUTPUT_END[]  = "</Output>";
        std::string Code;
        ++At;
        for(;;)
        {
            while(At < InGraphText.size() && (InGraphText[At] == ' ' || InGraphText[At] == '\t'
                                               || InGraphText[At] == '\r' || InGraphText[At] == '\n'))
            {
                ++At;
            }
            if(InGraphText.compare(At, sizeof(OUTPUT_END) - 1, OUTPUT_END) == 0)
            {
                OutCode = std::move(Code);
                return true;
            }
            if(InGraphText.compare(At, sizeof(CDATA_OPEN) - 1, CDATA_OPEN) != 0)
            {
                return false;   // not what NodeGraph writes
            }
            const size_t Start = At + sizeof(CDATA_OPEN) - 1;
            const size_t End   = InGraphText.find(CDATA_CLOSE, Start);
            if(End == std::string::npos)
            {
                return false;
            }
            Code.append(InGraphText, Start, End - Start);
            At = End + sizeof(CDATA_CLOSE) - 1;
        }
    }
}
