#ifndef RENDERCORE_CUSTOMRENDERPASSINTERNAL_H
#define RENDERCORE_CUSTOMRENDERPASSINTERNAL_H

#include <memory>
#include <vector>

#include "Public/RenderCore/CustomRenderPass.h"

/*
 * The registry's other side (CustomRenderPass.cpp): what WorldBridge and
 * RenderContext read of it.
 */
namespace RenderCore
{
    /* One registration, as a frame sees it. */
    struct CustomPassEntry
    {
        std::shared_ptr<CustomRenderPass> Pass;
        RenderPassHandle                  Handle = 0;
        ERenderHook                       Hook   = ERenderHook::BeforeScene;
        int32_t                           Order  = 0;
    };

    /* InWorld's passes, by hook, then order, then registration. Any thread. */
    void GatherCustomRenderPasses(const void* InWorld, std::vector<CustomPassEntry>& OutPasses);

    /* GAME THREAD: Extract on each of InWorld's passes. */
    void ExtractCustomRenderPasses(const void* InWorld);

    /* RENDER THREAD: the registrations removed since the last call, whose
     * targets and buffers are to be freed. */
    void TakeRetiredCustomRenderPasses(std::vector<RenderPassHandle>& OutHandles);
}

#endif //RENDERCORE_CUSTOMRENDERPASSINTERNAL_H
