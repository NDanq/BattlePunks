#include "Public/RenderCore/RenderScene.h"

#include <vector>

#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/RenderThreadInternal.h"

/*
 * A plain dense vector of proxies, swap-removed by the SceneIndex each proxy
 * carries -- the same sparse-set-adjacent shape GameCore::ComponentPool uses
 * for the same reason: O(1) remove, contiguous iteration for Render(). No
 * sparse lookup table here (unlike ComponentPool), because nothing ever asks
 * "does entity E have a proxy" through this class -- WorldBridge already
 * knows through the component's own RenderProxyHandle.
 */

namespace RenderCore
{
    struct RenderScene::Impl
    {
        std::vector<RenderProxy*> Proxies;
        RenderView                View;
        EngineCore::Color         ClearColor{24, 26, 32, 255};
    };

    RenderScene::RenderScene() : P(new Impl())
    {
    }

    RenderScene::~RenderScene()
    {
        if(!P->Proxies.empty())
        {
            RENDERCORE_LOG_WARN("RenderScene destroyed with proxies still present; "
                                 "somebody skipped RemoveProxy -- see ReleaseAll")
        }
        delete P;
    }

    void RenderScene::AddProxy(RenderProxy* InProxy, RenderContext& InContext)
    {
        if(!InProxy)
        {
            return;
        }

        InProxy->SceneIndex = int32_t(P->Proxies.size());
        P->Proxies.push_back(InProxy);

        InProxy->InitRenderResources(InContext);
    }

    void RenderScene::RemoveProxy(RenderProxy* InProxy, RenderContext& InContext)
    {
        if(!InProxy || InProxy->SceneIndex < 0)
        {
            return;
        }

        InProxy->ReleaseRenderResources(InContext);

        const size_t Index    = size_t(InProxy->SceneIndex);
        const size_t LastSlot = P->Proxies.size() - 1;

        if(Index != LastSlot)
        {
            /* Swap-and-pop: move the last live proxy into the hole, exactly
             * ComponentPool::Remove's own POD swap-remove -- except here it
             * is a pointer swap, not a memcpy, since a RenderProxy is a
             * polymorphic object rather than trivially copyable bytes. */
            RenderProxy* Moved = P->Proxies[LastSlot];
            P->Proxies[Index]  = Moved;
            Moved->SceneIndex  = int32_t(Index);
        }
        P->Proxies.pop_back();

        InProxy->SceneIndex = -1;

        EngineCore::Memory::DestroyObject(InProxy);
        EngineCore::Memory::Free(InProxy);
    }

    void RenderScene::SetView(const RenderView& InView)
    {
        P->View = InView;
    }

    void RenderScene::SetClearColor(const EngineCore::Color InColor)
    {
        P->ClearColor = InColor;
    }

    void RenderScene::Render(RenderContext& InContext)
    {
        /* Pushed here, not in SetView(), so GetView() is always in step with
         * what Render() is about to draw against -- SetView() merely records
         * what the NEXT Render() will use; a caller changing its mind
         * between SetView() and Render() (unlikely, but not forbidden) never
         * leaves GetView() reporting a view nothing has drawn against yet. */
        InContext.SetView(P->View);

        GDIBeginRendering();
        GDISetViewport(0, 0, uint(P->View.ViewportPixels.Width), uint(P->View.ViewportPixels.Height));
        GDIClear(CM_Color | CM_Depth, P->ClearColor);

        uint64_t VisibleCount = 0;
        uint64_t CulledCount  = 0;

        for(RenderProxy* Proxy : P->Proxies)
        {
            if(!Proxy->IsVisible())
            {
                continue;
            }

            /*
             * EMPTY MEANS "NO BASIS TO CULL", on EITHER side -- not "never
             * overlaps". GameCore::AABB::Overlaps already returns false the
             * instant either box IsEmpty(), which is exactly backwards for
             * this decision: a caller with no real view (a hand-built scene
             * in a test) still wants everything drawn, and a proxy that
             * never called SetLocalBounds (RenderProxy's own default
             * LocalBounds/WorldBounds -- AABB()'s empty state, unlike
             * PrimitiveProxy which always sets one in ApplyComponent) has
             * given RenderScene nothing to cull it BY -- it is not
             * "everywhere and nowhere", it is "undeclared", and undeclared
             * must default to visible or a proxy type that never opts into
             * bounds can never be drawn at all, discovered the hard way by
             * this file's own smoke test's test-defined proxy (which sets
             * no local bounds on purpose, to prove exactly this path).
             * Explicitly NOT AABB::Overlaps' own empty-handling, which this
             * comment exists to override.
             */
            const bool bViewHasBounds  = !P->View.Bounds.IsEmpty();
            const bool bProxyHasBounds = !Proxy->GetWorldBounds().IsEmpty();
            const bool bVisible        = !bViewHasBounds || !bProxyHasBounds
                                       || Proxy->GetWorldBounds().Overlaps(P->View.Bounds);

            if(!bVisible)
            {
                ++CulledCount;
                continue;
            }

            Proxy->Draw(InContext);
            ++VisibleCount;
        }

        SetSceneStats(uint64_t(P->Proxies.size()), CulledCount);

        GDIEndRendering();
    }

    size_t RenderScene::GetProxyCount() const
    {
        return P->Proxies.size();
    }

    void RenderScene::ReleaseAll(RenderContext& InContext)
    {
        while(!P->Proxies.empty())
        {
            /* RemoveProxy pops from the back on the LastSlot path, so walking
             * from the back means every removal here is the cheap case --
             * matching World's own destructor style of tearing down without
             * caring which order, only that every one is actually freed. */
            RemoveProxy(P->Proxies.back(), InContext);
        }
    }
}
