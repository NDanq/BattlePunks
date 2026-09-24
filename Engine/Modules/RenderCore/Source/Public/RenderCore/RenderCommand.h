#ifndef RENDERCORE_RENDERCOMMAND_H
#define RENDERCORE_RENDERCOMMAND_H

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * ENQUEUE_RENDER_COMMAND: the game-thread -> render-thread bridge, and the
 * primary way work crosses between them. Modelled on GDI's own
 * ENQUEUE_RENDERCOMMAND_* macros (Public/GDI/GDI.h) but built over
 * CoreMemory.h's Alloc/Free rather than new/delete, so a command allocated by
 * a hot-reloadable module is destroyed correctly relative to the image
 * barrier -- see EnqueueRenderCommand's own comment.
 *
 * WHAT A COMMAND MAY CAPTURE. Trivially copyable and trivially destructible
 * only -- values, POD, a RenderProxy* or RenderProxyHandle. No std::string,
 * no TRef: the payload is memcpy-constructed and its destructor is NEVER RUN,
 * so a capture that owns anything leaks, and a captured TRef would be refcount
 * traffic off the collector's thread with nothing to catch it.
 * RenderCommandEnqueuer::operator() static_asserts both -- JobSystem::Dispatch's
 * rule (CoreJob.h), for the same reason.
 *
 * ONE FRAME OF LAG, NOT A FENCE. A game-thread caller may enqueue at ANY POINT
 * during its tick; the render thread need not be idle first.
 * RenderThread::EndFrame enqueues a FRAME MARKER telling the drain loop where
 * this frame's commands end, so commands enqueued after it sit behind the
 * marker untouched until the next kick. That is what makes "enqueue throughout
 * the tick" and "the drain loop can tell frame N from N-1" both true at once.
 */

namespace RenderCore
{
    class RenderContext;

    /* What a queued command actually runs: the type-erased entry point into
     * TRenderCommand<T>::Execute, cast back to the real lambda type there --
     * the one place that cast happens, matching DelegateList::RawFn's own
     * "cast back in exactly one place" discipline (GameCore's Delegate.h). */
    using RenderCommandFn = void (*)(void* InCommand, RenderContext& InContext);

    enum ERenderCommandKind : uint8_t
    {
        RCK_Command     = 0,   // an ordinary enqueued command
        RCK_FrameMarker = 1,   // built only by RenderThread::EndFrame
        RCK_FlushMarker = 2,   // built only by RenderThread::Flush
    };

    /*
     * FROZEN LAYOUT: written by TRenderCommand<T>'s constructor in the
     * CALLING image (a template instantiates there), read by
     * RenderThread.cpp's drain loop inside RenderCore.dll. Neither side may
     * assume anything about TLambda -- only this header's four fields.
     */
    struct RenderCommandHeader
    {
        RenderCommandFn     Fn          = nullptr;
        const char*         DebugName   = nullptr;   // the macro's Name, stringised -- for logs and a future profiler
        EngineCore::ImageID OwningImage = EngineCore::IMAGE_Host;   // GImageID at the call site -- JobSystem tags a job the same way
        uint8_t             Kind        = RCK_Command;
    };

    template<typename TLambda>
    struct TRenderCommand : RenderCommandHeader
    {
        TLambda Lambda;

        static void Execute(void* InCommand, RenderContext& InContext)
        {
            static_cast<TRenderCommand*>(InCommand)->Lambda(InContext);
        }
    };

    /*
     * Queues InCommand for the render thread. GAME THREAD ONLY -- CORE_CHECKed
     * against Threads::IsMainThread() the way GameCore and GameWindow check
     * their own single-thread contracts; the render thread never calls this
     * (RenderCommandEnqueuer::operator() runs a render-thread caller's lambda
     * inline instead, never through the queue).
     *
     * Before RenderThread::Start() or after Stop(), InCommand is logged as an
     * error, freed, and dropped -- there is no thread to drain it and holding
     * it would leak.
     */
    RENDERCORE_API void EnqueueRenderCommand(RenderCommandHeader* InCommand);

    /* True on the thread that called AllocateResourcesGDI() -- see
     * RenderThread.cpp's Start(). False everywhere else, including before
     * Start() and after Stop(). */
    RENDERCORE_API bool IsInRenderThread();

    /* The render thread's RenderContext. CORE_CHECKs IsInRenderThread():
     * meaningful only from inside a command's own lambda, or from
     * RenderThread.cpp's drain loop itself. */
    RENDERCORE_API RenderContext& GetRenderThreadContext();

