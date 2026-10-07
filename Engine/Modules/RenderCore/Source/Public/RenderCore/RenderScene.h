#ifndef RENDERCORE_RENDERSCENE_H
#define RENDERCORE_RENDERSCENE_H

#include <cstddef>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"

/*
 * The render thread's container of proxies. RENDER THREAD ONLY after
 * construction -- every method here assumes it, matching RenderContext's own
 * contract; nothing about a RenderScene is safe to touch from the game
 * thread except by enqueuing a command that captures a pointer to it (the
 * pointer itself is trivially copyable, satisfying RenderCommand.h's capture
 * rule).
 *
 * A game constructs exactly one (inside Renderer, a later build-order step)
 * and only ever mutates it through commands -- AddProxy/RemoveProxy/SetView
 * are called from inside WorldBridge's own enqueued lambdas, never directly
 * from the game thread.
 */
namespace RenderCore
{
    class RenderContext;
    class RenderProxy;
    class MaterialPreviewMailbox;
    struct SceneCaptureList;   // Private/RenderCore/SceneCapture.h

#if WITH_EDITOR
    /* EDITOR ONLY -- a RequestMaterialPreview as a (trivially copyable) command. */
    /* One preview, as Renderer::RequestMaterialPreview / RequestMeshPreview
     * queue it: plain bytes, so it rides in a render command. */
    struct MaterialPreviewRequest
    {
        uint64_t                Request = 0;
        char                    Material[128] = {};     // RENDER_MATERIAL_PATH_MAX; every section's, or "" for Slots
        char                    Mesh[128] = {};         // RENDER_MESH_PATH_MAX; "" draws Material on a quad
        char                    Slots[8][128] = {};     // RENDER_MATERIAL_SLOTS: per section, when Material is ""
        int32_t                 Width  = 256;
        int32_t                 Height = 256;
        float                   Yaw   = 30.0f;          // degrees round the mesh, 0 looking along +Z
        float                   Pitch = 20.0f;          // degrees above it
        float                   Zoom  = 1.0f;           // 1 frames the mesh's bounds
        EngineCore::Color       Tint{255, 255, 255, 255};
        uint8_t                 Channel = 0;            // EMaterialPreviewChannel
        MaterialPreviewMailbox* Mailbox = nullptr;

        /* A skinned mesh's pose (ANIMATION.md): SkinBones palette entries,
         * 12 floats each; 0: its reference pose. Inline, so the request stays
         * a plain copy across threads. */
        uint32_t                SkinBones = 0;
        float                   SkinPalette[256 * 12] = {};   // SKELETON_MAX_BONES
    };
#endif

    class RENDERCORE_API RenderScene
    {
    public:
        RenderScene();
        ~RenderScene();

        RenderScene(const RenderScene&)            = delete;
        RenderScene& operator=(const RenderScene&) = delete;

        /* Stores InProxy's SceneIndex, then calls its InitRenderResources --
         * see RenderProxy.h's own lifecycle comment for why that must happen
         * here and not in InProxy's constructor. */
        void AddProxy(RenderProxy* InProxy, RenderContext& InContext);

        /* ReleaseRenderResources, swap-remove from the dense array, then
         * Memory::DestroyObject + Memory::Free -- the destructor runs
         * through the allocation header's Destruct pointer, in the image
         * that created InProxy (RENDERCORE_DEFINE_PROXY_COMPONENT's factory
         * tags every allocation with GImageID at the call site), exactly the
         * same "the header knows, not the caller" trick TUnique::Reset uses. */
        void RemoveProxy(RenderProxy* InProxy, RenderContext& InContext);

        void SetView(const RenderView& InView);
        void SetClearColor(EngineCore::Color InColor);

        /* A vertical gradient instead (Renderer::SetClearGradient). */
        void SetClearGradient(EngineCore::Color InTop, EngineCore::Color InBottom);

        /* The light the next Render() shades Lit materials with, and the
         * PostProcess materials it draws over the scene afterwards. Both are
         * re-sent every frame by WorldBridge. */
        void SetLighting(const RenderLighting& InLighting);
        void SetPostProcess(const RenderPostProcess& InPostProcess);

        /* What the view shows (Renderer::SetViewMode): its picture, Unlit, or
         * the render target named InTarget -- one of GetViewableTargets(); the
         * picture while it names none. Render thread. */
        void SetViewMode(ERenderViewMode InMode, const std::string& InTarget);

        /* The render targets the last Render() could have shown, by name
         * (RenderContext::ListViewableTargets). Any thread. */
        std::vector<std::string> GetViewableTargets() const;

        /* This frame's scene captures (WorldBridge): rendered into their
         * render targets at the start of the next Render(), before the
         * scene's own pass, so it samples them. */
        void SetCaptures(const SceneCaptureList& InCaptures);

        /* The UI over this scene (Renderer::SubmitUIDrawList): InBlock, a packed
         * UIDrawList in EngineCore::Memory, owned from here and drawn by every
         * Render() until the next; null clears it. */
        void SetUIDrawList(void* InBlock, size_t InBytes);

#if WITH_EDITOR
        /* EDITOR ONLY. Lines for the next Render() only. */
        void SetDebugLines(const DebugLine* InLines, size_t InCount);

        /* EDITOR ONLY. Drawn off-screen by the next Render(), before the back
         * buffer bind (RenderContext::RenderMaterialPreview). */
        void AddMaterialPreview(const MaterialPreviewRequest& InRequest);

        /* EDITOR ONLY. The next Render()'s final colour, without the debug
         * lines, read back into InMailbox under InRequest. */
        void AddCapture(uint64_t InRequest, MaterialPreviewMailbox* InMailbox);

        /* The render target at InPath read back after the next Render()'s
         * captures (Renderer::RequestRenderTargetReadback). */
        void AddTargetReadback(uint64_t InRequest, const std::string& InPath, MaterialPreviewMailbox* InMailbox);
#endif

        /* BeginRendering, SetViewport, Clear, draw every visible proxy whose
         * WorldBounds overlaps the current view's Bounds (or whose Bounds is
         * empty -- GameCore::AABB::Overlaps already treats an empty box as
         * "excluded from culling" the same way BoundsComponent's own default
         * does), EndRendering. */
        void Render(RenderContext& InContext);

        size_t GetProxyCount() const;

        /* Whether Render() reports its proxy counts to RenderStats. On by
         * default; a Renderer's extra scenes turn it off, so the stats stay
         * the default scene's. */
        void SetReportsStats(bool bInReports);

        /* GAME (UI) THREAD -- the one exception to this class's render-thread
         * rule. Presents the colour the last Render() finished in to InWindow,
         * through that window's own swapchain (GDI PresentRenderTargetToWindow).
         * Nothing before the first Render(). The scene must outlive the call. */
        void PresentToWindow(EngineCore::WindowHandle InWindow) const;

        /* Shutdown: ReleaseRenderResources + free every proxy still present,
         * logging a warning per one -- somebody skipped their RemoveProxy,
         * the same "cannot enforce it, but can detect and warn" posture
         * GameCore::World's own destructor takes for its delegates. */
        void ReleaseAll(RenderContext& InContext);

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERSCENE_H
