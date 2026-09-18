#ifndef RENDERCORE_RENDERSCENE_H
#define RENDERCORE_RENDERSCENE_H

#include <cstddef>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
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

        /* BeginRendering, SetViewport, Clear, draw every visible proxy whose
         * WorldBounds overlaps the current view's Bounds (or whose Bounds is
         * empty -- GameCore::AABB::Overlaps already treats an empty box as
         * "excluded from culling" the same way BoundsComponent's own default
         * does), EndRendering. */
        void Render(RenderContext& InContext);

        size_t GetProxyCount() const;

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