    /* GAME THREAD ONLY. Blocks until every command enqueued so far -- across
     * every image -- has executed, by enqueuing a FLUSH MARKER and waiting
     * for the render thread to reach it. A no-op if called from the render
     * thread (there is nothing to wait on: a render-thread caller's own
     * commands already ran inline, by definition). */
    RENDERCORE_API void FlushRenderCommands();

    /* Bumped whenever a render-thread caller's command runs inline rather
     * than through the queue -- see RenderCommandEnqueuer::operator(). Feeds
     * RenderStats::CommandsExecutedInline. */
    RENDERCORE_API void NoteInlineRenderCommand();

    /*
     * The callable ENQUEUE_RENDER_COMMAND(Name) produces. Never named
     * directly -- always through the macro, which supplies Name and reads
     * GImageID at the CALL SITE (the same trap CoreMemory.h's Allocate
     * documents for GImageID: read here, in a function that compiles into
     * RenderCore.dll, it would always be IMAGE_Host).
     */
    struct RenderCommandEnqueuer
    {
        const char*         Name;
        EngineCore::ImageID Image;

        template<typename TLambda>
        void operator()(TLambda&& InLambda) const
        {
            using T = typename std::decay<TLambda>::type;

            static_assert(std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value,
                          "A render command's captures must be trivial: values, POD, proxy pointers. "
                          "No std::string, no TRef -- see RenderCommand.h's header comment.");

            /*
             * SAME-FRAME SEMANTICS, NO QUEUE. A command enqueued from inside
             * a command already executing on the render thread runs right
             * here, synchronously -- preserving the behaviour GDI's own
             * GRenderCommands documents ("commands enqueued from inside a
             * command's Execute() run in the same frame") without the drain
             * loop needing to distinguish this case at all: it never sees
             * these commands, because they never reach the queue.
             */
            if(IsInRenderThread())
            {
                InLambda(GetRenderThreadContext());
                NoteInlineRenderCommand();
                return;
            }

            /*
             * RAW BYTES, NOT Memory::Alloc<TRenderCommand<T>>() -- that would
             * default-construct the whole struct, and a capturing lambda's
             * closure type has no default constructor (a reference capture
             * in particular makes one impossible even in principle), which
             * deletes TRenderCommand<T>'s own implicit default constructor
             * along with it. Memory::Allocate hands back uninitialised
             * storage exactly like Memory::AllocIn does before its placement
             * new -- see CoreMemory.h -- so the header fields and the lambda
             * are each placement-constructed here, once, in the only order
             * that is ever valid: the header first (Execute/DebugName/
             * OwningImage/Kind), then TLambda's own copy/move constructor
             * over InLambda.
             *
             * GImageID read HERE, in the caller's image -- see CoreMemory.h's
             * Allocate for why this cannot move into an out-of-line function.
             */
            using TCommand = TRenderCommand<T>;
            void* Block = EngineCore::Memory::Allocate(sizeof(TCommand), alignof(TCommand), EngineCore::POOL_Default,
                                                        EngineCore::Memory::DestructorFor<TCommand>(),
                                                        EngineCore::AF_None, EngineCore::GImageID);
            CORE_CHECK(Block != nullptr, "ENQUEUE_RENDER_COMMAND: allocation failed")

            auto* Cmd = static_cast<TCommand*>(Block);
            ::new (static_cast<void*>(static_cast<RenderCommandHeader*>(Cmd))) RenderCommandHeader{
                &TCommand::Execute, Name, Image, RCK_Command
            };
            ::new (static_cast<void*>(&Cmd->Lambda)) T(std::forward<TLambda>(InLambda));

            EnqueueRenderCommand(Cmd);
        }
    };
}

/*
 * ENQUEUE_RENDER_COMMAND(UpdateColor)([Proxy, NewColor](RenderCore::RenderContext& Ctx)
 * {
 *     Proxy->SetTint(NewColor);
 * });
 *
 * Name is stringised for DebugName and must be a bare identifier; two
 * enqueues sharing a Name are fine, it is only for a debugger or profiler to
 * read. GImageID is read here, at the macro's expansion site, which is the
 * caller's image -- see RenderCommandEnqueuer's own comment.
 */
#define ENQUEUE_RENDER_COMMAND(Name) ::RenderCore::RenderCommandEnqueuer{ #Name, ::EngineCore::GImageID }

#endif //RENDERCORE_RENDERCOMMAND_H
