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
 * CoreJob.h makes for SetJobSystem/GetJobSystem and CoreModule.h makes for
 * ModuleManager's own locator: a seam with no state of its own belongs here,
 * and the OWNING object -- the RenderThread the host actually constructs and
 * destroys, at a point of ITS choosing -- lives in Renderer (later steps).
 * Private because RenderCommand.h's public free functions (EnqueueRenderCommand,
 * IsInRenderThread, ...) are the only surface a consumer needs; the locator
 * itself would just be one more way to reach the same RenderThread a caller
 * either already has (Renderer's owner) or has no business reaching directly.
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
}

#endif //RENDERCORE_RENDERTHREADINTERNAL_H
