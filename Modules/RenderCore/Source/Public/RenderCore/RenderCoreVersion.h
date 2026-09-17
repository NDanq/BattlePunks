#ifndef RENDERCORE_RENDERCOREVERSION_H
#define RENDERCORE_RENDERCOREVERSION_H

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * One out-of-line, RENDERCORE_API function -- present from the skeleton step
 * onward for a structural reason, not a diagnostic one: a SHARED library
 * with no exported symbol produces no import library on MSVC (the linker has
 * nothing to bind an import to), so a consumer target -- RenderCoreSmokeTest,
 * initially -- fails at link with LNK1104 even though RenderCore.dll itself
 * built and linked cleanly. Every later build-order step adds real exports
 * (RenderThread, RenderProxy, ...) that would have covered this anyway; this
 * function exists so the skeleton step is independently linkable before any
 * of them exist.
 */
namespace RenderCore
{
    RENDERCORE_API const char* GetModuleName();
}

#endif //RENDERCORE_RENDERCOREVERSION_H
