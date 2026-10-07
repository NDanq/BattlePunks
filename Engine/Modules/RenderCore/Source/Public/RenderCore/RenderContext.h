#ifndef RENDERCORE_RENDERCONTEXT_H
#define RENDERCORE_RENDERCONTEXT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/ParticleTypes.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderPipeline.h"
#include "Public/RenderCore/RenderTargetAsset.h"
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

class GDIReadback;           // Public/GDI/GDIRenderTarget.h
class GDIOcclusionQueries;   // Public/GDI/GDIQuery.h

namespace RenderCore
{
    struct SceneTargets;             // Private/RenderCore/SceneTargets.h
    struct IBLResources;             // likewise: a scene's probe atlas and cube strip
    struct StagedDraw;               // Private/RenderCore/DrawPolicy.h
    struct MaterialPreviewRequest;   // RenderScene.h
    struct RenderMaterialParameter;  // Components/RenderComponent.h
    struct UISceneState;             // Private/RenderCore/UIRenderer.h: a scene's UI draw list
    struct ParticleEmitterGpu;       // Private/RenderCore/ParticleSimulation.h: an emitter's GPU state

    /* An entity's material parameter overrides for one draw call: each entry
     * applies to the material in its Slot. Must stay valid until the scene
     * has drawn -- a proxy's own storage, as RenderComponentProxy passes. */
    struct MaterialParameterOverrides
    {
        const RenderMaterialParameter* Items = nullptr;
        uint32_t                       Count = 0;

        /* The entity's baked lighting (RenderLighting.h); null: none. Kept,
         * like Items, by pointer: it must outlive the frame's draws. */
        const RenderBakedDraw*         Baked = nullptr;

        /* A skinned mesh's bones (ANIMATION.md): SkinBones AnimMatrix rows --
         * 12 floats each, the bone's pose times the mesh's inverse bind, in
         * model space. Null: its bind pose. Kept by pointer, as Items. */
        const float*                   SkinPalette = nullptr;
        uint32_t                       SkinBones   = 0;

        /* Its palette the frame before, as SkinPalette, for velocity; null:
         * SkinPalette (it did not move). */
        const float*                   PrevSkinPalette = nullptr;
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

        /* The render thread's clock, as shaders see it (Scene_Time): seconds
         * since the context began, and since the frame before. */
        double GetTimeSeconds()  const;
        float  GetDeltaSeconds() const;

        /*
         * GPU PARTICLES (ParticleTypes.h; RENDERPIPELINE.md, "Particles"),
         * where the backend runs compute shaders -- Vulkan, and Null, which
         * runs nothing. An emitter is its particles on the GPU; a proxy makes
         * one, simulates it once a frame (RenderProxy::Simulate, before any
         * target is bound) and draws it from every view (Draw).
         */
        bool                SupportsParticles() const;
        /* For an emitter of InType (EParticleType): a ribbon or beam has a
         * second instance buffer. InName (its system and emitter) names its
         * buffers for capture tools. Null where unsupported. Freed by
         * ReleaseParticleEmitter. */
        ParticleEmitterGpu* CreateParticleEmitter(uint32_t InMaxParticles, uint8_t InType = PT_Sprite, const char* InName = nullptr);
        void                ReleaseParticleEmitter(ParticleEmitterGpu* InEmitter);
        /* Spawns, moves and sorts its particles for this frame and this view:
         * two dispatches, InDeltaSeconds on (below 0: the frame's own delta),
         * in a debug region named InLabel if set (a capture tool's name for it).
         * False if the shaders are missing (warned once). Outside a frame, as
         * from a render command, they run at once and are waited for. */
        bool                SimulateParticles(ParticleEmitterGpu& InEmitter, const ParticleEmitterSettings& InSettings,
                                              const EngineCore::Vector3& InEmitterPosition, float InDeltaSeconds = -1.0f,
                                              const char* InLabel = nullptr);
        /* Its particles as InMeshPath instanced, with InMaterial on every
         * section (null or "": DefaultLit), queued as one draw per section
         * sorted at InSortOrigin -- where the backend draws indirectly
         * (GDISupportsIndirectDraw), only as many as the simulation counted
         * alive -- a ribbon or beam as its strip's two
         * triangle meshes instead (PARTICLE_RIBBON_MESH_A/B). InLabel, if
         * set, is what its draws are called in a capture tool, and must
         * outlive the frame. Nothing,
         * returning false, before its first simulation or if the mesh or
         * material cannot draw. */
        bool                DrawParticles(ParticleEmitterGpu& InEmitter, const EngineCore::Vector3& InSortOrigin,
                                          const char* InMeshPath, const char* InMaterial,
                                          const char* InLabel = nullptr);
        /* Its particles as the GPU left them, one per slot (MaxParticles),
         * waiting for the GPU: for tests and tools. False where unsupported. */
        bool                ReadParticles(const ParticleEmitterGpu& InEmitter, std::vector<ParticleSample>& OutParticles);
        /* How many instances its draws draw: the live count the last
         * simulation wrote for them (sprites and meshes: the living
         * particles; ribbons and beams: the living segments). False where
         * unsupported. */
        bool                ReadParticleDrawCount(const ParticleEmitterGpu& InEmitter, uint32_t& OutCount);
        bool                ReadParticleInstances(const ParticleEmitterGpu& InEmitter, std::vector<ParticleInstanceSample>& OutInstances);

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

