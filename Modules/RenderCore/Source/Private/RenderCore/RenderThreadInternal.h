#ifndef RENDERCORE_RENDERTHREADINTERNAL_H
#define RENDERCORE_RENDERTHREADINTERNAL_H

#include <cstdint>
#include <string>

#include "Public/RenderCore/RenderCoreAPI.h"
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

    /* The reporting scene's static draw lists (RenderStats::StaticProxiesDrawn
     * and StaticListBuilds). */
    void SetStaticDrawStats(uint64_t InDrawn, uint64_t InBuilds);
    void GetStaticDrawStats(uint64_t& OutDrawn, uint64_t& OutBuilds);

    /* "stat unit": Renderer::EndFrame publishes the averaged times and
     * whether to show them (Stats.Unit); RenderScene::Render draws them and
     * RenderThread::GetStats copies them into RenderStats. */
    struct StatUnitSnapshot
    {
        float FrameMs        = 0.0f;
        float GameThreadMs   = 0.0f;
        float RenderThreadMs = 0.0f;
        bool  bShow          = false;   // Stats.Unit
        bool  bShowFrame     = false;   // Stats.Frame
    };
    void             SetStatUnit(const StatUnitSnapshot& InSnapshot);
    StatUnitSnapshot GetStatUnit();

    /* The overlay's text, a line each: Frame, Game, Render (ms), and FPS. */
    RENDERCORE_API std::string FormatStatUnit(const StatUnitSnapshot& InSnapshot);

    /* "stat frame"'s text: a heading, then a line per ERenderGpuTimer with its
     * milliseconds ("-" where InGpuMs is negative: not timed); one line saying
     * so if this backend cannot time the GPU. */
    RENDERCORE_API std::string FormatStatFrame(const float* InGpuMs, bool bInSupported);

    /* The stats-reporting scene's GPU times, for RenderThread::GetStats
     * (RenderStats::GpuMs): RGT_Count of them. */
    void SetReportedGpuTimes(const float* InGpuMs);
    void GetReportedGpuTimes(float* OutGpuMs);
}

#endif //RENDERCORE_RENDERTHREADINTERNAL_H
