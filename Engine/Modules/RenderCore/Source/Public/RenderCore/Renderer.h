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

        /* Registers RenderComponent's proxy type, starts the render thread
         * (RenderThread::Start) and builds the RenderScene. False on failure. */
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
        // Compiled out of Client and Server targets.

        /* Replaces the CameraComponent view from the next EndFrame() until
         * SetViewOverride(nullptr). Copied. The frame's ViewportPixels still
         * wins. GAME THREAD ONLY. */
        void SetViewOverride(const RenderView* InView);

        /* Immediate-mode overlay line for the next EndFrame() only (see
         * DebugLine). GAME THREAD ONLY. */
        void DrawDebugLine(const DebugLine& InLine);

        /* InBox's edges via DrawDebugLine. Empty draws nothing. */
        void DrawDebugBox(const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels = 2.0f);

        /* Reloads material InName (nullptr or "": all) from disk on its next
         * draw, e.g. after a recook. Names over RENDER_MATERIAL_NAME_MAX are
         * ignored. GAME THREAD ONLY. */
        void ReloadMaterial(const char* InName);

        /* Renders material InName on an InSize (16-1024) square quad during
         * the next EndFrame() and reads it back; the readback stalls briefly,
         * so don't call every frame. Returns an id for TakeMaterialPreview, 0
         * if not running. GAME THREAD ONLY. */
        uint64_t RequestMaterialPreview(const char* InName, int InSize, EngineCore::Color InTint);

        /* True once, when done: OutImage (RGBA8, top row first), or empty with
         * OutError. False while pending. GAME THREAD ONLY. */
        bool TakeMaterialPreview(uint64_t InRequest, EngineCore::Image& OutImage, std::string& OutError);
#endif // WITH_EDITOR

        void GetStats(RenderStats& OutStats) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERER_H
