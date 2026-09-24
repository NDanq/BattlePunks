#include "Public/RenderCore/Renderer.h"

#include <cstring>
#include <type_traits>
#include <vector>

#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/AABB.h"
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
#if WITH_EDITOR
    static_assert(std::is_trivially_copyable<DebugLine>::value,
                  "DebugLine crosses to the render thread by memcpy -- see Renderer::EndFrame");
#endif

    struct Renderer::Impl
    {
        RenderThread Thread;
        RenderScene* Scene  = nullptr;   // constructed on the render thread by Initialize(); render-thread-owned from then on
        WorldBridge* Bridge = nullptr;   // game-thread-owned; wraps Scene

        bool bPrimitiveRegistered = false;

#if WITH_EDITOR
        /* Set by SetViewOverride; replaces the camera lookup every frame. */
        bool       bHasViewOverride = false;
        RenderView ViewOverride;

        /* Game-thread side of DrawDebugLine; handed over and cleared by
         * every EndFrame(). */
        std::vector<DebugLine> PendingDebugLines;

        /* Hands this frame's lines to the scene, ahead of the render command
         * EndFrame enqueues, so the scene holds them when it renders. One
         * Memory::Allocate'd copy, freed by the command -- DebugLine is
         * trivially copyable, and a command may only capture values
         * (RenderCommand.h). */
        void SubmitDebugLines()
        {
            if(Scene && !PendingDebugLines.empty())
            {
                const size_t Count = PendingDebugLines.size();
                void* Blob = EngineCore::Memory::Allocate(sizeof(DebugLine) * Count, alignof(DebugLine), EngineCore::POOL_Default,
                                                          nullptr, EngineCore::AF_None, EngineCore::GImageID);
                if(Blob)
                {
                    std::memcpy(Blob, PendingDebugLines.data(), sizeof(DebugLine) * Count);
                    RenderScene* SceneCapture = Scene;
                    ENQUEUE_RENDER_COMMAND(SetDebugLines)([SceneCapture, Blob, Count](RenderContext&)
                    {
                        SceneCapture->SetDebugLines(static_cast<const DebugLine*>(Blob), Count);
                        EngineCore::Memory::Free(Blob);
                    });
                }
            }
            PendingDebugLines.clear();
        }
#endif
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

#if WITH_EDITOR
    void Renderer::DrawDebugLine(const DebugLine& InLine)
    {
        P->PendingDebugLines.push_back(InLine);
    }

    void Renderer::DrawDebugBox(const GameCore::AABB& InBox, const EngineCore::Color InColor, const float InThicknessPixels)
    {
        if(InBox.IsEmpty())
        {
            return;
        }

        /* Corner i takes Max on axis k when bit k of i is set. An edge joins
         * two corners that differ in exactly one bit -- twelve of them. An
         * axis with no extent is collapsed first: its corners coincide, and
         * walking both would draw every remaining edge twice. */
        int FlatAxes = 0;
        if(InBox.Min.GetX() == InBox.Max.GetX()) { FlatAxes |= 1; }
        if(InBox.Min.GetY() == InBox.Max.GetY()) { FlatAxes |= 2; }
        if(InBox.Min.GetZ() == InBox.Max.GetZ()) { FlatAxes |= 4; }

        EngineCore::Vector3 Corners[8];
        for(int Index = 0; Index < 8; ++Index)
        {
            Corners[Index] = EngineCore::Vector3((Index & 1) ? InBox.Max.GetX() : InBox.Min.GetX(),
                                                 (Index & 2) ? InBox.Max.GetY() : InBox.Min.GetY(),
                                                 (Index & 4) ? InBox.Max.GetZ() : InBox.Min.GetZ());
        }
        for(int From = 0; From < 8; ++From)
        {
            if(From & FlatAxes)
            {
                continue;
            }
            for(int Bit = 1; Bit < 8; Bit <<= 1)
            {
                const int To = From | Bit;
                if(To == From || (Bit & FlatAxes))
                {
                    continue;
                }
                DebugLine Line;
                Line.Start           = Corners[From];
                Line.End             = Corners[To];
                Line.Color           = InColor;
                Line.ThicknessPixels = InThicknessPixels;
                P->PendingDebugLines.push_back(Line);
            }
        }
    }

    void Renderer::SetViewOverride(const RenderView* InView)
    {
        P->bHasViewOverride = InView != nullptr;
        if(InView)
        {
            P->ViewOverride = *InView;
        }
    }
#endif // WITH_EDITOR

    void Renderer::EndFrame(const FrameInfo& InFrame)
    {
        /* The editor's two hooks into the frame: its overlay lines and its own
         * camera. A Client or Server target has neither -- the view always
         * comes from the World's CameraComponent. */
        const RenderView* ViewOverride = nullptr;
#if WITH_EDITOR
        P->SubmitDebugLines();
        ViewOverride = P->bHasViewOverride ? &P->ViewOverride : nullptr;
#endif

        if(P->Bridge && P->Bridge->IsBound())
        {
            P->Bridge->ExtractAndRender(InFrame, ViewOverride);
        }
        else if(P->Scene)
        {
            /* No World bound yet -- still render (a clear, nothing to draw)
             * so a host's stats and pacing stay meaningful, matching
             * Renderer.h's own comment. */
            RenderScene* Scene = P->Scene;
            RenderView View = ViewOverride
                            ? *ViewOverride
                            : RenderView::MakeOrthographic(InFrame.ViewportPixels, EngineCore::Vector3(0, 1, -1), -1.0f, 1000.0f);
            View.ViewportPixels = InFrame.ViewportPixels;
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
