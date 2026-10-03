#ifndef GAMECORE_GRAPHTEXT_H
#define GAMECORE_GRAPHTEXT_H

#include <string>

#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /*
     * A node graph file's generated code for InExtension (".lua", ".ppstack"):
     * its <gen_code><Output Extension="..."> CDATA, as NodeGraph writes it. No
     * XML library -- a game build has none -- so a runtime reads a graph asset
     * (a script graph's Lua, a post process stack) without NodeGraph. False if
     * there is none.
     */
    GAMECORE_API bool ExtractGraphGeneratedCode(const std::string& InGraphText, const char* InExtension, std::string& OutCode);
}

#endif //GAMECORE_GRAPHTEXT_H
