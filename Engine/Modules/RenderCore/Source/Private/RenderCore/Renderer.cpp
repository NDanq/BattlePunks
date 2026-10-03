#include "Public/RenderCore/Renderer.h"

#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>

#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/AABB.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/RenderThread.h"
#include "Private/RenderCore/MaterialPreview.h"
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

    /* One scene the Renderer draws: the render thread's RenderScene and the
     * game thread's bridge and per-frame editor state for it. */
    struct RendererScene
    {
        RenderSceneID      ID     = RENDER_INVALID_SCENE;
        RenderScene*       Scene  = nullptr;   // constructed on the render thread; render-thread-owned from then on
        WorldBridge*       Bridge = nullptr;   // game-thread-owned; wraps Scene
        EngineCore::Size   ViewportPixels;     // extra scenes only; the default takes FrameInfo's

#if WITH_EDITOR
        /* Set by SetViewOverride; replaces the camera lookup every frame. */
        bool       bHasViewOverride = false;
        RenderView ViewOverride;

        /* Handed over and cleared by every EndFrame(). */
        std::vector<DebugLine> PendingDebugLines;

        /* Enqueued ahead of EndFrame's render command, as one allocated copy
         * the command frees (commands capture values only). */
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

    struct Renderer::Impl
    {
        RenderThread Thread;

        /* [0] is the default scene, from Initialize() to Shutdown(). */
        std::vector<RendererScene*> Scenes;
        RenderSceneID               NextSceneID = RENDER_DEFAULT_SCENE + 1;

        bool bRenderRegistered = false;

#if WITH_EDITOR
        /* RequestMaterialPreview's results, delivered by the render thread. */
        MaterialPreviewMailbox Previews;
        uint64_t               LastPreviewRequest = 0;
#endif

        RendererScene* Find(const RenderSceneID InScene) const
        {
            for(RendererScene* Each : Scenes)
            {
                if(Each->ID == InScene) { return Each; }
            }
            return nullptr;
        }

        /* The default scene's RenderScene: previews and the clear colour's default. */
        RenderScene* DefaultScene() const
        {
            return Scenes.empty() ? nullptr : Scenes.front()->Scene;
        }

        /* Builds InID's RenderScene on the render thread (flushed) and its
         * bridge. Null if the render thread is not running or failed. */
        RendererScene* MakeScene(const RenderSceneID InID)
        {
            if(!Thread.IsRunning())
            {
                return nullptr;
            }
            RenderScene* Built = nullptr;
            RenderScene** ScenePtr = &Built;
            ENQUEUE_RENDER_COMMAND(CreateScene)([ScenePtr](RenderContext&)
            {
                *ScenePtr = new RenderScene();
            });
            FlushRenderCommands();
            if(!Built)
            {
                return nullptr;
            }

            auto* Made   = new RendererScene();
            Made->ID     = InID;
            Made->Scene  = Built;
            Made->Bridge = new WorldBridge(*Built);
            return Made;
        }

        /* Unbinds, releases on the render thread (flushed), frees. */
        void FreeScene(RendererScene* InScene)
        {
            if(InScene->Bridge)
            {
                InScene->Bridge->UnbindWorld();
                delete InScene->Bridge;
                InScene->Bridge = nullptr;
            }
            if(InScene->Scene && Thread.IsRunning())
            {
                RenderScene* Scene = InScene->Scene;
                ENQUEUE_RENDER_COMMAND(ReleaseScene)([Scene](RenderContext& Ctx)
                {
                    Scene->ReleaseAll(Ctx);
                });
                FlushRenderCommands();
            }
            /* The scene itself was allocated with plain new (RenderScene has
             * no GAMECORE_-style pool contract to honour), on the render
             * thread -- freed here, on the game thread, once nothing on the
             * render thread is touching it: after the flush above, or after
             * Stop() has joined the thread. */
            delete InScene->Scene;
            delete InScene;
        }
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
        if(!P->bRenderRegistered)
        {
            P->bRenderRegistered = RegisterProxyType(RenderComponent::StaticType().ID, GetRenderComponentFactory());
        }

        if(!P->Thread.Start(InBackend, InWindow))
        {
            return false;
        }

        RendererScene* Default = P->MakeScene(RENDER_DEFAULT_SCENE);
        if(!Default)
        {
            RENDERCORE_LOG_ERROR("Renderer::Initialize: the render thread failed to construct a RenderScene")
            P->Thread.Stop();
            return false;
        }
        P->Scenes.push_back(Default);

        /* The engine's own materials (DefaultLit) and shared vertex shaders. */
        MountShaderCache("Engine");
        return true;
    }

    void Renderer::Shutdown()
    {
        /* Extra scenes first, then the default: each unbinds, then drains. */
        while(!P->Scenes.empty())
        {
            RendererScene* Last = P->Scenes.back();
            P->Scenes.pop_back();
            P->FreeScene(Last);
        }

        P->Thread.Stop();
    }

    void Renderer::BindWorld(GameCore::World& InWorld)
    {
        BindWorld(RENDER_DEFAULT_SCENE, InWorld);
    }

    void Renderer::UnbindWorld()
    {
        UnbindWorld(RENDER_DEFAULT_SCENE);
    }

    void Renderer::BindWorld(const RenderSceneID InScene, GameCore::World& InWorld)
    {
        RendererScene* Scene = P->Find(InScene);
        if(!Scene || !Scene->Bridge)
        {
            RENDERCORE_LOG_ERROR("Renderer::BindWorld called before a successful Initialize(), or with an unknown scene")
            return;
        }
        Scene->Bridge->BindWorld(InWorld);
    }

    void Renderer::UnbindWorld(const RenderSceneID InScene)
    {
        if(RendererScene* Scene = P->Find(InScene))
        {
            if(Scene->Bridge) { Scene->Bridge->UnbindWorld(); }
        }
    }

    RenderSceneID Renderer::CreateScene()
    {
        if(P->Scenes.empty())
        {
            return RENDER_INVALID_SCENE;   // before Initialize(), or after Shutdown()
        }
        RendererScene* Made = P->MakeScene(P->NextSceneID);
        if(!Made)
        {
            RENDERCORE_LOG_ERROR("Renderer::CreateScene: the render thread failed to construct a RenderScene")
            return RENDER_INVALID_SCENE;
        }
        ++P->NextSceneID;

        /* RenderStats stay the default scene's. */
        RenderScene* Scene = Made->Scene;
        ENQUEUE_RENDER_COMMAND(SceneStatsOff)([Scene](RenderContext&)
        {
            Scene->SetReportsStats(false);
        });

        P->Scenes.push_back(Made);
        return Made->ID;
    }

    void Renderer::DestroyScene(const RenderSceneID InScene)
    {
        if(InScene == RENDER_DEFAULT_SCENE)
        {
            return;
        }
        for(auto It = P->Scenes.begin(); It != P->Scenes.end(); ++It)
        {
            if((*It)->ID == InScene)
            {
                RendererScene* Gone = *It;
                P->Scenes.erase(It);
                P->FreeScene(Gone);
                return;
            }
        }
    }

    void Renderer::SetViewportPixels(const RenderSceneID InScene, const EngineCore::Size InPixels)
    {
        if(RendererScene* Scene = P->Find(InScene))
        {
            Scene->ViewportPixels = InPixels;
        }
    }

    void Renderer::WaitForPreviousFrame()
    {
        P->Thread.WaitForFrame();
    }

#if WITH_EDITOR
    void Renderer::DrawDebugLine(const DebugLine& InLine)
    {
        DrawDebugLine(RENDER_DEFAULT_SCENE, InLine);
    }

    void Renderer::DrawDebugLine(const RenderSceneID InScene, const DebugLine& InLine)
    {
        if(RendererScene* Scene = P->Find(InScene))
        {
            Scene->PendingDebugLines.push_back(InLine);
        }
    }

    void Renderer::DrawDebugBox(const GameCore::AABB& InBox, const EngineCore::Color InColor, const float InThicknessPixels)
    {
        DrawDebugBox(RENDER_DEFAULT_SCENE, InBox, InColor, InThicknessPixels);
    }

    void Renderer::DrawDebugBox(const RenderSceneID InScene, const GameCore::AABB& InBox, const EngineCore::Color InColor,
                                const float InThicknessPixels)
    {
        RendererScene* Scene = P->Find(InScene);
        if(!Scene || InBox.IsEmpty())
        {
            return;
        }

        /* Corner i takes Max on axis k when bit k is set; edges join corners
         * one bit apart. Flat axes are collapsed to avoid duplicate edges. */
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
                Scene->PendingDebugLines.push_back(Line);
            }
        }
    }

    void Renderer::ReloadMaterial(const char* InName)
    {
        if(!P->Thread.IsRunning())
        {
            return;
        }

        /* By value: commands capture trivially copyable values only. */
        struct NameBuffer { char Text[RENDER_MATERIAL_PATH_MAX]; };
        NameBuffer Name{};
        const size_t Length = InName ? std::strlen(InName) : 0;
        if(Length >= sizeof(Name.Text))
        {
            return;
        }
        if(Length > 0) { std::memcpy(Name.Text, InName, Length); }

        ENQUEUE_RENDER_COMMAND(ReloadMaterial)([Name](RenderContext& Ctx)
        {
            Ctx.ReloadMaterial(Name.Text[0] ? Name.Text : nullptr);
        });
    }

    uint64_t Renderer::RequestMaterialPreview(const char* InName, const int InSize, const EngineCore::Color InTint)
    {
        const size_t Length = InName ? std::strlen(InName) : 0;
        if(!P->DefaultScene() || !P->Thread.IsRunning() || Length == 0 || Length >= RENDER_MATERIAL_PATH_MAX)
        {
            return 0;
        }

        MaterialPreviewRequest Request;
        Request.Request = ++P->LastPreviewRequest;
        std::memcpy(Request.Material, InName, Length);
        Request.Width   = InSize;
        Request.Height  = InSize;
        Request.Tint    = InTint;
        Request.Mailbox = &P->Previews;

        RenderScene* Scene = P->DefaultScene();
        ENQUEUE_RENDER_COMMAND(AddMaterialPreview)([Scene, Request](RenderContext&)
        {
            Scene->AddMaterialPreview(Request);
        });
        return Request.Request;
    }

    uint64_t Renderer::RequestMeshPreview(const MeshPreview& InPreview)
    {
        MaterialPreviewRequest Request;
        const auto Copy = [](const std::string& InText, char* OutBuffer, const size_t InCapacity)
        {
            if(InText.size() >= InCapacity) { return false; }
            std::memcpy(OutBuffer, InText.data(), InText.size());
            return true;
        };
        if(!P->DefaultScene() || !P->Thread.IsRunning() || InPreview.Mesh.empty()
           || !Copy(InPreview.Mesh, Request.Mesh, sizeof(Request.Mesh))
           || !Copy(InPreview.Material, Request.Material, sizeof(Request.Material)))
        {
            return 0;
        }
        for(size_t Slot = 0; Slot < InPreview.Sections.size() && Slot < std::size(Request.Slots); ++Slot)
        {
            if(!Copy(InPreview.Sections[Slot], Request.Slots[Slot], sizeof(Request.Slots[Slot]))) { return 0; }
        }
        Request.Request = ++P->LastPreviewRequest;
        Request.Width   = InPreview.Width;
        Request.Height  = InPreview.Height;
        Request.Yaw     = InPreview.Yaw;
        Request.Pitch   = InPreview.Pitch;
        Request.Zoom    = InPreview.Zoom;
        Request.Tint    = InPreview.Tint;
        Request.Mailbox = &P->Previews;

        RenderScene* Scene = P->DefaultScene();
        ENQUEUE_RENDER_COMMAND(AddMeshPreview)([Scene, Request](RenderContext&)
        {
            Scene->AddMaterialPreview(Request);
        });
        return Request.Request;
    }

    bool Renderer::TakeMaterialPreview(const uint64_t InRequest, EngineCore::Image& OutImage, std::string& OutError)
    {
        return P->Previews.Take(InRequest, OutImage, OutError);
    }

    void Renderer::SetViewOverride(const RenderView* InView)
    {
        SetViewOverride(RENDER_DEFAULT_SCENE, InView);
    }

    void Renderer::SetViewOverride(const RenderSceneID InScene, const RenderView* InView)
    {
        RendererScene* Scene = P->Find(InScene);
        if(!Scene)
        {
            return;
        }
        Scene->bHasViewOverride = InView != nullptr;
        if(InView)
        {
            Scene->ViewOverride = *InView;
        }
    }
