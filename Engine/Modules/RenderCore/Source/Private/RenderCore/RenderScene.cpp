#include "Public/RenderCore/RenderScene.h"

#include <array>
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

        /*
         * THE SCENE'S RENDER TARGETS, and the whole reason anything ever
         * reaches the screen.
         *
         * GDIDynamicDriver::PresentToWindow presents
         * CurrentStateData->ActiveRenderTarget -- the most recently bound RT
         * -- and VulkanDynamicGDI.cpp's own comment states the contract it
         * expects of a renderer: "the renderer always rebinds Main right
         * before EndRendering, so CurrentStateData->ActiveRenderTarget is
         * the main RT". Render() below is what makes that true here.
         * Without an RT bound at all, ActiveRenderTarget stays null
         * (BeginRendering clears it) and every present silently returns --
         * which is exactly what "the quad is not visible" looked like.
         *
         * A FIXED TABLE INDEXED BY THE ENUM, not a grown container: the set
         * of targets a scene has is decided at compile time by ERT_MAX, so
         * std::array bakes the size in and value-initialises every slot to
         * null. There is no reserve/resize distinction to get wrong and no
         * allocation to fail -- a vector here only offers ways to be holding
         * it wrong.
         *
         * Deliberately NOT GDIRenderTargetManager, which holds
         * Main/Buffer1/PostBuffer1 and the Resolve/Restore pair: those are
         * paint-application concepts (they exist for canvas stroke
         * buffering). A scene with one pass needs one target; the other two
         * would be three times the VRAM for nothing. ERT_MAX is where a
         * second pass would be added.
         */
        enum ERenderTarget : uint8_t
        {
            ERT_BackBuffer = 0,
            ERT_MAX
        };

        std::array<GDIRenderTarget*, ERT_MAX> RenderTargets{};

        /* The size every entry in RenderTargets was built at; what
         * AllocateRenderTargets compares against to decide whether this
         * frame needs to do anything at all. Zero until the first one. */
        uint WindowWidth  = 0;
        uint WindowHeight = 0;

        /* Creates the targets, or resizes them to match InViewportSize.
         * Render thread only, like everything else on this class.
         *
         * CHEAP AND IDEMPOTENT ON PURPOSE -- Render() calls this every
         * frame, so the steady-state path must be the early-out below.
         * Recreating a GPU image per frame would not merely be slow, it
         * would destroy the target the UI thread is presenting from while
         * it may still be sampling it. */
        void AllocateRenderTargets(const EngineCore::Size InViewportSize)
        {
            /* A zero-sized viewport (a minimised window, or a view built
             * before the first real pixel size is known) would make an
             * invalid image; keep whatever we already had and let the next
             * frame with a real size sort it out. */
            if(InViewportSize.Width <= 0 || InViewportSize.Height <= 0)
            {
                return;
            }

            const uint Width  = uint(InViewportSize.Width);
            const uint Height = uint(InViewportSize.Height);

            if(WindowWidth == Width && WindowHeight == Height)
            {
                return;
            }

            if(!RenderTargets[ERT_BackBuffer])
            {
                RenderTargets[ERT_BackBuffer] = GDICreateRenderTarget("RenderCore.BackBuffer", Width, Height);
            }
            else
            {
                /* GDIRenderTarget::Resize is the backend's own recreate
                 * path -- on Vulkan it also raises the driver's bResizing
                 * flag, which makes the UI-thread present skip until a full
                 * frame has been rendered into the new images (cleared by
                 * EndRendering). Nothing extra to coordinate here, and it
                 * is why this is a Resize rather than a release-and-create. */
                RenderTargets[ERT_BackBuffer]->Resize(Width, Height);
            }

            WindowWidth  = Width;
            WindowHeight = Height;
        }

        GDIRenderTarget* GetRenderTarget(const ERenderTarget InRenderTarget) const
        {
            /* std::array carries its own bound, so this is the enum's
             * contract being checked, not the container's. */
            if(InRenderTarget < ERT_MAX)
            {
                return RenderTargets[InRenderTarget];
            }
            return nullptr;
        }

        /* Idempotent: every slot is nulled as it is freed, so a scene that
         * runs ReleaseAll() and is then destroyed does not double-free. */
        void ReleaseRenderTargets()
        {
            for(GDIRenderTarget*& RenderTarget : RenderTargets)
            {
                if(RenderTarget)
                {
                    GDIReleaseRenderTarget(RenderTarget);
                    delete RenderTarget;
                    RenderTarget = nullptr;
                }
            }

            WindowWidth  = 0;
            WindowHeight = 0;
        }
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
        P->ReleaseRenderTargets();

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

        /* Created on first use and resized with the viewport -- see Impl's
         * own comment on why a scene owns these at all. Early-outs on every
         * frame whose viewport has not changed, which is nearly all of them. */
        P->AllocateRenderTargets(P->View.ViewportPixels);

        GDIBeginRendering();

        /*
         * AFTER BeginRendering, never before: VulkanDynamicGDI::SetRenderTarget
         * bails out entirely when CurrentCmd is null (i.e. outside a
         * BeginRendering/EndRendering pair), so a bind placed above would be
         * a silent no-op and the clear and draws below would land nowhere --
         * GDIRenderTarget.cpp's own DrawTexture comment describes exactly
         * this trap ("on Vulkan Bind() is a no-op -- the driver must open a
         * dynamic-rendering region ... or the clear and draw below are
         * silently dropped").
         *
         * This is also what leaves the back buffer as the driver's
         * ActiveRenderTarget when EndRendering runs, which is what
         * PresentToWindow presents and what EndRendering transitions to
         * SHADER_READ_ONLY_OPTIMAL for the present blit to sample.
         */
        if(GDIRenderTarget* BackBuffer = P->GetRenderTarget(Impl::ERT_BackBuffer))
        {
            GDISetRenderTarget(BackBuffer);
        }

        GDISetViewport(0, 0, uint(P->View.ViewportPixels.Width), uint(P->View.ViewportPixels.Height));

        /*
         * THE COLOUR MASK IS NOT OPTIONAL, and nothing supplies a usable
         * default: VKBlendState's ColorAttachment is a value-initialised
         * VkPipelineColorBlendAttachmentState, so colorWriteMask starts at
         * ZERO -- every channel masked off. The pipeline builds, the draw is
         * recorded, vkCmdDraw executes, fragments are shaded, and not one
         * byte reaches the attachment. An entirely clear image with healthy
         * draw-call counts and no validation error is exactly what that
         * looks like from the outside, and is what it looked like here.
         *
         * GDI's own GDIRenderTarget::DrawTexture opens with this same call
         * for this same reason; it was simply the only caller, because every
         * other drawing path in the tree goes through it. A scene that
         * renders its own geometry is the case nobody had written yet.
         *
         * Blend and depth are set alongside it rather than left implicit:
         * the pipeline key is built from whatever VKCurrentStateData happens
         * to hold, so "never set it" means "inherit whatever the last
         * unrelated caller left there", which is not a contract a scene can
         * rely on. Straight alpha blending and a plain less-than depth test
         * are what a 2D sprite pass wants.
         */
        GDISetColorMask(EColM_RGBA);
        GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
        GDISetDepthState(IDepthStateDesc());

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

        /* AFTER the proxies, not before: a proxy's own ReleaseRenderResources
         * may still reference the target it was drawing into. Runs on the
         * render thread, like the rest of this class, and before
         * Renderer::Shutdown stops that thread -- which is what keeps the
         * GPU image's destruction on the thread that owns the context. */
        P->ReleaseRenderTargets();
    }
}
