#ifndef RENDERCORE_RENDERCONTEXT_H
#define RENDERCORE_RENDERCONTEXT_H

#include <cstdint>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"

/*
 * What a render command and a RenderProxy draw with. RENDER THREAD ONLY --
 * every method here assumes it, and GetRenderThreadContext() (RenderCommand.h)
 * is the only way to reach one.
 *
 * NO GDI TYPE APPEARS HERE, deliberately: RenderCoreInternal.h is the only
 * header in this module allowed to include one, and it lives under Private/
 * (see its own comment). A proxy or a command written against this class
 * never needs to know GDI exists, which is what makes RenderCore.dll the
 * ONLY image in the process that links it -- see RenderCoreAPI.h.
 */
namespace EngineCore { class Image; }

namespace RenderCore
{
    class RENDERCORE_API RenderContext
    {
    public:
        RenderContext();
        ~RenderContext();

        RenderContext(const RenderContext&)            = delete;
        RenderContext& operator=(const RenderContext&) = delete;

        ERenderBackend    GetBackend()     const;
        uint64_t          GetFrameNumber() const;
        const RenderView& GetView()        const;

        /*
         * Draws a quad centred on InTransform's position (scaled by
         * InWidth/InHeight, rotated and offset the same way
         * TransformComponent::GetLocalMatrix composes -- see
         * RenderTypes.h's RenderTransform::ToMatrix), tinted InTint, through
         * the Basic vertex/pixel shaders every backend already registers in
         * InitializeGDI (GDI.cpp). One 1x1 quad mesh serves every size and
         * every proxy -- scaled through GDITransform::Scale rather than
         * GDICreateQuadMesh(w, h) allocating a mesh per size.
         *
         * Counts one draw call, read back through
         * RenderThread::GetStats()'s RenderStats::DrawCalls.
         */
        void DrawQuad(const RenderTransform& InTransform, float InWidth, float InHeight, EngineCore::Color InTint);

        /*
         * DrawQuad with a material graph's pixel shader: "Glow" is the cooked
         * Materials/Glow.fsh plus defaults from Glow.material, loaded on first
         * use. MaterialParams holds the defaults in columns 0-2 and InTint in
         * column 3 (layout: NodeGraph's MaterialSchema.cpp).
         *
         * An unloadable material is warned once and drawn as DrawQuad.
         */
        void DrawQuadWithMaterial(const RenderTransform& InTransform, float InWidth, float InHeight,
                                  EngineCore::Color InTint, const char* InMaterial);

        /*
         * Draws the .nmmesh at InMeshPath, one draw per section, with the
         * first that loads of: InMaterial, the section's material, or InTint
         * lit by a fixed light (Mesh.fsh). The GPU mesh is rebuilt when
         * MeshAssetCache's copy changes.
         *
         * False, drawing nothing, if the mesh cannot be loaded or the backend
         * lacks indexed meshes.
         */
        bool DrawMesh(const RenderTransform& InTransform, const char* InMeshPath, EngineCore::Color InTint,
                      const char* InMaterial);

#if WITH_EDITOR
        /*
         * EDITOR ONLY -- Renderer::DrawDebugLine's draw: a camera-facing
         * ribbon InThicknessPixels wide at any distance, extended half that at
         * each end so corners close. Uses DrawQuad's quad with the placement
         * folded into the view matrix. Skips degenerate or end-on segments.
         */
        void DrawLine(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd,
                      EngineCore::Color InColor, float InThicknessPixels);
#endif // WITH_EDITOR

        /* Render-thread only, matching GDI's own GDIBeginDrawEvent/EndDrawEvent
         * (Public/GDI/GDI.h) -- a scoped pair for a profiler's capture, cheap
         * to call even with no profiler attached. */
        void BeginDrawEvent(const char* InName);
        void EndDrawEvent();

        /* Lifetime total of DrawQuad calls -- what RenderThread::GetStats()
         * reads into RenderStats::DrawCalls. Not reset per frame: a snapshot
         * consumer (a per-second log line) differences two reads itself,
         * the same convention RenderStats's own comment states. */
        uint64_t GetDrawCallCount() const;

    private:
        friend class RenderThread;
        friend class RenderScene;
        friend class Renderer;

        /* RenderThread::Start()/EndFrame() and RenderScene::SetView() write
         * these directly -- all three live in this DLL and all run on the
         * render thread, so there is no cross-image or cross-thread hazard
         * in bypassing an accessor here. */
        void SetBackend(ERenderBackend InBackend);
        void SetFrameNumber(uint64_t InFrameNumber);
        void SetView(const RenderView& InView);

        /* Renderer::ReloadMaterial's command (nullptr: all). Old shaders are
         * kept until ReleaseGPUResources: pipelines may be cached by shader
         * handle, and a freed handle could be reused. */
        void ReloadMaterial(const char* InName);

        /* Material shaders and the preview target; before the driver's Destroy(). */
        void ReleaseGPUResources();

#if WITH_EDITOR
        /*
         * EDITOR ONLY -- draws InMaterial on an InSize x InSize quad into a
         * private target and reads it back (RGBA8, top row first). Called by
         * RenderScene::Render before the scene's target bind; restores the
         * view. False, with OutError, if it cannot draw or read back.
         */
        bool RenderMaterialPreview(const char* InMaterial, int InSize, EngineCore::Color InTint,
                                   EngineCore::Image& OutImage, std::string& OutError);
#endif

        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERCONTEXT_H
