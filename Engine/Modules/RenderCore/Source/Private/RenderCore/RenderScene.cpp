#include "Public/RenderCore/RenderScene.h"

#include <atomic>
#include <vector>

#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Private/RenderCore/MaterialPreview.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/RenderThreadInternal.h"
#include "Private/RenderCore/SceneTargets.h"

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
#if WITH_EDITOR
        std::vector<DebugLine>    DebugLines;   // consumed by the next Render()
        std::vector<MaterialPreviewRequest> Previews;   // likewise
#endif
        RenderView                View;
        EngineCore::Color         ClearColor{24, 26, 32, 255};
        RenderLighting            Lighting;
        RenderPostProcess         PostProcess;

        /*
         * THE SCENE'S RENDER TARGETS, and the whole reason anything reaches
         * the screen. GDIDynamicDriver::PresentToWindow presents
         * CurrentStateData->ActiveRenderTarget -- the most recently bound RT --
         * which RenderContext::RenderSceneStages leaves as the scene's final
         * colour (after PostProcess, if any). With no RT bound at all,
         * ActiveRenderTarget stays null and every present silently returns.
         *
         * Deliberately NOT GDIRenderTargetManager, whose Main/Buffer1/
         * PostBuffer1 and Resolve/Restore pair are paint-application concepts
         * (canvas stroke buffering). See SceneTargets.h.
         *
         * Resized every frame the viewport changes, and cheap otherwise:
         * recreating a GPU image per frame would destroy the target the UI
         * thread is presenting from while it may still be sampling it. A
         * zero-sized viewport (a minimised window) keeps what there was.
         */
        SceneTargets Targets{"RenderCore.Scene"};

        /* Targets.Final as of the last Render(), for PresentToWindow on the UI
         * thread. A target is resized in place, never replaced, while the
         * scene lives, so the pointer stays good; GDI skips a present while a
         * resize is in progress. */
        std::atomic<GDIRenderTarget*> PresentTarget{nullptr};

        bool bReportsStats = true;
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

        /* Belt and braces: ReleaseAll() is the ordinary path and already
         * did this, but a scene destroyed without it (a test, an early
         * failure) must still not leak a GPU image. Idempotent. */
        P->Targets.Release();

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

    void RenderScene::SetLighting(const RenderLighting& InLighting)
    {
        P->Lighting = InLighting;
    }

    void RenderScene::SetPostProcess(const RenderPostProcess& InPostProcess)
    {
        P->PostProcess = InPostProcess;
    }

#if WITH_EDITOR
    void RenderScene::SetDebugLines(const DebugLine* InLines, const size_t InCount)
    {
        P->DebugLines.assign(InLines, InLines + InCount);
    }

    void RenderScene::AddMaterialPreview(const MaterialPreviewRequest& InRequest)
    {
        P->Previews.push_back(InRequest);
    }