        /*
         * A shadow caster: InMeshPath's Opaque and Masked sections (by the
         * same slot rules as DrawMesh) into the shadow view RenderScene is
         * gathering casters for (RenderProxy::DrawShadow). Draws nothing,
         * returning false, outside that, or if the mesh cannot be loaded.
         */
        bool DrawMeshShadow(const RenderTransform& InTransform, const char* InMeshPath,
                            const char* const* InSlots, uint32_t InSlotCount,
                            const MaterialParameterOverrides& InOverrides = MaterialParameterOverrides());

        /*
         * An occluder (RENDERPIPELINE.md, "Occlusion culling"): InMeshPath's
         * Opaque sections, depth only, into the PrePass -- the depth the
         * frame's occlusion queries are tested against -- seen from the
         * view, with their materials' vertex shaders (World Position Offset).
         * Masked sections are left out: their holes would hide what is
         * behind them. Draws nothing, returning false, unless RenderScene is
         * gathering occluders (RenderProxy::DrawOccluder) or the mesh cannot
         * be loaded.
         */
        bool DrawMeshOccluder(const RenderTransform& InTransform, const char* InMeshPath,
                              const char* const* InSlots, uint32_t InSlotCount,
                              const MaterialParameterOverrides& InOverrides = MaterialParameterOverrides());

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
        /* Particle emitters simulated so far, one a SimulateParticles: what culling and pausing save. */
        uint64_t GetParticleSimulationCount() const;

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

        /* ERenderViewMode::Unlit for the stages that follow: base colour and
         * emissive, no light (DeferredLighting.fsh). RenderScene sets it for
         * its own view only, and clears it after. */
        void SetUnlit(bool bInUnlit);

        /* Every colour target a view can show in its place (ERenderViewMode::
         * RenderTarget): InTargets' stages, the shadow atlases, the image-based
         * lighting atlas and strip, and every render target asset captured,
         * by readable name. Render thread. */
        void ListViewableTargets(const SceneTargets& InTargets, std::vector<std::string>& OutNames) const;

        /* The one ListViewableTargets names InName, stretched over
         * InTargets.Final, which stays bound for the present; nothing if it
         * names none, or Final itself. */
        void ShowTarget(const SceneTargets& InTargets, const std::string& InName);

        /* RenderScene::Render's: queue the proxies' draws in their stages'
         * policies, then run the stages into InTargets (RENDERPIPELINE.md),
         * clearing to InClearColor. Leaves the final colour target bound --
         * what the present shows. */
        void BeginSceneDraws();

        /* VELOCITY (RENDERPIPELINE.md, "Velocity"): the world motion of what
         * queues draws next -- its transform the frame before times the
         * inverse of its transform now. Each draw queued until the next call
         * takes it, its model the frame before that times its model now.
         * Null: nothing moved. RenderScene sets it per proxy; a static draw
         * list's replays never move. */
        void SetDrawMotion(const EngineCore::Matrix4x4* InMotion);

