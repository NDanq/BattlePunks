#include "Public/RenderCore/RenderCommand.h"

#include <atomic>
#include <string>

#include "Public/Core/CoreThread.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/RenderThreadInternal.h"

/*
 * The four free functions RenderCommand.h declares. Thin: the real state --
 * the queue, the render-thread id, the RenderContext -- lives in
 * RenderThread's Impl (RenderThread.cpp), reached through the process-wide
 * locator RenderThreadInternal.h exposes. That split mirrors CoreJob.h's own
 * "locator pair" note: a free-function seam with no state of its own is
 * right as free functions over a G-pointer; RenderThread itself owns OS
 * threads and is a class the host can choose when to construct and destroy,
 * exactly the argument CoreJob.h makes for JobSystem.
 */

namespace RenderCore
{
    namespace
    {
        std::atomic<uint64_t> GInlineRenderCommands{0};
    }

    void EnqueueRenderCommand(RenderCommandHeader* InCommand)
    {
        CORE_CHECK_MAIN_THREAD("RenderCore::EnqueueRenderCommand")

        RenderThread* Thread = GetActiveRenderThread();
        if(!Thread || !Thread->IsRunning())
        {
            RENDERCORE_LOG_ERROR(std::string("EnqueueRenderCommand(\"") + (InCommand->DebugName ? InCommand->DebugName : "?")
                                  + "\") called with no render thread running; dropping the command")
            EngineCore::Memory::Free(InCommand);
            return;
        }

        Thread->Enqueue(InCommand);
    }

    bool IsInRenderThread()
    {
        RenderThread* Thread = GetActiveRenderThread();
        return Thread && Thread->IsRunning() && EngineCore::appThreadID() == Thread->GetThreadID();
    }

    RenderContext& GetRenderThreadContext()
    {
        CORE_CHECK(IsInRenderThread(), "GetRenderThreadContext() called from outside the render thread")
        return GetActiveRenderThread()->GetContext();
    }

    void FlushRenderCommands()
    {
        if(IsInRenderThread())
        {
            /* Nothing to wait on: a render-thread caller's own commands
             * already ran inline, by definition -- see
             * RenderCommandEnqueuer::operator(). */
            return;
        }

        RenderThread* Thread = GetActiveRenderThread();
        if(Thread && Thread->IsRunning())
        {
            Thread->Flush();
        }
    }

    void NoteInlineRenderCommand()
    {
        GInlineRenderCommands.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t GetInlineRenderCommandCount()
    {
        return GInlineRenderCommands.load(std::memory_order_relaxed);
    }
}
