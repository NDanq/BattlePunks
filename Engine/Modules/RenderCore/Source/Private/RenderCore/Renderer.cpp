#include "Public/RenderCore/Renderer.h"

#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/RenderThread.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/WorldBridge.h"

/*
 * The facade. See Renderer.h for the per-method contract; this file wires
 * RenderThread, RenderScene and WorldBridge together in the order
 * Initialize/Shutdown/BindWorld/UnbindWorld/EndFrame need them touched.
 */

namespace RenderCore
{
    struct Renderer::Impl
    {
        RenderThread Thread;
        RenderScene* Scene  = nullptr;   // constructed on the render thread by Initialize(); render-thread-owned from then on
        WorldBridge* Bridge = nullptr;   // game-thread-owned; wraps Scene

        bool bPrimitiveRegistered = false;
    };

    Renderer::Renderer() : P(new Impl())
    {
    }

    Renderer::~Renderer()
    {
        Shutdown();
        delete P;
    }

    bool Renderer::Initialize(const ERenderBackend InBackend, const EngineCore::WindowHandle InWindow)
    {
        /* Registered once per process in practice (RegisterProxyType refuses
         * a duplicate), but Initialize() may run more than once across a
         * test's lifetime -- guard rather than log a spurious error every
         * time. */
        if(!P->bPrimitiveRegistered)
        {
            P->bPrimitiveRegistered = RegisterProxyType(PrimitiveComponent::StaticType().ID, GetPrimitiveComponentFactory());
        }

        if(!P->Thread.Start(InBackend, InWindow))
        {
            return false;
        }

        RenderScene** ScenePtr = &P->Scene;
        ENQUEUE_RENDER_COMMAND(CreateScene)([ScenePtr](RenderContext&)
        {
            *ScenePtr = new RenderScene();
        });
        FlushRenderCommands();

        if(!P->Scene)
        {
            RENDERCORE_LOG_ERROR("Renderer::Initialize: the render thread failed to construct a RenderScene")
            P->Thread.Stop();
            return false;
        }

        P->Bridge = new WorldBridge(*P->Scene);
        return true;
    }

    void Renderer::Shutdown()
    {
        UnbindWorld();

        delete P->Bridge;
        P->Bridge = nullptr;

        if(P->Scene && P->Thread.IsRunning())
        {
            RenderScene* Scene = P->Scene;
            ENQUEUE_RENDER_COMMAND(ReleaseScene)([Scene](RenderContext& Ctx)
            {
                Scene->ReleaseAll(Ctx);
            });
            FlushRenderCommands();
        }

        P->Thread.Stop();

        if(P->Scene)
        {
            /* The scene itself was allocated with plain new (RenderScene has
             * no GAMECORE_-style pool contract to honour), on the render
             * thread inside Initialize() -- freed here, on the game thread,
             * only after Stop() has joined that thread, so nothing is still
             * touching it. */
            delete P->Scene;
            P->Scene = nullptr;
        }
    }

    void Renderer::BindWorld(GameCore::World& InWorld)
    {
        if(!P->Bridge)
        {
            RENDERCORE_LOG_ERROR("Renderer::BindWorld called before a successful Initialize()")
            return;
        }
        P->Bridge->BindWorld(InWorld);
    }

    void Renderer::UnbindWorld()
    {
        if(P->Bridge)
        {
            P->Bridge->UnbindWorld();
        }
    }

    void Renderer::WaitForPreviousFrame()
    {
        P->Thread.WaitForFrame();
    }

    void Renderer::EndFrame(const FrameInfo& InFrame)
    {
        if(P->Bridge && P->Bridge->IsBound())
        {
            P->Bridge->ExtractAndRender(InFrame);
        }
        else if(P->Scene)
        {
            /* No World bound yet -- still render (a clear, nothing to draw)
             * so a host's stats and pacing stay meaningful, matching
             * Renderer.h's own comment. */
            RenderScene* Scene = P->Scene;
            const RenderView View = RenderView::MakeOrthographic(InFrame.ViewportPixels, EngineCore::Vector3(0, 1, -1), -1.0f, 1000.0f);
            ENQUEUE_RENDER_COMMAND(SetViewAndRenderEmpty)([Scene, View](RenderContext& Ctx)
            {
                Scene->SetView(View);
                Scene->Render(Ctx);
            });
        }

        P->Thread.EndFrame(InFrame.FrameNumber);
    }

    void Renderer::PresentToWindow(const EngineCore::WindowHandle InWindow)
    {
        /*
         * Straight through to the driver -- no command, no queue. This is
         * the ONE piece of GPU work RenderCore deliberately does NOT route
         * through the render thread, because GDI's present path is built
         * for a second thread to run it (its own command pool, its own
         * per-swapchain-image semaphores, submitted under the device queue
         * mutex and ordered against the render thread by a timeline
         * semaphore). Routing it through the render thread would serialise
         * exactly the two things that machinery exists to overlap. See
         * Renderer.h for the full contract.
         *
         * The render thread must be running: the driver, its swapchain and
         * the target being presented all belong to it, and after Stop() the
         * driver has already been destroyed and released.
         */
        if(!P->Thread.IsRunning())
        {
            return;
        }

        if(GDynamicGDI)
        {
            GDynamicGDI->PresentToWindow(InWindow);
        }
    }

    void Renderer::SetClearColor(const EngineCore::Color InColor)
    {
        if(!P->Scene)
        {
            return;
        }
        RenderScene* Scene = P->Scene;
        ENQUEUE_RENDER_COMMAND(SetClearColor)([Scene, InColor](RenderContext&)
        {
            Scene->SetClearColor(InColor);
        });
    }

    void Renderer::GetStats(RenderStats& OutStats) const
    {
        P->Thread.GetStats(OutStats);
    }
}
