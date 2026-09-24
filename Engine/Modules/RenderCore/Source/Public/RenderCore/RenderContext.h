#ifndef RENDERCORE_RENDERCONTEXT_H
#define RENDERCORE_RENDERCONTEXT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"

/*
 * What a render command and a RenderProxy draw with. RENDER THREAD ONLY --
 * every method here assumes it, and GetRenderThreadContext() (RenderCommand.h)
 * is the only way to reach one.
 *
 * NO GDI TYPE APPEARS HERE, deliberately: RenderCoreInternal.h is the only
 * header in this module allowed to include one, and it lives under Private/
 * (see its own comment). A proxy or a command written against this class
 * never needs to know GDI exists, which is what makes RenderCore.dll the
 * ONLY image in the process that links it -- see RenderCoreAPI.h.
 */
namespace RenderCore
{
    class RENDERCORE_API RenderContext
    {
    public:
        RenderContext();
        ~RenderContext();

        RenderContext(const RenderContext&)            = delete;
        RenderContext& operator=(const RenderContext&) = delete;

        ERenderBackend    GetBackend()     const;
        uint64_t          GetFrameNumber() const;
        const RenderView& GetView()        const;

        /*
         * Draws a quad centred on InTransform's position (scaled by
         * InWidth/InHeight, rotated and offset the same way
         * TransformComponent::GetLocalMatrix composes -- see
         * RenderTypes.h's RenderTransform::ToMatrix), tinted InTint, through
         * the Basic vertex/pixel shaders every backend already registers in
         * InitializeGDI (GDI.cpp). One 1x1 quad mesh serves every size and
         * every proxy -- scaled through GDITransform::Scale rather than
         * GDICreateQuadMesh(w, h) allocating a mesh per size.
         *
         * Counts one draw call, read back through
         * RenderThread::GetStats()'s RenderStats::DrawCalls.
         */
        void DrawQuad(const RenderTransform& InTransform, float InWidth, float InHeight, EngineCore::Color InTint);

        /* Render-thread only, matching GDI's own GDIBeginDrawEvent/EndDrawEvent
         * (Public/GDI/GDI.h) -- a scoped pair for a profiler's capture, cheap
         * to call even with no profiler attached. */
        void BeginDrawEvent(const char* InName);
        void EndDrawEvent();

        /* Lifetime total of DrawQuad calls -- what RenderThread::GetStats()
         * reads into RenderStats::DrawCalls. Not reset per frame: a snapshot
         * consumer (a per-second log line) differences two reads itself,
         * the same convention RenderStats's own comment states. */
        uint64_t GetDrawCallCount() const;

    private:
        friend class RenderThread;
        friend class RenderScene;

        /* RenderThread::Start()/EndFrame() and RenderScene::SetView() write
         * these directly -- all three live in this DLL and all run on the
         * render thread, so there is no cross-image or cross-thread hazard
         * in bypassing an accessor here. */
        void SetBackend(ERenderBackend InBackend);
        void SetFrameNumber(uint64_t InFrameNumber);
        void SetView(const RenderView& InView);

        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERCONTEXT_H
