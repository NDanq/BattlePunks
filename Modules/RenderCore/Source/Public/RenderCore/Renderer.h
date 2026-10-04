#ifndef RENDERCORE_RENDERER_H
#define RENDERCORE_RENDERER_H

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"

#include <cstdint>
#include <string>
#include <vector>

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

    /*
     * A scene the Renderer draws: its own World, view and render targets.
     * Scene 0 (RENDER_DEFAULT_SCENE) exists from Initialize() to Shutdown()
     * and is what every call without a SceneID means -- a game only ever
     * needs that one. A tool adds more with CreateScene, one per viewport,
     * each presented to its own window.
     */
    using RenderSceneID = uint32_t;
    enum : RenderSceneID { RENDER_DEFAULT_SCENE = 0, RENDER_INVALID_SCENE = ~RenderSceneID(0) };

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

        // === Scenes ==========================================================
        // GAME THREAD ONLY. Every frame renders every scene; each has its own
        // World binding, view override, overlay lines and render targets.

        /* A new, empty scene. RENDER_INVALID_SCENE before Initialize() or if
         * the render thread could not build it. */
        RenderSceneID CreateScene();

        /* Unbinds its World, releases its proxies and targets (flushed: gone
         * when this returns). Stop presenting it first. The default scene and
         * unknown ids are ignored. */
        void DestroyScene(RenderSceneID InScene);

        /* BindWorld / UnbindWorld for InScene. A World is bound to one scene
         * at a time. */
        void BindWorld(RenderSceneID InScene, GameCore::World& InWorld);
        void UnbindWorld(RenderSceneID InScene);

        /* InScene's viewport, physical pixels, from the next EndFrame() on. A
         * scene other than the default with an empty viewport is not rendered
         * at all (a hidden window). The default scene takes
         * FrameInfo::ViewportPixels instead. */
        void SetViewportPixels(RenderSceneID InScene, EngineCore::Size InPixels);

        /* Presents InScene's last finished frame to InWindow, through that
         * window's own swapchain -- several windows can each show their own
         * scene. Same thread and ordering rules as PresentToWindow(InWindow). */
        void PresentToWindow(RenderSceneID InScene, EngineCore::WindowHandle InWindow);

        /* Destroys InWindow's swapchain, if it has one. Call before the native
         * window goes; presenting to it again makes a new one. */
        void ReleaseWindow(EngineCore::WindowHandle InWindow);

        /* Blocks until the previous frame (if any) has finished rendering --
         * the "at most one frame in flight" fence, forwarded to
         * RenderThread::WaitForFrame(). Call once per host frame, before
         * EndFrame(). */
        void WaitForPreviousFrame();

        /* Marks the start of the host's frame -- before input and the game's
         * tick -- for "stat unit"'s Game Thread time (RenderStats). Without
         * it, a frame's game time starts at the previous EndFrame, so it
         * also counts whatever the host waited between frames. */
        void BeginGameFrame();

        /* Extracts this frame's bound components (if a World is bound),
         * builds the view, enqueues the render, and kicks the frame --
         * forwards to WorldBridge::ExtractAndRender then
         * RenderThread::EndFrame(InFrame.FrameNumber). With no World bound,
         * still kicks an empty frame (a clear, nothing drawn) so the host's
         * pacing and stats stay meaningful even before a World exists. */
        void EndFrame(const FrameInfo& InFrame);

        /*
         * Presents the default scene's most recently completed frame to InWindow. GAME THREAD
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
        void SetClearColor(RenderSceneID InScene, EngineCore::Color InColor);

        /* The background as a vertical gradient, InTop at the top of the view
         * to InBottom at the bottom, under everything drawn (RENDERPIPELINE.md,
         * "Background"). SetClearColor goes back to one colour. */
        void SetClearGradient(RenderSceneID InScene, EngineCore::Color InTop, EngineCore::Color InBottom);

