#ifndef RENDERCORE_RENDERTHREADINTERNAL_H
#define RENDERCORE_RENDERTHREADINTERNAL_H

#include <cstdint>

#include "Public/RenderCore/RenderThread.h"

/*
 * The process-wide locator for the one live RenderThread, plus the
 * inline-command counter RenderCommand.cpp feeds and RenderThread::GetStats
 * reads back.
 *
 * FREE FUNCTIONS OVER A NON-OWNING POINTER, not a class -- the same call
 * CoreJob.h makes for SetJobSystem/GetJobSystem: a seam with no state of its
 * own belongs here, while the OWNING object lives in Renderer. Private because
 * RenderCommand.h's public free functions (EnqueueRenderCommand,
 * IsInRenderThread, ...) are the only surface a consumer needs.
 *
 * SET IT BEFORE ANY COMMAND IS ENQUEUED, CLEAR IT BEFORE THE RenderThread
 * DIES -- a dangling pointer here is a dispatch into a destroyed thread, the
 * same warning CoreJob.h's own locator carries.
 */
namespace RenderCore
{
    void         SetActiveRenderThread(RenderThread* InThread);
    RenderThread* GetActiveRenderThread();

    /* Lifetime total of every render command that ran inline because its
     * caller was already on the render thread -- see
     * RenderCommandEnqueuer::operator() and NoteInlineRenderCommand(). Read
     * by RenderThread::GetStats() into RenderStats::CommandsExecutedInline. */
    uint64_t GetInlineRenderCommandCount();

    /*
     * RenderScene::Render() reports its own proxy/cull counts here, once per
     * frame, and RenderThread::GetStats() reads them back into
     * RenderStats::ProxiesLive/ProxiesCulledLastFrame -- a scene has no
     * reach into RenderThread's own counters (there may be more than one
     * RenderScene in a test, and only ever one active RenderThread), so the
     * relay is the same shape as NoteInlineRenderCommand's own one-way feed.
     */
    void SetSceneStats(uint64_t InProxiesLive, uint64_t InProxiesCulled);
    void GetSceneStats(uint64_t& OutProxiesLive, uint64_t& OutProxiesCulled);
}

#endif //RENDERCORE_RENDERTHREADINTERNAL_H