#endif // WITH_EDITOR

    namespace
    {
        /* A module name, by value for a command's capture. */
        struct ModuleName
        {
            char Text[64] = {};

            bool Set(const char* InName)
            {
                const size_t Length = InName ? std::strlen(InName) : 0;
                if(Length == 0 || Length >= sizeof(Text)) { return false; }
                std::memcpy(Text, InName, Length);
                return true;
            }
        };
    }

    void Renderer::MountShaderCache(const char* InModule)
    {
        ModuleName Name;
        if(!P->Thread.IsRunning() || !Name.Set(InModule))
        {
            return;
        }
        ENQUEUE_RENDER_COMMAND(MountShaderCache)([Name](RenderContext& Ctx)
        {
            Ctx.MountShaderCache(Name.Text);
        });
    }

    void Renderer::UnmountShaderCache(const char* InModule)
    {
        ModuleName Name;
        if(!P->Thread.IsRunning() || !Name.Set(InModule))
        {
            return;
        }
        ENQUEUE_RENDER_COMMAND(UnmountShaderCache)([Name](RenderContext& Ctx)
        {
            Ctx.UnmountShaderCache(Name.Text);
        });
    }

    void Renderer::EndFrame(const FrameInfo& InFrame)
    {
        for(RendererScene* Scene : P->Scenes)
        {
            FrameInfo Frame = InFrame;
            if(Scene->ID != RENDER_DEFAULT_SCENE)
            {
                /* An extra scene with no viewport is a hidden window: skip it,
                 * extraction and all. Its overlay lines are dropped. */
                Frame.ViewportPixels = Scene->ViewportPixels;
                if(Frame.ViewportPixels.IsEmpty())
                {
#if WITH_EDITOR
                    Scene->PendingDebugLines.clear();
#endif
                    continue;
                }
            }

            /* The editor's overlay lines and camera override. */
            const RenderView* ViewOverride = nullptr;
#if WITH_EDITOR
            Scene->SubmitDebugLines();
            ViewOverride = Scene->bHasViewOverride ? &Scene->ViewOverride : nullptr;
#endif

            if(Scene->Bridge && Scene->Bridge->IsBound())
            {
                Scene->Bridge->ExtractAndRender(Frame, ViewOverride);
            }
            else if(Scene->Scene)
            {
                /* No World bound yet -- still render (a clear, nothing to draw)
                 * so a host's stats and pacing stay meaningful, matching
                 * Renderer.h's own comment. */
                RenderScene* RS = Scene->Scene;
                RenderView View = ViewOverride
                                ? *ViewOverride
                                : RenderView::MakeOrthographic(Frame.ViewportPixels, EngineCore::Vector3(0, 1, -1), -1.0f, 1000.0f);
                View.ViewportPixels = Frame.ViewportPixels;
                ENQUEUE_RENDER_COMMAND(SetViewAndRenderEmpty)([RS, View](RenderContext& Ctx)
                {
                    RS->SetView(View);
                    RS->Render(Ctx);
                });
            }
        }

        P->Thread.EndFrame(InFrame.FrameNumber);
    }

    void Renderer::PresentToWindow(const EngineCore::WindowHandle InWindow)
    {
        PresentToWindow(RENDER_DEFAULT_SCENE, InWindow);
    }

    void Renderer::PresentToWindow(const RenderSceneID InScene, const EngineCore::WindowHandle InWindow)
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
         * The render thread must be running: the driver, its swapchains and
         * the target being presented all belong to it, and after Stop() the
         * driver has already been destroyed and released.
         *
         * The scene's own final target, not GDI's last-bound one: with
         * several scenes, the last bound is whichever rendered last.
         */
        if(!P->Thread.IsRunning())
        {
            return;
        }
        if(const RendererScene* Scene = P->Find(InScene))
        {
            Scene->Scene->PresentToWindow(InWindow);
        }
    }

    void Renderer::ReleaseWindow(const EngineCore::WindowHandle InWindow)
    {
        if(P->Thread.IsRunning() && GDynamicGDI)
        {
            GDynamicGDI->ReleaseWindow(InWindow);
        }
    }

    void Renderer::SetClearColor(const EngineCore::Color InColor)
    {
        SetClearColor(RENDER_DEFAULT_SCENE, InColor);
    }

    void Renderer::SetClearColor(const RenderSceneID InScene, const EngineCore::Color InColor)
    {
        const RendererScene* Found = P->Find(InScene);
        if(!Found || !Found->Scene)
        {
            return;
        }
        RenderScene* Scene = Found->Scene;
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
