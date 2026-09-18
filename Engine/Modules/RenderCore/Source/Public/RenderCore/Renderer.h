#ifndef RENDERCORE_RENDERER_H
#define RENDERCORE_RENDERER_H

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"

namespace GameCore { class World; }

/*
 * The facade NimbleEngine::Application owns (a later build-order step) --
 * everything a host needs from RenderCore in five calls, with the render
 * thread, the command queue, RenderProxy and the World bridge all details
 * behind it. GAME THREAD ONLY.
 */
namespace RenderCore
{
    class RENDERCORE_API Renderer
    {
    public:
        Renderer();
        ~Renderer();

        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;

        /* Registers PrimitiveComponent's proxy type, starts the render
         * thread (see RenderThread::Start for the InBackend/InWindow
         * contract), and constructs the RenderScene commands from here on
         * touch exclusively through the queue. False on any failure --
         * see RenderThread::Start's own comment on what that means for
         * InBackend/InWindow. */
        bool Initialize(ERenderBackend InBackend, EngineCore::WindowHandle InWindow);

        /* UnbindWorld() if a World is still bound, drains the scene
         * (ReleaseAll), stops the render thread. NULL-SAFE AND IDEMPOTENT --
         * a host's own Shutdown() runs unconditionally even after a failed
         * Initialize(), the same contract NimbleEngine::Application::Shutdown
         * already keeps for every other subsystem it owns. */
        void Shutdown();

        /* Binds this Renderer's WorldBridge to InWorld -- see WorldBridge.h's
         * own contract for what that means (proxy lifecycle, the existing-
         * components sweep). One World at a time; binding again first
         * unbinds whatever was bound before, logging a warning. */
        void BindWorld(GameCore::World& InWorld);

        /* Reverses BindWorld(). Safe with no World bound. */
        void UnbindWorld();

        /* Blocks until the previous frame (if any) has finished rendering --
         * the "at most one frame in flight" fence, forwarded to
         * RenderThread::WaitForFrame(). Call once per host frame, before
         * EndFrame(). */
        void WaitForPreviousFrame();

        /* Extracts this frame's bound components (if a World is bound),
         * builds the view, enqueues the render, and kicks the frame --
         * forwards to WorldBridge::ExtractAndRender then
         * RenderThread::EndFrame(InFrame.FrameNumber). With no World bound,
         * still kicks an empty frame (a clear, nothing drawn) so the host's
         * pacing and stats stay meaningful even before a World exists. */
        void EndFrame(const FrameInfo& InFrame);

        /*
         * Presents the most recently completed frame to InWindow. GAME THREAD
         * ONLY -- GDI's contract, not a choice made here: GDI.h documents it as
         * "Called from the UI thread after RendererFinished()", and Vulkan's
         * swapchain keeps its own command pool and per-image semaphores so this
         * thread can submit the present blit while the render thread records
         * the next frame (both under the device queue mutex, ordered by a
         * timeline semaphore -- VKCommands.cpp EndRendering, VKSwapchain::PresentFrame).
         *
         * CALL IT AFTER WaitForPreviousFrame(), the point at which the frame
         * being presented is known complete and submitted. Presenting N-1 while
         * the game thread builds N is the one-frame-of-lag this design already
         * has, not an extra cost.
         *
         * Safe before the first frame, with no World bound, or on a backend
         * that presents differently: every implementation early-outs on its own
         * preconditions (no swapchain, nothing rendered yet, mid-resize) and the
         * GDIDynamicDriver default is empty -- which OpenGL uses deliberately,
         * presenting through SwapBuffers instead.
         */
        void PresentToWindow(EngineCore::WindowHandle InWindow);

        void SetClearColor(EngineCore::Color InColor);

        void GetStats(RenderStats& OutStats) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERER_H