#if WITH_EDITOR
        // === Editor only =====================================================
        // Compiled out of Client and Server targets.

        /* Replaces the CameraComponent view from the next EndFrame() until
         * SetViewOverride(nullptr). Copied. The frame's ViewportPixels still
         * wins. GAME THREAD ONLY. */
        void SetViewOverride(const RenderView* InView);
        void SetViewOverride(RenderSceneID InScene, const RenderView* InView);

        /* Immediate-mode overlay line for the next EndFrame() only (see
         * DebugLine). GAME THREAD ONLY. */
        void DrawDebugLine(const DebugLine& InLine);
        void DrawDebugLine(RenderSceneID InScene, const DebugLine& InLine);

        /* InBox's edges via DrawDebugLine. Empty draws nothing. */
        void DrawDebugBox(const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels = 2.0f);
        void DrawDebugBox(RenderSceneID InScene, const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels = 2.0f);

        /* Reloads material InName (nullptr or "": all) from disk on its next
         * draw, e.g. after a recook. Names over RENDER_MATERIAL_PATH_MAX are
         * ignored. GAME THREAD ONLY. */
        void ReloadMaterial(const char* InName);

        /* Renders material InName on an InSize (16-1024) square quad during
         * the next EndFrame() and reads it back; the readback stalls briefly,
         * so don't call every frame. Returns an id for TakeMaterialPreview, 0
         * if not running. GAME THREAD ONLY. */
        uint64_t RequestMaterialPreview(const char* InName, int InSize, EngineCore::Color InTint);

        /* True once, when done: OutImage (RGBA8, top row first), or empty with
         * OutError. False while pending. Answers RequestMeshPreview too.
         * GAME THREAD ONLY. */
        bool TakeMaterialPreview(uint64_t InRequest, EngineCore::Image& OutImage, std::string& OutError);

        /*
         * Renders a mesh as a tool shows it, during the next EndFrame(), and
         * reads it back: framed by a perspective camera orbiting its bounds
         * (Yaw round it, Pitch above it, Zoom in), lit by a fixed studio light.
         * Material, when set, goes on every section; otherwise each section i
         * < 8 takes Sections[i], or (when that is "") the mesh's own material,
         * as RenderComponent slots do. A PostProcess Material is drawn full
         * screen instead. Width and Height are 16-1024. Returns an id for
         * TakeMaterialPreview, 0 if not running or a name is too long.
         * GAME THREAD ONLY.
         */
        struct MeshPreview
        {
            std::string              Mesh;        // runtime-relative .nmmesh
            std::string              Material;
            std::vector<std::string> Sections;
            int                      Width  = 256;
            int                      Height = 256;
            float                    Yaw    = 30.0f;
            float                    Pitch  = 20.0f;
            float                    Zoom   = 1.0f;
            EngineCore::Color        Tint{255, 255, 255, 255};
        };
        uint64_t RequestMeshPreview(const MeshPreview& InPreview);

        /* The default scene's final colour after the next EndFrame(), without
         * debug lines, read back for TakeMaterialPreview (a screenshot, a
         * test). Stalls briefly. 0 if not running. GAME THREAD ONLY. */
        uint64_t RequestSceneCapture();

        /* The render target at InPath (RenderTargetAsset.h) as a material
         * samples it after the default scene's next captures, through
         * TakeMaterialPreview -- what the editor bakes into the target's file. */
        uint64_t RequestRenderTargetReadback(const std::string& InPath);
#endif // WITH_EDITOR

        /*
         * Mounts InModule's shader cache, Shaders/Cache/<Module>.<Platform>.nmsc
         * (ShaderCache.h), as a read-only file layer over the installed
         * provider, and reloads every material through it; Unmount takes it
         * away again. Initialize() mounts "Engine"; GameModuleHost mounts a
         * game module's own after it starts up. A missing cache is not an
         * error: the materials load from loose files. Names of 64 characters
         * or more are ignored. GAME THREAD ONLY. Every target.
         */
        void MountShaderCache(const char* InModule);
        void UnmountShaderCache(const char* InModule);

        void GetStats(RenderStats& OutStats) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERER_H