#endif

    void RenderScene::Render(RenderContext& InContext)
    {
        /* Pushed here, not in SetView(), so GetView() is always in step with
         * what Render() is about to draw against -- SetView() merely records
         * what the NEXT Render() will use; a caller changing its mind
         * between SetView() and Render() (unlikely, but not forbidden) never
         * leaves GetView() reporting a view nothing has drawn against yet. */
        InContext.SetView(P->View);
        InContext.SetLighting(P->Lighting);

        /* Created on first use and resized with the viewport -- see Impl's
         * own comment on why a scene owns these at all. Early-outs on every
         * frame whose viewport has not changed, which is nearly all of them. */
        if(P->View.ViewportPixels.Width > 0 && P->View.ViewportPixels.Height > 0)
        {
            P->Targets.Allocate(uint(P->View.ViewportPixels.Width), uint(P->View.ViewportPixels.Height));
        }

        GDIBeginRendering();

#if WITH_EDITOR
        /* Previews FIRST, before the scene's own stages, so the scene's final
         * target is the active one at EndRendering and the present never
         * shows a preview. */
        for(const MaterialPreviewRequest& Each : P->Previews)
        {
            EngineCore::Image Pixels;
            std::string       Error;
            InContext.RenderMaterialPreview(Each, Pixels, Error);
            if(Each.Mailbox) { Each.Mailbox->Deliver(Each.Request, std::move(Pixels), std::move(Error)); }
        }
        P->Previews.clear();
#endif

        /*
         * PrePass: queue each visible proxy's draws in their stages' draw
         * policies (RENDERPIPELINE.md). The stages themselves run below, AFTER
         * BeginRendering, never before: VulkanDynamicGDI::SetRenderTarget bails
         * out entirely when CurrentCmd is null (outside a BeginRendering/
         * EndRendering pair), so a bind placed above it would be a silent no-op.
         */
        InContext.BeginSceneDraws();

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
             * overlaps", which is what GameCore::AABB::Overlaps gives (it
             * returns false the instant either box IsEmpty()) and is exactly
             * backwards here. A caller with no real view still wants
             * everything drawn, and a proxy that never called SetLocalBounds
             * has given RenderScene nothing to cull it BY: undeclared, not
             * "everywhere and nowhere". Undeclared must default to visible or
             * a proxy type that never opts into bounds could never be drawn --
             * this file's smoke test has one, deliberately, to prove the path.
             * Explicitly overrides AABB::Overlaps' own empty-handling.
             */
            const bool bViewHasBounds  = !P->View.Bounds.IsEmpty();
            const bool bProxyHasBounds = !Proxy->GetWorldBounds().IsEmpty();

            /* World Position Offset can move geometry past its bounds: pad
             * them by the largest Max Offset among its materials. */
            GameCore::AABB Bounds = Proxy->GetWorldBounds();
            const float Padding = bViewHasBounds && bProxyHasBounds ? Proxy->GetBoundsPadding(InContext) : 0.0f;
            if(Padding > 0.0f)
            {
                const EngineCore::Vector3 Pad(Padding, Padding, Padding);
                Bounds = GameCore::AABB(Bounds.Min - Pad, Bounds.Max + Pad);
            }
            const bool bVisible = !bViewHasBounds || !bProxyHasBounds || Bounds.Overlaps(P->View.Bounds);

            if(!bVisible)
            {
                ++CulledCount;
                continue;
            }

            Proxy->Draw(InContext);
            ++VisibleCount;
        }

        /* The scene's Post Process components, Priority low first (WorldBridge sorted them). */
        for(uint32_t Index = 0; Index < P->PostProcess.Count && Index < RENDER_POST_PROCESS_MAX; ++Index)
        {
            InContext.SubmitPostProcess(P->PostProcess.Materials[Index]);
        }

        /* Base Pass, Lighting, Unlit, the translucent stages, PostProcess;
         * leaves the final colour bound, for the present and the overlay. */
        InContext.RenderSceneStages(P->Targets, P->ClearColor);

        if(P->bReportsStats)
        {
            SetSceneStats(uint64_t(P->Proxies.size()), CulledCount);
        }

#if WITH_EDITOR
        /* Tool overlay over everything, unculled; consumed each frame. */
        if(!P->DebugLines.empty())
        {
            /* No depth test: an outline or a gizmo shows through what it marks. */
            GDISetDepthState(IDepthStateDesc());
            InContext.BeginDrawEvent("DebugLines");
            for(const DebugLine& Line : P->DebugLines)
            {
                InContext.DrawLine(Line.Start, Line.End, Line.Color, Line.ThicknessPixels);
            }
            InContext.EndDrawEvent();
            P->DebugLines.clear();
        }
#endif

        GDIEndRendering();

        /* After EndRendering: the final colour is now in its presentable state. */
        P->PresentTarget.store(P->Targets.Final, std::memory_order_release);
    }

    void RenderScene::SetReportsStats(const bool bInReports)
    {
        P->bReportsStats = bInReports;
    }

    void RenderScene::PresentToWindow(const EngineCore::WindowHandle InWindow) const
    {
        GDIRenderTarget* Target = P->PresentTarget.load(std::memory_order_acquire);
        if(Target && GDynamicGDI)
        {
            GDynamicGDI->PresentRenderTargetToWindow(InWindow, Target);
        }
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

        /* AFTER the proxies, not before: a proxy's own ReleaseRenderResources
         * may still reference the target it was drawing into. Runs on the
         * render thread, like the rest of this class, and before
         * Renderer::Shutdown stops that thread -- which is what keeps the
         * GPU image's destruction on the thread that owns the context. */
        P->PresentTarget.store(nullptr, std::memory_order_release);
        P->Targets.Release();
    }
}
