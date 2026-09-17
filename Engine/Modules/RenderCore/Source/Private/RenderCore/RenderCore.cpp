#include "Private/RenderCore/RenderCoreInternal.h"
#include "Public/RenderCore/RenderCoreVersion.h"

/*
 * Step 1 (skeleton) has no real behaviour yet -- RenderThread, RenderCommand
 * and everything above them arrive in later build-order steps (see
 * DESIGN.md). This translation unit exists so the module has at least one
 * .cpp: it is what actually instantiates RenderCoreInternal.h's
 * ERenderBackend <-> ERenderPlatform static_asserts, and what defines
 * RenderCoreVersion.h's one exported function -- see that header's comment
 * for why a SHARED module needs at least one before it can link.
 */
namespace RenderCore
{
    const char* GetModuleName()
    {
        return "RenderCore";
    }
}
