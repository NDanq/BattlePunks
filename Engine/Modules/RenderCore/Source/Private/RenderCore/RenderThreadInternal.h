#ifndef RENDERCORE_RENDERTHREADINTERNAL_H
#define RENDERCORE_RENDERTHREADINTERNAL_H

#include <cstdint>
#include <string>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderThread.h"
#include "Public/RenderCore/RenderTypes.h"

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

    /* The reporting scene's occlusion culling (RenderStats::ProxiesOccludedLastFrame
     * and OcclusionQueriesLastFrame). */
    void SetOcclusionStats(uint64_t InOccluded, uint64_t InQueries);
    void GetOcclusionStats(uint64_t& OutOccluded, uint64_t& OutQueries);

    /* The reporting scene's captures (RenderStats::GIProbesUnlit, GIProbesTotal, bCapturesBusy). */
    void SetCaptureStats(uint32_t InUnlit, uint32_t InTotal, bool bInBusy);
    void GetCaptureStats(uint32_t& OutUnlit, uint32_t& OutTotal, bool& bOutBusy);

    /* "stat unit": Renderer::EndFrame publishes the averaged times and
     * whether to show them (Stats.Unit); RenderScene::Render draws them and
     * RenderThread::GetStats copies them into RenderStats. */
    struct StatUnitSnapshot
    {
        float FrameMs        = 0.0f;
        float GameThreadMs   = 0.0f;
        float RenderThreadMs = 0.0f;
        float WaitMs         = 0.0f;   // the game thread waiting on the render thread's frame
        float PresentMs      = 0.0f;   // ... presenting to its windows (the swapchain's vsync)
        float IdleMs         = 0.0f;   // ... between frames: the host's loop, a frame timer
        bool  bShow          = false;   // Stats.Unit
        bool  bShowFrame     = false;   // Stats.Frame

        /* Renderer::SetStatText's sections, joined, drawn below the rest:
         * other modules' stats ("stat physics"). Empty: none. */
        std::string Extra;
    };
    void             SetStatUnit(const StatUnitSnapshot& InSnapshot);
    RENDERCORE_API StatUnitSnapshot GetStatUnit();

    /* The overlay's text, a line each: Frame, Game, Render, Wait, Present,
     * Idle (ms), and FPS. Frame = Game + Wait + Present + Idle. */
    RENDERCORE_API std::string FormatStatUnit(const StatUnitSnapshot& InSnapshot);

    /* "stat streaming"'s text: the pool, what is resident and wanted against
     * all of it, the loads under way and done, the last frame's commits. */
    RENDERCORE_API std::string FormatStatStreaming(const TextureStreamingStats& InStats);

    /* "stat frame"'s text: a heading, then a line per ERenderGpuTimer with its
     * GPU milliseconds, the render thread's milliseconds and its draws ("-"
     * where negative: not timed or not counted; the GPU column all "-", with a
     * line saying so, where this backend cannot time the GPU), then the
     * render thread's culling and queueing (InQueueMs). Any array may be null. */
    RENDERCORE_API std::string FormatStatFrame(const float* InGpuMs, const float* InCpuMs, const int32_t* InDraws,
                                               float InQueueMs, bool bInGpuSupported);

    /* The stats-reporting scene's GPU times, for RenderThread::GetStats
     * (RenderStats::GpuMs): RGT_Count of them. */
    void SetReportedGpuTimes(const float* InGpuMs);
    void GetReportedGpuTimes(float* OutGpuMs);

    /* Its render-thread side (RenderStats::CpuMs, StageDraws: RGT_Count each;
     * QueueCpuMs). */
    void SetReportedStageTimes(const float* InCpuMs, const int32_t* InDraws, float InQueueMs);
    void GetReportedStageTimes(float* OutCpuMs, int32_t* OutDraws, float& OutQueueMs);
}

#endif //RENDERCORE_RENDERTHREADINTERNAL_H
