#ifndef RENDERCORE_RENDERCONTEXT_H
#define RENDERCORE_RENDERCONTEXT_H

#include <cstdint>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderPipeline.h"
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
    struct SceneTargets;             // Private/RenderCore/SceneTargets.h
    struct MaterialPreviewRequest;   // RenderScene.h
    struct RenderMaterialParameter;  // Components/RenderComponent.h

    /* An entity's material parameter overrides for one draw call: each entry
     * applies to the material in its Slot. Must stay valid until the scene
     * has drawn -- a proxy's own storage, as RenderComponentProxy passes. */
    struct MaterialParameterOverrides
    {
        const RenderMaterialParameter* Items = nullptr;
        uint32_t                       Count = 0;
    };

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
         * DrawQuad with a material: "Glow" is Materials/Glow.material and its
         * cooked permutations (MATERIALS.md), loaded on first use. The
         * material block carries the description's defaults and InTint
         * (Instance Tint). Drawn in the stage its Domain and Shading name
         * (RENDERPIPELINE.md): queued inside a scene, at once outside one.
         *
         * An unloadable material, or a PostProcess one, is warned once and
         * drawn as DrawQuad.
         */
        void DrawQuadWithMaterial(const RenderTransform& InTransform, float InWidth, float InHeight,
                                  EngineCore::Color InTint, const char* InMaterial,
                                  const MaterialParameterOverrides& InOverrides = MaterialParameterOverrides());

        /*
         * Draws the .nmmesh at InMeshPath, one draw per section, each in its
         * material's stage. Section i draws with the first that loads of:
         * InSlots[i] (when i < InSlotCount and it is not empty), the section's
         * own material, the engine's DefaultLit (InTint, lit by the scene), or
         * InTint lit by a fixed light (Mesh.fsh). The GPU mesh is rebuilt when
         * MeshAssetCache's copy changes.
         *
         * False, drawing nothing, if the mesh cannot be loaded or the backend
         * lacks indexed meshes.
         */
        bool DrawMesh(const RenderTransform& InTransform, const char* InMeshPath, EngineCore::Color InTint,
                      const char* const* InSlots, uint32_t InSlotCount,
                      const MaterialParameterOverrides& InOverrides = MaterialParameterOverrides());

        /* The same with InMaterial in every section's slot ("" or null: none). */
        bool DrawMesh(const RenderTransform& InTransform, const char* InMeshPath, EngineCore::Color InTint,
                      const char* InMaterial);

        /* How far InMaterial's World Position Offset reaches (its Max Offset),
         * for a proxy to pad its culling bounds; 0 without one, or if it
         * cannot load. Loads it if need be -- warning as a draw of it would,
         * unless bInQuietIfMissing (a mesh section's own material). */
        float GetMaterialMaxOffset(const char* InMaterial, bool bInQuietIfMissing = false);

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

        /* How many draws InStage queued in the last scene this context drew
         * (a viewport frame or a preview): the routing, as tests see it. The
         * Lighting stage counts its one pass when it ran. */
        uint32_t GetStageDrawCount(ERenderStage InStage) const;

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

        /* The scene block's light and ambient (RenderScene, once a frame). */
        void SetLighting(const RenderLighting& InLighting);

        /* RenderScene::Render's: queue the proxies' draws in their stages'
         * policies, then run the stages into InTargets (RENDERPIPELINE.md),
         * clearing to InClearColor. Leaves the final colour target bound --
         * what the present shows. */
        void BeginSceneDraws();
        void RenderSceneStages(SceneTargets& InTargets, EngineCore::Color InClearColor);

        /* Both DrawMesh overloads: bInEvery puts InSlots[0] in every slot. */
        bool DrawMeshSections(const RenderTransform& InTransform, const char* InMeshPath, EngineCore::Color InTint,
                              const char* const* InSlots, uint32_t InSlotCount, bool bInEvery,
                              const MaterialParameterOverrides& InOverrides);

        /* Queues a full-screen pass of a PostProcess material, after those
         * already queued (outside a scene: draws it into the bound target,
         * the caller having bound the scene so far to material texture 0).
         * False (warned) if it is not a usable PostProcess material. */
        bool SubmitPostProcess(const char* InMaterial);

        /* Renderer::MountShaderCache's command: reads
         * Shaders/Cache/<Module>.<Platform>.nmsc and mounts it as a file layer
         * (ShaderCache.h); every material reloads through it. False, logged,
         * if there is none or it does not fit this build. */
        bool MountShaderCache(const char* InModule);
        void UnmountShaderCache(const char* InModule);

        /* Renderer::ReloadMaterial's command (nullptr: all). Old shaders are
         * kept until ReleaseGPUResources: pipelines may be cached by shader
         * handle, and a freed handle could be reused. */
        void ReloadMaterial(const char* InName);

        /* Material shaders and the preview targets; before the driver's Destroy(). */
        void ReleaseGPUResources();

#if WITH_EDITOR
        /*
         * EDITOR ONLY -- draws InRequest through the same stages as the
         * viewport, into private targets, and reads it back (RGBA8, top row
         * first): its material on a quad filling the image, or its mesh,
         * framed by an orbiting perspective camera and lit by a fixed studio
         * light, with its material (or its slots) on its sections. A
         * PostProcess material always runs full screen. Called by
         * RenderScene::Render before the scene's own draws; restores the view
         * and lighting. False, with OutError, if it cannot draw or read back.
         */
        bool RenderMaterialPreview(const MaterialPreviewRequest& InRequest, EngineCore::Image& OutImage, std::string& OutError);
#endif

        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERCONTEXT_H