        /*
         * OCCLUSION CULLING (RenderScene; RENDERPIPELINE.md, "Occlusion
         * culling"), for the scene being queued -- BeginSceneDraws forgets it,
         * so a capture or preview has none. With InQueries set:
         *
         *   AddOcclusionQuery   a world box to test against the occluders'
         *                       depth in the PrePass; its query's index, or -1
         *                       when the set is full (then: not tested, drawn)
         *   SetDrawOcclusion    the draws queued next are skipped by the GPU
         *                       if query InQuery finds nothing (-1: drawn);
         *                       shadow casters never are
         *   SetGatheringOccluders  while on, DrawMeshOccluder queues: what
         *                       RenderScene calls each occluder's DrawOccluder in
         *   CanPredicateOcclusion  whether the GPU skips SetDrawOcclusion's
         *                       draws, or (this backend) draws them all
         *
         * RenderSceneStages then draws the occluders, runs the queries and
         * resolves them before the Base Pass, and forgets all of it.
         */
        void    SetOcclusionQueries(GDIOcclusionQueries* InQueries);
        int32_t AddOcclusionQuery(const GameCore::AABB& InBox);
        void    SetDrawOcclusion(int32_t InQuery);
        void    SetGatheringOccluders(bool bInGathering);
        bool    CanPredicateOcclusion() const;

        /* STATIC DRAW LISTS (RenderScene): between BeginCapture and
         * EndCapture, whatever a proxy's Draw or DrawShadow queues lands in
         * *InOut instead of this frame's stages. SubmitCaptured queues a kept
         * list for this frame -- its depths recomputed for this view, and a
         * shadow list (InShadowView >= 0) aimed at that shadow view. */
        void BeginCapture(std::vector<StagedDraw>* InOut);
        void EndCapture();
        void SubmitCaptured(const std::vector<StagedDraw>& InDraws, int32_t InShadowView = -1);

        /*
         * SCENE CAPTURE (RenderScene::Render; RENDERPIPELINE.md). For a probe
         * capturing into the render target at InPath (RenderTargetAsset.h):
         * BeginTargetCapture readies its texture at InResolution (0: the
         * target's own size) and returns the stage targets each face renders
         * through, with the colour to clear them to -- null, warned once, if
         * the path names no target or one of the other kind. After each
         * face's RenderSceneStages, FinishTargetFace copies it into the
         * target (a cube's face InFace into its place in the strip).
         * ReadRenderTarget reads back what a material would sample now (the
         * editor's bake).
         */
        SceneTargets* BeginTargetCapture(const char* InPath, uint32_t InResolution, ERenderTargetKind InKind, EngineCore::Color& OutClear);

        /*
         * IMAGE-BASED LIGHTING (RenderScene::Render; RENDERPIPELINE.md). Only
         * with an HDR scene colour (CanImageBasedLight); the Lighting stage's
         * hemisphere ambient otherwise.
         *
         *   EnsureIBLResources   the atlas, and the strip at InFace a side
         *   RenderSkyToStrip     the current lighting's sky (its HDRI, else its colours)
         *   CopyCaptureToStrip   a cube face InStages rendered (its linear scene colour)
         *   PrefilterIBLBlock    the strip into atlas block InBlock (0-3 probes, 4 the sky)
         *   SetImageBasedLighting  what lights the next RenderSceneStages: InResources'
         *                        atlas (null: none), InCount probes as Scene_Probes' vec4s
         *                        (3 each), and whether the sky block is valid
         *   SetSkyBackground     draw the sky (HDRI or colours) behind every view
         *                        while set: a reflection probe's captures
         */
        bool CanImageBasedLight() const;
        bool EnsureIBLResources(IBLResources& InOut, uint32_t InFace);
        void ReleaseIBLResources(IBLResources& InOut);
        bool RenderSkyToStrip(IBLResources& InResources);
        bool CopyCaptureToStrip(SceneTargets& InStages, IBLResources& InResources, uint32_t InFace);
        bool PrefilterIBLBlock(IBLResources& InResources, uint32_t InBlock);
        void SetImageBasedLighting(const IBLResources* InResources, const float (*InProbes)[4], uint32_t InCount, bool bInSky);
        void SetSkyBackground(bool bInForce);
        void          FinishTargetFace(const char* InPath, uint32_t InFace);
        bool          ReadRenderTarget(const char* InPath, EngineCore::Image& OutPixels);

        /* Moves when anything a kept draw points at may have gone: a material
         * reloaded or retired, a mesh rebuilt or reimported. */
        uint64_t GetContentGeneration() const;

