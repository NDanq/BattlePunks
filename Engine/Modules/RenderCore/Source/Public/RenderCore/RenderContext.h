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
         * DrawQuad through a material graph's pixel shader instead of Basic's:
         * material "Glow" is the cooked Materials/Glow.fsh (under Shaders/Cooked/
         * <Platform>/, as GDI names its own) with the parameter defaults in
         * Materials/Glow.material, both read through the file provider on first
         * use and kept. The shader's one uniform, MaterialParams, is those
         * defaults in columns 0-2 and InTint (0-1 RGBA) in column 3 -- see
         * NodeGraph's MaterialSchema.cpp for the layout and why it is one mat4.
         *
         * A material that cannot be loaded (no .material, or no cooked shader
         * for this backend) is warned about once and drawn as DrawQuad would.
         */
        void DrawQuadWithMaterial(const RenderTransform& InTransform, float InWidth, float InHeight,
                                  EngineCore::Color InTint, const char* InMaterial);

        /*
         * Draws the .nmmesh at InMeshPath (runtime-relative, MeshAsset.h) at
         * InTransform, its own size times the transform's scale. One draw per
         * section, each with:
         *   - InMaterial, when given and it loads (as DrawQuadWithMaterial);
         *   - else the section's own material, if one by that name exists;
         *   - else InTint, lit by one fixed light (Mesh.fsh).
         * The GPU mesh is built on first use and rebuilt when MeshAssetCache
         * has a different one (MeshAssetCache::Invalidate, after a reimport).
         *
         * False, drawing nothing, if the mesh cannot be loaded or this backend
         * has no indexed meshes -- the caller draws something else.
         */
        bool DrawMesh(const RenderTransform& InTransform, const char* InMeshPath, EngineCore::Color InTint,
                      const char* InMaterial);

#if WITH_EDITOR
        /*
         * EDITOR ONLY -- the draw behind Renderer::DrawDebugLine.
         *
         * A world-space segment as a flat ribbon facing the camera,
         * InThicknessPixels wide on screen at any distance, extended by half
         * that at each end so segments meeting at a corner close it. Drawn
         * with the same unit quad and shaders as DrawQuad: the ribbon's
         * placement is folded into the view matrix for this one draw, and the
         * model transform is identity -- so no rotation is ever built, and
         * any orientation works. Uses the view from the last SetView().
         *
         * Nothing is drawn for a segment seen end-on (it would be a point) or
         * shorter than a millionth of a unit. Counts one draw call.
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

        /* Forgets material InName (or every material, for nullptr) so its
         * next draw loads it afresh -- Renderer::ReloadMaterial's command.
         * The old shader is kept, not released, until ReleaseGPUResources:
         * a backend may cache pipelines by the shader module's handle, and a
         * released handle can come back as the new shader's. */
        void ReloadMaterial(const char* InName);

        /* Releases every GPU object this context created itself -- the
         * material shaders and the preview target. RenderThread's exit calls
         * it before the driver's Destroy(), while the device they belong to
         * still exists. */
        void ReleaseGPUResources();

#if WITH_EDITOR
        /*
         * EDITOR ONLY -- RenderScene::Render's, for a material preview: material
         * InMaterial on a quad filling an InSize x InSize target of this
         * context's own, InTint as its Instance Tint, read back into OutImage
         * (RGBA8, top row first). Between GDIBeginRendering and the scene's
         * own target bind; leaves the view as it found it. False, with
         * OutError, when the material cannot be drawn or the backend reads
         * nothing back (Null).
         */
        bool RenderMaterialPreview(const char* InMaterial, int InSize, EngineCore::Color InTint,
                                   EngineCore::Image& OutImage, std::string& OutError);
#endif

        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERCONTEXT_H
