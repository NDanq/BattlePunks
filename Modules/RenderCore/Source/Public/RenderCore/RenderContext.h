#ifndef RENDERCORE_RENDERCONTEXT_H
#define RENDERCORE_RENDERCONTEXT_H

#include <cstdint>

#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"

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
 *
 * STEP 2 (this file's current state): the frame/backend/view accessors and
 * draw-event bracketing RenderThread's own drain loop already needs. Real
 * drawing (DrawQuad) arrives in the build-order step that also brings
 * RenderView and RenderProxy -- see DESIGN.md -- since a quad mesh and the
 * Basic shaders belong with the code that first has something to draw.
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

        ERenderBackend GetBackend()     const;
        uint64_t       GetFrameNumber() const;

        /* Render-thread only, matching GDI's own GDIBeginDrawEvent/EndDrawEvent
         * (Public/GDI/GDI.h) -- a scoped pair for a profiler's capture, cheap
         * to call even with no profiler attached. */
        void BeginDrawEvent(const char* InName);
        void EndDrawEvent();

    private:
        friend class RenderThread;

        /* RenderThread::Start()/EndFrame() write these directly -- both live
         * in this DLL and both run on the render thread, so there is no
         * cross-image or cross-thread hazard in bypassing an accessor here. */
        void SetBackend(ERenderBackend InBackend);
        void SetFrameNumber(uint64_t InFrameNumber);

        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERCONTEXT_H