        /* RenderScene::Render's, after BeginSceneDraws: the frame's shadow
         * views from the view and lighting (none if [Shadows] is off or no
         * light casts). For each, RenderScene sets it as the caster view and
         * calls DrawShadow on every proxy within its bounds; -1 ends that.
         *
         * InCasters: the visible proxies asking for an inset shadow, most
         * deserving first; those that get a view are its only caster
         * (GetShadowViewInset: their index, else -1), and are left out of the
         * sun's views (IsSunShadowView), which their own replaces.
         * InReceivers: the box round everything the view draws, if known --
         * where an inset's shadow can land, so how much screen it covers. */
        uint32_t              PrepareShadows(const InsetShadowCaster* InCasters = nullptr, uint32_t InCasterCount = 0,
                                             const GameCore::AABB* InReceivers = nullptr);
        const GameCore::AABB& GetShadowViewBounds(uint32_t InView) const;
        int32_t               GetShadowViewInset(uint32_t InView) const;
        bool                  IsSunShadowView(uint32_t InView) const;
        void                  SetShadowCasterView(int32_t InView);
        /* InClearBottom: the background is a gradient, InClearColor at the
         * top to it at the bottom (ClearGradient.fsh), under everything.
         * bInTimed: write "stat frame"'s GPU timestamps around each stage --
         * a scene's own frame only, once per GDI block (BeginTimedBlock). */
        void RenderSceneStages(SceneTargets& InTargets, EngineCore::Color InClearColor, bool bInTimed = false,
                               const EngineCore::Color* InClearBottom = nullptr);

        /* RenderScene::Render's, right after GDIBeginRendering: hands back the
         * previous GDI block's stage times (ms per ERenderGpuTimer, -1 where
         * not timed) and the scene that wrote them -- null if none did -- and
         * notes InOwner as this block's, if bInTime and the backend can time. */
        const void* BeginTimedBlock(const void* InOwner, bool bInTime, float (&OutGpuMs)[RGT_Count]);

        /* InTimer's start (or end) timestamp, outside RenderSceneStages (the
         * scene captures' span); nothing unless this block is timed. */
        void WriteGpuTimestamp(ERenderGpuTimer InTimer, bool bInEnd);

        /* Both DrawMesh overloads: bInEvery puts InSlots[0] in every slot. */
        bool DrawMeshSections(const RenderTransform& InTransform, const char* InMeshPath, EngineCore::Color InTint,
                              const char* const* InSlots, uint32_t InSlotCount, bool bInEvery,
                              const MaterialParameterOverrides& InOverrides);

        /* Queues a full-screen pass of a PostProcess material at InStage of
         * the post process stack (EPostProcessStage), after those already
         * queued there (outside a scene: draws it into the bound target, the
         * caller having bound the scene so far to material texture 0). False
         * (warned) if it is not a usable PostProcess material. */
        bool SubmitPostProcess(const char* InMaterial, uint8_t InStage = PPS_AfterEffects);

        /* RenderScene::Render's, before RenderSceneStages: the stack's settings
         * (its materials are submitted separately) and last frame's
         * Projection * View, for motion blur -- null on a first frame. The
         * next RenderSceneStages uses and forgets them. */
        void SetPostProcess(const PostProcessSettings& InSettings, const EngineCore::Matrix4x4* InPreviousViewProjection);

        /* RenderScene::Render's text overlay ("stat unit"): InText, lines split
         * by '\n', in a panel at the top right of the bound InWidth x
         * InHeight target, blended over it (StatsOverlay.fsh). */
        void DrawStatUnit(uint32_t InWidth, uint32_t InHeight, const std::string& InText);

        /* RenderScene::Render's UI (Renderer::SubmitUIDrawList, RENDERPIPELINE.md
         * "UI"): InState's list over the bound InWidth x InHeight target, blended,
         * one draw per command, each clipped by its scissor (UI.vsh / UI.fsh). */
        void DrawUI(UISceneState& InState, uint32_t InWidth, uint32_t InHeight);

        /* Renderer::SetUIImage's command: InImage as UI texture InKey ("ui:..."),
         * replacing any; null releases it. */
        void SetUIImage(const char* InKey, const EngineCore::Image* InImage);
        bool CanTimeGpu() const;

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
         * and lighting. The answer goes to InRequest's mailbox: at once if it
         * cannot draw, else when the readback is in -- a frame or so later,
         * not waited for (DeliverMaterialPreviews).
         */
        void RenderMaterialPreview(const MaterialPreviewRequest& InRequest);

        /* The previews whose readbacks are in, to their mailboxes: each
         * frame, before its own previews. */
        void DeliverMaterialPreviews();

    private:
        bool DrawMaterialPreview(const MaterialPreviewRequest& InRequest, std::unique_ptr<GDIReadback>& OutReadback,
                                 std::string& OutError);

    public:
#endif

        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERCONTEXT_H
