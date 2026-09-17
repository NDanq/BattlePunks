#include "Public/RenderCore/RenderContext.h"

#include "Private/RenderCore/RenderCoreInternal.h"

/*
 * Step 2's slice: frame/backend bookkeeping and draw-event bracketing over
 * GDI's own GDIBeginDrawEvent/EndDrawEvent. The quad mesh and shaders that
 * DrawQuad needs arrive with RenderView and RenderProxy in a later
 * build-order step -- see RenderContext.h's own comment.
 */

namespace RenderCore
{
    struct RenderContext::Impl
    {
        ERenderBackend Backend      = ERenderBackend::Null;
        uint64_t       FrameNumber  = 0;
    };

    RenderContext::RenderContext() : P(new Impl()) {}

    RenderContext::~RenderContext()
    {
        delete P;
    }

    ERenderBackend RenderContext::GetBackend() const
    {
        return P->Backend;
    }

    uint64_t RenderContext::GetFrameNumber() const
    {
        return P->FrameNumber;
    }

    void RenderContext::BeginDrawEvent(const char* InName)
    {
        GDIBeginDrawEvent(InName ? InName : "");
    }

    void RenderContext::EndDrawEvent()
    {
        GDIEndDrawEvent();
    }

    void RenderContext::SetBackend(const ERenderBackend InBackend)
    {
        P->Backend = InBackend;
    }

    void RenderContext::SetFrameNumber(const uint64_t InFrameNumber)
    {
        P->FrameNumber = InFrameNumber;
    }
}
