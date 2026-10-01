#ifndef RENDERCORE_RENDERER_H
#define RENDERCORE_RENDERER_H

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"

#include <cstdint>
#include <string>

namespace GameCore { class World; struct AABB; }
namespace EngineCore { class Image; }

/*
 * The facade NimbleEngine::Application owns (a later build-order step) --
 * everything a host needs from RenderCore in five calls, with the render
 * thread, the command queue, RenderProxy and the World bridge all details
 * behind it. GAME THREAD ONLY.
 */
namespace RenderCore
{
    struct RenderView;

    class RENDERCORE_API Renderer
    {
    public:
        Renderer();
        ~Renderer();

        Renderer(const Renderer&)            = delete;
        Renderer& operator=(const Renderer&) = delete;

        /* Registers RenderComponent's proxy type, starts the render
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

#if WITH_EDITOR
        // === Editor only =====================================================
        // Compiled out of Client and Server targets (Engine/CMakeLists.txt,
        // NIMBLE_TARGET): a game renders through its CameraComponent and has
        // no overlay, so neither the entry points nor their per-frame work
        // exist there.

        /*
         * Replaces the view WorldBridge would build from the World's first
         * active CameraComponent -- for a tool with its own camera (the
         * editor viewport), which must not have to move an entity to look
         * around. Copied, so InView need not outlive the call; applies from
         * the next EndFrame() until SetViewOverride(nullptr). The frame's
         * ViewportPixels still wins over InView->ViewportPixels, since the
         * render targets are sized from it. GAME THREAD ONLY.
         */
        void SetViewOverride(const RenderView* InView);

        /*
         * Queue a world-space line for the NEXT EndFrame() only -- an
         * immediate-mode overlay for tools (selection bounds, gizmos), drawn
         * after every proxy and never culled. Call again every frame the line
         * should stay. See DebugLine (RenderTypes.h) and
         * RenderContext::DrawLine for how it is drawn. GAME THREAD ONLY.
         */
        void DrawDebugLine(const DebugLine& InLine);

        /* The twelve edges of InBox, through DrawDebugLine. An empty box
         * (GameCore::AABB's default) draws nothing; a box flat on an axis
         * draws its outline once, that axis collapsed. */
        void DrawDebugBox(const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels = 2.0f);

        /*
         * Forgets material InName -- every material, for nullptr or "" -- so
         * its next draw reads Materials/<Name>.material and its cooked shader
         * from disk again. For the editor, after it recooks a material graph:
         * the change shows from the next frame. GAME THREAD ONLY; queued like
         * any render command. Names longer than RENDER_MATERIAL_NAME_MAX
         * are ignored, as no primitive can refer to them.
         */
        void ReloadMaterial(const char* InName);

        /*
         * Asks the render thread for a picture of material InName: a quad
         * filling InSize x InSize pixels (16 to 1024), InTint as its Instance
         * Tint, drawn by this renderer -- same backend, same material library,
         * same cooked shader the viewport uses -- into a target of its own
         * during the next EndFrame(), and read back. Returns a request id for
         * TakeMaterialPreview; 0 if the renderer is not running. GAME THREAD
         * ONLY. The readback stalls that frame briefly: a tool asks after an
         * edit, not every frame.
         */
        uint64_t RequestMaterialPreview(const char* InName, int InSize, EngineCore::Color InTint);

        /* The preview for InRequest once the render thread has drawn it: true
         * exactly once, moving it into OutImage (RGBA8, top row first) --
         * empty, with OutError, if it could not be drawn. False while it is
         * still pending. GAME THREAD ONLY. */
        bool TakeMaterialPreview(uint64_t InRequest, EngineCore::Image& OutImage, std::string& OutError);
#endif // WITH_EDITOR

        void GetStats(RenderStats& OutStats) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERER_H
