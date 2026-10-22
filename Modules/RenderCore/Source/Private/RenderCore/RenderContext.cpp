#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/ParallelRecording.h"
#include "Public/Core/CoreJob.h"

#include <algorithm>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CorePack.h"
#include "Public/GDI/Shaders/Classes/TestbedShaders.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderPipeline.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/ShaderCache.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Public/RenderCore/TextureCompression.h"
#include "Private/RenderCore/ParticleSimulation.h"
#include "Public/RenderCore/RenderTargetAsset.h"
#include "Private/RenderCore/CustomRenderPassInternal.h"
#include "Private/RenderCore/DrawPolicy.h"
#include "Private/RenderCore/MaterialPreview.h"
#include "Private/RenderCore/OverlayFont.h"
#include "Private/RenderCore/UIRenderer.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/SceneTargets.h"
#include "Private/RenderCore/ShadowSetup.h"
#include "Private/RenderCore/TextureStreaming.h"

/*
 * DrawQuad: a 1x1 quad mesh, Basic_VS/Basic_PS, and a GDIView whose camera
 * matrices are copied from the RenderView.
 *
 * MATERIALS (MATERIALS.md). A material is its .material description and the
 * cooked permutations of the engine's templates: its own pixel shader, and
 * either its own vertex shaders (World Position Offset) or the shared
 * MaterialDefault ones. Each draw fills the material block (binding 2) from
 * the description's defaults plus the tint, and the scene block (binding 3)
 * once a frame, through GDISetUniformBlock.
 *
 * STAGES (RENDERPIPELINE.md). Inside a scene (BeginSceneDraws to
 * RenderSceneStages) each draw is queued in the draw policy of the stage its
 * material names -- Base Pass (the G-buffer), Unlit, Translucent Lit,
 * Translucent Unlit, PostProcess -- and the stages then run in order, with
 * the Lighting stage resolving the G-buffer between the Base Pass and Unlit.
 * Outside a scene (a test) draws are issued at once.
 */

/* Each draw in a debug region named for its mesh and material (RenderContext::Issue),
 * for capture tools; off saves building the names. Only where the backend records
 * regions (GDIHasDrawEvents: Vulkan with VK_EXT_debug_utils, as under RenderDoc). */
NIMBLE_CONFIG_BOOL(GRenderDrawLabels, "Rendering", "DrawLabels", true, nullptr);
/* Skin's screen-space subsurface scattering (SHADING_MODELS.md, PPSubsurface.fsh): on, and the
 * diffusion profile's width in world units (1.2: skin, in centimetres). Off, Skin lights as
 * Subsurface does: its diffuse in place, with its transmission. */
NIMBLE_CONFIG_BOOL(GRenderSubsurfaceScattering, "Rendering", "SubsurfaceScattering", true, nullptr);
NIMBLE_CONFIG_FLOAT(GRenderSubsurfaceWidth, "Rendering", "SubsurfaceWidth", 1.2f, nullptr);

namespace RenderCore
{
    /* CreateInstanceBuffer's: the GDI buffer behind the public handle, and how many it holds. */
    struct RenderInstanceBuffer
    {
        GDIBuffer* Buffer = nullptr;
        uint32_t   Count  = 0;
    };

    /* A loaded material: DrawCommand points at one (DrawPolicy.h). */
    struct RenderMaterial
    {
        std::string          Name;                 // its graph's path, as drawn: a draw's label (Issue)
        MaterialDescription  Description;
        GDIShader*           Pixel = nullptr;      // null: failed to load, drawn as the fallback
        GDIShader*           Vertex[5] = {};       // its own Quad, Mesh, InstancedMesh and SkinnedMesh shaders (by EMaterialVertexInput), with an offset only
        std::vector<uint8_t> Block;                // the defaults; the tint is written per draw
        /* Its textures as found before the parallel pass under way, by
         * Description.Textures' index (ResolveForParallel): a recording
         * thread binds these and never looks one up. ResolvedPass: which pass. */
        std::vector<GDITextureResource*> Resolved;
        uint64_t             ResolvedPass = 0;
        /* TEXTURE STREAMING: its textures' streaming entries, by
         * Description.Textures' index (null: not streamed), found in
         * StreamEpoch; a draw notes its demand on them (NoteStreamingDemand). */
        std::vector<TextureStreamer::Entry*> StreamEntries;
        uint64_t             StreamEpoch = 0;
    };

    /*
     * One recording thread's issue state (MULTITHREADED_RENDERING.md, phase 3):
     * what RenderContext::Impl's issue functions otherwise keep on the Impl --
     * the GDI view and mesh objects Place() writes, the bound-shader memo,
     * the shadow view's matrices, the scratch blocks -- so chunks of one stage
     * issue at once. The render thread keeps using the Impl's own; a thread
     * recording a chunk has GIssueScratch set to its own.
     */
    struct RenderIssueScratch
    {
        GDIShader*                   BoundVertex     = nullptr;
        GDIShader*                   BoundPixel      = nullptr;
        GDIView*                     View3D          = nullptr;
        GDIViewObject*               MeshObject      = nullptr;
        const EngineCore::Matrix4x4* PlaceProjection = nullptr;
        const EngineCore::Matrix4x4* PlaceView       = nullptr;
        int32_t                      ShadowView      = -1;      // the view its viewport and matrices are set for
        std::vector<uint8_t>         OverrideBlock;
        std::vector<uint8_t>         MaterialBlock;

        ~RenderIssueScratch()
        {
            if(MeshObject) { delete MeshObject->Transform; delete MeshObject; }
            delete View3D;
        }
    };
    namespace
    {
        thread_local RenderIssueScratch* GIssueScratch = nullptr;

        /* A chunk: at least this many draws, or splitting costs about what it saves. */
        constexpr uint32_t PARALLEL_MIN_DRAWS_PER_CHUNK = 32;
    }

    namespace
    {
        const char* const MATERIAL_FOLDER      = "Materials/";            // the shared MaterialDefault vertex shaders
        const char* const DEFAULT_LIT_MATERIAL = "Engine/Content/Materials/DefaultLit.material.nmgraph";   // a mesh with none
        const char* const LIGHTING_SHADER      = "DeferredLighting.fsh";  // Engine/Content/Shaders: the Lighting stage
        const char* const SHADOW_DEPTH_SHADER  = "ShadowDepth.fsh";       // the Shadows stage's casters
        const char* const UI_VERTEX_SHADER     = "UI.vsh";                // DrawUI's, with UI.fsh (PP_UI)

        /* The post process stack's passes (Engine/Content/Shaders/PP*.fsh). */
        enum EPostPass : uint32_t
        {
            PP_DofPrepare, PP_DofBlur, PP_DofComposite, PP_MotionBlur,
            PP_BloomDownsample, PP_BloomUpsample, PP_BloomComposite, PP_Fxaa,
            PP_AmbientOcclusion, PP_AmbientBlur, PP_StatsOverlay, PP_ClearGradient, PP_CaptureCopy, PP_Tonemap,
            PP_IBLSky, PP_IBLPrefilter, PP_IBLLighting, PP_Sky, PP_ViewTarget, PP_UI,
            PP_Luminance, PP_LuminanceReduce, PP_AdaptExposure, PP_TemporalAA, PP_ScreenReflections, PP_Copy,
            PP_VolumetricFog, PP_FogComposite, PP_ViewSites, PP_AmbientApply, PP_Subsurface,
            PP_PassCount
        };
        const char* const POST_PASS_SHADERS[PP_PassCount] = {
            "PPDofPrepare.fsh", "PPDofBlur.fsh", "PPDofComposite.fsh", "PPMotionBlur.fsh",
            "PPBloomDownsample.fsh", "PPBloomUpsample.fsh", "PPBloomComposite.fsh", "PPFxaa.fsh",
            "PPAmbientOcclusion.fsh", "PPAmbientBlur.fsh", "StatsOverlay.fsh", "ClearGradient.fsh", "CaptureCopy.fsh", "Tonemap.fsh",
            "IBLSky.fsh", "IBLPrefilter.fsh", "IBLLighting.fsh", "Sky.fsh", "ViewTarget.fsh", "UI.fsh",
            "PPLuminance.fsh", "PPLuminanceReduce.fsh", "PPAdaptExposure.fsh", "PPTemporalAA.fsh", "PPScreenReflections.fsh",
            "PPCopy.fsh", "PPVolumetricFog.fsh", "PPFogComposite.fsh", "ViewSites.fsh", "PPAmbientApply.fsh", "PPSubsurface.fsh",
        };
        /* Each pass's draw event (DrawPost). */
        const char* const POST_PASS_NAMES[PP_PassCount] = {
            "DofPrepare", "DofBlur", "DofComposite", "MotionBlur",
            "BloomDownsample", "BloomUpsample", "BloomComposite", "Fxaa",
            "AmbientOcclusion", "AmbientBlur", "StatsOverlay", "ClearGradient", "CaptureCopy", "Tonemap",
            "IBLSky", "IBLPrefilter", "IBLLighting", "Sky", "ViewTarget", "UI",
            "Luminance", "LuminanceReduce", "AdaptExposure", "TemporalAA", "ScreenReflections", "Copy",
            "VolumetricFog", "FogComposite", "ViewSites", "AmbientApply", "Subsurface",
        };

        /* A pass's PP_Block: { vec4 settings; vec4 sizes; }, with a mat4
         * ahead of them for motion blur and ambient occlusion. */
        struct PostBlock
        {
            float Matrix[16] = {};
            float A[4] = {};
            float Sizes[4] = {};
        };
        constexpr size_t  RENDER_PREVIEW_SLOTS = 8;                       // MaterialPreviewRequest::Slots

        /* A material's shader: no name-based uniforms, the material binding layout. */
        class MaterialShader final : public GDIShader
        {
        public:
            explicit MaterialShader(const std::string& InName) : GDIShader(InName) {}
            void InitUniformBindings() override {}
        };

        using Material = RenderMaterial;

        /* Scene_Locals' vec4s per light. */
        constexpr uint32_t SCENE_LOCAL_VEC4S = 6;

        /* DeferredLighting.fsh's Lighting_Block: Params, the inset light and
         * tint, then RENDER_INSET_SHADOW_MAX insets and as many screen rects. */
        constexpr uint32_t LIGHTING_BLOCK_FLOATS = (3 + 2 * RENDER_INSET_SHADOW_MAX) * 4;
        static_assert(LIGHTING_BLOCK_FLOATS * sizeof(float) <= GDI_MATERIAL_BLOCK_MAX_BYTES, "Lighting_Block fits the material block");

        /* SceneUniforms.glsl's Scene_Block, byte for byte: vec4s and mat4s
         * (column-major, as GDI uploads matrices) and arrays of them. */
        struct SceneUniforms
        {
            float CameraPosition[4]     = {};
            float Time[4]               = {};
            float LightDirection[4]     = {};
            float LightColor[4]         = {};
            float SkyColor[4]           = {};
            float GroundColor[4]        = {};
            float InvViewProjection[16] = {};

            float ShadowParams[4]       = {};
            float LightShadow[4]        = { -1.0f, 0.0f, 0.0f, 0.0f };
            float LightShadowTint[4]    = {};
            float CascadeSplits[4]      = {};
            float CameraForward[4]      = {};
            float CascadeMatrices[RENDER_CASCADE_MAX][16] = {};

            float LocalCounts[4]        = {};
            float Locals[RENDER_LOCAL_LIGHT_MAX * SCENE_LOCAL_VEC4S][4] = {};
            float LocalShadowMatrices[SHADOW_LOCAL_ATLAS_COLUMNS * SHADOW_LOCAL_ATLAS_ROWS][16] = {};
            float CascadeTexels[4]      = {};
            float Output[4]             = { 0.0f, 1.0f, 0.0f, 0.0f };   // HDR, exposure multiplier, tone map operator
            float IBL[4]                = {};                           // on, probes, sky block weight (IBLCommon.glsl)
            float Probes[12][4]         = {};                           // 3 vec4s a probe
            float Baked[4]              = {};                           // drawing with a bake, it has the sky (BakedLighting.glsl)
            float View[4]               = {};                           // 1: Unlit (ERenderViewMode)
            float ViewProjection[16]    = {};                           // world to clip: contact shadows' march (Lighting.glsl)
            float ContactShadow[4]      = {};                           // length (0: off), thickness, steps, strength
            float ContactLocal[4]       = {};                           // 1: shadowed point and spot lights' too
            float RevealParams[4]       = {};                           // the reveal: points, aspect, kept in the middle, depth margin
            float Reveal[4][4]          = {};                           // each point: NDC xy, radius (NDC y), view depth
        };
        static_assert(offsetof(SceneUniforms, Time) == 16 && offsetof(SceneUniforms, LightDirection) == 32
                      && offsetof(SceneUniforms, GroundColor) == 80 && offsetof(SceneUniforms, InvViewProjection) == 96,
                      "Scene_Block's members are in this order");
        static_assert(offsetof(SceneUniforms, ShadowParams) == 160 && offsetof(SceneUniforms, CameraForward) == 224
                      && offsetof(SceneUniforms, CascadeMatrices) == 240 && offsetof(SceneUniforms, LocalCounts) == 496
                      && offsetof(SceneUniforms, Locals) == 512 && offsetof(SceneUniforms, LocalShadowMatrices) == 2048,
                      "Scene_Block's shadow and local-light members are in this order");
        static_assert(offsetof(SceneUniforms, CascadeTexels) == 3584 && offsetof(SceneUniforms, Output) == 3600
                      && offsetof(SceneUniforms, IBL) == 3616 && offsetof(SceneUniforms, Probes) == 3632,
                      "Scene_CascadeTexels, Scene_Output, Scene_IBL and Scene_Probes come last");
        static_assert(offsetof(SceneUniforms, Baked) == 3824, "Scene_Baked comes after Scene_Probes");
        static_assert(offsetof(SceneUniforms, View) == 3840, "Scene_View comes after Scene_Baked");
        static_assert(offsetof(SceneUniforms, ViewProjection) == 3856 && offsetof(SceneUniforms, ContactShadow) == 3920,
                      "Scene_ViewProjection and Scene_ContactShadow come after Scene_View");
        static_assert(offsetof(SceneUniforms, ContactLocal) == 3936, "Scene_ContactLocal comes after Scene_ContactShadow");
        static_assert(offsetof(SceneUniforms, RevealParams) == 3952 && offsetof(SceneUniforms, Reveal) == 3968,
                      "Scene_RevealParams and Scene_Reveal come last");
        static_assert(sizeof(SceneUniforms) == 4032 && sizeof(SceneUniforms) <= GDI_SCENE_BLOCK_MAX_BYTES,
                      "SceneUniforms must match Scene_Block (SceneUniforms.glsl) and fit the scene block");

        /* Row-major InMatrix into a GLSL mat4's column-major floats. */
        void PutMatrix(float (&OutValue)[16], const EngineCore::Matrix4x4& InMatrix)
        {
            for(int Row = 0; Row < 4; ++Row)
            {
                for(int Col = 0; Col < 4; ++Col)
                {
                    OutValue[Col * 4 + Row] = float(InMatrix.Data[Row][Col]);
                }
            }
        }

        void PutColor(float (&OutValue)[4], const EngineCore::Color InColor, const float InAlpha)
        {
            OutValue[0] = InColor.Rf();
            OutValue[1] = InColor.Gf();
            OutValue[2] = InColor.Bf();
            OutValue[3] = InAlpha;
        }

        /* A shader file's cooked path, as GDI resolves it (GetCookedShaderPath).
         * InFile is relative to the cooked folder: "Materials/Glow@0000000a.fsh". */
        std::string CookedPath(const std::string& InFile, const ERenderBackend InBackend)
        {
            const std::string& File = InFile;
            switch(InBackend)
            {
                case ERenderBackend::OpenGL: return "Shaders/Cooked/OpenGL/" + File;
                case ERenderBackend::Vulkan: return "Shaders/Cooked/Vulkan/" + File + ".spv";
                case ERenderBackend::Metal:  return "Shaders/Cooked/Metal/"  + File + ".metal";
                case ERenderBackend::D3D12:  return "Shaders/Cooked/D3D12/"  + File + ".cso";
                case ERenderBackend::Null:   break;
            }
            return std::string();
        }

        /* View row 2 is forward (LookAt). */
        EngineCore::Vector3 ViewForward(const RenderView& InView)
        {
            return EngineCore::Vector3(InView.View.Data[2][0], InView.View.Data[2][1], InView.View.Data[2][2]);
        }

        bool IsOrthographic(const RenderView& InView)
        {
            return InView.Projection.Data[3][2] == 0.0f && InView.Projection.Data[3][3] != 0.0f;
        }

        /* Which sections a depth-only draw of a mesh queues (DrawMeshShadow,
         * DrawMeshOccluder): the materials that write depth. */
        enum class EDepthSections : uint8_t
        {
            OpaqueAndMasked,   // a shadow caster: Masked whole, for now -- its clip is not run here
            Opaque,            // an occluder: a Masked one's holes would hide what is behind them
        };
    }

    struct RenderContext::Impl
    {
        ERenderBackend Backend      = ERenderBackend::Null;
        uint64_t       FrameNumber  = 0;
        RenderView     View;

        GDIView*       View3D    = nullptr;   // owns the GDICamera DrawQuad overwrites per SetView()
        GDIViewObject* ViewObject = nullptr;   // owns the shared unit quad mesh

        /* DrawMesh's; its Mesh borrows whichever GDI mesh is drawing. */
        GDIViewObject* MeshObject = nullptr;

        /* A full-screen NDC quad, for PostProcess passes. */
        GDIViewObject* ScreenObject = nullptr;

        /* DrawUI's; its Mesh borrows the scene's UI mesh while it draws. */
        GDIViewObject* UIObject = nullptr;

        /* Keyed by path; rebuilt when MeshAssetCache's pointer differs from
         * Source. Gpu null with Source set: the backend couldn't build it. */
        struct GpuMesh
        {
            std::shared_ptr<const MeshData> Source;
            GDIMesh*                        Gpu = nullptr;

            /* A skinned mesh's reference pose (MakeRefPosePalette): how it
             * draws when nothing poses it. Remade when its skeleton is. */
            std::shared_ptr<const SkeletonData> Skeleton;
            std::vector<float>                  RefPalette;

            /* TEXTURE STREAMING: the world units one UV unit spans, by
             * section (one for a mesh of none), its triangles' areas
             * averaged; 0 where UV0 has no area. */
            std::vector<float>                  WorldPerUV;
        };
        std::unordered_map<std::string, GpuMesh> Meshes;

        /* InPath's reference-pose palette; null if it is not skinned or has no skeleton. */
        const std::vector<float>* FindRefPalette(const std::string& InPath)
        {
            const auto Found = Meshes.find(InPath);
            if(Found == Meshes.end() || !Found->second.Source || !Found->second.Source->IsSkinned())
            {
                return nullptr;
            }
            GpuMesh& Entry = Found->second;
            std::shared_ptr<const SkeletonData> Skeleton = SkeletonAssetCache::Find(Entry.Source->Skeleton);
            if(Skeleton != Entry.Skeleton)
            {
                Entry.Skeleton = Skeleton;
                if(Skeleton) { MakeRefPosePalette(*Entry.Source, *Skeleton, Entry.RefPalette); }
                else         { Entry.RefPalette.clear(); }
            }
            return Entry.RefPalette.empty() ? nullptr : &Entry.RefPalette;
        }

        /* A material's textures, keyed by path and the sampler settings they
         * resolve to; rebuilt when TextureAssetCache's pointer differs from
         * Source. Replaced ones are kept until ReleaseGPUResources: a frame in
         * flight may still sample them. */
        struct GpuTexture
        {
            std::shared_ptr<const TextureData> Source;
            GDITextureResource*                Gpu = nullptr;
            bool                               bStreamed = false;   // Source is a streamed resident set
        };
        std::unordered_map<std::string, GpuTexture> Textures;
        std::vector<GDITextureResource*>            RetiredTextures;

        /*
         * TEXTURE STREAMING (TEXTURE_STREAMING.md). The streamer; the GPU
         * textures its swaps replaced, each freed once the rendering serial
         * is past every frame in flight that could sample it; StreamEpoch,
         * bumped when its entries are forgotten (materials find theirs
         * again); the frame it was last updated in, and whether it streams
         * this frame; the screen pixels a world unit spans at what queues
         * draws now (SetDrawStreamingScale; 0: not known); and whether the
         * draws being made bypass it (previews).
         */
        TextureStreamer Streamer;
        struct RetiredStream
        {
            GDITextureResource* Texture = nullptr;
            uint64_t            Serial  = 0;
        };
        std::vector<RetiredStream> StreamRetired;
        uint64_t StreamEpoch        = 1;
        uint64_t StreamUpdatedFrame = ~uint64_t(0);
        bool     bStreamingThisFrame = false;
        float    DrawPixelsPerWorld  = 0.0f;
        bool     bStreamBypass       = false;

        void RetireStreamed(GDITextureResource* InTexture)
        {
            StreamRetired.push_back({ InTexture, GDIGetRenderingSerial() });
        }

        /* The replaced textures no frame in flight can still sample; all of them with bInAll. */
        void FreeRetiredStreamed(const bool bInAll)
        {
            const uint64_t Serial = GDIGetRenderingSerial();
            const uint64_t Behind = uint64_t(GDIGetFramesInFlight()) + 1;
            size_t Kept = 0;
            for(RetiredStream& Each : StreamRetired)
            {
                if(bInAll || Serial >= Each.Serial + Behind)
                {
                    Each.Texture->Release();
                    delete Each.Texture;
                }
                else
                {
                    StreamRetired[Kept++] = Each;
                }
            }
            StreamRetired.resize(Kept);
        }

        /* A draw queued in a scene: its material's streamed textures asked
         * for at the level its pixels need -- a mesh section's screen pixels
         * per UV unit where it is; anything else, or a draw where that is
         * not known, all of each. */
        void NoteStreamingDemand(const DrawCommand& InItem)
        {
            if(!bStreamingThisFrame || bStreamBypass || !InItem.Mat) { return; }
            RenderMaterial& Mat = *InItem.Mat;
            if(Mat.StreamEpoch != StreamEpoch)
            {
                Mat.StreamEntries.assign(Mat.Description.Textures.size(), nullptr);
                for(size_t Index = 0; Index < Mat.Description.Textures.size(); ++Index)
                {
                    const std::string& Path = Mat.Description.Textures[Index].Path;
                    Mat.StreamEntries[Index] = IsRenderTargetPath(Path) ? nullptr : Streamer.FindEntry(Path);
                }
                Mat.StreamEpoch = StreamEpoch;
            }
            const float Demand = InItem.Kind == DrawCommand::EKind::MeshSection && InItem.WorldPerUV > 0.0f
                               ? InItem.WorldPerUV * DrawPixelsPerWorld : 0.0f;
            /* Per texture: its samples read UV0 times StreamUVScale, so one of its
             * UV units spans 1 / StreamUVScale of UV0's -- that many times the pixels. */
            for(size_t Index = 0; Index < Mat.StreamEntries.size(); ++Index)
            {
                if(TextureStreamer::Entry* Each = Mat.StreamEntries[Index])
                {
                    TextureStreamer::NoteDemand(*Each, Demand / Mat.Description.Textures[Index].StreamUVScale);
                }
            }
        }

        /* Section by section (the whole mesh with none): sqrt of world area
         * over UV area -- how many world units a UV unit spans on average. */
        static void MeasureWorldPerUV(const MeshData& InMesh, std::vector<float>& OutWorldPerUV)
        {
            const auto Measure = [&InMesh](const uint32_t InFirst, const uint32_t InCount) -> float
            {
                const size_t Vertices = InMesh.Positions.size() / 3;
                if(InMesh.UV0.size() < Vertices * 2) { return 0.0f; }
                double World = 0.0, Uv = 0.0;
                const uint32_t End = std::min<uint32_t>(InFirst + InCount, uint32_t(InMesh.Indices.size()));
                for(uint32_t At = InFirst; At + 3 <= End; At += 3)
                {
                    const uint32_t A = InMesh.Indices[At], B = InMesh.Indices[At + 1], C = InMesh.Indices[At + 2];
                    if(A >= Vertices || B >= Vertices || C >= Vertices) { continue; }
                    const float* PA = &InMesh.Positions[size_t(A) * 3];
                    const float* PB = &InMesh.Positions[size_t(B) * 3];
                    const float* PC = &InMesh.Positions[size_t(C) * 3];
                    const double E1[3] = { PB[0] - PA[0], PB[1] - PA[1], PB[2] - PA[2] };
                    const double E2[3] = { PC[0] - PA[0], PC[1] - PA[1], PC[2] - PA[2] };
                    const double X = E1[1] * E2[2] - E1[2] * E2[1];
                    const double Y = E1[2] * E2[0] - E1[0] * E2[2];
                    const double Z = E1[0] * E2[1] - E1[1] * E2[0];
                    World += std::sqrt(X * X + Y * Y + Z * Z) * 0.5;
                    const float* UA = &InMesh.UV0[size_t(A) * 2];
                    const float* UB = &InMesh.UV0[size_t(B) * 2];
                    const float* UC = &InMesh.UV0[size_t(C) * 2];
                    Uv += std::abs(double(UB[0] - UA[0]) * double(UC[1] - UA[1]) - double(UB[1] - UA[1]) * double(UC[0] - UA[0])) * 0.5;
                }
                return Uv > 1.0e-12 && World > 0.0 ? float(std::sqrt(World / Uv)) : 0.0f;
            };
            OutWorldPerUV.clear();
            if(InMesh.Sections.empty())
            {
                OutWorldPerUV.push_back(Measure(0, uint32_t(InMesh.Indices.size())));
                return;
            }
            for(const MeshSection& Section : InMesh.Sections) { OutWorldPerUV.push_back(Measure(Section.FirstIndex, Section.IndexCount)); }
        }

        /*
         * SCENE CAPTURE (RENDERPIPELINE.md): each render target by path -- its
         * texture (a cube's 6S x S strip), the stage targets a face renders
         * through, and its bake uploaded for until the first capture.
         */
        struct GpuRenderTarget
        {
            std::shared_ptr<const RenderTargetData> Source;
            RenderTargetDesc                        Desc;              // as captured: Resolution applied
            GDIRenderTarget*                        Target  = nullptr;
            SceneTargets*                           Capture = nullptr;
            GDITextureResource*                     Baked   = nullptr;
            bool                                    bCaptured = false;
        };
        std::unordered_map<std::string, GpuRenderTarget> RenderTargets;

        /* InPath's entry with its source current; null if it names nothing. */
        GpuRenderTarget* FindRenderTarget(const std::string& InPath)
        {
            std::shared_ptr<const RenderTargetData> Source = RenderTargetAssetCache::Find(InPath);
            if(!Source)
            {
                return nullptr;
            }
            GpuRenderTarget& Entry = RenderTargets[InPath];
            if(Entry.Source != Source)
            {
                /* Edited or re-baked: its bake again, its settings for the next capture. */
                if(Entry.Baked) { RetiredTextures.push_back(Entry.Baked); }
                Entry.Baked  = nullptr;
                Entry.Source = Source;
                if(!Entry.Target) { Entry.Desc = Source->Desc; }
            }
            return &Entry;
        }

        /* What a material samples for InPath: the last capture, else the bake,
         * else nothing (the backend's fallback: transparent black). */
        GDITextureResource* RenderTargetTexture(const std::string& InPath)
        {
            GpuRenderTarget* Entry = FindRenderTarget(InPath);
            if(!Entry)
            {
                return nullptr;
            }
            if(Entry->bCaptured && Entry->Target)
            {
                GDITexture2D* Texture = Entry->Target->ToGDITexture2D();
                return Texture ? Texture->Resource : nullptr;
            }
            if(!Entry->Baked && !Entry->Source->Baked.IsNull())
            {
                const EGDITextureBorder Border = Entry->Source->Desc.Wrap == ETextureWrap::Repeat ? GDI_TB_Repeat
                                               : Entry->Source->Desc.Wrap == ETextureWrap::Mirror ? GDI_TB_Mirrored_Repeat : GDI_TB_Clamp_To_Edge;
                Entry->Baked = GDICreateTextureResource(Entry->Source->Baked, Border,
                                                        Entry->Source->Desc.Filter == ETextureFilter::Nearest ? GDI_TF_Nearest : GDI_TF_Linear);
            }
            return Entry->Baked;
        }

        void ReleaseRenderTargets()
        {
            for(auto& Each : RenderTargets)
            {
                if(Each.second.Target)  { GDIReleaseRenderTarget(Each.second.Target); delete Each.second.Target; }
                if(Each.second.Capture) { Each.second.Capture->Release(); delete Each.second.Capture; }
                if(Each.second.Baked)   { RetiredTextures.push_back(Each.second.Baked); }
            }
            RenderTargets.clear();
        }

        /* Skips redundant rebinds. Reset each frame. */
        GDIShader*     BoundVertex = nullptr;
        GDIShader*     BoundPixel  = nullptr;

        std::unordered_map<std::string, Material> Materials;   // node-based: a Material* survives inserts
        GDIShader*              DefaultVertex[5] = {};          // MaterialDefault, by EMaterialVertexInput
        bool                    bDefaultVertexTried[5] = {};
        std::vector<GDIShader*> RetiredShaders;                 // see RenderContext::ReloadMaterial
        std::vector<uint8_t>    OverrideBlock;                  // BindMaterial's: a block with an entity's overrides

        /* Parallel recording (MULTITHREADED_RENDERING.md, phase 3): the gate as
         * this frame sees it, each chunk's scratch, the lightmaps resolved for
         * the pass, whether its draws are labelled, and how many passes ran. */
        ParallelRecordingState  Parallel;
        std::vector<std::unique_ptr<RenderIssueScratch>> Scratches;
        GDITextureResource*     LightmapResolved[RENDER_BAKED_ATLASES_MAX] = {};
        uint64_t                ParallelPassSerial = 0;
        bool                    bLabelsThisPass    = false;
        bool                    bParallelLatched   = false;
        std::atomic<uint64_t>   ParallelPassCount{0};
        std::mutex              WarnLock;

        /* The issue state of the calling thread: a recording chunk's, else the Impl's own. */
        GDIShader*&                   RefBoundVertex()     { return GIssueScratch ? GIssueScratch->BoundVertex : BoundVertex; }
        GDIShader*&                   RefBoundPixel()      { return GIssueScratch ? GIssueScratch->BoundPixel : BoundPixel; }
        GDIView*                      RefView3D()          { return GIssueScratch ? GIssueScratch->View3D : View3D; }
        GDIViewObject*                RefMeshObject()      { return GIssueScratch ? GIssueScratch->MeshObject : MeshObject; }
        const EngineCore::Matrix4x4*  RefPlaceProjection() { return GIssueScratch ? GIssueScratch->PlaceProjection : PlaceProjection; }
        const EngineCore::Matrix4x4*  RefPlaceView()       { return GIssueScratch ? GIssueScratch->PlaceView : PlaceView; }
        std::vector<uint8_t>&         RefOverrideBlock()   { return GIssueScratch ? GIssueScratch->OverrideBlock : OverrideBlock; }
        std::set<std::string>   Warned;                         // one warning per material and reason

        /* The scene block, refreshed by SetView and SetLighting. */
        RenderLighting                        Lighting;
        SceneUniforms                         Scene;
        std::chrono::steady_clock::time_point Start     = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point LastFrame = Start;
        float                                 FrameDelta = 0.0f;

        /* Inside a scene (BeginSceneDraws to RenderSceneStages) draws queue in
         * their stage's policy; outside one they are issued at once. */
        bool       bDeferring = false;
        DrawPolicy Policies[size_t(ERenderStage::Count)];

        /* BeginCapture's list: draws land here instead of the policies. */
        std::vector<StagedDraw>* Capture = nullptr;

        /* GetContentGeneration's own half: bumped when a material or GPU mesh
         * a kept draw could point at is replaced or retired. */
        uint64_t ContentGeneration = 0;
        uint32_t   StageDrawCounts[size_t(ERenderStage::Count)] = {};   // the last RenderSceneStages'

        /* DeferredLighting.fsh, loaded on first use; null if it cannot be (warned). */
        GDIShader* LightingShader = nullptr;
        bool       bLightingTried = false;

        /* SHADOWS (RENDERPIPELINE.md). The frame's views, built by
         * PrepareShadows for a scene (never for a preview); the two atlases,
         * shared by every scene this context draws, sized by [Shadows]
         * Resolution; ShadowDepth.fsh. While a proxy queues its casters,
         * CasterView is the view they draw into; while the Shadows stage
         * issues them, PlaceProjection and PlaceView replace the camera. */
        ShadowFrame      Shadows;
        GDIRenderTarget* ShadowAtlas[SA_Count] = {};
        uint32_t         ShadowAtlasSize       = 0;
        GDIShader*       ShadowDepthShader     = nullptr;
        bool             bShadowDepthTried     = false;
        int32_t          CasterView            = -1;

        /* POST PROCESS (RENDERPIPELINE.md, "Post process"). The settings and
         * last frame's view-projection the next RenderSceneStages runs the
         * stack with -- set by RenderScene, consumed (reset) by that call, so
         * a preview's never sees a scene's. PostSceneDepth: the scene depth a
         * PostProcess material samples (material slot 7) while the stack runs. */
        PostProcessSettings   PostSettings;
        EngineCore::Matrix4x4 PrevViewProjection;
        bool                  bHasPrevViewProjection = false;
        bool                  bTemporalResolved      = false;   // this frame's TAA ran: the stack's FXAA stands down
        float                 TemporalJitter[2]      = {};      // the main view's TAA jitter, NDC (SetTemporalJitter)
        GDITextureResource*   PostSceneDepth = nullptr;
        GDIShader*            PostShaders[PP_PassCount] = {};
        bool                  bPostShaderTried[PP_PassCount] = {};
        const EngineCore::Matrix4x4* PlaceProjection = nullptr;
        const EngineCore::Matrix4x4* PlaceView       = nullptr;

        /* RenderMaterialPreview's targets, square, resized as asked; and the
         * target the last RenderSceneStages finished in. */
        SceneTargets     PreviewTargets{"RenderCore.MaterialPreview"};
        GDIRenderTarget* FinalTarget = nullptr;

#if WITH_EDITOR
        /* Previews drawn and on their way back (DeliverMaterialPreviews). */
        struct PendingPreview
        {
            std::unique_ptr<GDIReadback> Readback;
            uint64_t                     Request = 0;
            MaterialPreviewMailbox*      Mailbox = nullptr;
        };
        std::vector<PendingPreview> PendingPreviews;
#endif

        std::atomic<uint64_t> DrawCallCount{0};
        std::atomic<uint64_t> ParticleSimulationCount{0};

        Impl()
        {
            for(size_t Index = 0; Index < size_t(ERenderStage::Count); ++Index)
            {
                const ERenderStage Stage        = ERenderStage(Index);
                const bool         bBackToFront = Stage == ERenderStage::TranslucentLit || Stage == ERenderStage::TranslucentUnlit;
                Policies[Index].Init(Stage, bBackToFront ? EDrawSort::BackToFront : EDrawSort::Submission);
            }
        }

        DrawPolicy& PolicyFor(const ERenderStage InStage) { return Policies[size_t(InStage)]; }

        ~Impl()
        {
            if(MeshObject)
            {
                delete MeshObject->Transform;
                delete MeshObject;
            }
            if(ScreenObject)
            {
                delete ScreenObject->Transform;
                delete ScreenObject->Mesh;
                delete ScreenObject;
            }
            if(UIObject)
            {
                UIObject->Mesh = nullptr;
                delete UIObject->Transform;
                delete UIObject;
            }
            delete ViewObject;
            delete View3D;
        }

        void EnsureInitialised()
        {
            if(View3D)
            {
                return;
            }

            View3D = new GDIView();

            /* GDIViewObject's own constructor creates a half-unit quad
             * (HalfQuadVertData in GDIView.cpp) through GDICreateQuadMesh --
             * exactly the "one mesh, scaled per draw" shape DrawQuad wants;
             * GDITransform::Scale (set per call below) is what actually
             * varies a primitive's on-screen size. */
            /* Not GDIViewObject's own quad: its UVs put v = 0 at local -Y,
             * the bottom in this engine's Y-up view, so textures would draw
             * upside down. Same half-unit square, v = 0 at the top. */
            static const float QuadVerts[] = { -0.5f, -0.5f, 0.f,  -0.5f, 0.5f, 0.f,  0.5f, 0.5f, 0.f,
                                                0.5f,  0.5f, 0.f,   0.5f, -0.5f, 0.f, -0.5f, -0.5f, 0.f };
            static const float QuadUVs[]   = { 0.f, 1.f,  0.f, 0.f,  1.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f };
            ViewObject = new GDIViewObject();
            ViewObject->Init(View3D);
            ViewObject->Mesh = GDICreateQuadMesh(QuadVerts, QuadUVs, 18, 12);

            MeshObject = new GDIViewObject();
            MeshObject->Init(View3D);

            /* The vertices and UVs every backend's own screen quad uses: v = 0
             * at NDC +1, a target's top row (GDI.h), so a pass's TexCoord is
             * the same pixel in every target it reads. */
            static const float Verts[] = { -1.f, -1.f, 0.f,  -1.f, 1.f, 0.f,  1.f, 1.f, 0.f,
                                            1.f,  1.f, 0.f,   1.f, -1.f, 0.f, -1.f, -1.f, 0.f };
            static const float UVs[]   = { 0.f, 1.f,  0.f, 0.f,  1.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f };
            ScreenObject = new GDIViewObject();
            ScreenObject->Init(View3D);
            ScreenObject->Mesh = GDICreateQuadMesh(Verts, UVs, 18, 12, false);

            UIObject = new GDIViewObject();
            UIObject->Init(View3D);
        }

        void Bind(GDIShader* InVertex, GDIShader* InPixel)
        {
            GDIShader*& Vertex = RefBoundVertex();
            GDIShader*& Pixel  = RefBoundPixel();
            if(InVertex != Vertex || InPixel != Pixel)
            {
                GDISetBoundShader(InVertex, InPixel);
                Vertex = InVertex;
                Pixel  = InPixel;
            }
        }

        /* Camera, and InTransform with scale times InScaleX/Y (1 for a mesh). */
        void Place(GDIViewObject* InObject, const RenderTransform& InTransform, const float InScaleX, const float InScaleY,
                   const DrawCommand* InItem = nullptr)
        {
            /* Here rather than in SetView(), so a RenderContext without a
             * RenderScene (tests) still draws. */
            GDIView* const Target = RefView3D();   // a recording chunk's own, else the Impl's
            Target->Camera->Projection = RefPlaceProjection() ? *RefPlaceProjection() : View.Projection;
            Target->Camera->ViewMat    = RefPlaceView() ? *RefPlaceView() : View.View;

            GDITransform* Transform = InObject->Transform;
            Transform->Translate = InTransform.Position;
            Transform->Rotate    = InTransform.Rotation;
            Transform->Scale     = EngineCore::Vector3(InTransform.Scale.GetX() * double(InScaleX),
                                                       InTransform.Scale.GetY() * double(InScaleY),
                                                       InTransform.Scale.GetZ());

            /* Where it was the frame before (Transform.PrevModel), for velocity. */
            Transform->bHasPrevModel = InItem && InItem->bMoved;
            if(Transform->bHasPrevModel)
            {
                Transform->PrevModel = InItem->Motion * Transform->GetModel();
            }
        }

        /* The camera and the quad's placement, shared by both quad draws. */
        void PlaceQuad(const RenderTransform& InTransform, const float InWidth, const float InHeight, const DrawCommand* InItem = nullptr)
        {
            Place(ViewObject, InTransform, InWidth, InHeight, InItem);
        }

        /* Null if unloadable or unbuildable; OutSource is set if found. */
        GDIMesh* FindMesh(const std::string& InPath, std::shared_ptr<const MeshData>& OutSource,
                          const std::vector<float>** OutWorldPerUV = nullptr)
        {
            OutSource = MeshAssetCache::Find(InPath);
            GpuMesh& Entry = Meshes[InPath];
            if(OutWorldPerUV) { *OutWorldPerUV = &Entry.WorldPerUV; }
            if(Entry.Source == OutSource)
            {
                return Entry.Gpu;
            }

            delete Entry.Gpu;   // each backend defers the buffers past any frame still using them
            Entry.Gpu    = nullptr;
            ++ContentGeneration;
            Entry.Source = OutSource;
            if(!OutSource)
            {
                return nullptr;
            }

            GDIMeshDesc Desc;
            Desc.Positions   = OutSource->Positions.data();
            Desc.Normals     = OutSource->Normals.data();
            Desc.UV0         = OutSource->UV0.data();
            Desc.Colors      = OutSource->Colors.empty() ? nullptr : OutSource->Colors.data();
            Desc.UV1         = OutSource->UV1.empty() ? nullptr : OutSource->UV1.data();
            /* A mesh made in memory may fill its arrays itself: tangents only if every vertex has one. */
            Desc.Tangents    = OutSource->Tangents.size() == size_t(OutSource->GetVertexCount()) * 4 ? OutSource->Tangents.data() : nullptr;
            Desc.NumVertices = OutSource->GetVertexCount();
            Desc.Indices     = OutSource->Indices.data();
            Desc.NumIndices  = uint32_t(OutSource->Indices.size());

            /* A skinned mesh's bone indices go to the GPU as floats, as its weights are. */
            std::vector<float> Joints;
            if(OutSource->IsSkinned())
            {
                Joints.assign(OutSource->Joints.begin(), OutSource->Joints.end());
                Desc.Joints  = Joints.data();
                Desc.Weights = OutSource->Weights.data();
            }
            Entry.Gpu = GDICreateMesh(Desc);
            MeasureWorldPerUV(*OutSource, Entry.WorldPerUV);
            GDISetDebugName(Entry.Gpu, InPath);   // its buffers in a capture tool; and its draws' labels (Issue)
            if(!Entry.Gpu)
            {
                RENDERCORE_LOG_WARN("mesh '" + InPath + "': this backend cannot draw it; drawing a quad")
            }
            return Entry.Gpu;
        }

        /* A material texture's GPU copy with its sampler: the node's settings
         * where it overrides them, else the asset's. Null (warned by the
         * cache) if the file cannot be loaded: the slot reads transparent.
         * Streamed (TEXTURE_STREAMING.md) unless bInWhole -- what keeps the
         * pointer (the UI), or draws outside a view (previews) -- or
         * streaming is off: then every level, from TextureAssetCache. */
        GDITextureResource* FindTexture(const MaterialTexture& InTexture, const bool bInWhole = false)
        {
            /* A render target (a scene capture), not a file. */
            if(IsRenderTargetPath(InTexture.Path))
            {
                return RenderTargetTexture(InTexture.Path);
            }
            TextureStreamer::Entry* Stream = bInWhole || bStreamBypass ? nullptr : Streamer.FindEntry(InTexture.Path);
            std::shared_ptr<const TextureData> Source = Stream ? TextureStreamer::GetResident(*Stream) : TextureAssetCache::Find(InTexture.Path);
            if(!Source)
            {
                return nullptr;
            }

            /* The streamed copy's sampler is the file's: a resident set has no say. */
            const ETextureFilter Filter = InTexture.bFilter ? InTexture.Filter : Source->Filter;
            const ETextureWrap   Wrap   = InTexture.bWrap ? InTexture.Wrap : Source->Wrap;
            const std::string    Key    = InTexture.Path + "|" + std::to_string(int(Filter)) + std::to_string(int(Wrap))
                                        + ((Stream || !Streamer.IsEnabled()) ? "" : "|whole");

            GpuTexture& Entry = Textures[Key];
            if(Entry.Source == Source)
            {
                return Entry.Gpu;
            }
            /* A streamed texture is replaced as its levels change: freed once
             * no frame in flight can sample it, not kept to the end. */
            if(Entry.Gpu && (Stream || Entry.bStreamed)) { RetireStreamed(Entry.Gpu); }
            else if(Entry.Gpu)                           { RetiredTextures.push_back(Entry.Gpu); }
            Entry.Source    = Source;
            Entry.bStreamed = Stream != nullptr;

            const EGDITextureBorder Border = Wrap == ETextureWrap::Clamp  ? GDI_TB_Clamp_To_Edge
                                           : Wrap == ETextureWrap::Mirror ? GDI_TB_Mirrored_Repeat : GDI_TB_Repeat;
            const EGDITextureFilter GpuFilter = Filter == ETextureFilter::Nearest ? GDI_TF_Nearest : GDI_TF_Linear;
            const auto Began = std::chrono::steady_clock::now();
            struct NoteCreate   // "stat streaming": what making a streamed texture costs this thread
            {
                TextureStreamer* Streamer;
                std::chrono::steady_clock::time_point Began;
                ~NoteCreate()
                {
                    if(Streamer) { Streamer->NoteCreateMs(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - Began).count()); }
                }
            } Noted{ Stream ? &Streamer : nullptr, Began };
            if(!Source->Blocks.empty())
            {
                Entry.Gpu = CreateBlockTexture(*Source, InTexture.Path, Border, GpuFilter);
            }
            else if(Source->Mips.empty())
            {
                Entry.Gpu = GDICreateTextureResource(Source->Pixels, Border, GpuFilter);
            }
            else
            {
                /* The baked chain (TextureAsset.h): trilinear with a Linear filter. */
                std::vector<EngineCore::Image> Levels;
                Levels.reserve(Source->Mips.size() + 1);
                Levels.push_back(Source->Pixels);
                Levels.insert(Levels.end(), Source->Mips.begin(), Source->Mips.end());
                Entry.Gpu = GDICreateTextureResourceMips(Levels.data(), uint32_t(Levels.size()), Border, GpuFilter);
            }
            if(!Entry.Gpu)
            {
                WarnOnce(InTexture.Path + "|texture", "texture '" + InTexture.Path + "': this backend could not create it");
            }
            GDISetDebugName(Entry.Gpu, InTexture.Path);   // what a capture tool lists it as
            return Entry.Gpu;
        }

        /* InSource's blocks as they are where the backend samples the format,
         * else decoded to RGBA8 -- the same image either way. */
        GDITextureResource* CreateBlockTexture(const TextureData& InSource, const std::string& InPath,
                                               const EGDITextureBorder InBorder, const EGDITextureFilter InFilter)
        {
            EGDICompressedFormat Format = GDI_CF_MAX;
            switch(InSource.Compression)
            {
                case ETextureCompression::BC1:  Format = GDI_CF_BC1; break;
                case ETextureCompression::BC3:  Format = GDI_CF_BC3; break;
                case ETextureCompression::BC4:  Format = GDI_CF_BC4; break;
                case ETextureCompression::BC5:  Format = GDI_CF_BC5; break;
                case ETextureCompression::BC7:  Format = GDI_CF_BC7; break;
                case ETextureCompression::None: break;
            }
            if(GDISupportsCompressedFormat(Format))
            {
                std::vector<GDICompressedLevel> Levels;
                Levels.reserve(InSource.Blocks.size());
                for(const TextureBlockLevel& Each : InSource.Blocks)
                {
                    Levels.push_back({ uint32_t(Each.Width), uint32_t(Each.Height), Each.Bytes.data(), Each.Bytes.size() });
                }
                if(GDITextureResource* Texture = GDICreateCompressedTextureResource(Format, Levels.data(), uint32_t(Levels.size()),
                                                                                   InBorder, InFilter))
                {
                    return Texture;
                }
            }

            WarnOnce(InPath + "|blocks", std::string("texture '") + InPath + "': this backend cannot sample "
                     + GetTextureCompressionName(InSource.Compression) + ", so it is decoded to RGBA8");
            std::vector<EngineCore::Image> Levels(InSource.Blocks.size());
            for(size_t Level = 0; Level < Levels.size(); ++Level)
            {
                if(!DecodeTextureBlocks(InSource.Blocks[Level], InSource.Compression, Levels[Level])) { return nullptr; }
            }
            return Levels.size() == 1 ? GDICreateTextureResource(Levels[0], InBorder, InFilter)
                                      : GDICreateTextureResourceMips(Levels.data(), uint32_t(Levels.size()), InBorder, InFilter);
        }

        void WarnOnce(const std::string& InKey, const std::string& InMessage)
        {
            std::lock_guard<std::mutex> Lock(WarnLock);   // recording chunks warn too
            if(Warned.insert(InKey).second)
            {
                RENDERCORE_LOG_WARN(InMessage)
            }
        }

        // === Shaders and materials ===========================================

        /* A cooked shader with the material binding layout -- a permutation
         * ("Materials/Glow@0000000a.fsh") or an engine one -- or null
         * (warned) if this backend has none. */
        GDIShader* LoadShader(const std::string& InFile, const EShaderType InType, const std::string& InWho, const bool bInQuiet)
        {
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            const std::string Cooked = CookedPath(InFile, Backend);

            /* Checked first: OpenGL's InitResource treats a missing file as fatal. */
            if(Backend != ERenderBackend::Null && (!Provider || Cooked.empty() || !Provider->Exists(Cooked)))
            {
                if(!bInQuiet)
                {
                    WarnOnce(InWho + "|" + InFile, InWho + ": no cooked shader at " + Cooked + " -- cook it for this backend");
                }
                return nullptr;
            }

            auto* Shader = new MaterialShader("Material:" + InFile);
            Shader->InitResource(InType, InFile);
            if(Shader->Resource)
            {
                Shader->Resource->SetMaterialLayout(true);
            }
            Shader->InitUniformBindings();
            return Shader;
        }

        GDIShader* GetDefaultVertex(const EMaterialVertexInput InInput)
        {
            const int Index = int(InInput);
            if(!bDefaultVertexTried[Index])
            {
                bDefaultVertexTried[Index] = true;
                DefaultVertex[Index] = LoadShader(MATERIAL_FOLDER + GetPermutationFileName(MATERIAL_DEFAULT_VERTEX, GetVertexPermutationKey(InInput), true),
                                                  ST_Vertex, "the shared material vertex shaders", false);
            }
            return DefaultVertex[Index];
        }

        /* Content/Shaders/<InFile>, a compute shader, loaded once and kept
         * (each pipeline belongs to one shader); null without compute or
         * cooked shader -- warned once by LoadShader. */
        std::unordered_map<std::string, GDIShader*> ComputeShaders;
        GDIShader* GetComputeShader(const char* InFile)
        {
            const auto Found = ComputeShaders.find(InFile);
            if(Found != ComputeShaders.end())
            {
                return Found->second;
            }
            GDIShader* Shader = GDISupportsCompute() ? LoadShader(InFile, ST_Compute, "GPU particles", false) : nullptr;
            ComputeShaders.emplace(InFile, Shader);
            return Shader;
        }

        GDIShader* GetLightingShader()
        {
            if(!bLightingTried)
            {
                bLightingTried = true;
                LightingShader = LoadShader(LIGHTING_SHADER, ST_Pixel, "the Lighting stage", false);
            }
            return LightingShader;
        }

        GDIShader* GetPostShader(const EPostPass InPass)
        {
            if(!bPostShaderTried[InPass])
            {
                bPostShaderTried[InPass] = true;
                PostShaders[InPass] = LoadShader(POST_PASS_SHADERS[InPass], ST_Pixel, "the post process stack", false);
            }
            return PostShaders[InPass];
        }

        /* How a full-screen pass meets what its target holds. */
        enum class EFullscreenBlend : uint8_t
        {
            Opaque,
            Additive,
            Transmit,     // the target times the source's alpha, plus its colour (a medium laid over it: fog)
            AlphaBlend,   // the source over the target by its alpha
            Multiply,     // the target times the source's colour, its alpha times the source's (PPAmbientApply.fsh)
        };

        /*
         * One full-screen pass: InPixel into InTarget (InWidth x InHeight at
         * InX, no depth), InTextures as material slots 0.., InBlock's
         * InBlockBytes as the material block. InSecond: a second target written
         * beside InTarget (output 1), blended the same way. False if a shader
         * or the target is missing.
         */
        bool DrawFullscreenShader(GDIShader* InPixel, const char* InEvent, GDIRenderTarget* InTarget, const uint InWidth, const uint InHeight,
                                  GDITextureResource* const* InTextures, const uint32_t InTextureCount, const void* InBlock,
                                  const uint InBlockBytes, const EFullscreenBlend InBlend, const uint InX = 0, const uint InY = 0,
                                  GDIRenderTarget* InSecond = nullptr)
        {
            GDIShader* Vertex = GetDefaultVertex(EMaterialVertexInput::Fullscreen);
            if(!InPixel || !Vertex || !InTarget)
            {
                return false;
            }
            SCOPED_DRAW_EVENT(InEvent);
            GDIRenderTarget* Targets[2] = { InTarget, InSecond };
            GDISetRenderTargets(Targets, InSecond ? 2 : 1, nullptr);
            GDISetViewport(InX, InY, InWidth, InHeight);   // InX: a cube face's place in its strip; InX, InY: a view's in the window
            GDISetColorMask(EColM_RGBA);

            for(uint32_t Slot = 0; Slot < InTextureCount; ++Slot)
            {
                GDISetMaterialTexture(Slot, InTextures[Slot]);
            }
            EnsureInitialised();
            Bind(Vertex, InPixel);
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, InBlock, InBlockBytes);
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));
            GDISetDepthState(IDepthStateDesc());
            switch(InBlend)
            {
                case EFullscreenBlend::Opaque:     GDISetBlendState(IBlendStateDesc()); break;
                case EFullscreenBlend::Additive:   GDISetBlendState(IBlendStateDesc(true, BM_One, BM_One)); break;
                case EFullscreenBlend::Transmit:   GDISetBlendState(IBlendStateDesc(true, BM_One, BM_Src_Alpha)); break;
                case EFullscreenBlend::AlphaBlend: GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha)); break;
                case EFullscreenBlend::Multiply:   GDISetBlendState(IBlendStateDesc(true, BM_Zero, BM_Src_Color)); break;
            }
            GDIDrawObject(ScreenObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
            for(uint32_t Slot = 0; Slot < InTextureCount; ++Slot)
            {
                GDISetMaterialTexture(Slot, nullptr);
            }
            return true;
        }

        /*
         * One full-screen pass of the stack: InPass's shader into InTarget
         * (InWidth x InHeight, no depth), InTextures as material slots 0..,
         * InBlock as PP_Block. Additive blends into what InTarget holds. False
         * (warned by LoadShader) if a shader is missing.
         */
        bool DrawPost(const EPostPass InPass, GDIRenderTarget* InTarget, const uint InWidth, const uint InHeight,
                      std::initializer_list<GDITextureResource*> InTextures, const PostBlock& InBlock, const bool bInAdditive = false,
                      const uint InX = 0, const bool bInTransmit = false)
        {
            /* These blocks carry their matrix first; the others start at A. */
            const bool bMatrix = InPass == PP_MotionBlur || InPass == PP_AmbientOcclusion || InPass == PP_IBLSky || InPass == PP_Sky
                              || InPass == PP_TemporalAA || InPass == PP_ScreenReflections || InPass == PP_VolumetricFog;
            const void* Block      = bMatrix ? static_cast<const void*>(&InBlock) : static_cast<const void*>(InBlock.A);
            const uint  BlockBytes = uint(bMatrix ? sizeof(InBlock) : sizeof(InBlock) - sizeof(InBlock.Matrix));
            return DrawFullscreenShader(GetPostShader(InPass), POST_PASS_NAMES[InPass], InTarget, InWidth, InHeight,
                                        InTextures.begin(), uint32_t(InTextures.size()), Block, BlockBytes,
                                        bInAdditive ? EFullscreenBlend::Additive
                                      : bInTransmit ? EFullscreenBlend::Transmit : EFullscreenBlend::Opaque, InX);
        }

        /* Auto exposure (RENDERPIPELINE.md, "Auto exposure"): InLinear (the HDR scene colour)
         * metered into InTargets.Luminance, 64 x 64 down to 1 x 1, then the adapted value
         * eased toward it into the other of InTargets.Exposure. The adapted 1 x 1 to read;
         * null (exposure stays as the stack sets it) if a target or pass is missing. */
        /* Temporal antialiasing (RENDERPIPELINE.md, "Temporal antialiasing"): InCurrent, this
         * frame's jittered scene colour, blended into InTargets.History by PPTemporalAA.fsh,
         * reprojected through the depth and velocity targets and PrevViewProjection (the
         * last frame's, unjittered). The resolve; null if a target is missing. */
        GDITextureResource* ResolveTemporal(SceneTargets& InTargets, GDITextureResource* InCurrent, const bool bInHasPrevious)
        {
            GDITexture2D* Depth = InTargets.SceneColor ? InTargets.SceneColor->ToDepthTexture2D() : nullptr;
            GDITexture2D* Motion = InTargets.Velocity ? InTargets.Velocity->ToGDITexture2D() : nullptr;
            if(!Depth || !Motion || !InTargets.EnsureHistory())
            {
                InTargets.bHistoryValid = false;
                return nullptr;
            }
            const auto Texture = [](GDIRenderTarget* InTarget) -> GDITextureResource*
            {
                GDITexture2D* Each = InTarget ? InTarget->ToGDITexture2D() : nullptr;
                return Each ? Each->Resource : nullptr;
            };
            const uint32_t Write = InTargets.HistoryCurrent ^ 1u;
            PostBlock Block;
            PutMatrix(Block.Matrix, PrevViewProjection);
            Block.A[0] = InTargets.bHistoryValid && bInHasPrevious ? 1.0f : 0.0f;
            Block.A[1] = 0.9f;    // history weight: about ten frames' worth
            Block.A[2] = 1.25f;   // clip box half width, in standard deviations
            // This frame's jitter in target UV: a pixel centre here shows the point the
            // unjittered view has that far back, so the reprojection takes it off.
            Block.Sizes[0] = TemporalJitter[0] * 0.5f;
            Block.Sizes[1] = -TemporalJitter[1] * 0.5f;
            Block.Sizes[2] = float(InTargets.Width);
            Block.Sizes[3] = float(InTargets.Height);
            const bool bDone = DrawPost(PP_TemporalAA, InTargets.History[Write], InTargets.Width, InTargets.Height,
                                        { InCurrent, Texture(InTargets.History[InTargets.HistoryCurrent]), Depth->Resource, Motion->Resource },
                                        Block);
            for(uint32_t Slot = 0; Slot < 4; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
            if(!bDone)
            {
                InTargets.bHistoryValid = false;
                return nullptr;
            }
            InTargets.HistoryCurrent = Write;
            InTargets.bHistoryValid  = true;
            return Texture(InTargets.History[Write]);
        }

        GDITextureResource* MeterExposure(SceneTargets& InTargets, GDITextureResource* InLinear,
                                          const PostProcessSettings& InSettings, const float InDeltaSeconds)
        {
            if(!InTargets.EnsureExposure())
            {
                return nullptr;
            }
            SCOPED_DRAW_EVENT("AutoExposure");
            const auto Texture = [](GDIRenderTarget* InTarget) -> GDITextureResource*
            {
                GDITexture2D* Each = InTarget ? InTarget->ToGDITexture2D() : nullptr;
                return Each ? Each->Resource : nullptr;
            };
            PostBlock Meter;
            Meter.A[0] = InSettings.AeCentreWeight;
            PostBlock Reduce;
            Reduce.A[0] = 8.0f;
            const uint32_t Write = InTargets.ExposureCurrent ^ 1u;
            PostBlock Adapt;
            Adapt.A[0] = InDeltaSeconds;
            Adapt.A[1] = InSettings.AeSpeedUp;
            Adapt.A[2] = InSettings.AeSpeedDown;
            Adapt.A[3] = InTargets.bExposureValid ? 1.0f : 0.0f;
            const bool bDone = DrawPost(PP_Luminance, InTargets.Luminance[0], 64, 64, { InLinear }, Meter)
                            && DrawPost(PP_LuminanceReduce, InTargets.Luminance[1], 8, 8, { Texture(InTargets.Luminance[0]) }, Reduce)
                            && DrawPost(PP_LuminanceReduce, InTargets.Luminance[2], 1, 1, { Texture(InTargets.Luminance[1]) }, Reduce)
                            && DrawPost(PP_AdaptExposure, InTargets.Exposure[Write], 1, 1,
                                        { Texture(InTargets.Luminance[2]), Texture(InTargets.Exposure[InTargets.ExposureCurrent]) }, Adapt);
            GDISetMaterialTexture(0, nullptr);
            GDISetMaterialTexture(1, nullptr);
            if(!bDone)
            {
                InTargets.bExposureValid = false;
                return nullptr;
            }
            InTargets.ExposureCurrent = Write;
            InTargets.bExposureValid  = true;
            return Texture(InTargets.Exposure[Write]);
        }

        /* RenderSceneStages' post process stack (RENDERPIPELINE.md, "Post
         * process") into InTargets' post targets; returns where the result is
         * -- the scene colour if nothing ran. */
        GDIRenderTarget* RunPostProcessStack(SceneTargets& InTargets, const PostProcessSettings& InPost, bool bInHasPrevious,
                                             GDIRenderTarget* InSource);

        /* The post process stack's Ambient Occlusion, between the Base Pass
         * and the Lighting stage (RENDERPIPELINE.md, "Ambient occlusion"):
         * into a half-size target AmbientOcclusion names for IssueLighting,
         * which stays null if InPost has none or this backend cannot. */
        void IssueAmbientOcclusion(SceneTargets& InTargets, const PostProcessSettings& InPost);
        GDITextureResource* AmbientOcclusion = nullptr;

        /* Whether any of this frame's Base Pass draws has InModel (SHADING_MODELS.md). */
        bool BasePassHasModel(const EMaterialShadingModel InModel) const
        {
            const DrawPolicy& Base = Policies[size_t(ERenderStage::BasePass)];
            for(size_t Index = 0; Index < Base.Num(); ++Index)
            {
                const RenderMaterial* Mat = Base[Index].Mat;
                if(Mat && Mat->Description.ShadingModel == InModel) { return true; }
            }
            return false;
        }

        /* Skin's screen-space subsurface scattering (SHADING_MODELS.md, PPSubsurface.fsh):
         * InDiffuse, its pixels' diffuse light before albedo, blurred across into
         * InTargets.SubsurfaceBlur, then down, times the albedo, added to InSceneColor. */
        void IssueSubsurface(SceneTargets& InTargets, GDIRenderTarget* InDiffuse, GDIRenderTarget* InSceneColor);
        GDITextureResource* OverlayFont      = nullptr;   // OverlayFont.h's glyphs side by side, made on first use

        /* DrawUI's: its vertex shader (UI.fsh is PP_UI), the white texture a
         * list's texture 0 is, and Renderer::SetUIImage's images by key --
         * UIImageEpoch counts their changes, for each scene's resolved textures. */
        GDIShader*                                           UIVertexShader = nullptr;
        bool                                                 bUIVertexTried = false;
        GDITextureResource*                                  UIWhite        = nullptr;
        std::unordered_map<std::string, GDITextureResource*> UIImages;
        uint64_t                                             UIImageEpoch   = 1;

        /* "stat frame": the scene timing this GDI block, and the scene that
         * timed each recent block, by its GDIGetRenderingSerial (slot: serial
         * mod 4) -- a block's times come back GDIGetFramesInFlight blocks on. */
        struct TimedBlock { uint64_t Serial = 0; const void* Owner = nullptr; uint32_t Views = 1; };
        bool        bTimeStages = false;
        const void* TimedOwner  = nullptr;
        TimedBlock  TimedBlocks[4];

        /*
         * SPLITSCREEN: each of the block's views times its stages, summed span
         * by span. View 0 writes 2t and 2t + 1, as a lone view always has; view
         * v > 0 its per-view spans (all but Total) from VIEW_TIMESTAMPS_BASE,
         * VIEW_TIMESTAMPS apiece; and with several views EndTimedViews writes
         * the last one's end at VIEWS_END_TIMESTAMP, Total running from view
         * 0's start to there. 64 at most: GDI's written mask is one uint64_t.
         */
        static constexpr uint32_t VIEW_TIMESTAMPS_BASE = uint32_t(RGT_Count) * 2;
        static constexpr uint32_t VIEW_TIMESTAMPS      = uint32_t(RGT_Total) * 2;
        static constexpr uint32_t VIEWS_END_TIMESTAMP  = GDI_GPU_TIMESTAMP_MAX - 1;
        static_assert(VIEW_TIMESTAMPS_BASE + (RENDER_VIEWS_MAX - 1) * VIEW_TIMESTAMPS <= VIEWS_END_TIMESTAMP,
                      "every view's spans and the views' end fit GDI's timestamps");
        uint32_t    TimedView  = 0;   // the view timed now (RenderView::ViewIndex)
        uint32_t    TimedViews = 0;   // how many the block has timed

        /* InTimer's start (or end) timestamp index for TimedView; -1: none (a later view's Total). */
        int32_t StageTimestampIndex(const ERenderGpuTimer InTimer, const bool bInEnd) const
        {
            const uint32_t Offset = uint32_t(InTimer) * 2 + (bInEnd ? 1 : 0);
            if(TimedView == 0) { return int32_t(Offset); }
            if(InTimer >= RGT_Total) { return -1; }
            return int32_t(VIEW_TIMESTAMPS_BASE + (TimedView - 1) * VIEW_TIMESTAMPS + Offset);
        }

        /* The render thread's time per ERenderGpuTimer span (RenderStats::CpuMs),
         * measured for the timed view whether or not the GPU is: when each
         * span started, and its last length (-1: did not run). */
        bool            bTimeStagesCpu = false;
        mutable int64_t StageCpuStartNs[RGT_Count] = {};
        mutable float   StageCpuMs[RGT_Count]      = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
        int32_t         StageSpanDraws[RGT_Count]  = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

        void MarkStageCpu(const ERenderGpuTimer InTimer, const bool bInEnd) const
        {
            const int64_t Now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if(!bInEnd) { StageCpuStartNs[InTimer] = Now; return; }
            /* A later view's span adds to the frame's (SPLITSCREEN); the first sets it. */
            const float Ms = float(double(Now - StageCpuStartNs[InTimer]) / 1.0e6);
            StageCpuMs[InTimer] = TimedView > 0 && StageCpuMs[InTimer] >= 0.0f ? StageCpuMs[InTimer] + Ms : Ms;
        }

        /* InTimer's start (or end) timestamp: indices 2t and 2t + 1. */
        void StageTimestamp(const ERenderGpuTimer InTimer, const bool bInEnd) const
        {
            if(bTimeStagesCpu) { MarkStageCpu(InTimer, bInEnd); }
            if(bTimeStages)
            {
                const int32_t Index = StageTimestampIndex(InTimer, bInEnd);
                if(Index >= 0) { GDIWriteGPUTimestamp(uint32_t(Index)); }
            }
        }

        /* A timed RenderSceneStages' draws per span, from its stage counts;
         * a later view's (SPLITSCREEN) added to the frame's. */
        void CountSpanDraws()
        {
            const auto Count = [this](const ERenderStage InStage) { return int32_t(StageDrawCounts[size_t(InStage)]); };
            int32_t Spans[RGT_Count];
            for(int32_t& Each : Spans) { Each = -1; }
            Spans[RGT_PrePass]     = Count(ERenderStage::PrePass);
            Spans[RGT_BasePass]    = Count(ERenderStage::BasePass);
            Spans[RGT_Shadows]     = Count(ERenderStage::Shadows);
            Spans[RGT_Lighting]    = Count(ERenderStage::Lighting);
            Spans[RGT_Forward]     = Count(ERenderStage::Unlit) + Count(ERenderStage::TranslucentLit) + Count(ERenderStage::TranslucentUnlit);
            Spans[RGT_PostProcess] = Count(ERenderStage::PostProcess);
            int32_t Total = 0;
            for(uint32_t Timer = RGT_PrePass; Timer < RGT_Total; ++Timer)
            {
                Total += std::max(Spans[Timer], 0);
            }
            Spans[RGT_Total] = Total;
            for(uint32_t Timer = RGT_PrePass; Timer <= RGT_Total; ++Timer)
            {
                StageSpanDraws[Timer] = TimedView > 0 && StageSpanDraws[Timer] >= 0 && Spans[Timer] >= 0
                                      ? StageSpanDraws[Timer] + Spans[Timer] : Spans[Timer];
            }
        }
        bool                bGBufferThisFrame = false;   // RenderSceneStages wrote the G-buffer: Bloom may read its emissive
        bool                bVelocityThisFrame = false;  // and the velocity beside it: Motion Blur may read it
        /* InMeshPath's depth-writing sections (InWhich) as depth-only draws
         * into InStage -- Shadows for InShadowView, or the PrePass -- by the
         * same slot rules as DrawMesh. False if the mesh cannot be loaded. */
        bool QueueDepthSections(const RenderTransform& InTransform, const char* InMeshPath, const char* const* InSlots,
                                uint32_t InSlotCount, const MaterialParameterOverrides& InOverrides, EDepthSections InWhich,
                                ERenderStage InStage, int32_t InShadowView);

        bool                  bDrawMotion = false;       // SetDrawMotion's, for the draws queued now
        EngineCore::Matrix4x4 DrawMotion;

        /*
         * CUSTOM RENDER PASSES (CustomRenderPass.h). The World whose passes
         * the next scene's stages run (SetCustomPassWorld; null: none) and
         * those passes, gathered as RenderSceneStages starts; the custom pass
         * mesh groups of the draws queued now (SetDrawCustomGroups) and each
         * group's draws this frame; each registration's own targets and
         * buffers, by name, freed when it is unregistered; and the shaders
         * passes name, by file, loaded once (null: missing, warned).
         */
        const void*                  PassWorld = nullptr;
        std::vector<CustomPassEntry> FramePasses;
        uint8_t                      DrawCustomGroups = 0;
        std::vector<DrawCommand>     GroupDraws[RENDER_PASS_MESH_GROUPS];
        struct PassTarget
        {
            GDIRenderTarget*  Target = nullptr;
            ERenderPassFormat Format = ERenderPassFormat::RGBA8;
        };
        struct PassResources
        {
            std::unordered_map<std::string, PassTarget> Targets;
            std::unordered_map<std::string, GDIBuffer*> Buffers;
        };
        /* By view (SPLITSCREEN): views of different sizes would remake a shared pool's targets every frame. */
        std::unordered_map<RenderPassHandle, PassResources> PassPools[RENDER_VIEWS_MAX];
        std::unordered_map<RenderPassHandle, PassResources>& PassPoolsOfView()
        {
            return PassPools[std::min<uint32_t>(View.ViewIndex, RENDER_VIEWS_MAX - 1)];
        }
        std::unordered_map<std::string, GDIShader*>         PassShaders;

        GDIShader* GetPassShader(const std::string& InFile, const EShaderType InType, const char* InPass)
        {
            const auto Found = PassShaders.find(InFile);
            if(Found != PassShaders.end())
            {
                return Found->second;
            }
            GDIShader* Shader = InType == ST_Compute && !GDISupportsCompute()
                              ? nullptr : LoadShader(InFile, InType, std::string("custom render pass '") + InPass + "'", false);
            PassShaders.emplace(InFile, Shader);
            return Shader;
        }

        void ReleasePassResources(PassResources& InOut)
        {
            for(auto& Each : InOut.Targets)
            {
                if(Each.second.Target) { GDIReleaseRenderTarget(Each.second.Target); delete Each.second.Target; }
            }
            for(auto& Each : InOut.Buffers)
            {
                if(Each.second) { Each.second->Release(); }
            }
            InOut.Targets.clear();
            InOut.Buffers.clear();
        }

        /* The unregistered passes' targets and buffers (CustomRenderPass.cpp). */
        void ReleaseRetiredPasses()
        {
            std::vector<RenderPassHandle> Retired;
            TakeRetiredCustomRenderPasses(Retired);
            for(const RenderPassHandle Each : Retired)
            {
                for(auto& Pools : PassPools)
                {
                    const auto Found = Pools.find(Each);
                    if(Found == Pools.end()) { continue; }
                    ReleasePassResources(Found->second);
                    Pools.erase(Found);
                }
            }
        }

        /* Runs InHook's passes (RenderPassContext::Frame, below); true if any ran. */
        bool RunHook(ERenderHook InHook, SceneTargets& InTargets, bool bInMainView, GDIRenderTarget** InOutCurrent);

        /* OCCLUSION CULLING (SetOcclusionQueries): the scene's query set and
         * the boxes it tests this frame; the query the draws queued now are
         * predicated on; whether DrawMeshOccluder queues; whether this frame's
         * queries are resolved (so draws may be predicated on them). The unit
         * box the queries draw is made on first use. */
        GDIOcclusionQueries*        OcclusionQueries    = nullptr;
        std::vector<GameCore::AABB> OcclusionBoxes;
        int32_t                     DrawOcclusion       = -1;
        bool                        bDrawReveal         = false;   // SetDrawReveal
        bool                        bGatheringOccluders = false;
        bool                        bOcclusionResolved  = false;
        GDIMesh*                    OcclusionBox        = nullptr;

        void ForgetOcclusion()
        {
            OcclusionQueries    = nullptr;
            OcclusionBoxes.clear();
            DrawOcclusion       = -1;
            bGatheringOccluders = false;
            bOcclusionResolved  = false;
        }

        /* Image-based lighting (SetImageBasedLighting): the scene's atlas and its probes, for the next
         * RenderSceneStages; and whether every view draws the sky behind it (a reflection probe's captures). */
        const IBLResources* IBL = nullptr;
        float               IBLProbes[12][4] = {};
        uint32_t            IBLProbeCount = 0;
        bool                bIBLSky = false;
        bool                bForceSkyBackground = false;

        /* Lighting's lightmap InAtlas (RenderLighting::BakedAtlases), point
         * sampled and clamped: RGBE, which BakedLighting.glsl filters itself. */
        GDITextureResource* LightmapTexture(const uint32_t InAtlas)
        {
            if(InAtlas >= Lighting.BakedAtlasCount || InAtlas >= RENDER_BAKED_ATLASES_MAX) { return nullptr; }
            MaterialTexture Lightmap;
            Lightmap.Slot    = 0;
            Lightmap.Path    = std::string(Lighting.BakedAtlases[InAtlas], strnlen(Lighting.BakedAtlases[InAtlas], sizeof(Lighting.BakedAtlases[InAtlas])));
            Lightmap.bFilter = true;
            Lightmap.Filter  = ETextureFilter::Nearest;
            Lightmap.bWrap   = true;
            Lightmap.Wrap    = ETextureWrap::Clamp;
            return Lightmap.Path.empty() ? nullptr : FindTexture(Lightmap, true);
        }

        GDITextureResource* IBLAtlasTexture() const
        {
            GDITexture2D* Each = IBL && IBL->Atlas ? IBL->Atlas->ToGDITexture2D() : nullptr;
            return Each ? Each->Resource : nullptr;
        }

        /* The current lighting's sky, as the sky passes' block wants it: colours linear, and its HDRI (null: none). */
        GDITextureResource* SkyBlock(PostBlock& OutBlock)
        {
            const auto Linear = [](float (&OutColor)[4], const EngineCore::Color InColor, const float InScale)
            {
                OutColor[0] = std::pow(InColor.Rf(), 2.2f) * InScale;
                OutColor[1] = std::pow(InColor.Gf(), 2.2f) * InScale;
                OutColor[2] = std::pow(InColor.Bf(), 2.2f) * InScale;
                OutColor[3] = 1.0f;
            };
            float Sky[4], Ground[4];
            Linear(Sky, Lighting.SkyColor, Lighting.SkyIntensity);
            Linear(Ground, Lighting.GroundColor, Lighting.SkyIntensity);
            std::memcpy(&OutBlock.Matrix[0], Sky, sizeof(Sky));
            std::memcpy(&OutBlock.Matrix[4], Ground, sizeof(Ground));
            GDITextureResource* Environment = nullptr;
            if(Lighting.SkyEnvironment[0] != '\0')
            {
                MaterialTexture Hdri;
                Hdri.Path    = Lighting.SkyEnvironment;
                Hdri.bFilter = true;
                Hdri.Filter  = ETextureFilter::Nearest;
                Hdri.bWrap   = true;
                Hdri.Wrap    = ETextureWrap::Repeat;
                Environment = FindTexture(Hdri, true);
            }
            OutBlock.A[0] = Environment ? 1.0f : 0.0f;
            OutBlock.A[1] = Lighting.SkyEnvironmentIntensity;
            OutBlock.A[2] = Lighting.SkyEnvironmentRotation * 3.14159265f / 180.0f;
            return Environment;
        }

        GDIShader* GetShadowDepthShader()
        {
            if(!bShadowDepthTried)
            {
                bShadowDepthTried = true;
                ShadowDepthShader = LoadShader(SHADOW_DEPTH_SHADER, ST_Pixel, "the Shadows stage", false);
            }
            return ShadowDepthShader;
        }

        /* Loaded on first use, by the graph path (or a bare name; see
         * GetMaterialStem); failures are remembered and warned once.
         * bQuietIfMissing: no warning if it doesn't exist (mesh sections). */
        Material& FindMaterial(const std::string& InName, const bool bQuietIfMissing = false)
        {
            const auto Found = Materials.find(InName);
            if(Found != Materials.end())
            {
                return Found->second;
            }

            Material& Loaded = Materials[InName];
            Loaded.Name = InName;
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            std::string Stem;
            const bool bNamed = GetMaterialStem(InName, Stem);
            const std::string DataPath = Stem + ".material";
            const std::string Who      = "material '" + InName + "'";

            if(!bNamed)
            {
                if(!bQuietIfMissing && !InName.empty())
                {
                    RENDERCORE_LOG_WARN(Who + ": not a material graph (*" + MATERIAL_GRAPH_EXTENSION + "); drawing the plain tint")
                }
                return Loaded;
            }

            std::string Text, Error;
            if(!Provider || !Provider->ReadAllText(DataPath, Text))
            {
                if(!bQuietIfMissing)
                {
                    RENDERCORE_LOG_WARN(Who + ": cannot read " + DataPath + " (is it cooked?); drawing the plain tint")
                }
                return Loaded;
            }
            if(!ParseMaterialDescription(Text, Loaded.Description, Error))
            {
                RENDERCORE_LOG_WARN(Who + ": " + DataPath + ": " + Error + "; drawing the plain tint")
                return Loaded;
            }

            const MaterialDescription& Desc = Loaded.Description;
            const uint32_t PixelKey = GetPixelPermutationKey(Desc.Domain, Desc.Shading,
                                                             Desc.Shading == EMaterialShading::Lit ? Desc.ShadingModel
                                                                                                   : EMaterialShadingModel::Default);
            GDIShader* Pixel = LoadShader(GetPermutationFileName(Stem, PixelKey, false), ST_Pixel, Who, false);
            if(!Pixel)
            {
                return Loaded;   // warned; drawn as the fallback
            }

            if(Desc.bHasOffset && Desc.Domain != EMaterialDomain::PostProcess)
            {
                for(const EMaterialVertexInput Input : { EMaterialVertexInput::Quad, EMaterialVertexInput::Mesh, EMaterialVertexInput::InstancedMesh,
                                                         EMaterialVertexInput::SkinnedMesh })
                {
                    GDIShader* Vertex = LoadShader(GetPermutationFileName(Stem, GetVertexPermutationKey(Input), true), ST_Vertex, Who, false);
                    if(!Vertex)
                    {
                        RetiredShaders.push_back(Pixel);
                        for(GDIShader*& Each : Loaded.Vertex) { if(Each) { RetiredShaders.push_back(Each); Each = nullptr; } }
                        return Loaded;
                    }
                    Loaded.Vertex[int(Input)] = Vertex;
                }
            }

            BuildMaterialBlock(Desc, EngineCore::Color(255, 255, 255, 255), Loaded.Block);
            Loaded.Pixel = Pixel;
            RENDERCORE_LOG(Who + " loaded (" + GetMaterialDomainName(Desc.Domain)
                           + (Desc.Shading == EMaterialShading::Lit ? ", Lit)" : ", Unlit)"))
            return Loaded;
        }

        /* The vertex shader InMaterial draws with for InInput: its own with an
         * offset, else the shared one. */
        GDIShader* VertexFor(const Material& InMaterial, const EMaterialVertexInput InInput)
        {
            if(InMaterial.Description.bHasOffset && InInput != EMaterialVertexInput::Fullscreen)
            {
                return InMaterial.Vertex[int(InInput)];
            }
            return GetDefaultVertex(InInput);
        }

        /* Draw state per domain. Plain quads keep the scene's old state:
         * blended, depth written. */
        static void ApplyDomainState(const EMaterialDomain InDomain)
        {
            IDepthStateDesc Depth;
            Depth.bDepthTest = InDomain != EMaterialDomain::PostProcess;
            Depth.bDepthMask = InDomain == EMaterialDomain::Opaque || InDomain == EMaterialDomain::Masked;
            Depth.DepthMode  = CTM_LEQUAL;
            GDISetDepthState(Depth);

            if(InDomain == EMaterialDomain::Translucent)
            {
                GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
            }
            else
            {
                GDISetBlendState(IBlendStateDesc());
            }
        }

        static void ApplyPlainState()
        {
            IDepthStateDesc Depth;
            Depth.bDepthTest = true;
            Depth.bDepthMask = true;
            Depth.DepthMode  = CTM_LEQUAL;
            GDISetDepthState(Depth);
            GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
        }

        /* How many floats a parameter of InType holds; 0 for an unknown type. */
        static uint32_t ParameterFloats(const std::string& InType)
        {
            if(InType == "float") { return 1; }
            if(InType == "vec2")  { return 2; }
            if(InType == "vec3")  { return 3; }
            if(InType == "vec4")  { return 4; }
            return 0;
        }

        /* InMaterial's block with InItem's parameter overrides for its slot
         * applied: a copy, so the material's own block keeps its defaults for
         * every other entity drawing with it. The block itself without any. */
        const std::vector<uint8_t>& BlockWithOverrides(Material& InMaterial, const DrawCommand* InItem)
        {
            return BlockWithOverrides(InMaterial, InMaterial.Block, InItem);
        }
        const std::vector<uint8_t>& BlockWithOverrides(Material& InMaterial, const std::vector<uint8_t>& InBase, const DrawCommand* InItem)
        {
            const std::vector<uint8_t>& Block = InBase;
            std::vector<uint8_t>& OverrideBlock = RefOverrideBlock();
            if(!InItem || InItem->OverrideCount == 0)
            {
                return Block;
            }
            bool bAny = false;
            for(uint32_t Index = 0; Index < InItem->OverrideCount; ++Index)
            {
                const RenderMaterialParameter& Each = InItem->Overrides[Index];
                if(Each.Slot != InItem->MaterialSlot || Each.Name[0] == '\0')
                {
                    continue;
                }
                const MaterialParameter* Param = InMaterial.Description.FindParameter(
                    std::string(Each.Name, strnlen(Each.Name, RENDER_MATERIAL_PARAMETER_NAME_MAX)));
                const uint32_t Floats = Param ? ParameterFloats(Param->Type) : 0;
                if(Floats == 0 || Param->Offset + Floats * sizeof(float) > Block.size())
                {
                    continue;   // not one of this material's parameters
                }
                if(!bAny)
                {
                    OverrideBlock = Block;
                    bAny = true;
                }
                std::memcpy(OverrideBlock.data() + Param->Offset, Each.Value, Floats * sizeof(float));
            }
            return bAny ? OverrideBlock : Block;
        }

        /* Binds InMaterial's shaders for InInput and its blocks with InTint,
         * and InItem's parameter overrides for its slot. False (nothing
         * bound) if a shader is missing. */
        /* InMaterial's textures, every other slot cleared; slot 0 (a PostProcess
         * pass's scene) is the caller's. A recording chunk binds those resolved
         * before it ran (ResolveForParallel). */
        void BindMaterialTextures(Material& InMaterial)
        {
            for(uint32_t Slot = MATERIAL_TEXTURE_SLOT_FIRST; Slot < MATERIAL_TEXTURE_SLOTS; ++Slot)
            {
                GDISetMaterialTexture(Slot, nullptr);
            }
            for(size_t Index = 0; Index < InMaterial.Description.Textures.size(); ++Index)
            {
                const MaterialTexture& Each = InMaterial.Description.Textures[Index];
                GDISetMaterialTexture(Each.Slot, !GIssueScratch ? FindTexture(Each)
                                                 : (Index < InMaterial.Resolved.size() ? InMaterial.Resolved[Index] : nullptr));
            }
        }

        bool BindMaterial(Material& InMaterial, const EMaterialVertexInput InInput, const EngineCore::Color InTint,
                          const DrawCommand* InItem = nullptr)
        {
            GDIShader* Vertex = VertexFor(InMaterial, InInput);
            if(!Vertex || !InMaterial.Pixel)
            {
                return false;
            }

            Bind(Vertex, InMaterial.Pixel);

            /* The tint and baked lighting are written per draw: into the
             * material's own block on the render thread, as always; into a
             * recording chunk's copy, as other chunks draw with it too. */
            std::vector<uint8_t>& Block = GIssueScratch ? (GIssueScratch->MaterialBlock = InMaterial.Block) : InMaterial.Block;
            const float Tint[4] = { InTint.Rf(), InTint.Gf(), InTint.Bf(), InTint.Af() };
            std::memcpy(Block.data(), Tint, sizeof(Tint));
            /* Material_Reserved0: whether this draw is dithered where it hides a player
             * (MaterialTemplate_PS.glsl); written every draw, so none lingers. */
            if(Block.size() >= MATERIAL_BLOCK_LIGHTMAP_OFFSET)
            {
                const float Reveal = InItem && InItem->bReveal ? 1.0f : 0.0f;
                std::memcpy(Block.data() + MATERIAL_BLOCK_REVEAL_OFFSET, &Reveal, sizeof(Reveal));
            }

            /* The draw's baked lighting, in the engine's bytes (MaterialDescription.h):
             * written every draw, as the tint is, so the last draw's never lingers. */
            const RenderBakedDraw* Baked = InItem && InItem->Baked && Lighting.bBaked ? InItem->Baked : nullptr;
            const bool bLightmap = Baked && Baked->Mode == uint8_t(EMaterialBakedLighting::Lightmap)
                                && Baked->Atlas < Lighting.BakedAtlasCount;
            const bool bCube     = Baked && Baked->Mode == uint8_t(EMaterialBakedLighting::AmbientCube);
            if(Block.size() >= MATERIAL_ENGINE_BLOCK_BYTES)
            {
                float Engine[(MATERIAL_ENGINE_BLOCK_BYTES - MATERIAL_BLOCK_LIGHTMAP_OFFSET) / sizeof(float)] = {};
                if(bLightmap) { std::memcpy(Engine, Baked->ScaleOffset, sizeof(Baked->ScaleOffset)); }
                Engine[4] = bLightmap ? float(EMaterialBakedLighting::Lightmap)
                          : (bCube ? float(EMaterialBakedLighting::AmbientCube) : 0.0f);
                if(bCube)
                {
                    for(int Face = 0; Face < 6; ++Face) { std::memcpy(&Engine[8 + Face * 4], Baked->Cube[Face], sizeof(Baked->Cube[Face])); }
                }
                std::memcpy(Block.data() + MATERIAL_BLOCK_LIGHTMAP_OFFSET, Engine, sizeof(Engine));
            }

            const std::vector<uint8_t>& Upload = BlockWithOverrides(InMaterial, Block, InItem);
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Upload.data(), uint(Upload.size()));
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));

            BindMaterialTextures(InMaterial);
            /* Slot 0, its own outside PostProcess (MaterialTemplate_PS.glsl): a Base
             * Pass material's lightmap, a forward Lit material's probe atlas. */
            if(InMaterial.Description.Domain != EMaterialDomain::PostProcess && InMaterial.Description.Shading == EMaterialShading::Lit)
            {
                const bool bBasePass = InMaterial.Description.Domain == EMaterialDomain::Opaque
                                    || InMaterial.Description.Domain == EMaterialDomain::Masked;
                if(bBasePass)
                {
                    GDISetMaterialTexture(0, !bLightmap ? nullptr
                                             : (GIssueScratch ? LightmapResolved[Baked->Atlas] : LightmapTexture(Baked->Atlas)));
                }
                else
                {
                    GDISetMaterialTexture(0, Scene.IBL[0] > 0.5f ? IBLAtlasTexture() : nullptr);
                }
            }

            ApplyDomainState(InMaterial.Description.Domain);
            return true;
        }

        // === The scene block ====================================================

        void UpdateScene(const EngineCore::Vector3* InCameraOverride = nullptr)
        {
            /* The eye. An orthographic view has none: a point far behind the
             * view's centre gives every pixel the same view direction. */
            EngineCore::Vector3 Camera = View.Origin;
            if(InCameraOverride)
            {
                Camera = *InCameraOverride;
            }
            else if(IsOrthographic(View))
            {
                const EngineCore::Vector3 Centre = View.Origin + EngineCore::Vector3(View.ViewportPixels.Width * 0.5,
                                                                                     View.ViewportPixels.Height * 0.5, 0.0);
                Camera = Centre - ViewForward(View) * 100000.0;
            }
            Scene.CameraPosition[0] = float(Camera.GetX());
            Scene.CameraPosition[1] = float(Camera.GetY());
            Scene.CameraPosition[2] = float(Camera.GetZ());

            Scene.Time[0] = std::chrono::duration<float>(LastFrame - Start).count();
            Scene.Time[1] = FrameDelta;

            /* Toward the light: the way it travels, reversed. */
            EngineCore::Vector3 ToLight = Lighting.LightDirection * -1.0;
            const double Length = ToLight.GetLength();
            ToLight = Length > 1e-9 ? ToLight * (1.0 / Length) : EngineCore::Vector3(0.0, -1.0, 0.0);
            Scene.LightDirection[0] = float(ToLight.GetX());
            Scene.LightDirection[1] = float(ToLight.GetY());
            Scene.LightDirection[2] = float(ToLight.GetZ());
            Scene.LightDirection[3] = Lighting.LightIntensity;

            PutColor(Scene.LightColor,  Lighting.LightColor, 1.0f);
            PutColor(Scene.SkyColor,    Lighting.SkyColor, Lighting.SkyIntensity);
            PutColor(Scene.GroundColor, Lighting.GroundColor, 1.0f);
            Scene.Baked[0] = Lighting.bBaked ? 1.0f : 0.0f;
            Scene.Baked[1] = Lighting.bBaked && Lighting.bBakedSky ? 1.0f : 0.0f;
            Scene.Baked[2] = Lighting.bBaked && Lighting.bBakedDirectional ? 1.0f : 0.0f;

            /* Clip back to world, for the Lighting stage: the inverse of
             * Projection * View, column-major as GDI uploads a mat4. */
            PutMatrix(Scene.InvViewProjection, EngineCore::Matrix4x4::Inverse(View.Projection * View.View));
            PutMatrix(Scene.ViewProjection, View.Projection * View.View);

            /* OCCLUSION REVEAL: each point on screen and how deep it is; a perspective view's alone. */
            std::memset(Scene.RevealParams, 0, sizeof(Scene.RevealParams));
            std::memset(Scene.Reveal, 0, sizeof(Scene.Reveal));
            if(!IsOrthographic(View) && View.RevealRadius > 0.0f && View.ViewportPixels.Height > 0)
            {
                uint32_t Count = 0;
                for(uint32_t Index = 0; Index < std::min<uint32_t>(View.RevealCount, RENDER_REVEAL_MAX); ++Index)
                {
                    const EngineCore::Vector3& Point = View.RevealPoints[Index];
                    const EngineCore::Vector4 Seen = View.View * EngineCore::Vector4(Point.GetX(), Point.GetY(), Point.GetZ(), 1.0);
                    const double Depth = Seen.GetZ();
                    if(Depth <= double(std::max(View.ZNear, 1.0f))) { continue; }
                    const EngineCore::Vector4 Clip = View.Projection * Seen;
                    if(std::abs(Clip.GetW()) < 1e-9) { continue; }
                    Scene.Reveal[Count][0] = float(Clip.GetX() / Clip.GetW());
                    Scene.Reveal[Count][1] = float(Clip.GetY() / Clip.GetW());
                    Scene.Reveal[Count][2] = float(double(View.RevealRadius) * std::abs(double(View.Projection.Data[1][1])) / Depth);
                    Scene.Reveal[Count][3] = float(Depth);
                    ++Count;
                }
                Scene.RevealParams[0] = float(Count);
                Scene.RevealParams[1] = float(View.ViewportPixels.Width) / float(View.ViewportPixels.Height);
                Scene.RevealParams[2] = 0.25f;                  // of the pixels kept in the middle: a screen door
                Scene.RevealParams[3] = View.RevealRadius;      // and only what is this much nearer than the player
            }

            /* View depth, for picking a cascade: from the view's own origin. */
            const EngineCore::Vector3 Forward = ViewForward(View);
            Scene.CameraForward[0] = float(Forward.GetX());
            Scene.CameraForward[1] = float(Forward.GetY());
            Scene.CameraForward[2] = float(Forward.GetZ());
            Scene.CameraForward[3] = float(View.Origin.Dot(Forward));

            /* Point and spot lights. */
            const uint32_t LocalCount = std::min<uint32_t>(Lighting.LocalCount, RENDER_LOCAL_LIGHT_MAX);
            Scene.LocalCounts[0] = float(LocalCount);
            for(uint32_t Index = 0; Index < LocalCount; ++Index)
            {
                const RenderLocalLight& Light = Lighting.Locals[Index];
                float (*Out)[4] = &Scene.Locals[Index * SCENE_LOCAL_VEC4S];
                const double Length = Light.Direction.GetLength();
                const EngineCore::Vector3 Direction = Length > 1e-9 ? Light.Direction * (1.0 / Length) : EngineCore::Vector3(0.0, 0.0, 1.0);
                const double Degrees = 3.14159265358979 / 180.0;

                Out[0][0] = float(Light.Position.GetX()); Out[0][1] = float(Light.Position.GetY());
                Out[0][2] = float(Light.Position.GetZ()); Out[0][3] = Light.Range;
                PutColor(Out[1], Light.Color, Light.Intensity);
                Out[2][0] = float(Direction.GetX()); Out[2][1] = float(Direction.GetY());
                Out[2][2] = float(Direction.GetZ()); Out[2][3] = Light.Type == RLL_Spot ? 1.0f : 0.0f;
                Out[3][0] = float(std::cos(double(Light.OuterConeAngle) * Degrees));
                Out[3][1] = float(std::cos(double(std::min(Light.InnerConeAngle, Light.OuterConeAngle - 0.01f)) * Degrees));
                Out[3][2] = -1.0f;                                  // PrepareShadows sets a shadowed one's
                Out[3][3] = float(Light.ShadowTechnique);
                PutColor(Out[4], Light.ShadowTint, 1.0f);
                Out[5][0] = Light.ShadowBias; Out[5][1] = Light.ShadowNormalBias; Out[5][2] = 0.0f; Out[5][3] = 0.0f;
            }

            /* No shadow until PrepareShadows draws some -- contact shadows included. */
            Scene.ShadowParams[0] = 0.0f;
            Scene.ContactShadow[0] = 0.0f;
            Scene.ContactLocal[0]  = 0.0f;
            Scene.LightShadow[0]  = -1.0f;
            Scene.LightShadow[1]  = 0.0f;
        }

        // === Shadows ============================================================

        /* Both atlases at InSize, created or resized; false if this backend
         * cannot draw into or sample one. */
        bool EnsureShadowAtlases(const uint32_t InSize)
        {
            for(uint32_t Atlas = 0; Atlas < SA_Count; ++Atlas)
            {
                if(!ShadowAtlas[Atlas])
                {
                    ShadowAtlas[Atlas] = GDICreateRenderTarget(Atlas == SA_Directional ? "RenderCore.ShadowDirectional" : "RenderCore.ShadowLocal",
                                                               InSize, InSize);
                }
                else if(ShadowAtlasSize != InSize)
                {
                    ShadowAtlas[Atlas]->Resize(InSize, InSize);
                }
                if(!ShadowAtlas[Atlas] || !ShadowAtlas[Atlas]->IsValid() || !ShadowAtlas[Atlas]->ToDepthTexture2D())
                {
                    return false;
                }
            }
            ShadowAtlasSize = InSize;
            return true;
        }

        void ReleaseShadowAtlases()
        {
            for(GDIRenderTarget*& Atlas : ShadowAtlas)
            {
                if(Atlas)
                {
                    GDIReleaseRenderTarget(Atlas);
                    delete Atlas;
                    Atlas = nullptr;
                }
            }
            ShadowAtlasSize = 0;
        }

        /* The frame's shadow views from View, Lighting and InCasters, into the scene block. */
        void PrepareShadows(const InsetShadowCaster* InCasters, const uint32_t InCasterCount, const GameCore::AABB* InReceivers)
        {
            BuildShadowFrame(Lighting, View, Shadows, InCasters, InCasterCount, InReceivers);
            if(Shadows.ViewCount == 0)
            {
                return;
            }
            if(Backend == ERenderBackend::Null || !EnsureShadowAtlases(Shadows.AtlasSize))
            {
                if(Backend != ERenderBackend::Null)
                {
                    WarnOnce("|shadow-atlas", "Shadows: this backend cannot draw or sample a shadow map; lights draw unshadowed");
                }
                Shadows.ViewCount = 0;
                return;
            }

            const float Size = float(Shadows.AtlasSize);
            Scene.ShadowParams[0] = 1.0f;
            Scene.ShadowParams[1] = float(std::clamp(Lighting.Shadows.Filter, 0, 2));
            Scene.ShadowParams[2] = 1.0f / Size;
            Scene.ShadowParams[3] = Size;

            /* The sun's contact shadows ([Shadows] ContactShadows): with shadows drawn at all, so
             * Enabled turns them off too. The Lighting stage marches only when the sun casts. */
            const RenderShadowSettings& Settings = Lighting.Shadows;
            const bool bContact = Settings.bContactShadows && Settings.ContactLength > 0.0f && Settings.ContactStrength > 0.0f;
            Scene.ContactShadow[0] = bContact ? Settings.ContactLength : 0.0f;
            Scene.ContactShadow[1] = Settings.ContactThickness;
            Scene.ContactShadow[2] = float(Settings.ContactSteps);
            Scene.ContactShadow[3] = Settings.ContactStrength;
            Scene.ContactLocal[0]  = bContact && Settings.bContactLocalLights ? 1.0f : 0.0f;

            uint32_t DirectionalViews = 0;
            if(Shadows.DirectionalFirstView >= 0)
            {
                DirectionalViews = Shadows.CascadeCount;
                Scene.LightShadow[0] = float(Lighting.LightShadowTechnique);
                Scene.LightShadow[1] = float(Shadows.CascadeCount);
                Scene.LightShadow[2] = Lighting.LightShadowBias;
                Scene.LightShadow[3] = Lighting.LightShadowNormalBias;
                PutColor(Scene.LightShadowTint, Lighting.LightShadowTint, 1.0f);
                for(uint32_t Index = 0; Index < RENDER_CASCADE_MAX; ++Index)
                {
                    Scene.CascadeSplits[Index] = Index < Shadows.CascadeCount ? Shadows.CascadeSplits[Index] : 0.0f;
                    if(Index < Shadows.CascadeCount)
                    {
                        const ShadowView& Cascade = Shadows.Views[uint32_t(Shadows.DirectionalFirstView) + Index];
                        PutMatrix(Scene.CascadeMatrices[Index], Cascade.WorldToAtlas);
                        /* An orthographic view: 2 / Projection[0][0] wide, over TileSize texels. */
                        Scene.CascadeTexels[Index] = float(2.0 / std::abs(double(Cascade.Projection.Data[0][0])) / double(std::max(Cascade.TileSize, 1u)));
                    }
                }
            }

            /* Local views follow the sun's, in Locals order; then the inset shadows'. */
            for(uint32_t View = DirectionalViews; View < Shadows.ViewCount; ++View)
            {
                PutMatrix(Scene.LocalShadowMatrices[View - DirectionalViews], Shadows.Views[View].WorldToAtlas);
            }
            for(uint32_t Index = 0; Index < std::min<uint32_t>(Lighting.LocalCount, RENDER_LOCAL_LIGHT_MAX); ++Index)
            {
                const int32_t First = Shadows.LocalFirstView[Index];
                Scene.Locals[Index * SCENE_LOCAL_VEC4S + 3][2] = First >= 0 ? float(uint32_t(First) - DirectionalViews) : -1.0f;
            }
        }

        /* A loaded material's own, else the section's, else DefaultLit: the
         * one a mesh section draws with (null: the plain fallback). */
        Material* SectionMaterial(const char* InSlot, const std::string& InSectionMaterial)
        {
            const auto Usable = [](Material& InFound) -> Material*
            {
                return InFound.Pixel && InFound.Description.Domain != EMaterialDomain::PostProcess ? &InFound : nullptr;
            };
            Material* Chosen = nullptr;
            if(InSlot)
            {
                Chosen = Usable(FindMaterial(InSlot));
            }
            if(!Chosen && !InSectionMaterial.empty())
            {
                Chosen = Usable(FindMaterial(InSectionMaterial, /*bQuietIfMissing*/ true));
            }
            if(!Chosen)
            {
                Chosen = Usable(FindMaterial(DEFAULT_LIT_MATERIAL, /*bQuietIfMissing*/ true));
            }
            return Chosen;
        }

        /* The vertex input InItem's mesh draws with: a skinned mesh's, with
         * its bones set in GDI's skin block (its bind pose without a palette),
         * else the plain mesh's. */
        EMaterialVertexInput PrepareMeshInput(const DrawCommand& InItem)
        {
            if(!InItem.Mesh || !InItem.Mesh->IsSkinned())
            {
                return EMaterialVertexInput::Mesh;
            }
            if(InItem.SkinPalette && InItem.SkinBones > 0)
            {
                const uint32_t Bones = std::min<uint32_t>(InItem.SkinBones, GDI_SKIN_MAX_BONES);
                GDISetUniformBlock(GDI_SKIN_BLOCK_BINDING, InItem.SkinPalette, uint(Bones * 12 * sizeof(float)));
                GDISetUniformBlock(GDI_PREV_SKIN_BLOCK_BINDING, InItem.PrevSkinPalette ? InItem.PrevSkinPalette : InItem.SkinPalette,
                                   uint(Bones * 12 * sizeof(float)));
            }
            else
            {
                static const std::vector<float> Identity = []()
                {
                    std::vector<float> Rows(size_t(GDI_SKIN_MAX_BONES) * 12, 0.0f);
                    for(size_t Bone = 0; Bone < GDI_SKIN_MAX_BONES; ++Bone)
                    {
                        Rows[Bone * 12 + 0] = Rows[Bone * 12 + 5] = Rows[Bone * 12 + 10] = 1.0f;
                    }
                    return Rows;
                }();
                GDISetUniformBlock(GDI_SKIN_BLOCK_BINDING, Identity.data(), uint(Identity.size() * sizeof(float)));
                GDISetUniformBlock(GDI_PREV_SKIN_BLOCK_BINDING, Identity.data(), uint(Identity.size() * sizeof(float)));
            }
            return EMaterialVertexInput::SkinnedMesh;
        }

        /* One caster section into the bound atlas tile, with its material's
         * vertex shader (World Position Offset) and ShadowDepth.fsh: depth
         * only. The PrePass draws its occluders, and its query boxes
         * (!bInDepthWrite: tested, never written), the same way; a custom
         * pass's mesh group too, with its own pixel shader (InPixel) and
         * perhaps no depth test. */
        void IssueShadowCaster(const DrawCommand& InItem, const bool bInDepthWrite = true, GDIShader* InPixel = nullptr,
                               const bool bInDepthTest = true)
        {
            EnsureInitialised();
            GDIShader* Pixel  = InPixel ? InPixel : GetShadowDepthShader();
            const EMaterialVertexInput Input = InItem.Instances ? EMaterialVertexInput::InstancedMesh : PrepareMeshInput(InItem);
            GDIShader* Vertex = InItem.Mat ? VertexFor(*InItem.Mat, Input) : GetDefaultVertex(Input);
            if(!Pixel || !Vertex)
            {
                return;   // warned by LoadShader
            }

            GDIViewObject* const MeshObject = RefMeshObject();
            Place(MeshObject, InItem.Transform, 1.0f, 1.0f);
            MeshObject->Mesh = InItem.Mesh;
            Bind(Vertex, Pixel);

            static const uint8_t NoMaterial[MATERIAL_ENGINE_BLOCK_BYTES] = {};
            if(InItem.Mat && !InItem.Mat->Block.empty())
            {
                /* The entity's overrides too: a parameter may drive its World Position Offset. */
                const std::vector<uint8_t>& Block = BlockWithOverrides(*InItem.Mat, &InItem);
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Block.data(), uint(Block.size()));
                /* And its textures, which that offset may sample (a custom pass's trail). */
                if(InItem.Mat->Description.bHasOffset && !InItem.Mat->Description.Textures.empty())
                {
                    BindMaterialTextures(*InItem.Mat);
                }
            }
            else
            {
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, NoMaterial, uint(sizeof(NoMaterial)));
            }
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));

            IDepthStateDesc Depth;
            Depth.bDepthTest = bInDepthTest;
            Depth.bDepthMask = bInDepthTest && bInDepthWrite;
            Depth.DepthMode  = CTM_LEQUAL;
            GDISetDepthState(Depth);
            GDISetBlendState(IBlendStateDesc());

            GDISetDrawRange(InItem.Mesh, InItem.First, InItem.Count);
            if(InItem.Instances) { GDISetInstanceBuffer(InItem.Instances, InItem.InstanceCount); }
            GDIDrawObject(MeshObject);
            if(InItem.Instances) { GDISetInstanceBuffer(nullptr, 0); }
            GDISetDrawRange(InItem.Mesh, 0, 0);
            MeshObject->Mesh = nullptr;
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        /* The unit box [-0.5, 0.5]^3 the occlusion queries draw, scaled to each
         * proxy's world box; null where the backend has no indexed meshes. */
        GDIMesh* GetOcclusionBox()
        {
            if(!OcclusionBox)
            {
                static const float Positions[8 * 3] = { -0.5f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,   0.5f, 0.5f, -0.5f,   -0.5f, 0.5f, -0.5f,
                                                        -0.5f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,   0.5f, 0.5f,  0.5f,   -0.5f, 0.5f,  0.5f };
                static const float    Normals[8 * 3] = {};   // nothing shades it
                static const float    UVs[8 * 2]     = {};
                static const uint32_t Indices[36]    = { 0, 2, 1,  0, 3, 2,   4, 5, 6,  4, 6, 7,   0, 1, 5,  0, 5, 4,
                                                         3, 6, 2,  3, 7, 6,   0, 4, 7,  0, 7, 3,   1, 2, 6,  1, 6, 5 };
                GDIMeshDesc Desc;
                Desc.Positions   = Positions;
                Desc.Normals     = Normals;
                Desc.UV0         = UVs;
                Desc.NumVertices = 8;
                Desc.Indices     = Indices;
                Desc.NumIndices  = 36;
                OcclusionBox = GDICreateMesh(Desc);
                GDISetDebugName(OcclusionBox, "RenderCore.OcclusionBox");
            }
            return OcclusionBox;
        }

        /*
         * The PrePass (RENDERPIPELINE.md, "Occlusion culling"), into the bound
         * scene depth, colour masked off: the occluders' depth, then a query
         * per box -- the box drawn against that depth, writing nothing -- then
         * the answers resolved for the stages after it to be predicated on.
         * Nothing without a query to run: the occluders would be for nothing.
         */
        void IssuePrePass()
        {
            const DrawPolicy& Policy = PolicyFor(ERenderStage::PrePass);
            const uint32_t    Count  = OcclusionQueries ? uint32_t(std::min<size_t>(OcclusionBoxes.size(), OcclusionQueries->GetCapacity())) : 0;
            StageDrawCounts[size_t(ERenderStage::PrePass)] = Count > 0 ? uint32_t(Policy.Num()) + Count : 0;
            GDIMesh* Box = Count > 0 ? GetOcclusionBox() : nullptr;
            if(!Box)
            {
                return;
            }

            SCOPED_DRAW_EVENT(GetRenderStageName(ERenderStage::PrePass));
            GDIBeginOcclusionRound(OcclusionQueries, Count);
            GDISetColorMask(EColM_None);
            if(!Policy.IsEmpty())
            {
                SCOPED_DRAW_EVENT("Occluders");
                const std::vector<uint32_t> Order = Policy.GetIssueOrder();
                IssueByMobility(Policy, Order.data(), Order.data() + Order.size(), [this](const DrawCommand& InItem)
                {
                    if(InItem.Mesh) { IssueShadowCaster(InItem); }
                });
            }
            {
                SCOPED_DRAW_EVENT("OcclusionQueries");
                DrawCommand Query;
                Query.Kind = DrawCommand::EKind::MeshSection;
                Query.Mesh = Box;
                for(uint32_t Index = 0; Index < Count; ++Index)
                {
                    const GameCore::AABB& Bounds = OcclusionBoxes[Index];
                    Query.Transform.Position = (Bounds.Min + Bounds.Max) * 0.5;
                    Query.Transform.Scale    = Bounds.Max - Bounds.Min;
                    GDIBeginOcclusionQuery(OcclusionQueries, Index);
                    IssueShadowCaster(Query, /*bInDepthWrite*/ false);
                    GDIEndOcclusionQuery(OcclusionQueries, Index);
                }
            }
            GDIResolveOcclusionQueries(OcclusionQueries);
            GDISetColorMask(EColM_RGBA);
            bOcclusionResolved = true;
        }

        /* The Shadows stage: clears each atlas in use, then each caster into
         * its view's tile, seen from the light. */
        void IssueShadows()
        {
            const DrawPolicy& Policy = PolicyFor(ERenderStage::Shadows);
            StageDrawCounts[size_t(ERenderStage::Shadows)] = uint32_t(Policy.Num());
            if(Shadows.ViewCount == 0)
            {
                return;
            }

            SCOPED_DRAW_EVENT(GetRenderStageName(ERenderStage::Shadows));
            const uint Size = uint(Shadows.AtlasSize);
            {
                SCOPED_DRAW_EVENT("ClearAtlases");
                for(uint32_t Atlas = 0; Atlas < SA_Count; ++Atlas)
                {
                    if(!Shadows.UsesAtlas(EShadowAtlas(Atlas)))
                    {
                        continue;
                    }
                    GDIRenderTarget* Target = ShadowAtlas[Atlas];
                    GDISetRenderTargets(&Target, 1, Target);
                    GDISetViewport(0, 0, Size, Size);
                    GDISetColorMask(EColM_RGBA);
                    GDIClear(CM_Color | CM_Depth, EngineCore::Color(0, 0, 0, 0), 1.0f);
                }
            }

            /* Queued view by view, the sun's first: each atlas binds once, and
             * each run of one view's casters is a draw event of its own. An
             * atlas's casters are recorded in parallel chunks where they can
             * be (IssueInParallel), each chunk setting a view's tile and
             * matrices as its casters reach it; else serially, view by view. */
            const std::vector<uint32_t> Order = Policy.GetIssueOrder();
            const auto ViewOf = [&](const size_t InAt) { return Policy[Order[InAt]].ShadowView; };
            const auto Valid  = [&](const int32_t InView) { return InView >= 0 && uint32_t(InView) < Shadows.ViewCount; };
            for(size_t AtlasStart = 0; AtlasStart < Order.size();)
            {
                /* This atlas's run of views: [AtlasStart, AtlasEnd). */
                size_t AtlasEnd = AtlasStart;
                int32_t Atlas = -1;
                while(AtlasEnd < Order.size())
                {
                    const int32_t ViewIndex = ViewOf(AtlasEnd);
                    const int32_t Each = Valid(ViewIndex) ? int32_t(Shadows.Views[ViewIndex].Atlas) : -1;
                    if(Atlas >= 0 && Each >= 0 && Each != Atlas) { break; }
                    if(Each >= 0) { Atlas = Each; }
                    ++AtlasEnd;
                }
                if(Atlas >= 0)
                {
                    GDIRenderTarget* Target = ShadowAtlas[Atlas];
                    GDISetRenderTargets(&Target, 1, Target);
                    GDISetColorMask(EColM_None);
                }

                std::vector<uint32_t> Flat;   // each view's casters, static then dynamic, the views in order
                for(size_t Start = AtlasStart; Start < AtlasEnd;)
                {
                    size_t End = Start + 1;
                    while(End < AtlasEnd && ViewOf(End) == ViewOf(Start)) { ++End; }
                    if(Valid(ViewOf(Start)))
                    {
                        for(const uint32_t Index : MobilityOrder(Policy, Order.data() + Start, Order.data() + End))
                        {
                            if(Policy[Index].Mesh) { Flat.push_back(Index); }
                        }
                    }
                    Start = End;
                }
                const bool bParallel = Atlas >= 0 && IssueInParallel(Policy, Flat, true, [this](const DrawCommand& InItem)
                {
                    if(GIssueScratch->ShadowView != InItem.ShadowView)
                    {
                        const ShadowView& View = Shadows.Views[InItem.ShadowView];
                        GDISetViewport(View.TileX, View.TileY, View.TileSize, View.TileSize);
                        GIssueScratch->PlaceProjection = &View.Projection;
                        GIssueScratch->PlaceView       = &View.View;
                        GIssueScratch->ShadowView      = InItem.ShadowView;
                    }
                    IssueShadowCaster(InItem);
                });

                for(size_t Start = AtlasStart; !bParallel && Start < AtlasEnd;)
                {
                    const int32_t ViewIndex = ViewOf(Start);
                    size_t End = Start + 1;
                    while(End < AtlasEnd && ViewOf(End) == ViewIndex) { ++End; }
                    if(Valid(ViewIndex))
                    {
                        const ShadowView& View = Shadows.Views[ViewIndex];
                        GDISetViewport(View.TileX, View.TileY, View.TileSize, View.TileSize);
                        PlaceProjection = &View.Projection;
                        PlaceView       = &View.View;

                        SCOPED_DRAW_EVENT(GetShadowViewName(ViewIndex));
                        IssueByMobility(Policy, Order.data() + Start, Order.data() + End, [this](const DrawCommand& InItem)
                        {
                            if(InItem.Mesh) { IssueShadowCaster(InItem); }
                        });
                    }
                    Start = End;
                }
                AtlasStart = AtlasEnd;
            }
            PlaceProjection = nullptr;
            PlaceView       = nullptr;
        }

        /* Lighting_Block's inset shadows, after its first vec4's x: y how
         * many, z and w the depth and normal bias; then the light (xyz toward
         * it, w 1 if Modulated over the whole pixel -- a baked sun's), its
         * tint and in w the insets' PCF radius ([Shadows] InsetFilter); per inset its matrix in Scene_LocalShadowMatrices, its texel
         * in world units and its tile's corner in atlas UV; then per inset the
         * pixels of an InWidth x InHeight target it can shadow (gl_FragCoord,
         * top-down: x, y min then max), every other pixel skipped. */
        void PutInsetShadows(float* OutBlock, const uint32_t InWidth, const uint32_t InHeight) const
        {
            const uint32_t Count = Shadows.ViewCount > 0 ? std::min<uint32_t>(Shadows.InsetCount, RENDER_INSET_SHADOW_MAX) : 0;
            OutBlock[1] = float(Count);
            if(Count == 0)
            {
                return;
            }
            OutBlock[2] = Lighting.InsetShadowBias;
            OutBlock[3] = Lighting.InsetShadowNormalBias;

            EngineCore::Vector3 ToLight = Lighting.InsetLightDirection * -1.0;
            const double Length = ToLight.GetLength();
            ToLight = Length > 1e-9 ? ToLight * (1.0 / Length) : EngineCore::Vector3(0.0, 1.0, 0.0);
            OutBlock[4] = float(ToLight.GetX());
            OutBlock[5] = float(ToLight.GetY());
            OutBlock[6] = float(ToLight.GetZ());
            OutBlock[7] = Lighting.bInsetLightBaked ? 1.0f : 0.0f;
            float Tint[4] = {};
            PutColor(Tint, Lighting.InsetShadowTint, 1.0f);
            std::memcpy(OutBlock + 8, Tint, sizeof(Tint));
            OutBlock[11] = float(std::clamp(Lighting.Shadows.InsetFilter, 0, 2));

            /* The local matrices start after the sun's views (PrepareShadows). */
            const uint32_t DirectionalViews = Shadows.DirectionalFirstView >= 0 ? Shadows.CascadeCount : 0;
            const double   Size             = double(std::max(Shadows.AtlasSize, 1u));
            for(uint32_t Index = 0; Index < Count; ++Index)
            {
                const ShadowView& Inset = Shadows.Views[uint32_t(Shadows.InsetFirstView) + Index];
                float* Out = OutBlock + 12 + Index * 4;
                Out[0] = float(uint32_t(Shadows.InsetFirstView) + Index - DirectionalViews);
                Out[1] = float(2.0 / std::abs(double(Inset.Projection.Data[0][0])) / double(std::max(Inset.TileSize, 1u)));
                Out[2] = float(double(Inset.TileX) / Size);
                Out[3] = float(double(Inset.TileY) / Size);

                /* NDC to pixels: y runs down the target (GDI.h), a pixel of slack each way. */
                float* Rect = OutBlock + 28 + Index * 4;
                const double Width = double(InWidth), Height = double(InHeight);
                Rect[0] = float(std::floor((Inset.ScreenNdc[0] * 0.5 + 0.5) * Width) - 1.0);
                Rect[1] = float(std::floor((0.5 - Inset.ScreenNdc[3] * 0.5) * Height) - 1.0);
                Rect[2] = float(std::ceil((Inset.ScreenNdc[2] * 0.5 + 0.5) * Width) + 1.0);
                Rect[3] = float(std::ceil((0.5 - Inset.ScreenNdc[1] * 0.5) * Height) + 1.0);
            }
        }

        /* "Sun", "Sun Cascade 1", "Spot 2", "Point 0 +X": what a shadow view
         * is of, for its draw event. */
        std::string GetShadowViewName(const int32_t InView) const
        {
            if(InView >= 0 && uint32_t(InView) < Shadows.ViewCount && Shadows.Views[InView].Inset >= 0)
            {
                return "Inset " + std::to_string(InView - Shadows.InsetFirstView);
            }
            const int32_t Sun = Shadows.DirectionalFirstView;
            if(Sun >= 0 && InView >= Sun && InView < Sun + int32_t(Shadows.CascadeCount))
            {
                return Shadows.CascadeCount > 1 ? "Sun Cascade " + std::to_string(InView - Sun) : std::string("Sun");
            }
            static const char* const POINT_FACE_NAMES[] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
            for(uint32_t Light = 0; Light < Lighting.LocalCount && Light < RENDER_LOCAL_LIGHT_MAX; ++Light)
            {
                const int32_t First  = Shadows.LocalFirstView[Light];
                const bool    bPoint = Lighting.Locals[Light].Type == RLL_Point;
                if(First >= 0 && InView >= First && InView < First + (bPoint ? int32_t(SHADOW_POINT_FACES) : 1))
                {
                    return bPoint ? "Point " + std::to_string(Light) + " " + POINT_FACE_NAMES[(InView - First) % 6]
                                  : "Spot " + std::to_string(Light);
                }
            }
            return "View " + std::to_string(InView);
        }

        // === Issuing ============================================================

        void IssueQuad(const DrawCommand& InItem)
        {
            EnsureInitialised();
            Bind(GDIGetShader<Basic_VS>(), GDIGetShader<Basic_PS>());
            PlaceQuad(InItem.Transform, InItem.Width, InItem.Height);
            ApplyPlainState();

            /* Basic_PS::InitUniformBindings (TestbedShaders.h) registers
             * "TestColor" as a Vector3 -- overwritten per draw, so two
             * proxies with different tints each see their own colour. */
            if(GDIShader* Pixel = GDIGetShader<Basic_PS>())
            {
                Pixel->SetUniform("TestColor", EngineCore::Vector3(InItem.Tint.Rf(), InItem.Tint.Gf(), InItem.Tint.Bf()));
            }

            GDIDrawObject(ViewObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        void IssueMaterialQuad(const DrawCommand& InItem)
        {
            EnsureInitialised();
            if(!BindMaterial(*InItem.Mat, EMaterialVertexInput::Quad, InItem.Tint, &InItem))
            {
                DrawCommand Plain = InItem;
                IssueQuad(Plain);
                return;
            }
            PlaceQuad(InItem.Transform, InItem.Width, InItem.Height, &InItem);
            GDIDrawObject(ViewObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        void IssueMeshSection(const DrawCommand& InItem)
        {
            EnsureInitialised();
            GDIViewObject* const MeshObject = RefMeshObject();
            Place(MeshObject, InItem.Transform, 1.0f, 1.0f, &InItem);
            MeshObject->Mesh = InItem.Mesh;

            /* Instanced: the material's InstancedMesh vertex shader, or nothing -- the plain fallback draws once. */
            if(InItem.Instances)
            {
                if(InItem.Mat && BindMaterial(*InItem.Mat, EMaterialVertexInput::InstancedMesh, InItem.Tint, &InItem))
                {
                    GDISetDrawRange(InItem.Mesh, InItem.First, InItem.Count);
                    GDISetInstanceBuffer(InItem.Instances, InItem.InstanceCount);
                    GDISetIndirectArgs(InItem.IndirectArgs, InItem.IndirectOffset);   // the live ones only, where it can
                    GDIDrawObject(MeshObject);
                    GDISetIndirectArgs(nullptr);
                    GDISetInstanceBuffer(nullptr, 0);
                    GDISetDrawRange(InItem.Mesh, 0, 0);
                    DrawCallCount.fetch_add(1, std::memory_order_relaxed);
                }
                MeshObject->Mesh = nullptr;
                return;
            }

            if(!InItem.Mat || !BindMaterial(*InItem.Mat, PrepareMeshInput(InItem), InItem.Tint, &InItem))
            {
                GDIShader* Pixel = GDIGetShader<Mesh_PS>();
                Bind(GDIGetShader<Mesh_VS>(), Pixel);
                ApplyPlainState();
                if(Pixel)
                {
                    Pixel->SetUniform("Tint", EngineCore::Vector3(InItem.Tint.Rf(), InItem.Tint.Gf(), InItem.Tint.Bf()));
                }
            }

            GDISetDrawRange(InItem.Mesh, InItem.First, InItem.Count);
            GDIDrawObject(MeshObject);
            GDISetDrawRange(InItem.Mesh, 0, 0);
            MeshObject->Mesh = nullptr;
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        /* A PostProcess material over the bound target; the scene so far is
         * material texture 0. False if its shaders are missing. */
        bool IssueFullscreen(const DrawCommand& InItem)
        {
            EnsureInitialised();
            if(!InItem.Mat || !BindMaterial(*InItem.Mat, EMaterialVertexInput::Fullscreen, EngineCore::Color(255, 255, 255, 255)))
            {
                return false;
            }
            /* After BindMaterial, which clears every slot but 0: Scene Depth's. */
            GDISetMaterialTexture(GDI_MATERIAL_TEXTURE_SLOTS - 1, PostSceneDepth);
            GDIDrawObject(ScreenObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        /* What a capture tool shows for InPath: its file name, extensions
         * dropped ("Engine/Content/Meshes/Sphere.nmmesh" is "Sphere"). */
        static std::string ShortName(const std::string& InPath)
        {
            const size_t Slash = InPath.find_last_of("/\\");
            const std::string File = Slash == std::string::npos ? InPath : InPath.substr(Slash + 1);
            const size_t Dot = File.find('.');
            return Dot == std::string::npos || Dot == 0 ? File : File.substr(0, Dot);
        }

        /* A draw's label (Issue): what it draws, then with what --
         * "Sphere [Sphere] | Glow", "Particles Sparks/Sparks (up to 512) | ParticleStreak". */
        static std::string LabelOf(const DrawCommand& InItem)
        {
            std::string Out;
            switch(InItem.Kind)
            {
                case DrawCommand::EKind::MeshSection:
                    Out = InItem.Label ? std::string(InItem.Label)
                        : InItem.Mesh && !InItem.Mesh->GetDebugName().empty() ? ShortName(InItem.Mesh->GetDebugName()) : std::string("Mesh");
                    if(!InItem.Label && InItem.Section && InItem.Section[0] != '\0') { Out += " [" + std::string(InItem.Section) + "]"; }
                    if(InItem.Instances)
                    {
                        /* Drawn indirectly, it draws only the live ones: the GPU's count, which a capture shows on the draw. */
                        Out += InItem.IndirectArgs ? " (up to " + std::to_string(InItem.InstanceCount) + ")"
                                                   : " x" + std::to_string(InItem.InstanceCount);
                    }
                    break;
                case DrawCommand::EKind::Quad:
                case DrawCommand::EKind::MaterialQuad: Out = InItem.Label ? std::string(InItem.Label) : std::string("Quad"); break;
                case DrawCommand::EKind::Fullscreen:   Out = "Fullscreen"; break;
            }
            if(InItem.Mat && !InItem.Mat->Name.empty())          { Out += " | " + ShortName(InItem.Mat->Name); }
            else if(InItem.Kind == DrawCommand::EKind::MeshSection) { Out += " | (plain tint)"; }
            return Out;
        }

        /* A draw's region, named for its mesh and material, so RenderDoc's Event
         * Browser (and PIX, Xcode) reads what it is; true if one was begun, to end
         * with EndDrawLabel. Nothing in shipping builds or where nothing records it. */
        bool BeginDrawLabel(const DrawCommand& InItem) const
        {
#if !(defined(NIMBLE_SHIPPING) && NIMBLE_SHIPPING)
            if(GIssueScratch ? bLabelsThisPass : (GRenderDrawLabels.GetBool() && GDIHasDrawEvents()))
            {
                GDIBeginDrawEvent(LabelOf(InItem));
                return true;
            }
#endif
            (void)InItem;
            return false;
        }

        static void EndDrawLabel(const bool bInBegun)
        {
            if(bInBegun) { GDIEndDrawEvent(); }
        }

        void Issue(const DrawCommand& InItem)
        {
            const bool bLabel = BeginDrawLabel(InItem);
            /* Skipped by the GPU if its proxy's occlusion query found nothing. */
            const bool bPredicated = InItem.Occlusion >= 0 && OcclusionQueries && bOcclusionResolved;
            if(bPredicated) { GDISetOcclusionPredicate(OcclusionQueries, InItem.Occlusion); }
            switch(InItem.Kind)
            {
                case DrawCommand::EKind::Quad:         IssueQuad(InItem);         break;
                case DrawCommand::EKind::MaterialQuad: IssueMaterialQuad(InItem); break;
                case DrawCommand::EKind::MeshSection:  IssueMeshSection(InItem);  break;
                case DrawCommand::EKind::Fullscreen:   IssueFullscreen(InItem);   break;
            }
            if(bPredicated) { GDISetOcclusionPredicate(nullptr, -1); }
            EndDrawLabel(bLabel);
        }

        /* Into InStage's policy inside a scene; at once outside one. */
        void Submit(DrawCommand InItem, const ERenderStage InStage)
        {
            InItem.CustomGroups = DrawCustomGroups;   // a static list's replay: the proxy's groups now
            InItem.bReveal      = bDrawReveal;        // and its reveal
            if(Capture)
            {
                Capture->push_back({ InItem, InStage });   // a static list's: it never moves
                return;
            }
            if(bDeferring) { NoteStreamingDemand(InItem); }   // a static list's replay too: this view's demand
            if(bDrawMotion && !InItem.bStatic)
            {
                InItem.bMoved = true;
                InItem.Motion = DrawMotion;
            }
            InItem.Occlusion = DrawOcclusion;   // a static list's replay too: its query is this frame's
            if(!bDeferring)
            {
                Issue(InItem);
                return;
            }
            const EngineCore::Vector3 Offset = (InItem.Instances ? InItem.SortOrigin : InItem.Transform.Position) - View.Origin;
            InItem.Depth = float(Offset.Dot(ViewForward(View)));
            PolicyFor(InStage).Add(InItem);
            /* And its custom pass mesh groups' (DrawMeshGroup): a surface's
             * sections, not its shadow casters or occluders. */
            if(InItem.CustomGroups != 0 && InItem.Kind == DrawCommand::EKind::MeshSection && InItem.Mesh
               && InStage != ERenderStage::Shadows && InStage != ERenderStage::PrePass && InStage != ERenderStage::PostProcess)
            {
                for(uint32_t Group = 0; Group < RENDER_PASS_MESH_GROUPS; ++Group)
                {
                    if(InItem.CustomGroups & (1u << Group)) { GroupDraws[Group].push_back(InItem); }
                }
            }
        }

        /* A usable material's stage; Unlit for the plain fallbacks. */
        static ERenderStage StageOf(const Material* InMaterial)
        {
            return InMaterial ? GetMaterialRenderStage(InMaterial->Description.Domain, InMaterial->Description.Shading)
                              : ERenderStage::Unlit;
        }

        /*
         * PARALLEL ISSUE (MULTITHREADED_RENDERING.md, phase 3).
         *
         * InOrder's draws of InPolicy, split into contiguous chunks, each
         * recorded on its own thread into its own GDI recording context
         * (GDIBeginParallelPass) -- the job system's workers and this thread --
         * and executed in chunk order, so the GPU sees the draws in InOrder's
         * order. False, with nothing issued, where the gate says no, the stage
         * is too small, a draw cannot be issued off the render thread, or GDI
         * cannot: then the caller issues serially.
         *
         * Before any chunk runs, on this thread: every draw's material,
         * vertex and depth shaders loaded, its textures and the lightmaps
         * resolved (ResolveForParallel) -- a chunk only reads what is loaded.
         * InIssue runs on the chunks' threads: only the issue functions that
         * keep their state in GIssueScratch (Issue, IssueMeshSection,
         * IssueShadowCaster and what they call).
         */
        template<typename IssueFn>
        bool IssueInParallel(const DrawPolicy& InPolicy, const std::vector<uint32_t>& InOrder, const bool bInShadows, IssueFn&& InIssue)
        {
            const uint32_t Count = uint32_t(InOrder.size());
            if(!ShouldRecordInParallel(Parallel, Count))
            {
                return false;
            }
            EngineCore::JobSystem* Jobs = Parallel.bSerialOnly ? nullptr : EngineCore::GetJobSystem();
            if(!Parallel.bSerialOnly && !Jobs)
            {
                return false;
            }
            const uint32_t Threads = Parallel.bSerialOnly ? 4u : Jobs->GetWorkerCount() + 1;
            const uint32_t ByDraws = std::max<uint32_t>(1, Count / PARALLEL_MIN_DRAWS_PER_CHUNK);
            const uint32_t Chunks  = std::min<uint32_t>(std::min<uint32_t>(Threads, ByDraws), GDI_PARALLEL_CHUNKS_MAX);
            EnsureInitialised();   // the Impl's own objects: never made on a chunk's thread
            if(Chunks < 2 || !ResolveForParallel(InPolicy, InOrder, bInShadows))
            {
                return false;
            }

            while(Scratches.size() < Chunks)
            {
                auto Scratch = std::make_unique<RenderIssueScratch>();
                Scratch->View3D     = new GDIView();
                Scratch->MeshObject = new GDIViewObject();
                Scratch->MeshObject->Init(Scratch->View3D);
                Scratches.push_back(std::move(Scratch));
            }
            for(uint32_t Chunk = 0; Chunk < Chunks; ++Chunk)
            {
                RenderIssueScratch& Scratch = *Scratches[Chunk];
                Scratch.BoundVertex     = BoundVertex;      // what GDI has bound as the pass begins
                Scratch.BoundPixel      = BoundPixel;
                Scratch.PlaceProjection = PlaceProjection;
                Scratch.PlaceView       = PlaceView;
                Scratch.ShadowView      = -1;
            }
            bLabelsThisPass = GRenderDrawLabels.GetBool() && GDIHasDrawEvents();

            GDIRecordingContext* Contexts[GDI_PARALLEL_CHUNKS_MAX] = {};
            if(!GDIBeginParallelPass(Chunks, Contexts))
            {
                return false;
            }
            const auto RunChunk = [&](const uint32_t InChunk)
            {
                const uint32_t Begin = uint32_t(uint64_t(Count) * InChunk / Chunks);
                const uint32_t End   = uint32_t(uint64_t(Count) * (InChunk + 1) / Chunks);
                GIssueScratch = Scratches[InChunk].get();
                GDISetCurrentRecording(Contexts[InChunk]);
                for(uint32_t Index = Begin; Index < End; ++Index)
                {
                    InIssue(InPolicy[InOrder[Index]]);
                }
                GDISetCurrentRecording(nullptr);
                GIssueScratch = nullptr;
            };
            if(Jobs)
            {
                Jobs->ParallelFor(Chunks, 1, [&RunChunk](const uint32_t InBegin, const uint32_t InEnd)
                {
                    for(uint32_t Chunk = InBegin; Chunk < InEnd; ++Chunk) { RunChunk(Chunk); }
                });
            }
            else
            {
                for(uint32_t Chunk = 0; Chunk < Chunks; ++Chunk) { RunChunk(Chunk); }
            }
            GDIEndParallelPass();
            ParallelPassCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        /* IssueInParallel's preparation, on the render thread: false if a draw
         * cannot be issued by a recording chunk -- not a mesh section with a
         * usable material and shaders (the fallbacks write shared shader
         * uniforms) -- else every shader loaded and every texture resolved. */
        bool ResolveForParallel(const DrawPolicy& InPolicy, const std::vector<uint32_t>& InOrder, const bool bInShadows)
        {
            ++ParallelPassSerial;
            if(bInShadows && !GetShadowDepthShader())
            {
                return false;
            }
            for(const uint32_t Index : InOrder)
            {
                const DrawCommand& Item = InPolicy[Index];
                if(!Item.Mesh || (!bInShadows && (Item.Kind != DrawCommand::EKind::MeshSection || !Item.Mat || !Item.Mat->Pixel)))
                {
                    return false;
                }
                const EMaterialVertexInput Input = Item.Instances ? EMaterialVertexInput::InstancedMesh
                                                 : (Item.Mesh->IsSkinned() ? EMaterialVertexInput::SkinnedMesh : EMaterialVertexInput::Mesh);
                if(bInShadows)
                {
                    if(!(Item.Mat ? VertexFor(*Item.Mat, Input) : GetDefaultVertex(Input))) { return false; }
                    /* A caster's offset may sample its textures (IssueShadowCaster). */
                    if(!Item.Mat || !Item.Mat->Description.bHasOffset || Item.Mat->Description.Textures.empty()) { continue; }
                }
                else if(!VertexFor(*Item.Mat, Input))
                {
                    return false;
                }
                Material& Mat = *Item.Mat;
                if(Mat.ResolvedPass != ParallelPassSerial)
                {
                    Mat.Resolved.resize(Mat.Description.Textures.size());
                    for(size_t Texture = 0; Texture < Mat.Description.Textures.size(); ++Texture)
                    {
                        Mat.Resolved[Texture] = FindTexture(Mat.Description.Textures[Texture]);
                    }
                    Mat.ResolvedPass = ParallelPassSerial;
                }
            }
            if(!bInShadows)
            {
                for(uint32_t Atlas = 0; Atlas < RENDER_BAKED_ATLASES_MAX; ++Atlas)
                {
                    LightmapResolved[Atlas] = Lighting.bBaked && Atlas < Lighting.BakedAtlasCount ? LightmapTexture(Atlas) : nullptr;
                }
            }
            return true;
        }

        /* InPolicy's draws [InBegin, InEnd) of an issue order: the static draw
         * lists' first, then the rest, each under its own draw event. Only for
         * a stage that neither blends nor depends on order within itself. */
        static std::vector<uint32_t> MobilityOrder(const DrawPolicy& InPolicy, const uint32_t* InBegin, const uint32_t* InEnd)
        {
            std::vector<uint32_t> Out;
            Out.reserve(size_t(InEnd - InBegin));
            for(const bool bStatic : { true, false })
            {
                for(const uint32_t* It = InBegin; It != InEnd; ++It)
                {
                    if(InPolicy[*It].bStatic == bStatic) { Out.push_back(*It); }
                }
            }
            return Out;
        }

        template<typename IssueFn>
        static void IssueByMobility(const DrawPolicy& InPolicy, const uint32_t* InBegin, const uint32_t* InEnd, IssueFn&& InIssue)
        {
            for(const bool bStatic : { true, false })
            {
                const auto Matches = [&InPolicy, bStatic](const uint32_t InIndex) { return InPolicy[InIndex].bStatic == bStatic; };
                if(std::none_of(InBegin, InEnd, Matches))
                {
                    continue;
                }
                SCOPED_DRAW_EVENT(bStatic ? "Static" : "Dynamic");
                for(const uint32_t* It = InBegin; It != InEnd; ++It)
                {
                    if(Matches(*It)) { InIssue(InPolicy[*It]); }
                }
            }
        }

        /* Issues InStage's policy in its order, under a draw event. The Base
         * Pass (no blending) splits into its Static and Dynamic draws. */
        void IssueStage(const ERenderStage InStage)
        {
            const DrawPolicy& Policy = PolicyFor(InStage);
            StageDrawCounts[size_t(InStage)] = uint32_t(Policy.Num());
            if(Policy.IsEmpty())
            {
                return;
            }
            SCOPED_DRAW_EVENT(GetRenderStageName(InStage));
            const std::vector<uint32_t> Order = Policy.GetIssueOrder();
            if(InStage == ERenderStage::BasePass)
            {
                /* In the same order serially or in chunks: static, then dynamic. */
                if(IssueInParallel(Policy, MobilityOrder(Policy, Order.data(), Order.data() + Order.size()), false,
                                   [this](const DrawCommand& InItem) { Issue(InItem); }))
                {
                    return;
                }
                IssueByMobility(Policy, Order.data(), Order.data() + Order.size(), [this](const DrawCommand& InItem) { Issue(InItem); });
                return;
            }
            for(const uint32_t Index : Order)
            {
                Issue(Policy[Index]);
            }
        }

        /*
         * The Lighting stage: DeferredLighting.fsh over InTargets' G-buffer
         * into the bound scene colour, whose depth is a texture here -- bound
         * as material texture 5, after GBuffer0-4. bInSubsurface: a second
         * target, Subsurface, is bound beside it for Skin pixels' diffuse
         * (SHADING_MODELS.md). False (warned) if this backend cannot.
         */
        bool IssueLighting(SceneTargets& InTargets, const bool bInSubsurface)
        {
            GDIShader* Pixel  = GetLightingShader();
            GDIShader* Vertex = GetDefaultVertex(EMaterialVertexInput::Fullscreen);
            GDITexture2D* Depth = InTargets.SceneColor ? InTargets.SceneColor->ToDepthTexture2D() : nullptr;
            if(!Pixel || !Vertex)
            {
                return false;   // warned by LoadShader
            }
            if(!Depth)
            {
                if(Backend == ERenderBackend::Null)
                {
                    return false;   // draws nothing by design; nothing to warn about
                }
                WarnOnce("|lighting-depth", "the Lighting stage: this backend cannot sample the scene depth; "
                                            "Opaque and Masked Lit materials draw nothing");
                return false;
            }

            for(uint32_t Index = 0; Index < RENDER_GBUFFER_TARGETS; ++Index)
            {
                GDITexture2D* Texture = InTargets.GBuffer[Index]->ToGDITexture2D();
                GDISetMaterialTexture(Index, Texture ? Texture->Resource : nullptr);
            }
            GDISetMaterialTexture(RENDER_GBUFFER_TARGETS, Depth->Resource);

            /* The shadow atlases this frame drew, after the scene depth. This
             * frame's ambient occlusion is in GBuffer0's alpha already (PPAmbientApply.fsh). */
            for(uint32_t Atlas = 0; Atlas < SA_Count; ++Atlas)
            {
                GDITexture2D* Map = Shadows.ViewCount > 0 && ShadowAtlas[Atlas] ? ShadowAtlas[Atlas]->ToDepthTexture2D() : nullptr;
                GDISetMaterialTexture(RENDER_GBUFFER_TARGETS + 1 + Atlas, Map ? Map->Resource : nullptr);
            }
            const uint32_t LastSlot = RENDER_GBUFFER_TARGETS + SA_Count;
            static_assert(RENDER_GBUFFER_TARGETS + SA_Count < GDI_MATERIAL_TEXTURE_SLOTS, "the Lighting stage's textures fit the material slots");

            /* Lighting_Block (DeferredLighting.fsh): whether Skin's diffuse has
             * its own target, then the inset shadows. */
            float LightingBlock[LIGHTING_BLOCK_FLOATS] = {};
            LightingBlock[0] = bInSubsurface ? 1.0f : 0.0f;
            PutInsetShadows(LightingBlock, InTargets.Width, InTargets.Height);
            EnsureInitialised();
            Bind(Vertex, Pixel);
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, LightingBlock, uint(sizeof(LightingBlock)));
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));
            GDISetDepthState(IDepthStateDesc());   // no depth test: there is no depth bound
            GDISetBlendState(IBlendStateDesc());
            GDIDrawObject(ScreenObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);

            /* Unbound again: the depth is about to be an attachment. */
            for(uint32_t Index = 0; Index <= LastSlot; ++Index)
            {
                GDISetMaterialTexture(Index, nullptr);
            }
            return true;
        }

        /* Retires every material and the shared vertex shaders. */
        void RetireMaterials()
        {
            ++ContentGeneration;
            for(auto& Each : Materials)
            {
                Material& M = Each.second;
                for(GDIShader* Shader : { M.Pixel, M.Vertex[0], M.Vertex[1], M.Vertex[2], M.Vertex[3], M.Vertex[4] })
                {
                    if(Shader) { RetiredShaders.push_back(Shader); }
                }
            }
            Materials.clear();
            for(int Index = 0; Index < 5; ++Index)
            {
                if(DefaultVertex[Index]) { RetiredShaders.push_back(DefaultVertex[Index]); }
                DefaultVertex[Index] = nullptr;
                bDefaultVertexTried[Index] = false;
            }
            if(LightingShader) { RetiredShaders.push_back(LightingShader); }
            LightingShader = nullptr;
            bLightingTried = false;
            for(auto& Each : ComputeShaders)
            {
                if(Each.second) { RetiredShaders.push_back(Each.second); }
            }
            ComputeShaders.clear();
            if(ShadowDepthShader) { RetiredShaders.push_back(ShadowDepthShader); }
            ShadowDepthShader = nullptr;
            bShadowDepthTried = false;
            if(UIVertexShader) { RetiredShaders.push_back(UIVertexShader); }
            UIVertexShader = nullptr;
            bUIVertexTried = false;
            for(uint32_t Pass = 0; Pass < PP_PassCount; ++Pass)
            {
                if(PostShaders[Pass]) { RetiredShaders.push_back(PostShaders[Pass]); }
                PostShaders[Pass]      = nullptr;
                bPostShaderTried[Pass] = false;
            }
            Warned.clear();
            BoundVertex = nullptr;
            BoundPixel  = nullptr;
        }
    };

    void RenderContext::Impl::IssueAmbientOcclusion(SceneTargets& InTargets, const PostProcessSettings& InPost)
    {
        AmbientOcclusion = nullptr;
        if(!InPost.bAmbientOcclusion || InPost.AoIntensity <= 0.001f || InPost.AoRadius <= 0.0f)
        {
            return;
        }
        GDITexture2D* DepthTexture = InTargets.SceneColor ? InTargets.SceneColor->ToDepthTexture2D() : nullptr;
        GDITexture2D* Normals      = InTargets.GBuffer[1] ? InTargets.GBuffer[1]->ToGDITexture2D() : nullptr;
        if(!DepthTexture || !Normals)
        {
            if(Backend != ERenderBackend::Null)
            {
                WarnOnce("|ao-depth", "ambient occlusion: this backend cannot sample the scene depth; it is skipped");
            }
            return;
        }
        if(!InTargets.EnsureHalf())
        {
            return;
        }
        const uint Width  = InTargets.Width;
        const uint Height = InTargets.Height;
        const uint HalfW  = SceneTargets::HalfSize(Width, 1);
        const uint HalfH  = SceneTargets::HalfSize(Height, 1);
        const auto Texture = [](GDIRenderTarget* InTarget) -> GDITextureResource*
        {
            GDITexture2D* Each = InTarget ? InTarget->ToGDITexture2D() : nullptr;
            return Each ? Each->Resource : nullptr;
        };

        SCOPED_DRAW_EVENT("AmbientOcclusion");
        PostBlock Block;
        PutMatrix(Block.Matrix, View.Projection * View.View);
        Block.A[0] = InPost.AoIntensity;
        Block.A[1] = InPost.AoRadius;
        Block.A[2] = InPost.AoSamples;
        Block.A[3] = InPost.AoPower;
        Block.Sizes[0] = 1.0f / float(HalfW);
        Block.Sizes[1] = 1.0f / float(HalfH);
        Block.Sizes[2] = float(Width);
        Block.Sizes[3] = float(Height);
        if(DrawPost(PP_AmbientOcclusion, InTargets.Half[0], HalfW, HalfH, { DepthTexture->Resource, Normals->Resource }, Block))
        {
            AmbientOcclusion = Texture(InTargets.Half[0]);
            if(InPost.bAoBlur
               && DrawPost(PP_AmbientBlur, InTargets.Half[1], HalfW, HalfH, { AmbientOcclusion, DepthTexture->Resource }, Block))
            {
                AmbientOcclusion = Texture(InTargets.Half[1]);
            }
            /* Folded into GBuffer0's alpha, the material's own occlusion: the Lighting
             * and IBL passes read one, and spend no texture slot on it. */
            GDITextureResource* Occlusion[1] = { AmbientOcclusion };
            DrawFullscreenShader(GetPostShader(PP_AmbientApply), POST_PASS_NAMES[PP_AmbientApply], InTargets.GBuffer[0], Width, Height,
                                 Occlusion, 1, Block.A, uint(sizeof(Block) - sizeof(Block.Matrix)), EFullscreenBlend::Multiply);
            GDISetMaterialTexture(0, nullptr);
        }
    }

    namespace
    {
        /* PPSubsurface.fsh's PP_Block. */
        constexpr int SUBSURFACE_TAPS = 25;
        struct SubsurfaceBlock
        {
            float Params[4] = {};
            float Scale[4]  = {};
            float Sizes[4]  = {};
            float Kernel[SUBSURFACE_TAPS][4] = {};
        };

        /*
         * Skin's separable kernel (Jimenez et al. 2015, "Separable Subsurface
         * Scattering", and its reference code): SUBSURFACE_TAPS offsets from -3
         * to 3, packed toward the centre, each weighted by the area it stands
         * for times d'Eon and Luebke's skin profile (GPU Gems 3, chapter 14:
         * a sum of Gaussians, the narrowest left out as the surface's own
         * reflection), stretched per channel by its falloff; normalised per
         * channel, the centre moved first, and mixed toward no blur by each
         * channel's strength.
         */
        void BuildSkinKernel(float (&OutKernel)[SUBSURFACE_TAPS][4])
        {
            constexpr float Range    = 3.0f;
            constexpr float Falloff[3]  = { 1.0f, 0.37f, 0.3f };
            constexpr float Strength[3] = { 0.48f, 0.41f, 0.28f };
            constexpr float Variances[5] = { 0.0484f, 0.187f, 0.567f, 1.99f, 7.41f };
            constexpr float Weights[5]   = { 0.100f, 0.118f, 0.113f, 0.358f, 0.078f };
            const auto Profile = [&](const float InR, const int InChannel)
            {
                float Sum = 0.0f;
                for(int Each = 0; Each < 5; ++Each)
                {
                    const float R = InR / (0.001f + Falloff[InChannel]);
                    Sum += Weights[Each] * std::exp(-(R * R) / (2.0f * Variances[Each])) / (2.0f * 3.14159265f * Variances[Each]);
                }
                return Sum;
            };

            float Offsets[SUBSURFACE_TAPS];
            const float Step = 2.0f * Range / float(SUBSURFACE_TAPS - 1);
            for(int Index = 0; Index < SUBSURFACE_TAPS; ++Index)
            {
                const float O = -Range + float(Index) * Step;
                Offsets[Index] = (O < 0.0f ? -1.0f : 1.0f) * O * O / Range;   // Range * sign(o) * o^2 / Range^2
            }
            float Kernel[SUBSURFACE_TAPS][4] = {};
            float Sums[3] = {};
            for(int Index = 0; Index < SUBSURFACE_TAPS; ++Index)
            {
                const float Before = Index > 0 ? std::fabs(Offsets[Index] - Offsets[Index - 1]) : 0.0f;
                const float After  = Index < SUBSURFACE_TAPS - 1 ? std::fabs(Offsets[Index] - Offsets[Index + 1]) : 0.0f;
                const float Area   = 0.5f * (Before + After);
                for(int Channel = 0; Channel < 3; ++Channel)
                {
                    Kernel[Index][Channel] = Area * Profile(Offsets[Index], Channel);
                    Sums[Channel] += Kernel[Index][Channel];
                }
                Kernel[Index][3] = Offsets[Index];
            }
            constexpr int Centre = SUBSURFACE_TAPS / 2;
            int Out = 0;
            std::memcpy(OutKernel[Out++], Kernel[Centre], sizeof(Kernel[Centre]));
            for(int Index = 0; Index < SUBSURFACE_TAPS; ++Index)
            {
                if(Index != Centre) { std::memcpy(OutKernel[Out++], Kernel[Index], sizeof(Kernel[Index])); }
            }
            for(int Index = 0; Index < SUBSURFACE_TAPS; ++Index)
            {
                for(int Channel = 0; Channel < 3; ++Channel)
                {
                    const float Normalised = OutKernel[Index][Channel] / Sums[Channel];
                    OutKernel[Index][Channel] = Index == 0 ? 1.0f + (Normalised - 1.0f) * Strength[Channel] : Normalised * Strength[Channel];
                }
            }
        }
    }

    void RenderContext::Impl::IssueSubsurface(SceneTargets& InTargets, GDIRenderTarget* InDiffuse, GDIRenderTarget* InSceneColor)
    {
        GDITexture2D* Depth   = InSceneColor ? InSceneColor->ToDepthTexture2D() : nullptr;
        GDITexture2D* Diffuse = InDiffuse ? InDiffuse->ToGDITexture2D() : nullptr;
        GDIRenderTarget* Blur = InTargets.EnsureFullHalfFloat(InTargets.SubsurfaceBlur, "SubsurfaceBlur");
        GDITexture2D* Blurred = Blur ? Blur->ToGDITexture2D() : nullptr;
        GDITexture2D* Albedo  = InTargets.GBuffer[0] ? InTargets.GBuffer[0]->ToGDITexture2D() : nullptr;
        if(!Depth || !Diffuse || !Blurred || !Albedo)
        {
            return;
        }
        static const SubsurfaceBlock Skin = []
        {
            SubsurfaceBlock Made;
            BuildSkinKernel(Made.Kernel);
            return Made;
        }();
        SubsurfaceBlock Block = Skin;
        const uint Width  = InTargets.Width;
        const uint Height = InTargets.Height;
        Block.Params[3] = float(SUBSURFACE_TAPS);
        Block.Scale[0]  = std::max(GRenderSubsurfaceWidth.GetFloat(), 0.0f);
        Block.Scale[1]  = View.Projection.Data[1][1];   // 1 / tan(half the vertical field of view)
        Block.Scale[2]  = float(Width) / float(std::max<uint>(Height, 1));
        Block.Sizes[0]  = 1.0f / float(Width);
        Block.Sizes[1]  = 1.0f / float(Height);
        Block.Sizes[2]  = float(Width);
        Block.Sizes[3]  = float(Height);

        SCOPED_DRAW_EVENT("Subsurface");
        GDIShader* Shader = GetPostShader(PP_Subsurface);
        Block.Params[0] = 1.0f;
        Block.Params[1] = 0.0f;
        Block.Params[2] = 0.0f;
        GDITextureResource* Across[3] = { Diffuse->Resource, Depth->Resource, Albedo->Resource };
        DrawFullscreenShader(Shader, "SubsurfaceAcross", Blur, Width, Height, Across, 3, &Block, uint(sizeof(Block)),
                             EFullscreenBlend::Opaque);
        Block.Params[0] = 0.0f;
        Block.Params[1] = 1.0f;
        Block.Params[2] = 1.0f;
        GDITextureResource* Down[3] = { Blurred->Resource, Depth->Resource, Albedo->Resource };
        DrawFullscreenShader(Shader, "SubsurfaceDown", InSceneColor, Width, Height, Down, 3, &Block, uint(sizeof(Block)),
                             EFullscreenBlend::Additive);
    }

    GDIRenderTarget* RenderContext::Impl::RunPostProcessStack(SceneTargets& InTargets, const PostProcessSettings& InPost, const bool bInHasPrevious,
                                                              GDIRenderTarget* InSource)
    {
        /* Ping-pongs two full-size targets; the scene colour (and its depth)
         * is only ever read, so every pass can sample the depth. InSource: the
         * scene colour, or its tone-mapped copy (PostProcess) in an HDR scene. */
        GDIRenderTarget* Source = InSource ? InSource : InTargets.SceneColor;
        const DrawPolicy& Materials = PolicyFor(ERenderStage::PostProcess);
        const uint Width  = InTargets.Width;
        const uint Height = InTargets.Height;
        const uint HalfW  = SceneTargets::HalfSize(Width, 1);
        const uint HalfH  = SceneTargets::HalfSize(Height, 1);

        GDITexture2D*       DepthTexture = InTargets.SceneColor->ToDepthTexture2D();
        GDITextureResource* Depth        = DepthTexture ? DepthTexture->Resource : nullptr;
        const auto Texture = [](GDIRenderTarget* InTarget) -> GDITextureResource*
        {
            GDITexture2D* Each = InTarget ? InTarget->ToGDITexture2D() : nullptr;
            return Each ? Each->Resource : nullptr;
        };
        const auto Next = [&]() -> GDIRenderTarget*
        {
            GDIRenderTarget* First = InTargets.EnsurePostProcess();
            return Source == First ? InTargets.EnsurePostProcessB() : First;
        };
        const auto Sizes = [](PostBlock& OutBlock, const uint InW, const uint InH)
        {
            OutBlock.Sizes[0] = 1.0f / float(InW);
            OutBlock.Sizes[1] = 1.0f / float(InH);
            OutBlock.Sizes[2] = float(InW);
            OutBlock.Sizes[3] = float(InH);
        };

        const bool bDof    = InPost.bDepthOfField && InPost.DofMaxBlur > 0.05f && Depth;
        const bool bMotion = InPost.bMotionBlur && InPost.MotionIntensity > 0.001f && bInHasPrevious && Depth;
        const bool bBloom  = InPost.bBloom && InPost.BloomIntensity > 0.001f;
        const bool bAa     = InPost.bAntialiasing != 0;
        uint32_t   Passes  = 0;
        StageDrawCounts[size_t(ERenderStage::PostProcess)] = uint32_t(Materials.Num());
        if(Materials.IsEmpty() && !bDof && !bMotion && !bBloom && !bAa)
        {
            return Source;
        }
        if(InPost.bDepthOfField && !Depth)
        {
            WarnOnce("|post-depth", "post process: this backend cannot sample the scene depth; Depth of Field and Motion Blur are skipped");
        }

        /* Materials at InStage, in their order: slot 0 the scene so far, slot 7 its depth. */
        const auto RunMaterials = [&](const uint8_t InStage)
        {
            const std::vector<uint32_t> Order = Materials.GetIssueOrder();
            if(std::none_of(Order.begin(), Order.end(), [&](const uint32_t InIndex) { return Materials[InIndex].PostStage == InStage; }))
            {
                return;
            }
            SCOPED_DRAW_EVENT(InStage == PPS_BeforeEffects ? "Materials (Before Effects)" : "Materials (After Effects)");
            PostSceneDepth = Depth;
            for(const uint32_t Index : Order)
            {
                if(Materials[Index].PostStage != InStage) { continue; }
                GDIRenderTarget* Dest = Next();
                if(!Dest) { break; }
                GDISetRenderTargets(&Dest, 1, nullptr);
                GDISetViewport(0, 0, Width, Height);
                GDISetColorMask(EColM_RGBA);
                GDISetMaterialTexture(0, Texture(Source));
                const bool bLabel = BeginDrawLabel(Materials[Index]);
                const bool bDrawn = IssueFullscreen(Materials[Index]);
                EndDrawLabel(bLabel);
                if(bDrawn)
                {
                    Source = Dest;
                    ++Passes;
                }
            }
            GDISetMaterialTexture(0, nullptr);
            GDISetMaterialTexture(GDI_MATERIAL_TEXTURE_SLOTS - 1, nullptr);
            PostSceneDepth = nullptr;
        };
        RunMaterials(PPS_BeforeEffects);

        /* Depth of Field: circle of confusion and a half-size copy; a disc blur
         * of that; the two mixed back at full size. */
        if(bDof && InTargets.EnsureHalf())
        {
            SCOPED_DRAW_EVENT("DepthOfField");
            PostBlock Block;
            Block.A[0] = InPost.DofFocusDistance;
            Block.A[1] = std::max(InPost.DofFocusRange, 1.0f);
            Block.A[2] = InPost.DofMaxBlur;
            Block.A[3] = InPost.bDofNearBlur ? 1.0f : 0.0f;
            Sizes(Block, Width, Height);
            GDIRenderTarget* Dest = Next();
            if(DrawPost(PP_DofPrepare, InTargets.Half[0], HalfW, HalfH, { Texture(Source), Depth }, Block))
            {
                Sizes(Block, HalfW, HalfH);
                DrawPost(PP_DofBlur, InTargets.Half[1], HalfW, HalfH, { Texture(InTargets.Half[0]) }, Block);
                if(DrawPost(PP_DofComposite, Dest, Width, Height, { Texture(Source), Texture(InTargets.Half[1]) }, Block))
                {
                    Source = Dest;
                    Passes += 3;
                }
            }
        }

        /* Motion Blur: along each pixel's motion since last frame's camera. */
        if(bMotion)
        {
            PostBlock Block;
            PutMatrix(Block.Matrix, PrevViewProjection);
            Block.A[0] = InPost.MotionIntensity;
            Block.A[1] = InPost.MotionMaxLength;
            Block.A[2] = InPost.MotionSamples;
            Block.A[3] = bVelocityThisFrame ? 1.0f : 0.0f;   // each surface's own motion too, from the Base Pass
            Sizes(Block, Width, Height);
            GDIRenderTarget* Dest = Next();
            GDITextureResource* Velocity = bVelocityThisFrame ? Texture(InTargets.Velocity) : nullptr;
            if(DrawPost(PP_MotionBlur, Dest, Width, Height, { Texture(Source), Depth, Velocity }, Block))
            {
                Source = Dest;
                ++Passes;
            }
        }

        /* Bloom: down a chain of half-size targets (the first keeping only
         * what passes the threshold, with the emissive the scene clipped added
         * back from GBuffer3), back up summing each with the one below into
         * BloomUp, then the top level added to the scene. */
        const uint32_t Levels = uint32_t(std::clamp(int(InPost.BloomLevels + 0.5f), 1, int(RENDER_BLOOM_LEVELS_MAX)));
        if(bBloom && InTargets.EnsureBloom(Levels))
        {
            SCOPED_DRAW_EVENT("Bloom");
            PostBlock Block;
            Block.A[0] = InPost.BloomThreshold;
            Block.A[1] = 0.2f;   // knee: the threshold's softness, a fraction of it
            Block.A[2] = 1.0f;
            GDITexture2D* Emissive = bGBufferThisFrame && InTargets.GBuffer[3] ? InTargets.GBuffer[3]->ToGDITexture2D() : nullptr;
            Block.A[3] = Emissive ? 1.0f : 0.0f;
            bool bChain = true;
            GDIRenderTarget* From = Source;
            uint FromW = Width, FromH = Height;
            for(uint32_t Level = 0; Level < Levels && bChain; ++Level)
            {
                Sizes(Block, FromW, FromH);
                const uint W = SceneTargets::HalfSize(Width, Level + 1), H = SceneTargets::HalfSize(Height, Level + 1);
                bChain = DrawPost(PP_BloomDownsample, InTargets.Bloom[Level], W, H,
                                  { Texture(From), Level == 0 && Emissive ? Emissive->Resource : nullptr }, Block);
                Block.A[2] = 0.0f;   // the threshold and the emissive, once
                Block.A[3] = 0.0f;
                From = InTargets.Bloom[Level];
                FromW = W;
                FromH = H;
            }
            Block.A[0] = std::clamp(InPost.BloomRadius, 0.0f, 1.0f);
            Block.A[3] = 0.0f;
            GDIRenderTarget* Summed = InTargets.Bloom[Levels - 1];
            for(uint32_t Level = Levels - 1; Level > 0 && bChain; --Level)
            {
                Sizes(Block, SceneTargets::HalfSize(Width, Level + 1), SceneTargets::HalfSize(Height, Level + 1));
                bChain = DrawPost(PP_BloomUpsample, InTargets.BloomUp[Level - 1], SceneTargets::HalfSize(Width, Level),
                                  SceneTargets::HalfSize(Height, Level), { Texture(Summed), Texture(InTargets.Bloom[Level - 1]) }, Block);
                Summed = InTargets.BloomUp[Level - 1];
            }
            PostBlock Composite;
            Composite.A[0] = InPost.BloomTint[0];
            Composite.A[1] = InPost.BloomTint[1];
            Composite.A[2] = InPost.BloomTint[2];
            Composite.A[3] = InPost.BloomIntensity;
            Sizes(Composite, Width, Height);
            GDIRenderTarget* Dest = Next();
            if(bChain && DrawPost(PP_BloomComposite, Dest, Width, Height, { Texture(Source), Texture(Summed) }, Composite))
            {
                Source = Dest;
                Passes += Levels * 2;
            }
        }

        RunMaterials(PPS_AfterEffects);

        /* Antialiasing, last: FXAA over everything before it -- unless TAA resolved this frame already. */
        if(bAa && !bTemporalResolved)
        {
            static const float SEARCH_STEPS[3] = { 4.0f, 8.0f, 12.0f };
            PostBlock Block;
            Block.A[0] = std::max(InPost.AaEdgeThreshold, 0.01f);
            Block.A[1] = SEARCH_STEPS[std::min<uint32_t>(InPost.AaQuality, 2)];
            Block.A[2] = 0.75f;   // sub-pixel smoothing
            Sizes(Block, Width, Height);
            GDIRenderTarget* Dest = Next();
            if(DrawPost(PP_Fxaa, Dest, Width, Height, { Texture(Source) }, Block))
            {
                Source = Dest;
                ++Passes;
            }
        }

        StageDrawCounts[size_t(ERenderStage::PostProcess)] = Passes;

        /* Left bound, as every stage leaves its target: what the present shows. */
        GDISetRenderTargets(&Source, 1, Source == InTargets.SceneColor ? Source : nullptr);
        GDISetViewport(0, 0, Width, Height);
        return Source;
    }

    RenderContext::RenderContext() : P(new Impl())
    {
    }

    RenderContext::~RenderContext()
    {
        delete P;
    }

    ERenderBackend RenderContext::GetBackend() const
    {
        return P->Backend;
    }

    uint64_t RenderContext::GetFrameNumber() const
    {
        return P->FrameNumber;
    }

    const RenderView& RenderContext::GetView() const
    {
        return P->View;
    }

    double RenderContext::GetTimeSeconds() const
    {
        return std::chrono::duration<double>(P->LastFrame - P->Start).count();
    }

    float RenderContext::GetDeltaSeconds() const
    {
        return P->FrameDelta;
    }

    // === GPU particles (ParticleSimulation.h) ======================================

    bool RenderContext::SupportsParticles() const
    {
        return GDISupportsCompute();
    }

    ParticleEmitterGpu* RenderContext::CreateParticleEmitter(const uint32_t InMaxParticles, const uint8_t InType, const char* InName)
    {
        ParticleEmitterGpu* Emitter = ParticleSimulation::Create(InMaxParticles, InType);
        if(Emitter && InName && InName[0] != '\0')
        {
            /* Its buffers as a capture tool lists them. */
            const std::string Name = std::string("Particles ") + InName;
            GDISetDebugName(Emitter->Particles, Name + " State");
            GDISetDebugName(Emitter->Instances, Name + (Emitter->InstancesB ? " Triangles A" : " Instances"));
            GDISetDebugName(Emitter->InstancesB, Name + " Triangles B");
            GDISetDebugName(Emitter->Args, Name + " Draws");
        }
        return Emitter;
    }

    void RenderContext::ReleaseParticleEmitter(ParticleEmitterGpu* InEmitter)
    {
        ParticleSimulation::Release(InEmitter);
    }

    bool RenderContext::SimulateParticles(ParticleEmitterGpu& InEmitter, const ParticleEmitterSettings& InSettings,
                                          const EngineCore::Vector3& InEmitterPosition, const float InDeltaSeconds,
                                          const char* InLabel)
    {
#if !(defined(NIMBLE_SHIPPING) && NIMBLE_SHIPPING)
        const bool bLabel = InLabel && InLabel[0] != '\0' && GRenderDrawLabels.GetBool() && GDIHasDrawEvents();
        if(bLabel) { GDIBeginDrawEvent(InLabel); }
        struct EndLabel { bool bOn; ~EndLabel() { if(bOn) { GDIEndDrawEvent(); } } } const Closer{ bLabel };
#endif
        /* Sorted for, and turned toward, the camera of the view being set up: the scene's own. */
        ParticleCamera Camera;
        Camera.Position = EngineCore::Vector3(P->Scene.CameraPosition[0], P->Scene.CameraPosition[1], P->Scene.CameraPosition[2]);
        const EngineCore::Matrix4x4& View = P->View.View;
        Camera.Right   = EngineCore::Vector3(View.Data[0][0], View.Data[0][1], View.Data[0][2]);
        Camera.Up      = EngineCore::Vector3(View.Data[1][0], View.Data[1][1], View.Data[1][2]);
        Camera.Forward = ViewForward(P->View);

        const ParticleParams Params = ParticleSimulation::BuildParams(InEmitter, InSettings, InEmitterPosition, Camera,
                                                                      InDeltaSeconds >= 0.0f ? InDeltaSeconds : P->FrameDelta,
                                                                      GetTimeSeconds());
        P->ParticleSimulationCount.fetch_add(1, std::memory_order_relaxed);
        ParticleSimulation::Shaders Shaders;
        Shaders.Simulate = P->GetComputeShader("ParticleSimulate.csh");
        if(InEmitter.Type == PT_Ribbon || InEmitter.Type == PT_Beam)
        {
            Shaders.Ribbon = P->GetComputeShader("ParticleRibbon.csh");
            Shaders.Beam   = InEmitter.Type == PT_Beam ? P->GetComputeShader("ParticleBeam.csh") : nullptr;
        }
        else
        {
            Shaders.Sort = P->GetComputeShader("ParticleSort.csh");
        }
        return ParticleSimulation::Dispatch(InEmitter, Params, Shaders);
    }

    bool RenderContext::ReadParticles(const ParticleEmitterGpu& InEmitter, std::vector<ParticleSample>& OutParticles)
    {
        OutParticles.clear();
        std::vector<uint8_t> Bytes;
        if(!InEmitter.Particles || !GDIReadBuffer(InEmitter.Particles, Bytes))
        {
            return false;
        }
        /* ParticleCommon.glsl's Particle: position and age, velocity and lifetime, its random bits. */
        static_assert(sizeof(ParticleSample) == 48, "ParticleSample mirrors the GPU's Particle");
        OutParticles.resize(std::min<size_t>(Bytes.size() / sizeof(ParticleSample), InEmitter.MaxParticles));
        std::memcpy(OutParticles.data(), Bytes.data(), OutParticles.size() * sizeof(ParticleSample));
        return true;
    }

    bool RenderContext::ReadParticleDrawCount(const ParticleEmitterGpu& InEmitter, uint32_t& OutCount)
    {
        OutCount = 0;
        std::vector<uint8_t> Bytes;
        if(!InEmitter.Args || InEmitter.ArgsEntries == 0 || !GDIReadBuffer(InEmitter.Args, Bytes) || Bytes.size() < GDI_INDIRECT_ARGS_STRIDE)
        {
            return false;
        }
        std::memcpy(&OutCount, Bytes.data() + sizeof(uint32_t), sizeof(uint32_t));   // entry 0's instance count
        return true;
    }

    bool RenderContext::ReadParticleInstances(const ParticleEmitterGpu& InEmitter, std::vector<ParticleInstanceSample>& OutInstances)
    {
        OutInstances.clear();
        std::vector<uint8_t> Bytes;
        if(!InEmitter.Instances || !GDIReadBuffer(InEmitter.Instances, Bytes))
        {
            return false;
        }
        static_assert(sizeof(ParticleInstanceSample) == GDI_INSTANCE_STRIDE, "ParticleInstanceSample mirrors GDI's instance layout");
        OutInstances.resize(std::min<size_t>(Bytes.size() / sizeof(ParticleInstanceSample), InEmitter.MaxParticles));
        std::memcpy(OutInstances.data(), Bytes.data(), OutInstances.size() * sizeof(ParticleInstanceSample));
        return true;
    }

    bool RenderContext::DrawParticles(ParticleEmitterGpu& InEmitter, const EngineCore::Vector3& InSortOrigin,
                                      const char* InMeshPath, const char* InMaterial, const char* InLabel)
    {
        if(!InEmitter.bSimulated || !InEmitter.Instances)
        {
            return false;
        }
        if(InEmitter.bIdle)
        {
            return true;   // every particle dead: nothing to draw
        }
        Material* Chosen = P->SectionMaterial(InMaterial && InMaterial[0] != '\0' ? InMaterial : nullptr, std::string());
        if(!Chosen)
        {
            return false;
        }

        /* The instances are in world space: drawn in an identity frame, sorted at the emitter.
         * Where the backend can, each draw takes its instance count from InEmitter.Args, entry
         * InFirstArgs + its section: the live count the compute passes wrote. */
        const bool bIndirect = GDISupportsIndirectDraw() && InEmitter.Args;
        const auto Draw = [&](const char* InPath, GDIBuffer* InInstances, const uint32_t InFirstArgs)
        {
            std::shared_ptr<const MeshData> Source;
            GDIMesh* Mesh = InPath && InPath[0] != '\0' ? P->FindMesh(InPath, Source) : nullptr;
            if(!Mesh || !Source || !InInstances)
            {
                return false;
            }
            static const MeshSection Everything;
            const std::vector<MeshSection>& Sections = Source->Sections;
            const size_t SectionCount = Sections.empty() ? 1 : Sections.size();

            /* A sprite's or mesh's entries, laid out per section of its mesh when it is first drawn
             * (or the mesh changes): each instance count all of them until the next simulation counts. */
            bool bArgs = bIndirect && (InEmitter.Type == PT_Ribbon || InEmitter.Type == PT_Beam);
            if(bIndirect && !bArgs)
            {
                if(InEmitter.ArgsMesh != Source.get() || InEmitter.ArgsEntries != SectionCount)
                {
                    std::vector<uint32_t> Ranges(SectionCount * 2);
                    for(size_t Index = 0; Index < SectionCount; ++Index)
                    {
                        Ranges[Index * 2]     = Sections.empty() ? uint32_t(Source->Indices.size()) : Sections[Index].IndexCount;
                        Ranges[Index * 2 + 1] = Sections.empty() ? 0u : Sections[Index].FirstIndex;
                    }
                    if(GDIBuffer* Laid = ParticleSimulation::CreateArgs(Ranges.data(), uint32_t(SectionCount), InEmitter.MaxParticles))
                    {
                        if(InEmitter.Args)
                        {
                            GDISetDebugName(Laid, InEmitter.Args->GetDebugName());
                            InEmitter.Args->Release();
                        }
                        InEmitter.Args        = Laid;
                        InEmitter.ArgsEntries = uint32_t(SectionCount);
                        InEmitter.ArgsMesh    = Source.get();
                    }
                }
                bArgs = InEmitter.ArgsMesh == Source.get();
            }
            for(size_t Index = 0; Index < SectionCount; ++Index)
            {
                const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];
                DrawCommand Item;
                Item.Kind          = DrawCommand::EKind::MeshSection;
                Item.Mat           = Chosen;
                Item.Mesh          = Mesh;
                Item.Label         = InLabel;
                Item.First         = Section.FirstIndex;
                Item.Count         = Sections.empty() ? 0u : Section.IndexCount;
                Item.Instances     = InInstances;
                Item.InstanceCount = InEmitter.MaxParticles;
                if(bArgs)
                {
                    Item.IndirectArgs   = InEmitter.Args;
                    Item.IndirectOffset = uint32_t(InFirstArgs + Index) * GDI_INDIRECT_ARGS_STRIDE;
                }
                Item.SortOrigin    = InSortOrigin;
                P->Submit(Item, Impl::StageOf(Chosen));
            }
            return true;
        };

        /* A ribbon or beam is its two triangles a segment (ParticleRibbon.csh); the mesh named is not used. */
        if(InEmitter.Type == PT_Ribbon || InEmitter.Type == PT_Beam)
        {
            const bool bA = Draw(PARTICLE_RIBBON_MESH_A, InEmitter.Instances, 0);
            const bool bB = Draw(PARTICLE_RIBBON_MESH_B, InEmitter.InstancesB, 1);
            return bA && bB;
        }
        return Draw(InMeshPath, InEmitter.Instances, 0);
    }

    void RenderContext::DrawQuad(const RenderTransform& InTransform, const float InWidth, const float InHeight,
                                  const EngineCore::Color InTint)
    {
        DrawCommand Item;
        Item.Kind      = DrawCommand::EKind::Quad;
        Item.Transform = InTransform;
        Item.Width     = InWidth;
        Item.Height    = InHeight;
        Item.Tint      = InTint;
        P->Submit(Item, ERenderStage::Unlit);
    }

    void RenderContext::DrawQuadWithMaterial(const RenderTransform& InTransform, const float InWidth, const float InHeight,
                                             const EngineCore::Color InTint, const char* InMaterial,
                                             const MaterialParameterOverrides& InOverrides)
    {
        const std::string Name = InMaterial ? std::string(InMaterial) : std::string();
        Material& Found = P->FindMaterial(Name);
        if(Found.Pixel && Found.Description.Domain == EMaterialDomain::PostProcess)
        {
            P->WarnOnce(Name + "|post-on-quad", "material '" + Name + "' is a PostProcess material: a Post Process "
                        "component draws it over the scene; this quad draws the plain tint");
        }
        if(!Found.Pixel || Found.Description.Domain == EMaterialDomain::PostProcess)
        {
            DrawQuad(InTransform, InWidth, InHeight, InTint);
            return;
        }

        DrawCommand Item;
        Item.Kind      = DrawCommand::EKind::MaterialQuad;
        Item.Transform = InTransform;
        Item.Width     = InWidth;
        Item.Height    = InHeight;
        Item.Tint      = InTint;
        Item.Mat       = &Found;
        Item.Overrides     = InOverrides.Items;
        Item.OverrideCount = InOverrides.Count;
        Item.Baked         = InOverrides.Baked;
        Item.MaterialSlot  = 0;   // a quad draws slot 0
        P->Submit(Item, Impl::StageOf(&Found));
    }

    bool RenderContext::DrawMesh(const RenderTransform& InTransform, const char* InMeshPath,
                                 const EngineCore::Color InTint, const char* InMaterial)
    {
        const char* Every[1] = { InMaterial };
        return DrawMeshSections(InTransform, InMeshPath, InTint, Every, 1, /*bInEvery*/ true, MaterialParameterOverrides());
    }

    bool RenderContext::DrawMesh(const RenderTransform& InTransform, const char* InMeshPath,
                                 const EngineCore::Color InTint, const char* const* InSlots, const uint32_t InSlotCount,
                                 const MaterialParameterOverrides& InOverrides)
    {
        return DrawMeshSections(InTransform, InMeshPath, InTint, InSlots, InSlotCount, /*bInEvery*/ false, InOverrides);
    }

    bool RenderContext::DrawMeshSections(const RenderTransform& InTransform, const char* InMeshPath,
                                         const EngineCore::Color InTint, const char* const* InSlots,
                                         const uint32_t InSlotCount, const bool bInEvery,
                                         const MaterialParameterOverrides& InOverrides)
    {
        if(!InMeshPath || InMeshPath[0] == '\0')
        {
            return false;
        }
        if(InOverrides.Instances && (InOverrides.InstanceCount == 0 || !InOverrides.Instances->Buffer))
        {
            return true;   // no instances: nothing to draw
        }

        std::shared_ptr<const MeshData> Source;
        const std::vector<float>* WorldPerUV = nullptr;
        GDIMesh* Mesh = P->FindMesh(InMeshPath, Source, &WorldPerUV);
        if(!Mesh || !Source)
        {
            return false;
        }
        /* TEXTURE STREAMING: its largest scale stretches its UVs as far. */
        const EngineCore::Vector3& Scale = InTransform.Scale;
        const float MaxScale = float(std::max({ std::abs(double(Scale.GetX())), std::abs(double(Scale.GetY())), std::abs(double(Scale.GetZ())) }));

        /* A skinned mesh nothing poses draws in its reference pose. */
        const float* SkinPalette = InOverrides.SkinPalette;
        uint32_t     SkinBones   = InOverrides.SkinBones;
        if(!SkinPalette && Source->IsSkinned())
        {
            if(const std::vector<float>* RefPalette = P->FindRefPalette(InMeshPath))
            {
                SkinPalette = RefPalette->data();
                SkinBones   = uint32_t(RefPalette->size() / 12);
            }
        }

        /* A section per material; a mesh with none is one section of everything. */
        static const MeshSection Everything;
        const std::vector<MeshSection>& Sections = Source->Sections;
        const size_t SectionCount = Sections.empty() ? 1 : Sections.size();

        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];

            /* The slot, else the section's material, else DefaultLit, else Mesh_PS (lit tint). */
            const size_t SlotIndex = bInEvery ? 0 : Index;
            const char*  Slot      = InSlots && SlotIndex < InSlotCount && InSlots[SlotIndex] && InSlots[SlotIndex][0] != '\0'
                                   ? InSlots[SlotIndex] : nullptr;
            Material* Chosen = P->SectionMaterial(Slot, Section.Material);

            DrawCommand Item;
            Item.Kind      = DrawCommand::EKind::MeshSection;
            Item.Transform = InTransform;
            Item.Tint      = InTint;
            Item.Mat       = Chosen;
            Item.Mesh      = Mesh;
            Item.Section   = Section.Name.c_str();
            Item.First     = Section.FirstIndex;
            Item.Count     = Sections.empty() ? 0u : Section.IndexCount;
            Item.Overrides     = InOverrides.Items;
            Item.OverrideCount = InOverrides.Count;
            Item.Baked         = InOverrides.Baked;
            Item.SkinPalette   = SkinPalette;
            Item.SkinBones     = SkinBones;
            Item.PrevSkinPalette = SkinPalette == InOverrides.SkinPalette ? InOverrides.PrevSkinPalette : nullptr;
            Item.MaterialSlot  = uint8_t(SlotIndex);   // the section's slot, whichever material it fell back to
            Item.WorldPerUV    = WorldPerUV && Index < WorldPerUV->size() ? (*WorldPerUV)[Index] * MaxScale : 0.0f;
            if(InOverrides.Instances)
            {
                Item.Instances     = InOverrides.Instances->Buffer;
                Item.InstanceCount = std::min(InOverrides.InstanceCount, InOverrides.Instances->Count);
                Item.SortOrigin    = InTransform.Position;
            }
            P->Submit(Item, Impl::StageOf(Chosen));
        }
        return true;
    }

    bool RenderContext::SupportsInstancing() const
    {
        return GDISupportsCompute();
    }

    RenderInstanceBuffer* RenderContext::CreateInstanceBuffer(const RenderInstance* InInstances, const uint32_t InCount, const char* InName)
    {
        static_assert(sizeof(RenderInstance) == GDI_INSTANCE_STRIDE, "RenderInstance is GDIBuffer.h's instance layout");
        if(!InInstances || InCount == 0 || !GDISupportsCompute())
        {
            return nullptr;
        }
        GDIBuffer* Buffer = GDICreateBuffer(InCount * GDI_INSTANCE_STRIDE, GDI_BU_Instance, InInstances);
        if(!Buffer)
        {
            return nullptr;
        }
        if(InName && InName[0] != '\0') { GDISetDebugName(Buffer, InName); }
        auto* Out   = new RenderInstanceBuffer();
        Out->Buffer = Buffer;
        Out->Count  = InCount;
        return Out;
    }

    void RenderContext::ReleaseInstanceBuffer(RenderInstanceBuffer* InBuffer)
    {
        if(!InBuffer) { return; }
        if(InBuffer->Buffer) { InBuffer->Buffer->Release(); }   // deferred past the frames still reading it
        delete InBuffer;
    }

    void RenderContext::ReleaseMesh(const char* InMeshPath)
    {
        if(!InMeshPath || InMeshPath[0] == '\0') { return; }
        const auto Found = P->Meshes.find(InMeshPath);
        if(Found == P->Meshes.end()) { return; }
        delete Found->second.Gpu;   // each backend defers the buffers past any frame still using them
        P->Meshes.erase(Found);
        ++P->ContentGeneration;     // static draw lists hold the pointer
    }

    float RenderContext::GetMaterialMaxOffset(const char* InMaterial, const bool bInQuietIfMissing)
    {
        if(!InMaterial || InMaterial[0] == '\0')
        {
            return 0.0f;
        }
        const Material& Found = P->FindMaterial(InMaterial, bInQuietIfMissing);
        return Found.Pixel && Found.Description.bHasOffset ? Found.Description.MaxOffset : 0.0f;
    }

#if WITH_EDITOR
    void RenderContext::DrawLine(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd,
                                 const EngineCore::Color InColor, const float InThicknessPixels)
    {
        using EngineCore::Vector3;

        const RenderView& View = P->View;
        const Vector3 Delta  = InEnd - InStart;
        const double  Length = Delta.GetLength();
        if(Length < 1e-6 || View.ViewportPixels.Height <= 0)
        {
            return;
        }
        const Vector3 Along = Delta * (1.0 / Length);
        const Vector3 Mid   = (InStart + InEnd) * 0.5;

        /* World units per pixel at the segment's depth. Data[1][1] is the Y
         * focal scale in both projections; only perspective varies with
         * depth. View row 2 is forward (LookAt). */
        const EngineCore::Matrix4x4& Proj = View.Projection;
        const Vector3 Forward = ViewForward(View);
        const bool    bOrthographic = IsOrthographic(View);
        const double  FocalY        = std::abs(double(Proj.Data[1][1]));
        if(FocalY < 1e-12)
        {
            return;
        }
        double WorldPerPixel = 2.0 / (FocalY * double(View.ViewportPixels.Height));
        if(!bOrthographic)
        {
            const double Depth = (Mid - View.Origin).Dot(Forward);
            if(Depth <= double(View.ZNear))
            {
                return;   // behind or in the near plane -- clipped anyway
            }
            WorldPerPixel *= Depth;
        }
        const double Width = double(InThicknessPixels) * WorldPerPixel;

        /* Width axis: perpendicular to segment and sight line. */
        const Vector3 Sight = bOrthographic ? Forward : (Mid - View.Origin);
        Vector3 Side = Along ^ Sight;
        const double SideLength = Side.GetLength();
        if(SideLength < 1e-9 * Sight.GetLength())
        {
            return;
        }
        Side = Side * (1.0 / SideLength);
        const Vector3 Normal = Along ^ Side;

        /* Unit quad -> ribbon: X along the segment (plus caps), Y across,
         * translated to the midpoint. */
        const Vector3 AxisX = Along * (Length + Width);
        const Vector3 AxisY = Side * Width;

        EngineCore::Matrix4x4 Segment;
        Segment.Data[0][0] = AxisX.GetX(); Segment.Data[0][1] = AxisY.GetX(); Segment.Data[0][2] = Normal.GetX(); Segment.Data[0][3] = Mid.GetX();
        Segment.Data[1][0] = AxisX.GetY(); Segment.Data[1][1] = AxisY.GetY(); Segment.Data[1][2] = Normal.GetY(); Segment.Data[1][3] = Mid.GetY();
        Segment.Data[2][0] = AxisX.GetZ(); Segment.Data[2][1] = AxisY.GetZ(); Segment.Data[2][2] = Normal.GetZ(); Segment.Data[2][3] = Mid.GetZ();
        Segment.Data[3][0] = 0.0f;         Segment.Data[3][1] = 0.0f;         Segment.Data[3][2] = 0.0f;          Segment.Data[3][3] = 1.0f;

        P->EnsureInitialised();
        P->Bind(GDIGetShader<Basic_VS>(), GDIGetShader<Basic_PS>());

        /* Model is identity; View * Segment places the quad. Uniforms are
         * per draw, so later DrawQuads are unaffected. */
        P->View3D->Camera->Projection = View.Projection;
        P->View3D->Camera->ViewMat    = View.View * Segment;

        GDITransform* Transform = P->ViewObject->Transform;
        Transform->Translate = Vector3(0.0, 0.0, 0.0);
        Transform->Rotate    = Vector3(0.0, 0.0, 0.0);
        Transform->Scale     = Vector3(1.0, 1.0, 1.0);

        if(GDIShader* Pixel = GDIGetShader<Basic_PS>())
        {
            Pixel->SetUniform("TestColor", Vector3(InColor.Rf(), InColor.Gf(), InColor.Bf()));
        }

        GDIDrawObject(P->ViewObject);

        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
    }
#endif // WITH_EDITOR

    void RenderContext::BeginDrawEvent(const char* InName)
    {
        GDIBeginDrawEvent(InName ? InName : "");
    }

    void RenderContext::EndDrawEvent()
    {
        GDIEndDrawEvent();
    }

    uint64_t RenderContext::GetParticleSimulationCount() const
    {
        return P->ParticleSimulationCount.load(std::memory_order_relaxed);
    }

    uint64_t RenderContext::GetDrawCallCount() const
    {
        return P->DrawCallCount.load(std::memory_order_relaxed);
    }

    uint32_t RenderContext::GetStageDrawCount(const ERenderStage InStage) const
    {
        return InStage < ERenderStage::Count ? P->StageDrawCounts[size_t(InStage)] : 0u;
    }

    void RenderContext::SetBackend(const ERenderBackend InBackend)
    {
        P->Backend = InBackend;
    }

    void RenderContext::SetFrameNumber(const uint64_t InFrameNumber)
    {
        P->FrameNumber = InFrameNumber;
        P->BoundVertex = nullptr;
        P->BoundPixel  = nullptr;

        const auto Now = std::chrono::steady_clock::now();
        P->FrameDelta = std::chrono::duration<float>(Now - P->LastFrame).count();
        P->LastFrame  = Now;
    }

    void RenderContext::SetView(const RenderView& InView)
    {
        P->View = InView;
        P->UpdateScene();
    }

    void RenderContext::SetLighting(const RenderLighting& InLighting)
    {
        P->Lighting = InLighting;
        P->UpdateScene();
    }

    void RenderContext::BeginSceneDraws()
    {
        for(DrawPolicy& Policy : P->Policies) { Policy.Clear(); }
        P->ForgetOcclusion();   // a scene's own, set again by RenderScene
        P->bDeferring        = true;
        P->DrawPixelsPerWorld = 0.0f;   // a scene's RenderScene sets it per proxy
        P->Shadows.ViewCount = 0;   // a preview has none; a scene's PrepareShadows adds them
        P->CasterView        = -1;
    }

    uint32_t RenderContext::PrepareShadows(const InsetShadowCaster* InCasters, const uint32_t InCasterCount,
                                           const GameCore::AABB* InReceivers)
    {
        P->PrepareShadows(InCasters, InCasterCount, InReceivers);
        return P->Shadows.ViewCount;
    }

    int32_t RenderContext::GetShadowViewInset(const uint32_t InView) const
    {
        return InView < P->Shadows.ViewCount ? P->Shadows.Views[InView].Inset : -1;
    }

    bool RenderContext::IsSunShadowView(const uint32_t InView) const
    {
        return InView < P->Shadows.ViewCount && P->Shadows.IsSunView(InView);
    }

    const GameCore::AABB& RenderContext::GetShadowViewBounds(const uint32_t InView) const
    {
        static const GameCore::AABB None;
        return InView < P->Shadows.ViewCount ? P->Shadows.Views[InView].WorldBounds : None;
    }

    void RenderContext::SetShadowCasterView(const int32_t InView)
    {
        P->CasterView = InView >= 0 && uint32_t(InView) < P->Shadows.ViewCount ? InView : -1;
    }

    bool RenderContext::Impl::QueueDepthSections(const RenderTransform& InTransform, const char* InMeshPath,
                                                 const char* const* InSlots, const uint32_t InSlotCount,
                                                 const MaterialParameterOverrides& InOverrides, const EDepthSections InWhich,
                                                 const ERenderStage InStage, const int32_t InShadowView)
    {
        Impl& P = *this;
        if(InOverrides.Instances && (InOverrides.InstanceCount == 0 || !InOverrides.Instances->Buffer))
        {
            return true;   // no instances: nothing to draw
        }
        std::shared_ptr<const MeshData> Source;
        GDIMesh* Mesh = P.FindMesh(InMeshPath, Source);
        if(!Mesh || !Source)
        {
            return false;
        }

        /* A skinned mesh nothing poses draws in its reference pose. */
        const float* SkinPalette = InOverrides.SkinPalette;
        uint32_t     SkinBones   = InOverrides.SkinBones;
        if(!SkinPalette && Source->IsSkinned())
        {
            if(const std::vector<float>* RefPalette = P.FindRefPalette(InMeshPath))
            {
                SkinPalette = RefPalette->data();
                SkinBones   = uint32_t(RefPalette->size() / 12);
            }
        }

        static const MeshSection Everything;
        const std::vector<MeshSection>& Sections = Source->Sections;
        const size_t SectionCount = Sections.empty() ? 1 : Sections.size();
        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];
            const char* Slot = InSlots && Index < InSlotCount && InSlots[Index] && InSlots[Index][0] != '\0' ? InSlots[Index] : nullptr;
            RenderMaterial* Chosen = P.SectionMaterial(Slot, Section.Material);

            /* Only what writes depth: Opaque, and Masked if asked. The plain fallback is opaque. */
            const EMaterialDomain Domain = Chosen ? Chosen->Description.Domain : EMaterialDomain::Opaque;
            if(Domain != EMaterialDomain::Opaque && (InWhich == EDepthSections::Opaque || Domain != EMaterialDomain::Masked))
            {
                continue;
            }

            DrawCommand Item;
            Item.Kind       = DrawCommand::EKind::MeshSection;
            Item.Transform  = InTransform;
            Item.Mat        = Chosen;
            Item.Mesh       = Mesh;
            Item.Section    = Section.Name.c_str();
            Item.First      = Section.FirstIndex;
            Item.Count      = Sections.empty() ? 0u : Section.IndexCount;
            Item.ShadowView    = InShadowView;
            Item.Overrides     = InOverrides.Items;
            Item.OverrideCount = InOverrides.Count;
            Item.SkinPalette   = SkinPalette;
            Item.SkinBones     = SkinBones;
            Item.MaterialSlot  = uint8_t(Index);
            if(InOverrides.Instances)
            {
                Item.Instances     = InOverrides.Instances->Buffer;
                Item.InstanceCount = std::min(InOverrides.InstanceCount, InOverrides.Instances->Count);
                Item.SortOrigin    = InTransform.Position;
            }
            if(P.Capture && InStage == ERenderStage::Shadows) { P.Capture->push_back({ Item, InStage }); }
            else                                              { P.PolicyFor(InStage).Add(Item); }
        }
        return true;
    }

    bool RenderContext::DrawMeshShadow(const RenderTransform& InTransform, const char* InMeshPath,
                                       const char* const* InSlots, const uint32_t InSlotCount,
                                       const MaterialParameterOverrides& InOverrides)
    {
        if(P->CasterView < 0 || !P->bDeferring || !InMeshPath || InMeshPath[0] == '\0')
        {
            return false;
        }
        return P->QueueDepthSections(InTransform, InMeshPath, InSlots, InSlotCount, InOverrides,
                                  EDepthSections::OpaqueAndMasked, ERenderStage::Shadows, P->CasterView);
    }

    bool RenderContext::DrawMeshOccluder(const RenderTransform& InTransform, const char* InMeshPath,
                                         const char* const* InSlots, const uint32_t InSlotCount,
                                         const MaterialParameterOverrides& InOverrides)
    {
        if(!P->bGatheringOccluders || !P->bDeferring || !InMeshPath || InMeshPath[0] == '\0')
        {
            return false;
        }
        return P->QueueDepthSections(InTransform, InMeshPath, InSlots, InSlotCount, InOverrides,
                                  EDepthSections::Opaque, ERenderStage::PrePass, -1);
    }

    void RenderContext::SetOcclusionQueries(GDIOcclusionQueries* InQueries)
    {
        P->ForgetOcclusion();
        P->OcclusionQueries = InQueries;
    }

    int32_t RenderContext::AddOcclusionQuery(const GameCore::AABB& InBox)
    {
        if(!P->OcclusionQueries || InBox.IsEmpty() || P->OcclusionBoxes.size() >= P->OcclusionQueries->GetCapacity())
        {
            return -1;
        }
        P->OcclusionBoxes.push_back(InBox);
        return int32_t(P->OcclusionBoxes.size() - 1);
    }

    void RenderContext::SetDrawOcclusion(const int32_t InQuery)
    {
        P->DrawOcclusion = P->OcclusionQueries && InQuery >= 0 && size_t(InQuery) < P->OcclusionBoxes.size() ? InQuery : -1;
    }

    void RenderContext::SetGatheringOccluders(const bool bInGathering)
    {
        P->bGatheringOccluders = bInGathering && P->OcclusionQueries;
    }

    bool RenderContext::CanPredicateOcclusion() const
    {
        return GDISupportsOcclusionPredication();
    }

    SceneTargets* RenderContext::BeginTargetCapture(const char* InPath, const uint32_t InResolution, const ERenderTargetKind InKind,
                                                    EngineCore::Color& OutClear)
    {
        const std::string Path = InPath ? InPath : "";
        Impl::GpuRenderTarget* Entry = Path.empty() ? nullptr : P->FindRenderTarget(Path);
        if(!Entry)
        {
            P->WarnOnce("|capture-target|" + Path, "scene capture: '" + Path + "' is not a render target (an .nmrt2d or .nmrt3d); "
                                                   "the probe captures nothing");
            return nullptr;
        }
        if(Entry->Source->Desc.Kind != InKind)
        {
            P->WarnOnce("|capture-kind|" + Path, "scene capture: '" + Path + (InKind == ERenderTargetKind::Cube
                        ? "' is a RenderTargetTex2D; an omnidirectional probe needs a RenderTargetTex3D (.nmrt3d)"
                        : "' is a RenderTargetTex3D; a planar mirror needs a RenderTargetTex2D (.nmrt2d)"));
            return nullptr;
        }

        /* The capture's size: Resolution, keeping a 2D target's aspect. */
        RenderTargetDesc Desc = Entry->Source->Desc;
        if(InResolution > 0)
        {
            const uint32_t Width = std::min<uint32_t>(InResolution, RENDER_TARGET_MAX_SIZE);
            Desc.Height = Desc.Kind == ERenderTargetKind::Cube
                        ? Width : std::max<uint32_t>(1u, uint32_t(double(Width) * double(Desc.Height) / double(Desc.Width) + 0.5));
            Desc.Width  = Width;
        }
        const uint32_t TextureWidth  = RenderTargetTextureWidth(Desc);
        const uint32_t TextureHeight = RenderTargetTextureHeight(Desc);
        if(!Entry->Target)
        {
            Entry->Target = GDICreateRenderTarget("RenderCore.Capture." + Path, TextureWidth, TextureHeight);
            Entry->bCaptured = false;
        }
        else if(Entry->Desc.Width != Desc.Width || Entry->Desc.Height != Desc.Height)
        {
            Entry->Target->Resize(TextureWidth, TextureHeight);
            Entry->bCaptured = false;
        }
        Entry->Desc = Desc;
        if(!Entry->Target || !Entry->Target->IsValid())
        {
            P->WarnOnce("|capture-gpu|" + Path, "scene capture: this backend cannot make the render target '" + Path + "'");
            return nullptr;
        }
        if(!Entry->Capture)
        {
            Entry->Capture = new SceneTargets("RenderCore.CaptureStages." + Path);
        }
        if(!Entry->Capture->Allocate(Desc.Width, Desc.Height))
        {
            return nullptr;
        }
        OutClear = Desc.Clear;
        return Entry->Capture;
    }

    void RenderContext::FinishTargetFace(const char* InPath, const uint32_t InFace)
    {
        const auto Found = P->RenderTargets.find(InPath ? InPath : "");
        if(Found == P->RenderTargets.end() || !Found->second.Target || !Found->second.Capture)
        {
            return;
        }
        Impl::GpuRenderTarget& Entry = Found->second;
        GDIRenderTarget* Rendered = Entry.Capture->Final ? Entry.Capture->Final : Entry.Capture->SceneColor;
        GDITexture2D*    Texture  = Rendered ? Rendered->ToGDITexture2D() : nullptr;
        if(!Texture)
        {
            return;
        }
        PostBlock Block;
        const uint32_t X = Entry.Desc.Kind == ERenderTargetKind::Cube ? InFace * Entry.Desc.Width : 0;
        if(P->DrawPost(PP_CaptureCopy, Entry.Target, Entry.Desc.Width, Entry.Desc.Height, { Texture->Resource }, Block, false, X))
        {
            Entry.bCaptured = true;
        }
    }

    bool RenderContext::ReadRenderTarget(const char* InPath, EngineCore::Image& OutPixels)
    {
        Impl::GpuRenderTarget* Entry = P->FindRenderTarget(InPath ? InPath : "");
        if(!Entry)
        {
            return false;
        }
        if(Entry->bCaptured && Entry->Target)
        {
            OutPixels = Entry->Target->ToImage();
            return OutPixels.GetWidth() > 0;
        }
        OutPixels = Entry->Source->Baked;
        return !OutPixels.IsNull();
    }

    void RenderContext::BeginCapture(std::vector<StagedDraw>* InOut)
    {
        P->Capture = InOut;
    }

    void RenderContext::EndCapture()
    {
        P->Capture = nullptr;
    }

    void RenderContext::SetDrawStreamingScale(const float InPixelsPerWorldUnit)
    {
        P->DrawPixelsPerWorld = InPixelsPerWorldUnit;
    }

    void RenderContext::SetDrawReveal(const bool bInReveal)
    {
        P->bDrawReveal = bInReveal;
    }

    void RenderContext::UpdateTextureStreaming()
    {
        if(P->StreamUpdatedFrame == P->FrameNumber) { return; }
        P->StreamUpdatedFrame = P->FrameNumber;
        P->FreeRetiredStreamed(false);
        const bool bWas = P->bStreamingThisFrame;
        P->bStreamingThisFrame = TextureStreamer::IsEnabled();
        if(bWas && !P->bStreamingThisFrame)
        {
            /* Turned off: forget the entries; materials find them again if it comes back. */
            P->Streamer.ReleaseAll();
            ++P->StreamEpoch;
        }
        if(P->bStreamingThisFrame) { P->Streamer.Update(); }
    }

    void RenderContext::FlushTextureStreaming()
    {
        if(TextureStreamer::IsEnabled()) { P->Streamer.Flush(); }
    }

    TextureStreamingStats RenderContext::GetTextureStreamingStats() const
    {
        return P->Streamer.GetStats();
    }

    void RenderContext::SetDrawMotion(const EngineCore::Matrix4x4* InMotion)
    {
        P->bDrawMotion = InMotion != nullptr;
        if(InMotion) { P->DrawMotion = *InMotion; }
    }

    void RenderContext::SetDrawCustomGroups(const uint8_t InGroups)
    {
        P->DrawCustomGroups = InGroups;
    }

    void RenderContext::SetCustomPassWorld(const void* InWorld)
    {
        P->PassWorld = InWorld;
    }

    // === Custom render passes (CustomRenderPass.h) ===========================

    /* What a RenderPassContext reads and draws with: the stages under way. */
    struct RenderPassContext::Frame
    {
        RenderContext::Impl*   P       = nullptr;
        SceneTargets*          Targets = nullptr;
        const CustomPassEntry* Entry   = nullptr;
        ERenderHook            Hook    = ERenderHook::BeforeScene;
        bool                   bMainView = false;
        GDIRenderTarget**      Current = nullptr;   // the post hooks' picture

        const char* PassName() const { return Entry && Entry->Pass ? Entry->Pass->GetName() : "?"; }
    };

    bool RenderContext::Impl::RunHook(const ERenderHook InHook, SceneTargets& InTargets, const bool bInMainView,
                                      GDIRenderTarget** InOutCurrent)
    {
        bool bRan = false;
        for(const CustomPassEntry& Entry : FramePasses)
        {
            if(Entry.Hook != InHook || !Entry.Pass)
            {
                continue;
            }
            RenderPassContext::Frame Frame;
            Frame.P         = this;
            Frame.Targets   = &InTargets;
            Frame.Entry     = &Entry;
            Frame.Hook      = InHook;
            Frame.bMainView = bInMainView;
            Frame.Current   = InOutCurrent;
            RenderPassContext Context(Frame);
            if(!Entry.Pass->ShouldRun(Context))
            {
                continue;
            }
            {
                SCOPED_DRAW_EVENT(std::string("Custom:") + Frame.PassName());
                Entry.Pass->Execute(Context);
            }
            for(uint32_t Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
            bRan = true;
        }
        return bRan;
    }

    namespace
    {
        GDITextureResource* PassTexture(const RenderPassTexture InTexture)
        {
            return static_cast<GDITextureResource*>(const_cast<void*>(InTexture.Handle));
        }
        GDIRenderTarget* PassTargetOf(const RenderPassTarget InTarget)
        {
            return static_cast<GDIRenderTarget*>(InTarget.Handle);
        }
        RenderPassTexture ColorOf(GDIRenderTarget* InTarget)
        {
            GDITexture2D* Texture = InTarget ? InTarget->ToGDITexture2D() : nullptr;
            RenderPassTexture Out;
            Out.Handle = Texture ? Texture->Resource : nullptr;
            return Out;
        }
        RenderPassTexture DepthOf(GDIRenderTarget* InTarget)
        {
            GDITexture2D* Texture = InTarget ? InTarget->ToDepthTexture2D() : nullptr;
            RenderPassTexture Out;
            Out.Handle = Texture ? Texture->Resource : nullptr;
            return Out;
        }
        EngineCore::Color ClearColorOf(const float (&InColor)[4])
        {
            const auto Byte = [](const float InValue) { return uint8_t(std::clamp(InValue, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return EngineCore::Color(Byte(InColor[0]), Byte(InColor[1]), Byte(InColor[2]), Byte(InColor[3]));
        }
    }

    ERenderHook       RenderPassContext::GetHook() const        { return F->Hook; }
    const RenderView& RenderPassContext::GetView() const        { return F->P->View; }
    uint32_t          RenderPassContext::GetWidth() const       { return F->Targets->Width; }
    uint32_t          RenderPassContext::GetHeight() const      { return F->Targets->Height; }
    bool              RenderPassContext::IsMainView() const     { return F->bMainView; }
    uint32_t          RenderPassContext::GetViewIndex() const   { return F->P->View.ViewIndex; }
    bool              RenderPassContext::IsHDR() const          { return F->Targets->bHDR; }
    uint64_t          RenderPassContext::GetFrameNumber() const { return F->P->FrameNumber; }
    float             RenderPassContext::GetDeltaSeconds() const { return F->P->FrameDelta; }

    RenderPassTexture RenderPassContext::GetSceneTexture(const ESceneTexture InTexture) const
    {
        const SceneTargets& Targets = *F->Targets;
        /* The G-buffer and velocity only once this frame's Base Pass wrote them. */
        const bool bWritten = F->Hook >= ERenderHook::AfterBasePass;
        switch(InTexture)
        {
            case ESceneTexture::SceneColor: return ColorOf(Targets.SceneColor);
            case ESceneTexture::SceneDepth: return DepthOf(Targets.SceneColor);
            case ESceneTexture::GBuffer0:
            case ESceneTexture::GBuffer1:
            case ESceneTexture::GBuffer2:
            case ESceneTexture::GBuffer3:
            case ESceneTexture::GBuffer4:
                return bWritten && F->P->bGBufferThisFrame
                     ? ColorOf(Targets.GBuffer[size_t(InTexture) - size_t(ESceneTexture::GBuffer0)]) : RenderPassTexture();
            case ESceneTexture::Velocity:
                return bWritten && F->P->bVelocityThisFrame ? ColorOf(Targets.Velocity) : RenderPassTexture();
            case ESceneTexture::Current:
                return ColorOf(F->Current && *F->Current ? *F->Current : Targets.SceneColor);
            case ESceneTexture::Count: break;
        }
        return RenderPassTexture();
    }

    RenderPassTarget RenderPassContext::AcquireTarget(const char* InName, const RenderPassTargetDesc& InDesc)
    {
        RenderPassTarget Out;
        if(!InName || InName[0] == '\0' || !F->Entry)
        {
            return Out;
        }
        const SceneTargets& Targets = *F->Targets;
        const float    Scale  = InDesc.Scale > 0.0f ? InDesc.Scale : 1.0f;
        const uint32_t Width  = InDesc.Width  > 0 ? InDesc.Width  : std::max<uint32_t>(1u, uint32_t(float(Targets.Width) * Scale + 0.5f));
        const uint32_t Height = InDesc.Height > 0 ? InDesc.Height : std::max<uint32_t>(1u, uint32_t(float(Targets.Height) * Scale + 0.5f));
        if(Width == 0 || Height == 0 || Width > RENDER_TARGET_MAX_SIZE || Height > RENDER_TARGET_MAX_SIZE)
        {
            return Out;
        }

        ERenderPassFormat Format = InDesc.Format;
        if(Format == ERenderPassFormat::RGBA16F && !GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F))
        {
            F->P->WarnOnce(std::string("|pass-format|") + F->PassName(), std::string("custom render pass '") + F->PassName()
                           + "': this backend has no half float targets; they are RGBA8");
            Format = ERenderPassFormat::RGBA8;
        }

        RenderContext::Impl::PassTarget& Entry = F->P->PassPoolsOfView()[F->Entry->Handle].Targets[InName];
        if(Entry.Target && Entry.Format != Format)
        {
            GDIReleaseRenderTarget(Entry.Target);
            delete Entry.Target;
            Entry.Target = nullptr;
        }
        if(!Entry.Target)
        {
            const std::string Name = std::string("RenderCore.Pass.") + F->PassName() + "." + InName;
            Entry.Target = Format == ERenderPassFormat::RGBA16F ? GDICreateRenderTargetFormat(Name, Width, Height, GDI_RTF_RGBA16F)
                                                                : GDICreateRenderTarget(Name, Width, Height);
            Entry.Format = Format;
        }
        else if(Entry.Target->GetWidth() != Width || Entry.Target->GetHeight() != Height)
        {
            Entry.Target->Resize(Width, Height);
        }
        if(Entry.Target && Entry.Target->IsValid())
        {
            Out.Handle = Entry.Target;
        }
        return Out;
    }

    RenderPassTarget RenderPassContext::AcquireAssetTarget(const char* InPath)
    {
        RenderPassTarget Out;
        const std::string Path = InPath ? InPath : "";
        RenderContext::Impl::GpuRenderTarget* Entry = Path.empty() ? nullptr : F->P->FindRenderTarget(Path);
        if(!Entry || Entry->Source->Desc.Kind != ERenderTargetKind::Tex2D)
        {
            F->P->WarnOnce("|pass-asset|" + Path, std::string("custom render pass '") + F->PassName() + "': '" + Path
                           + "' is not a 2D render target (an .nmrt2d); it draws nothing there");
            return Out;
        }
        const RenderTargetDesc& Desc = Entry->Source->Desc;
        if(!Entry->Target)
        {
            Entry->Target = GDICreateRenderTarget("RenderCore.Capture." + Path, Desc.Width, Desc.Height);
        }
        else if(Entry->Desc.Width != Desc.Width || Entry->Desc.Height != Desc.Height)
        {
            Entry->Target->Resize(Desc.Width, Desc.Height);
        }
        Entry->Desc = Desc;
        if(!Entry->Target || !Entry->Target->IsValid())
        {
            return Out;
        }
        /* What materials sample from now: this target, as a capture's would be. */
        Entry->bCaptured = true;
        Out.Handle = Entry->Target;
        return Out;
    }

    RenderPassTexture RenderPassContext::AsTexture(const RenderPassTarget InTarget) const
    {
        return ColorOf(PassTargetOf(InTarget));
    }

    RenderPassTexture RenderPassContext::AsDepthTexture(const RenderPassTarget InTarget) const
    {
        return DepthOf(PassTargetOf(InTarget));
    }

    bool RenderPassContext::Clear(const RenderPassTarget InTarget, const float (&InColor)[4], const bool bInDepth)
    {
        GDIRenderTarget* Target = PassTargetOf(InTarget);
        if(!Target)
        {
            return false;
        }
        GDISetRenderTargets(&Target, 1, bInDepth ? Target : nullptr);
        GDISetViewport(0, 0, Target->GetWidth(), Target->GetHeight());
        GDISetColorMask(EColM_RGBA);
        GDIClear(bInDepth ? (CM_Color | CM_Depth) : CM_Color, ClearColorOf(InColor));
        return true;
    }

    bool RenderPassContext::DrawFullscreen(const char* InPixelShader, const RenderPassTarget InTarget,
                                           const std::initializer_list<RenderPassTexture> InInputs, const RenderPassParams& InParams,
                                           const ERenderPassBlend InBlend)
    {
        GDIRenderTarget* Target = InTarget.IsValid() ? PassTargetOf(InTarget) : F->Targets->SceneColor;
        if(!InPixelShader || !Target)
        {
            return false;
        }
        if(InInputs.size() > RENDER_PASS_INPUTS_MAX)
        {
            F->P->WarnOnce(std::string("|pass-inputs|") + F->PassName(), std::string("custom render pass '") + F->PassName()
                           + "': more than 7 inputs to a full-screen draw; the rest are not bound");
        }
        GDITextureResource* Textures[GDI_MATERIAL_TEXTURE_SLOTS] = {};
        uint32_t Slot = 0;
        for(const RenderPassTexture Each : InInputs)
        {
            if(Slot >= RENDER_PASS_INPUTS_MAX) { break; }
            Textures[Slot++] = PassTexture(Each);
        }
        Textures[GDI_MATERIAL_TEXTURE_SLOTS - 1] = PassTexture(DepthOf(F->Targets->SceneColor));   // slot 7: the scene depth
        static_assert(sizeof(RenderPassParams) == 96 && sizeof(RenderPassParams) <= GDI_MATERIAL_BLOCK_MAX_BYTES,
                      "RenderPassParams is PP_Block: mat4, vec4, vec4");
        using Blends = RenderContext::Impl::EFullscreenBlend;
        const Blends Blend = InBlend == ERenderPassBlend::Additive   ? Blends::Additive
                           : InBlend == ERenderPassBlend::AlphaBlend ? Blends::AlphaBlend : Blends::Opaque;
        GDIShader* Pixel = F->P->GetPassShader(InPixelShader, ST_Pixel, F->PassName());
        return F->P->DrawFullscreenShader(Pixel, InPixelShader, Target, Target->GetWidth(), Target->GetHeight(), Textures,
                                          GDI_MATERIAL_TEXTURE_SLOTS, &InParams, uint(sizeof(InParams)), Blend);
    }

    uint32_t RenderPassContext::GetMeshGroupDrawCount(const uint32_t InGroup) const
    {
        return InGroup < RENDER_PASS_MESH_GROUPS ? uint32_t(F->P->GroupDraws[InGroup].size()) : 0;
    }

    bool RenderPassContext::DrawMeshGroup(const uint32_t InGroup, const char* InPixelShader, const RenderPassTarget InTarget,
                                          ERenderPassDepth InDepth, const bool bInClear)
    {
        GDIRenderTarget* Target = PassTargetOf(InTarget);
        if(InGroup >= RENDER_PASS_MESH_GROUPS || !Target)
        {
            return false;
        }
        RenderContext::Impl& P = *F->P;
        GDIRenderTarget* SceneColor = F->Targets->SceneColor;
        if(InDepth == ERenderPassDepth::Scene
           && (!SceneColor || SceneColor->GetWidth() != Target->GetWidth() || SceneColor->GetHeight() != Target->GetHeight()))
        {
            P.WarnOnce(std::string("|pass-depth|") + F->PassName(), std::string("custom render pass '") + F->PassName()
                       + "': a mesh group drawn against the scene depth needs a target the scene's size; drawn without depth");
            InDepth = ERenderPassDepth::None;
        }
        GDIRenderTarget* DepthFrom = InDepth == ERenderPassDepth::Own ? Target : InDepth == ERenderPassDepth::Scene ? SceneColor : nullptr;
        GDIShader* Pixel = InPixelShader ? P.GetPassShader(InPixelShader, ST_Pixel, F->PassName()) : nullptr;
        if(InPixelShader && !Pixel)
        {
            return false;   // warned by LoadShader
        }

        SCOPED_DRAW_EVENT("MeshGroup" + std::to_string(InGroup));
        GDISetRenderTargets(&Target, 1, DepthFrom);
        GDISetViewport(0, 0, Target->GetWidth(), Target->GetHeight());
        GDISetColorMask(EColM_RGBA);
        if(bInClear)
        {
            GDIClear(InDepth == ERenderPassDepth::Own ? (CM_Color | CM_Depth) : CM_Color, EngineCore::Color(0, 0, 0, 0));
        }
        const std::vector<DrawCommand>& Draws = P.GroupDraws[InGroup];
        for(const DrawCommand& Each : Draws)
        {
            if(Pixel)
            {
                P.IssueShadowCaster(Each, /*bInDepthWrite*/ InDepth == ERenderPassDepth::Own, Pixel,
                                    /*bInDepthTest*/ InDepth != ERenderPassDepth::None);
            }
            else
            {
                P.Issue(Each);
            }
        }
        return !Draws.empty();
    }

    bool RenderPassContext::SupportsCompute() const
    {
        return GDISupportsCompute();
    }

    RenderPassBuffer RenderPassContext::AcquireBuffer(const char* InName, const uint32_t InBytes, const void* InInitialData,
                                                      const bool bInInstances)
    {
        RenderPassBuffer Out;
        if(!InName || InName[0] == '\0' || InBytes == 0 || !F->Entry || !GDISupportsCompute())
        {
            return Out;
        }
        GDIBuffer*& Buffer = F->P->PassPoolsOfView()[F->Entry->Handle].Buffers[InName];
        if(Buffer && Buffer->GetSize() != InBytes)
        {
            Buffer->Release();
            Buffer = nullptr;
        }
        if(!Buffer)
        {
            Buffer = GDICreateBuffer(InBytes, uint8_t(GDI_BU_Storage | (bInInstances ? GDI_BU_Instance : 0)), InInitialData);
            GDISetDebugName(Buffer, std::string("RenderCore.Pass.") + F->PassName() + "." + InName);
        }
        Out.Handle = Buffer;
        return Out;
    }

    bool RenderPassContext::Dispatch(const char* InComputeShader, const void* InParams, const uint32_t InParamBytes,
                                     const std::initializer_list<RenderPassBuffer> InBuffers, const uint32_t InX, const uint32_t InY,
                                     const uint32_t InZ)
    {
        if(!InComputeShader || InBuffers.size() > RENDER_PASS_BUFFERS_MAX || InParamBytes > RENDER_PASS_COMPUTE_PARAMS_MAX)
        {
            return false;
        }
        GDIShader* Shader = F->P->GetPassShader(InComputeShader, ST_Compute, F->PassName());
        if(!Shader)
        {
            return false;
        }
        GDIBuffer* Buffers[RENDER_PASS_BUFFERS_MAX] = {};
        uint32_t Count = 0;
        for(const RenderPassBuffer Each : InBuffers)
        {
            if(!Each.IsValid()) { return false; }
            Buffers[Count++] = static_cast<GDIBuffer*>(Each.Handle);
        }
        SCOPED_DRAW_EVENT(InComputeShader);
        return GDIDispatchCompute(Shader, InParams, InParamBytes, Buffers, Count, InX, InY, InZ);
    }

    bool RenderPassContext::ReadBuffer(const RenderPassBuffer InBuffer, std::vector<uint8_t>& OutBytes)
    {
        return InBuffer.IsValid() && GDIReadBuffer(static_cast<GDIBuffer*>(InBuffer.Handle), OutBytes);
    }

    void RenderPassContext::SetCurrent(const RenderPassTarget InTarget)
    {
        if((F->Hook == ERenderHook::AfterToneMap || F->Hook == ERenderHook::AfterPostProcess) && F->Current && InTarget.IsValid())
        {
            *F->Current = PassTargetOf(InTarget);
        }
    }

    void RenderPassContext::BeginDrawEvent(const char* InName)
    {
        GDIBeginDrawEvent(InName ? InName : "");
    }

    void RenderPassContext::EndDrawEvent()
    {
        GDIEndDrawEvent();
    }

    void RenderContext::SubmitCaptured(const std::vector<StagedDraw>& InDraws, const int32_t InShadowView)
    {
        for(const StagedDraw& Each : InDraws)
        {
            DrawCommand Item = Each.Item;
            Item.bStatic     = true;
            if(Each.Stage == ERenderStage::Shadows)
            {
                if(InShadowView < 0) { continue; }
                Item.ShadowView = InShadowView;
                P->PolicyFor(ERenderStage::Shadows).Add(Item);
                continue;
            }
            P->Submit(Item, Each.Stage);   // its depth, for this view
        }
    }

    uint64_t RenderContext::GetContentGeneration() const
    {
        return P->ContentGeneration + MeshAssetCache::GetGeneration();
    }

    bool RenderContext::CanTimeGpu() const
    {
        return P->Backend != ERenderBackend::Null && GDISupportsGPUTimestamps();
    }

    const void* RenderContext::BeginTimedBlock(const void* InOwner, const bool bInTime, float (&OutGpuMs)[RGT_Count])
    {
        /* The times GDI hands back are the block FramesInFlight back's (its
         * frame slot, just reused): that block's owner, if one timed it. */
        const uint64_t Serial  = GDIGetRenderingSerial();
        const uint64_t Behind  = std::min<uint64_t>(GDIGetFramesInFlight(), 3);
        const Impl::TimedBlock& Then = P->TimedBlocks[(Serial - Behind) % 4];
        const void* Previous = Serial > Behind && Then.Serial == Serial - Behind ? Then.Owner : nullptr;
        const uint32_t ThenViews = Then.Views;   // read before this block's entry is written over it
        P->TimedOwner = bInTime && CanTimeGpu() ? InOwner : nullptr;
        P->TimedBlocks[Serial % 4] = { Serial, P->TimedOwner, 1 };
        P->TimedViews = 0;
        for(float& Each : OutGpuMs) { Each = -1.0f; }
        if(!Previous)
        {
            return nullptr;
        }
        double         Stamps[GDI_GPU_TIMESTAMP_MAX] = {};
        const uint64_t Written = GDIGetPreviousGPUTimestamps(Stamps);
        const auto Span = [&](const uint32_t InStart, const uint32_t InEnd, double& OutMs)
        {
            const uint64_t Both = (uint64_t(1) << InStart) | (uint64_t(1) << InEnd);
            if((Written & Both) != Both) { return false; }
            OutMs = std::max(Stamps[InEnd] - Stamps[InStart], 0.0);
            return true;
        };
        for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
        {
            double Ms = 0.0;
            if(Span(Timer * 2, Timer * 2 + 1, Ms))
            {
                OutGpuMs[Timer] = float(Ms);
            }
        }
        /* SPLITSCREEN: the later views' spans added in; Total from view 0's start to the last's end. */
        for(uint32_t View = 1; View < ThenViews && View < RENDER_VIEWS_MAX; ++View)
        {
            for(uint32_t Timer = 0; Timer < RGT_Total; ++Timer)
            {
                const uint32_t Start = Impl::VIEW_TIMESTAMPS_BASE + (View - 1) * Impl::VIEW_TIMESTAMPS + Timer * 2;
                double Ms = 0.0;
                if(Span(Start, Start + 1, Ms))
                {
                    OutGpuMs[Timer] = float(std::max(OutGpuMs[Timer], 0.0f) + Ms);
                }
            }
        }
        if(ThenViews > 1)
        {
            double Ms = 0.0;
            OutGpuMs[RGT_Total] = Span(uint32_t(RGT_Total) * 2, Impl::VIEWS_END_TIMESTAMP, Ms) ? float(Ms) : -1.0f;
        }
        return Written ? Previous : nullptr;
    }

    void RenderContext::EndTimedViews()
    {
        if(P->TimedOwner && P->TimedViews > 1)
        {
            GDIWriteGPUTimestamp(Impl::VIEWS_END_TIMESTAMP);
        }
        Impl::TimedBlock& Block = P->TimedBlocks[GDIGetRenderingSerial() % 4];
        if(Block.Owner) { Block.Views = std::max<uint32_t>(P->TimedViews, 1); }
    }

    void RenderContext::WriteGpuTimestamp(const ERenderGpuTimer InTimer, const bool bInEnd)
    {
        P->MarkStageCpu(InTimer, bInEnd);
        if(P->TimedOwner) { GDIWriteGPUTimestamp(uint32_t(InTimer) * 2 + (bInEnd ? 1 : 0)); }
    }

    uint64_t RenderContext::GetParallelPassCount() const
    {
        return P->ParallelPassCount.load(std::memory_order_relaxed);
    }

    void RenderContext::GetStageCpuTimes(float (&OutCpuMs)[RGT_Count], int32_t (&OutDraws)[RGT_Count]) const
    {
        for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
        {
            OutCpuMs[Timer] = P->StageCpuMs[Timer];
            OutDraws[Timer] = P->StageSpanDraws[Timer];
        }
    }

    void RenderContext::SetUnlit(const bool bInUnlit)
    {
        P->Scene.View[0] = bInUnlit ? 1.0f : 0.0f;
    }

    namespace
    {
        /* What ListViewableTargets names: a target's colour, or its depth --
         * grey (a shadow atlas), or as distance (the scene's). ViewTarget.fsh's modes. */
        struct ViewableTarget
        {
            std::string   Name;
            GDITexture2D* Texture = nullptr;
            float         Mode    = 0.0f;
            const void*   Target  = nullptr;   // the render target it is of, so Final is never drawn over itself
        };

        void CollectViewableTargets(const SceneTargets& InTargets, GDIRenderTarget* const (&InShadows)[SA_Count], const IBLResources* InIBL,
                                    const std::vector<std::pair<std::string, GDIRenderTarget*>>& InAssets, std::vector<ViewableTarget>& Out)
        {
            const auto Add = [&Out](GDIRenderTarget* InTarget, std::string InName)
            {
                if(GDITexture2D* Texture = InTarget ? InTarget->ToGDITexture2D() : nullptr) { Out.push_back({ std::move(InName), Texture, 0.0f, InTarget }); }
            };
            const auto AddDepth = [&Out](GDIRenderTarget* InTarget, std::string InName, const float InMode)
            {
                if(GDITexture2D* Texture = InTarget ? InTarget->ToDepthTexture2D() : nullptr) { Out.push_back({ std::move(InName), Texture, InMode, nullptr }); }
            };
            Add(InTargets.SceneColor, "Scene colour");
            AddDepth(InTargets.SceneColor, "Scene depth", 2.0f);
            const char* const GBufferNames[RENDER_GBUFFER_TARGETS] = { "GBuffer0 (base colour, AO)", "GBuffer1 (normal, octahedral)",
                                                                       "GBuffer2 (metallic, roughness, specular, shading model)",
                                                                       "GBuffer3 (emissive)", "GBuffer4 (shading model data)" };
            for(uint32_t Index = 0; Index < RENDER_GBUFFER_TARGETS; ++Index) { Add(InTargets.GBuffer[Index], GBufferNames[Index]); }
            Add(InTargets.Velocity,     "Velocity (world motion, units a frame)");
            Add(InTargets.Subsurface,     "Subsurface (Skin's diffuse before albedo)");
            Add(InTargets.SubsurfaceBlur, "Subsurface blur (across)");
            Add(InTargets.PostProcess,  "Post process");
            Add(InTargets.PostProcessB, "Post process B");
            for(size_t Index = 0; Index < std::size(InTargets.Half); ++Index)    { Add(InTargets.Half[Index], "Half size " + std::to_string(Index) + " (ambient occlusion)"); }
            for(size_t Index = 0; Index < std::size(InTargets.Bloom); ++Index)   { Add(InTargets.Bloom[Index], "Bloom " + std::to_string(Index)); }
            for(size_t Index = 0; Index < std::size(InTargets.BloomUp); ++Index) { Add(InTargets.BloomUp[Index], "Bloom up " + std::to_string(Index)); }
            AddDepth(InShadows[SA_Directional], "Shadow atlas (directional)", 1.0f);
            AddDepth(InShadows[SA_Local],       "Shadow atlas (point and spot)", 1.0f);
            if(InIBL)
            {
                Add(InIBL->Atlas, "Image-based lighting atlas");
                Add(InIBL->Strip, "Image-based lighting cube strip");
            }
            for(const auto& Each : InAssets) { Add(Each.second, "Render target: " + Each.first); }
        }
    }

    void RenderContext::ListViewableTargets(const SceneTargets& InTargets, std::vector<std::string>& OutNames) const
    {
        std::vector<std::pair<std::string, GDIRenderTarget*>> Assets;
        for(const auto& Each : P->RenderTargets) { if(Each.second.Target) { Assets.emplace_back(Each.first, Each.second.Target); } }
        std::sort(Assets.begin(), Assets.end());
        std::vector<ViewableTarget> All;
        CollectViewableTargets(InTargets, P->ShadowAtlas, P->IBL, Assets, All);
        OutNames.clear();
        for(ViewableTarget& Each : All) { OutNames.push_back(std::move(Each.Name)); }
    }

    void RenderContext::ShowTarget(const SceneTargets& InTargets, const std::string& InName)
    {
        std::vector<std::pair<std::string, GDIRenderTarget*>> Assets;
        for(const auto& Each : P->RenderTargets) { if(Each.second.Target) { Assets.emplace_back(Each.first, Each.second.Target); } }
        std::vector<ViewableTarget> All;
        CollectViewableTargets(InTargets, P->ShadowAtlas, P->IBL, Assets, All);
        const auto Found = std::find_if(All.begin(), All.end(), [&InName](const ViewableTarget& Each) { return Each.Name == InName; });
        if(Found == All.end() || !InTargets.Final || Found->Target == InTargets.Final)
        {
            return;
        }
        PostBlock Block;
        Block.A[0] = Found->Mode;
        P->DrawPost(PP_ViewTarget, InTargets.Final, InTargets.Width, InTargets.Height, { Found->Texture->Resource }, Block);
    }

    void RenderContext::CompositeViews(GDIRenderTarget* InTarget, GDIRenderTarget* const* InSources, const RenderViewRect* InRects,
                                       const uint32_t InCount)
    {
        if(!InTarget)
        {
            return;
        }
        SCOPED_DRAW_EVENT("Composite");
        const uint Width = InTarget->GetWidth(), Height = InTarget->GetHeight();
        GDISetRenderTargets(&InTarget, 1, nullptr);
        GDISetViewport(0, 0, Width, Height);
        GDIClear(CM_Color, EngineCore::Color(0, 0, 0, 255));   // what no view covers: black

        /* ViewTarget.fsh as a colour target as it is (PP_View.x 0), stretched
         * over the viewport: the view's rectangle, clipped to the window. */
        PostBlock Block;
        for(uint32_t Index = 0; Index < InCount; ++Index)
        {
            GDIRenderTarget* Source = InSources[Index];
            const RenderViewRect& Rect = InRects[Index];
            GDITexture2D* Texture = Source ? Source->ToGDITexture2D() : nullptr;
            const int32_t X = std::max(Rect.X, 0), Y = std::max(Rect.Y, 0);
            if(!Texture || Rect.Width <= 0 || Rect.Height <= 0 || uint(X) >= Width || uint(Y) >= Height)
            {
                continue;
            }
            const uint ClippedWidth  = std::min(uint(Rect.Width), Width - uint(X));
            const uint ClippedHeight = std::min(uint(Rect.Height), Height - uint(Y));
            P->DrawFullscreenShader(P->GetPostShader(PP_ViewTarget), "View", InTarget, ClippedWidth, ClippedHeight,
                                    &Texture->Resource, 1, Block.A, uint(sizeof(Block) - sizeof(Block.Matrix)),
                                    Impl::EFullscreenBlend::Opaque, uint(X), uint(Y));
        }
    }

    void RenderContext::CompositeViewSites(GDIRenderTarget* InTarget, GDIRenderTarget* const* InSources, const RenderViewSites& InSites)
    {
        const uint32_t Count = std::min<uint32_t>(InSites.Count, RENDER_VIEWS_MAX);
        if(!InTarget || Count < 2)
        {
            return;
        }
        /* ViewSites_Block: SitesA, SitesB, Params, Color, Sizes. */
        struct SitesBlock
        {
            float SitesA[4];
            float SitesB[4];
            float Params[4];
            float Color[4];
            float Sizes[4];
        } Block = {};
        static_assert(sizeof(SitesBlock) == 80, "ViewSites.fsh's PP_Block");
        float* const Sites[2] = { Block.SitesA, Block.SitesB };
        for(uint32_t Index = 0; Index < RENDER_VIEWS_MAX; ++Index)
        {
            Sites[Index / 2][(Index % 2) * 2]     = InSites.X[Index];
            Sites[Index / 2][(Index % 2) * 2 + 1] = InSites.Y[Index];
        }
        Block.Params[0] = float(Count);
        Block.Params[1] = std::max(InSites.DividerPixels, 0.0f);
        Block.Params[2] = std::clamp(InSites.DividerAlpha, 0.0f, 1.0f);
        Block.Color[0]  = float(InSites.DividerColor.R) / 255.0f;
        Block.Color[1]  = float(InSites.DividerColor.G) / 255.0f;
        Block.Color[2]  = float(InSites.DividerColor.B) / 255.0f;
        Block.Color[3]  = 1.0f;
        Block.Sizes[0]  = float(InTarget->GetWidth());
        Block.Sizes[1]  = float(InTarget->GetHeight());

        /* A view not drawn this frame shows view 0 (never expected: each site is a drawn view). */
        GDITextureResource* Textures[RENDER_VIEWS_MAX] = {};
        for(uint32_t Index = 0; Index < RENDER_VIEWS_MAX; ++Index)
        {
            GDIRenderTarget* Source = Index < Count && InSources[Index] ? InSources[Index] : InSources[0];
            GDITexture2D* Texture   = Source ? Source->ToGDITexture2D() : nullptr;
            Textures[Index] = Texture ? Texture->Resource : nullptr;
        }
        if(!Textures[0])
        {
            return;
        }
        for(GDITextureResource*& Each : Textures) { if(!Each) { Each = Textures[0]; } }
        SCOPED_DRAW_EVENT("Composite");
        P->DrawFullscreenShader(P->GetPostShader(PP_ViewSites), "ViewSites", InTarget, InTarget->GetWidth(), InTarget->GetHeight(),
                                Textures, RENDER_VIEWS_MAX, &Block, uint(sizeof(Block)), Impl::EFullscreenBlend::Opaque);
    }

    void RenderContext::SeedViewHistory(SceneTargets& InTo, const SceneTargets& InFrom)
    {
        if(InTo.Width != InFrom.Width || InTo.Height != InFrom.Height || InTo.Width == 0 || InTo.Height == 0)
        {
            return;
        }
        SCOPED_DRAW_EVENT("SeedViewHistory");
        PostBlock Copy;
        const auto CopyInto = [this, &Copy](GDIRenderTarget* InSource, GDIRenderTarget* InDest, const uint InWidth, const uint InHeight)
        {
            GDITexture2D* Texture = InSource ? InSource->ToGDITexture2D() : nullptr;
            return Texture && InDest && P->DrawPost(PP_Copy, InDest, InWidth, InHeight, { Texture->Resource }, Copy);
        };

        /* TAA's history: last frame's resolve, at the same index. */
        InTo.bHistoryValid = false;
        if(InFrom.bHistoryValid && InTo.EnsureHistory())
        {
            InTo.HistoryCurrent = InFrom.HistoryCurrent;
            InTo.bHistoryValid  = CopyInto(InFrom.History[InFrom.HistoryCurrent], InTo.History[InTo.HistoryCurrent], InTo.Width, InTo.Height);
        }
        /* Auto exposure's adapted value, so it does not ease in from scratch. */
        InTo.bExposureValid = false;
        if(InFrom.bExposureValid && InTo.EnsureExposure())
        {
            InTo.ExposureCurrent = InFrom.ExposureCurrent;
            InTo.bExposureValid  = CopyInto(InFrom.Exposure[InFrom.ExposureCurrent], InTo.Exposure[InTo.ExposureCurrent], 1, 1);
        }
        /* Screen-space reflections' last colour: TAA's history when that is what it was. */
        InTo.LastColor = InFrom.LastColor && InFrom.LastColor == InFrom.History[InFrom.HistoryCurrent] && InTo.bHistoryValid
                       ? InTo.History[InTo.HistoryCurrent] : nullptr;
    }

    void RenderContext::RenderSceneStages(SceneTargets& InTargets, const EngineCore::Color InClearColor, const bool bInTimed,
                                          const EngineCore::Color* InClearBottom)
    {
        /* Timed only for the block's owning scene: a preview's stages in the
         * same block would write the same indices. */
        struct TimedScope
        {
            Impl* Owner;
            bool  bTimed;
            TimedScope(Impl* InOwner, const bool bInTimed) : Owner(InOwner), bTimed(bInTimed)
            {
                Owner->bTimeStages    = bInTimed && Owner->TimedOwner;
                Owner->bTimeStagesCpu = bInTimed;
                Owner->TimedView      = bInTimed ? std::min<uint32_t>(Owner->View.ViewIndex, RENDER_VIEWS_MAX - 1) : 0;
                if(bInTimed)
                {
                    Owner->TimedViews = std::max(Owner->TimedViews, Owner->TimedView + 1);
                }
                if(bInTimed && Owner->TimedView == 0)   // the frame's first view: its spans start the frame's
                {
                    for(uint32_t Timer = RGT_PrePass; Timer <= RGT_Total; ++Timer) { Owner->StageCpuMs[Timer] = -1.0f; }
                }
            }
            ~TimedScope()
            {
                if(bTimed) { Owner->CountSpanDraws(); }
                Owner->bTimeStages    = false;
                Owner->bTimeStagesCpu = false;
                Owner->TimedView      = 0;
            }
        } Timed(P, bInTimed);

        P->bDeferring  = false;
        P->FinalTarget = nullptr;
        InTargets.Final = nullptr;
        /* The gate as this frame sees it (ParallelRecording.h): latched once,
         * again after the console or an ini changes it. */
        if(TakeParallelRecordingChange() || !P->bParallelLatched)
        {
            P->Parallel         = GetParallelRecordingState();
            P->bParallelLatched = true;
        }
        for(uint32_t& Count : P->StageDrawCounts) { Count = 0; }

        const auto Finish = [this]()
        {
            for(DrawPolicy& Policy : P->Policies) { Policy.Clear(); }
            for(std::vector<DrawCommand>& Group : P->GroupDraws) { Group.clear(); }
            P->FramePasses.clear();
            P->ForgetOcclusion();
        };

        /* The World's custom render passes for these stages (CustomRenderPass.h);
         * the GPU resources of those unregistered since, freed first. */
        P->ReleaseRetiredPasses();
        GatherCustomRenderPasses(P->PassWorld, P->FramePasses);

        /* Nothing to draw into (an empty viewport): the routing still counts. */
        GDIRenderTarget* SceneColor = InTargets.SceneColor;
        if(!SceneColor)
        {
            for(size_t Index = 0; Index < size_t(ERenderStage::Count); ++Index)
            {
                P->StageDrawCounts[Index] = uint32_t(P->Policies[Index].Num());
            }
            Finish();
            return;
        }

        const uint Width  = InTargets.Width;
        const uint Height = InTargets.Height;
        const auto BindTargets = [Width, Height](GDIRenderTarget* const* InColors, const uint InCount, GDIRenderTarget* InDepthFrom)
        {
            GDISetRenderTargets(InColors, InCount, InDepthFrom);
            GDISetViewport(0, 0, Width, Height);
            /* THE COLOUR MASK IS REQUIRED: Vulkan's default colorWriteMask is
             * zero, so everything draws with no validation error and nothing
             * reaches the attachment. Blend and depth are set per draw. */
            GDISetColorMask(EColM_RGBA);
        };

        /* HDR: what the shaders write into the scene colour, and the tone map
         * this scene's stack asks for (a capture or a preview has none: clamped). */
        const PostProcessSettings& Tone = P->PostSettings;
        P->Scene.Output[0] = InTargets.bHDR ? 1.0f : 0.0f;
        P->Scene.Output[1] = Tone.bToneMapping ? std::exp2(Tone.Exposure) : 1.0f;
        P->Scene.Output[2] = Tone.bToneMapping ? float(Tone.ToneMapper) : 0.0f;

        /* Image-based lighting, with the atlas this scene set (HDR only). */
        const bool bIBL = InTargets.bHDR && P->IBL && P->IBL->Atlas && P->IBLAtlasTexture();
        P->Scene.IBL[0] = bIBL ? 1.0f : 0.0f;
        P->Scene.IBL[1] = bIBL ? float(P->IBLProbeCount) : 0.0f;
        P->Scene.IBL[2] = bIBL && P->bIBLSky ? 1.0f : 0.0f;
        std::memcpy(P->Scene.Probes, P->IBLProbes, sizeof(P->Scene.Probes));

        P->StageTimestamp(RGT_Total, false);
        {
            SCOPED_DRAW_EVENT("Background");
            BindTargets(&SceneColor, 1, SceneColor);
            GDIClear(CM_Color | CM_Depth, InClearColor);

            /* The background gradient, over the clear and under everything: the
             * Lighting stage writes lit pixels only, so it shows wherever the
             * clear colour would. An HDR scene draws even a plain clear this way,
             * un-tone-mapped so it comes out as authored (ToneMapping.glsl). */
            const bool bSky = InTargets.bHDR
                           && (P->bForceSkyBackground || (P->Lighting.SkyEnvironment[0] != '\0' && P->Lighting.bSkyBackground));
            if(bSky)
            {
                /* The sky behind the scene: the HDRI along each view ray (or the sky colours), linear. */
                PostBlock Block;
                GDITextureResource* Environment = P->SkyBlock(Block);
                if(P->DrawPost(PP_Sky, SceneColor, InTargets.Width, InTargets.Height, { Environment }, Block))
                {
                    BindTargets(&SceneColor, 1, SceneColor);
                }
                GDISetMaterialTexture(0, nullptr);
            }
            else if(InClearBottom || InTargets.bHDR)
            {
                PostBlock Block;
                const auto Put = [](float (&OutColor)[4], const EngineCore::Color InColor)
                {
                    OutColor[0] = float(InColor.R) / 255.0f;
                    OutColor[1] = float(InColor.G) / 255.0f;
                    OutColor[2] = float(InColor.B) / 255.0f;
                    OutColor[3] = 1.0f;
                };
                Put(Block.A, InClearColor);
                Put(Block.Sizes, InClearBottom ? *InClearBottom : InClearColor);
                if(P->DrawPost(PP_ClearGradient, SceneColor, InTargets.Width, InTargets.Height, {}, Block))
                {
                    BindTargets(&SceneColor, 1, SceneColor);   // the depth back for the stages
                }
            }
        }

        /* Custom passes before the scene: what its materials sample this frame. */
        GDIRenderTarget* Current = nullptr;   // the post hooks' picture (SetCurrent)
        if(P->RunHook(ERenderHook::BeforeScene, InTargets, bInTimed, &Current))
        {
            BindTargets(&SceneColor, 1, SceneColor);   // the PrePass draws into the bound depth
        }

        /* PrePass: RenderScene culled against the view before the draws were
         * queued; now the occluders' depth and the occlusion queries, which
         * every stage after it may be predicated on. */
        P->StageTimestamp(RGT_PrePass, false);
        P->IssuePrePass();
        P->StageTimestamp(RGT_PrePass, true);
        if(P->RunHook(ERenderHook::AfterPrePass, InTargets, bInTimed, &Current))
        {
            BindTargets(&SceneColor, 1, SceneColor);
        }

        /* Base Pass: the G-buffer, created the first time something needs it. */
        const DrawPolicy& Base = P->PolicyFor(ERenderStage::BasePass);
        const bool bDeferred = !Base.IsEmpty() && InTargets.EnsureGBuffer();
        P->bGBufferThisFrame  = bDeferred;
        P->bVelocityThisFrame = false;
        if(bDeferred)
        {
            P->StageTimestamp(RGT_BasePass, false);
            /* And the velocity beside it, where the backend has one: what
             * nothing here writes stays 0, not moving (the camera's motion
             * alone, to Motion Blur). */
            GDIRenderTarget* Targets[RENDER_GBUFFER_TARGETS + 1] = { InTargets.GBuffer[0], InTargets.GBuffer[1], InTargets.GBuffer[2],
                                                                     InTargets.GBuffer[3], InTargets.GBuffer[4], InTargets.EnsureVelocity() };
            P->bVelocityThisFrame = Targets[RENDER_GBUFFER_TARGETS] != nullptr;
            BindTargets(Targets, P->bVelocityThisFrame ? RENDER_GBUFFER_TARGETS + 1 : RENDER_GBUFFER_TARGETS, SceneColor);
            GDIClear(CM_Color, EngineCore::Color(0, 0, 0, 0));   // alpha 0 in GBuffer1: no lit surface
            P->IssueStage(ERenderStage::BasePass);
            P->StageTimestamp(RGT_BasePass, true);
        }
        else if(!Base.IsEmpty())
        {
            P->StageDrawCounts[size_t(ERenderStage::BasePass)] = uint32_t(Base.Num());
            P->WarnOnce("|gbuffer", "the Base Pass: this backend could not create the G-buffer; "
                                    "Opaque and Masked Lit materials draw nothing");
        }

        /* Custom passes after the Base Pass (its G-buffer, on a deferred frame);
         * AO, Shadows and the stages after bind their own. */
        P->RunHook(ERenderHook::AfterBasePass, InTargets, bInTimed, &Current);

        /* Lighting: Shadows (only when something will sample them), then
         * the resolve into the scene colour. */
        if(bDeferred)
        {
            P->StageTimestamp(RGT_AmbientOcclusion, false);
            P->IssueAmbientOcclusion(InTargets, P->PostSettings);
            P->StageTimestamp(RGT_AmbientOcclusion, true);
            P->StageTimestamp(RGT_Shadows, false);
            P->IssueShadows();
            P->StageTimestamp(RGT_Shadows, true);
        }
        else
        {
            P->StageDrawCounts[size_t(ERenderStage::Shadows)] = uint32_t(P->PolicyFor(ERenderStage::Shadows).Num());
        }
        if(bDeferred)
        {
            P->StageTimestamp(RGT_Lighting, false);
            SCOPED_DRAW_EVENT(GetRenderStageName(ERenderStage::Lighting));

            /* Skin (SHADING_MODELS.md): where a Skin material drew and the scene is HDR, its
             * diffuse goes to its own target, cleared first, for PPSubsurface to blur. */
            GDIRenderTarget* Subsurface = nullptr;
            if(GRenderSubsurfaceScattering.GetBool() && InTargets.bHDR && P->BasePassHasModel(EMaterialShadingModel::Skin))
            {
                Subsurface = InTargets.EnsureFullHalfFloat(InTargets.Subsurface, "Subsurface");
                if(Subsurface)
                {
                    BindTargets(&Subsurface, 1, nullptr);
                    GDIClear(CM_Color, EngineCore::Color(0, 0, 0, 0));
                }
            }
            GDIRenderTarget* LightingTargets[2] = { SceneColor, Subsurface };
            BindTargets(LightingTargets, Subsurface ? 2 : 1, nullptr);
            {
                SCOPED_DRAW_EVENT("DeferredLighting");
                if(P->IssueLighting(InTargets, Subsurface != nullptr))
                {
                    P->StageDrawCounts[size_t(ERenderStage::Lighting)] = 1;
                }
            }
            /* Image-based lighting, added over what the Lighting stage lit (IBLLighting.fsh). */
            if(bIBL)
            {
                const auto Texture = [](GDIRenderTarget* InTarget) -> GDITextureResource*
                {
                    GDITexture2D* Each = InTarget ? InTarget->ToGDITexture2D() : nullptr;
                    return Each ? Each->Resource : nullptr;
                };
                GDITexture2D* Depth = SceneColor->ToDepthTexture2D();

                /* Screen-space reflections (PPScreenReflections.fsh): what the IBL pass blends
                 * over the probes' specular, from the last frame's colour. */
                GDITextureResource* Screen = nullptr;
                const PostProcessSettings& Stack = P->PostSettings;
                GDITexture2D* Motion = InTargets.Velocity ? InTargets.Velocity->ToGDITexture2D() : nullptr;
                if(Stack.bScreenReflections && InTargets.LastColor && P->bHasPrevViewProjection && Depth && Motion)
                {
                    if(GDIRenderTarget* Reflections = InTargets.EnsureFullHalfFloat(InTargets.Reflections, "Reflections"))
                    {
                        PostBlock Ssr;
                        PutMatrix(Ssr.Matrix, P->PrevViewProjection);
                        Ssr.A[0]     = std::clamp(Stack.SsrIntensity, 0.0f, 1.0f);
                        Ssr.A[1]     = std::clamp(Stack.SsrMaxRoughness, 0.05f, 1.0f);
                        Ssr.A[2]     = std::clamp(Stack.SsrSteps, 8.0f, 64.0f);
                        Ssr.A[3]     = std::max(Stack.SsrThickness, 0.1f);
                        Ssr.Sizes[0] = std::max(Stack.SsrMaxDistance, 10.0f);
                        Ssr.Sizes[1] = 1.0f;
                        Ssr.Sizes[2] = float(Width);
                        Ssr.Sizes[3] = float(Height);
                        if(P->DrawPost(PP_ScreenReflections, Reflections, Width, Height,
                                       { Texture(InTargets.GBuffer[1]), Texture(InTargets.GBuffer[2]), Depth->Resource,
                                         Texture(InTargets.LastColor), Motion->Resource, Texture(InTargets.GBuffer[4]) }, Ssr))
                        {
                            Screen = Texture(Reflections);
                        }
                        for(uint32_t Slot = 0; Slot < 6; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
                    }
                }

                PostBlock Block;
                Block.A[0] = Subsurface ? 1.0f : 0.0f;
                Block.A[1] = Screen ? 1.0f : 0.0f;
                GDITextureResource* IBLTextures[] = { Texture(InTargets.GBuffer[0]), Texture(InTargets.GBuffer[1]), Texture(InTargets.GBuffer[2]),
                                                      Texture(InTargets.GBuffer[4]), Depth ? Depth->Resource : nullptr,
                                                      P->IBLAtlasTexture(), Screen };
                P->DrawFullscreenShader(P->GetPostShader(PP_IBLLighting), POST_PASS_NAMES[PP_IBLLighting], SceneColor, Width, Height,
                                        IBLTextures, uint32_t(std::size(IBLTextures)), Block.A,
                                        uint(sizeof(Block) - sizeof(Block.Matrix)), Impl::EFullscreenBlend::Additive, 0, 0, Subsurface);
            }

            /* Skin's diffuse blurred across, then down and into the scene colour (PPSubsurface.fsh). */
            if(Subsurface)
            {
                P->IssueSubsurface(InTargets, Subsurface, SceneColor);
            }
            P->AmbientOcclusion = nullptr;
            P->StageTimestamp(RGT_Lighting, true);
        }

        /* Custom passes over the lit scene; the forward stages bind theirs. */
        P->RunHook(ERenderHook::AfterLighting, InTargets, bInTimed, &Current);

        /* Forward, over the lit scene and against its depth. */
        P->StageTimestamp(RGT_Forward, false);
        BindTargets(&SceneColor, 1, SceneColor);
        P->IssueStage(ERenderStage::Unlit);
        P->IssueStage(ERenderStage::TranslucentLit);
        P->IssueStage(ERenderStage::TranslucentUnlit);
        P->RunHook(ERenderHook::AfterForward, InTargets, bInTimed, &Current);   // fog and the stack bind their own

        /* Volumetric fog (PPVolumetricFog.fsh): marched at half size through the depth and the
         * sun's shadow map, then laid over everything drawn so far, before TAA smooths it. */
        const PostProcessSettings& FogStack = P->PostSettings;
        if(FogStack.bVolumetricFog && InTargets.bHDR && FogStack.FogDensity > 0.0f)
        {
            GDITexture2D* Depth = SceneColor->ToDepthTexture2D();
            GDIRenderTarget* Fog = InTargets.EnsureFog();
            if(Depth && Fog)
            {
                SCOPED_DRAW_EVENT("VolumetricFog");
                GDITexture2D* Sun = P->Shadows.ViewCount > 0 && P->ShadowAtlas[SA_Directional]
                                  ? P->ShadowAtlas[SA_Directional]->ToDepthTexture2D() : nullptr;
                PostBlock March;
                const float Settings[16] = {
                    FogStack.FogDensity, FogStack.FogHeightFalloff, FogStack.FogBaseHeight, std::max(FogStack.FogMaxDistance, 10.0f),
                    std::clamp(FogStack.FogAnisotropy, 0.0f, 0.95f), FogStack.FogSun, FogStack.FogSky, FogStack.FogLights,
                    FogStack.FogAlbedo[0], FogStack.FogAlbedo[1], FogStack.FogAlbedo[2], FogStack.bFogShadows && Sun ? 1.0f : 0.0f,
                    std::clamp(FogStack.FogSteps, 4.0f, 64.0f), 0.0f, 0.0f, 0.0f };
                std::memcpy(March.Matrix, Settings, sizeof(Settings));   // PPVolumetricFog.fsh's four vec4s, in the matrix's place
                March.Sizes[2] = float(Width);
                March.Sizes[3] = float(Height);
                const uint HalfW = Fog->GetWidth(), HalfH = Fog->GetHeight();
                const bool bMarched = P->DrawPost(PP_VolumetricFog, Fog, HalfW, HalfH, { Depth->Resource, Sun ? Sun->Resource : nullptr }, March);
                GDISetMaterialTexture(0, nullptr);
                GDISetMaterialTexture(1, nullptr);
                GDITexture2D* Marched = Fog->ToGDITexture2D();
                PostBlock Composite;
                if(bMarched && Marched)
                {
                    P->DrawPost(PP_FogComposite, SceneColor, Width, Height, { Marched->Resource }, Composite,
                                /*bInAdditive*/ false, 0, /*bInTransmit*/ true);
                    GDISetMaterialTexture(0, nullptr);
                }
            }
        }
        P->StageTimestamp(RGT_Forward, true);
        P->RunHook(ERenderHook::BeforePostProcess, InTargets, bInTimed, &Current);

        /* PostProcess: the stack (RENDERPIPELINE.md, "Post process"), once a
         * scene's; consumed here, so the next call (a preview) runs none. */
        const PostProcessSettings Post = P->PostSettings;
        const bool bHasPrev = P->bHasPrevViewProjection;
        P->PostSettings           = PostProcessSettings();
        P->bHasPrevViewProjection = false;
        P->StageTimestamp(RGT_PostProcess, false);
        GDIRenderTarget* Source = nullptr;
        {
            SCOPED_DRAW_EVENT(GetRenderStageName(ERenderStage::PostProcess));
            /* An HDR scene's tone map first: everything after works in display space. */
            GDIRenderTarget* Display = SceneColor;
            if(InTargets.bHDR)
            {
                GDIRenderTarget* Mapped = InTargets.EnsurePostProcess();
                GDITexture2D*    Scene  = SceneColor->ToGDITexture2D();
                GDITextureResource* Linear = Scene ? Scene->Resource : nullptr;
                /* Temporal antialiasing: this frame's (jittered) colour into the history; what it
                 * resolves is the scene from here on. FXAA stands in when it cannot run. */
                P->bTemporalResolved = false;
                if(Post.bAntialiasing && Post.AaMethod == 1 && Linear)
                {
                    if(GDITextureResource* Resolved = P->ResolveTemporal(InTargets, Linear, bHasPrev))
                    {
                        Linear = Resolved;
                        P->bTemporalResolved = true;
                    }
                }
                else
                {
                    InTargets.bHistoryValid = false;
                }
                /* The next frame's last colour, for screen-space reflections: the resolve TAA
                 * keeps anyway, else a copy of this frame's scene -- only while they are on. */
                if(P->bTemporalResolved)
                {
                    InTargets.LastColor = InTargets.History[InTargets.HistoryCurrent];
                }
                else if(Post.bScreenReflections && Linear && InTargets.EnsureFullHalfFloat(InTargets.LastCopy, "LastColor"))
                {
                    PostBlock Copy;
                    InTargets.LastColor = P->DrawPost(PP_Copy, InTargets.LastCopy, Width, Height, { Linear }, Copy)
                                        ? InTargets.LastCopy : nullptr;
                    GDISetMaterialTexture(0, nullptr);
                }
                else
                {
                    InTargets.LastColor = nullptr;
                }
                PostBlock Block;
                /* Auto exposure: meter the scene, ease the adapted value toward it, and let the tone map read it. */
                GDITextureResource* Adapted = nullptr;
                if(Post.bAutoExposure && Post.bToneMapping && Linear)
                {
                    Adapted = P->MeterExposure(InTargets, Linear, Post, GetDeltaSeconds());
                }
                else
                {
                    InTargets.bExposureValid = false;   // back on, it starts from what it meters
                }
                if(Adapted)
                {
                    Block.A[0] = 1.0f;
                    Block.A[1] = Post.AeKey;
                    Block.A[2] = std::min(Post.AeMinEV, Post.AeMaxEV);
                    Block.A[3] = std::max(Post.AeMinEV, Post.AeMaxEV);
                }
                /* After TAA, its softening sharpened back: in the tone map, so never into the history. */
                Block.Sizes[0] = P->bTemporalResolved ? std::clamp(Post.AaSharpness, 0.0f, 1.0f) : 0.0f;
                if(Mapped && Linear && P->DrawPost(PP_Tonemap, Mapped, Width, Height, { Linear, Adapted }, Block))
                {
                    Display = Mapped;
                }
                GDISetMaterialTexture(0, nullptr);
                GDISetMaterialTexture(1, nullptr);
            }
            /* Custom passes in display space; one may hand the stack another picture. */
            Current = Display;
            P->RunHook(ERenderHook::AfterToneMap, InTargets, bInTimed, &Current);
            Display = Current ? Current : Display;
            Source = P->RunPostProcessStack(InTargets, Post, bHasPrev, Display);

            /* And over the finished picture, before the UI -- left bound, as the stack leaves it. */
            Current = Source;
            if(P->RunHook(ERenderHook::AfterPostProcess, InTargets, bInTimed, &Current))
            {
                Source = Current ? Current : Source;
                BindTargets(&Source, 1, nullptr);
            }
        }
        P->StageTimestamp(RGT_PostProcess, true);
        P->StageTimestamp(RGT_Total, true);
        P->FinalTarget  = Source;
        InTargets.Final = Source;
        Finish();
    }

    // === Image-based lighting ==================================================

    bool RenderContext::CanImageBasedLight() const
    {
        return P->Backend != ERenderBackend::Null && GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F);
    }

    bool RenderContext::EnsureIBLResources(IBLResources& InOut, const uint32_t InFace)
    {
        if(!CanImageBasedLight() || InFace == 0)
        {
            return false;
        }
        if(!InOut.Atlas)
        {
            InOut.Atlas = GDICreateRenderTargetFormat("RenderCore.IBLAtlas", 768, 768, GDI_RTF_RGBA16F);
        }
        if(!InOut.Strip)
        {
            InOut.Strip = GDICreateRenderTargetFormat("RenderCore.IBLStrip", InFace * 6, InFace, GDI_RTF_RGBA16F);
        }
        else if(InOut.StripFace != InFace)
        {
            InOut.Strip->Resize(InFace * 6, InFace);
        }
        InOut.StripFace = InFace;
        return InOut.Atlas && InOut.Strip;
    }

    void RenderContext::ReleaseIBLResources(IBLResources& InOut)
    {
        if(P->IBL == &InOut) { P->IBL = nullptr; }
        for(GDIRenderTarget** Each : { &InOut.Atlas, &InOut.Strip })
        {
            if(*Each)
            {
                GDIReleaseRenderTarget(*Each);
                delete *Each;
                *Each = nullptr;
            }
        }
        InOut.StripFace = 0;
        InOut.bSheenTable = false;
    }

    bool RenderContext::RenderSkyToStrip(IBLResources& InResources)
    {
        if(!InResources.Strip || InResources.StripFace == 0)
        {
            return false;
        }
        PostBlock Block;
        GDITextureResource* Environment = P->SkyBlock(Block);
        Block.A[3] = float(InResources.StripFace);
        const bool bDrawn = P->DrawPost(PP_IBLSky, InResources.Strip, InResources.StripFace * 6, InResources.StripFace, { Environment }, Block);
        GDISetMaterialTexture(0, nullptr);
        return bDrawn;
    }

    bool RenderContext::CopyCaptureToStrip(SceneTargets& InStages, IBLResources& InResources, const uint32_t InFace)
    {
        GDITexture2D* Source = InStages.SceneColor ? InStages.SceneColor->ToGDITexture2D() : nullptr;
        if(!Source || !InResources.Strip || InFace >= 6)
        {
            return false;
        }
        const uint32_t Size = InResources.StripFace;
        PostBlock Block;
        const bool bDrawn = P->DrawPost(PP_CaptureCopy, InResources.Strip, Size, Size, { Source->Resource }, Block, false, InFace * Size);
        GDISetMaterialTexture(0, nullptr);
        return bDrawn;
    }

    bool RenderContext::PrefilterIBLBlock(IBLResources& InResources, const uint32_t InBlock)
    {
        GDITexture2D* Strip = InResources.Strip ? InResources.Strip->ToGDITexture2D() : nullptr;
        if(!Strip || !InResources.Atlas || InBlock > 4)
        {
            return false;
        }
        PostBlock Block;
        Block.A[0] = float(InBlock);
        Block.A[1] = 0.0f;   // the strip holds linear light
        const bool bDrawn = P->DrawPost(PP_IBLPrefilter, InResources.Atlas, 768, 768, { Strip->Resource }, Block);
        /* Once per atlas, Cloth's sheen table into its spare sixth block (IBLCommon.glsl, IBL_SheenDG). */
        if(bDrawn && !InResources.bSheenTable)
        {
            Block.A[0] = 5.0f;
            InResources.bSheenTable = P->DrawPost(PP_IBLPrefilter, InResources.Atlas, 768, 768, { Strip->Resource }, Block);
        }
        GDISetMaterialTexture(0, nullptr);
        return bDrawn;
    }

    void RenderContext::SetImageBasedLighting(const IBLResources* InResources, const float (*InProbes)[4], const uint32_t InCount,
                                              const bool bInSky)
    {
        P->IBL           = InResources;
        P->IBLProbeCount = InResources ? std::min<uint32_t>(InCount, 4) : 0;
        P->bIBLSky       = InResources && bInSky;
        std::memset(P->IBLProbes, 0, sizeof(P->IBLProbes));
        if(InProbes)
        {
            std::memcpy(P->IBLProbes, InProbes, sizeof(float) * 4 * 3 * P->IBLProbeCount);
        }
    }

    void RenderContext::SetSkyBackground(const bool bInForce)
    {
        P->bForceSkyBackground = bInForce;
    }

    void RenderContext::DrawStatUnit(const uint32_t InWidth, const uint32_t InHeight, const std::string& InText)
    {
        GDIShader* Pixel  = P->GetPostShader(PP_StatsOverlay);
        GDIShader* Vertex = P->GetDefaultVertex(EMaterialVertexInput::Fullscreen);
        if(!Pixel || !Vertex || InWidth == 0 || InHeight == 0 || P->Backend == ERenderBackend::Null)
        {
            return;
        }
        if(!P->OverlayFont)
        {
            const EngineCore::EImageFormat Format = P->Backend == ERenderBackend::OpenGL ? EngineCore::EImageFormat::BGRA8
                                                                                         : EngineCore::EImageFormat::RGBA8;
            EngineCore::Image Font(int(OVERLAY_GLYPH_COUNT * OVERLAY_GLYPH_WIDTH), int(OVERLAY_GLYPH_HEIGHT), Format);
            for(uint32_t Glyph = 0; Glyph < OVERLAY_GLYPH_COUNT; ++Glyph)
            {
                for(uint32_t Row = 0; Row < OVERLAY_GLYPH_HEIGHT; ++Row)
                {
                    for(uint32_t Column = 0; Column < OVERLAY_GLYPH_WIDTH; ++Column)
                    {
                        const bool bSet = (OVERLAY_FONT[Glyph][Row] >> (OVERLAY_GLYPH_WIDTH - 1 - Column)) & 1u;
                        uint8_t* Px = Font.GetData() + Font.GetStride() * Row + (Glyph * OVERLAY_GLYPH_WIDTH + Column) * 4;
                        Px[0] = Px[1] = Px[2] = 255;
                        Px[3] = bSet ? 255 : 0;
                    }
                }
            }
            P->OverlayFont = GDICreateTextureResource(Font, GDI_TB_Clamp_To_Edge, GDI_TF_Nearest);
            if(!P->OverlayFont)
            {
                return;
            }
        }

        /* Overlay_Block: 4 vec4s, then 60 uvec4s of characters (1024 bytes). */
        struct OverlayBlock
        {
            float    Layout[4];
            float    Grid[4];
            float    TextColor[4];
            float    PanelColor[4];
            uint32_t Text[240];
        } Block = {};
        static_assert(sizeof(OverlayBlock) == 1024, "Overlay_Block is the whole material block");

        uint32_t Columns = 0, Rows = 0, Column = 0;
        for(const char Each : InText)
        {
            if(Each == '\n') { ++Rows; Column = 0; continue; }
            Columns = std::max(Columns, ++Column);
        }
        Rows += InText.empty() || InText.back() == '\n' ? 0 : 1;
        if(Columns == 0 || Rows == 0 || Columns * Rows > 960)
        {
            return;
        }
        uint32_t Row = 0;
        Column = 0;
        for(const char Each : InText)
        {
            if(Each == '\n') { ++Row; Column = 0; continue; }
            const uint32_t Index = Row * Columns + Column++;
            Block.Text[Index / 4] |= uint32_t(uint8_t(Each)) << ((Index % 4) * 8);
        }

        const float Scale   = InHeight >= 900 ? 3.0f : InHeight >= 480 ? 2.0f : 1.0f;
        const float Padding = 3.0f * Scale;
        const float Width   = float(Columns) * 6.0f * Scale + 2.0f * Padding;
        const float Margin  = 8.0f;
        Block.Layout[0] = std::max(float(InWidth) - Width - Margin, 0.0f);
        Block.Layout[1] = Margin;
        Block.Layout[2] = Scale;
        Block.Layout[3] = float(InWidth);
        Block.Grid[0] = float(Columns);
        Block.Grid[1] = float(Rows);
        Block.Grid[2] = float(InHeight);
        Block.Grid[3] = Padding;
        const float TextColor[4]  = { 0.85f, 1.0f, 0.85f, 1.0f };
        const float PanelColor[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
        std::memcpy(Block.TextColor, TextColor, sizeof(TextColor));
        std::memcpy(Block.PanelColor, PanelColor, sizeof(PanelColor));

        SCOPED_DRAW_EVENT("StatUnit");
        GDISetViewport(0, 0, InWidth, InHeight);
        GDISetColorMask(EColM_RGBA);
        GDISetMaterialTexture(0, P->OverlayFont);
        P->EnsureInitialised();
        P->Bind(Vertex, Pixel);
        GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, &Block, uint(sizeof(Block)));
        GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &P->Scene, uint(sizeof(P->Scene)));
        GDISetDepthState(IDepthStateDesc());
        GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
        GDIDrawObject(P->ScreenObject);
        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        GDISetMaterialTexture(0, nullptr);
    }

    void RenderContext::SetUIImage(const char* InKey, const EngineCore::Image* InImage)
    {
        if(!InKey || !*InKey) return;
        const std::string Key = InKey;
        auto Found = P->UIImages.find(Key);
        if(Found != P->UIImages.end())
        {
            /* Kept until ReleaseGPUResources: a frame in flight may still sample it. */
            if(Found->second) { P->RetiredTextures.push_back(Found->second); }
            P->UIImages.erase(Found);
        }
        if(InImage && InImage->GetWidth() > 0 && InImage->GetHeight() > 0 && P->Backend != ERenderBackend::Null)
        {
            P->UIImages[Key] = GDICreateTextureResource(*InImage, GDI_TB_Clamp_To_Edge, GDI_TF_Linear);
        }
        ++P->UIImageEpoch;
    }

    void RenderContext::DrawUI(UISceneState& InState, const uint32_t InWidth, const uint32_t InHeight)
    {
        const UIDrawListView& List = InState.List;
        if(List.CommandCount == 0 || InWidth == 0 || InHeight == 0 || P->Backend == ERenderBackend::Null)
        {
            return;
        }
        if(!P->bUIVertexTried)
        {
            P->bUIVertexTried = true;
            P->UIVertexShader = P->LoadShader(UI_VERTEX_SHADER, ST_Vertex, "the UI", false);
        }
        GDIShader* Pixel  = P->GetPostShader(PP_UI);
        GDIShader* Vertex = P->UIVertexShader;
        if(!Pixel || !Vertex)
        {
            return;
        }

        /* The mesh: grown to fit (by half again, so a growing UI does not
         * rebuild every frame), rewritten only for a list it does not hold. */
        if(!InState.Mesh || InState.Mesh->GetDynamicVertexCapacity() < List.VertexCount
           || InState.Mesh->GetDynamicIndexCapacity() < List.IndexCount)
        {
            delete InState.Mesh;   // every backend defers the buffers past a frame still using them
            InState.UploadedGeneration = 0;
            InState.Mesh = GDICreateDynamicMesh(std::max<uint32_t>(4096, List.VertexCount + List.VertexCount / 2),
                                                std::max<uint32_t>(6144, List.IndexCount + List.IndexCount / 2));
            if(!InState.Mesh)
            {
                P->WarnOnce("ui|mesh", "the UI: this backend has no dynamic meshes; not drawn");
                return;
            }
        }
        if(InState.UploadedGeneration != InState.Generation)
        {
            const size_t Count = List.VertexCount;
            InState.Positions.resize(Count * 3);
            InState.UVs.resize(Count * 2);
            InState.Colors.resize(Count * 4);
            for(size_t At = 0; At < Count; ++At)
            {
                const UIVertex& V = List.Vertices[At];
                InState.Positions[At * 3 + 0] = V.X;
                InState.Positions[At * 3 + 1] = V.Y;
                InState.Positions[At * 3 + 2] = 0.0f;
                InState.UVs[At * 2 + 0] = V.U;
                InState.UVs[At * 2 + 1] = V.V;
                for(int Channel = 0; Channel < 4; ++Channel)
                {
                    InState.Colors[At * 4 + Channel] = float((V.Color >> (Channel * 8)) & 0xFFu) / 255.0f;
                }
            }
            GDIMeshDesc Desc;
            Desc.Positions   = InState.Positions.data();
            Desc.UV0         = InState.UVs.data();
            Desc.Colors      = InState.Colors.data();
            Desc.NumVertices = List.VertexCount;
            Desc.Indices     = List.Indices;
            Desc.NumIndices  = List.IndexCount;
            if(!GDIUpdateDynamicMesh(InState.Mesh, Desc))
            {
                P->WarnOnce("ui|update", "the UI: could not upload its draw list; not drawn");
                return;
            }
            InState.UploadedGeneration = InState.Generation;
        }

        /* Each command's texture, resolved once per list (and per change to
         * the UI images): "" white, "ui:..." an image, else a texture asset. */
        if(InState.TexturesGeneration != InState.Generation || InState.TexturesEpoch != P->UIImageEpoch)
        {
            if(!P->UIWhite)
            {
                EngineCore::Image White(1, 1, EngineCore::EImageFormat::RGBA8);
                std::memset(White.GetData(), 0xFF, 4);
                P->UIWhite = GDICreateTextureResource(White, GDI_TB_Clamp_To_Edge, GDI_TF_Nearest);
            }
            std::vector<GDITextureResource*> ByTexture(List.Textures.size(), nullptr);
            for(size_t At = 0; At < List.Textures.size(); ++At)
            {
                const std::string& Key = List.Textures[At];
                if(Key.empty())
                {
                    ByTexture[At] = P->UIWhite;
                }
                else if(Key.compare(0, 3, "ui:") == 0)
                {
                    auto Found = P->UIImages.find(Key);
                    ByTexture[At] = Found != P->UIImages.end() ? Found->second : nullptr;
                }
                else
                {
                    MaterialTexture Texture;
                    Texture.Path = Key;
                    ByTexture[At] = P->FindTexture(Texture, true);   // kept in the UI state: never swapped
                }
            }
            InState.CommandTextures.resize(List.CommandCount);
            for(uint32_t At = 0; At < List.CommandCount; ++At)
            {
                InState.CommandTextures[At] = ByTexture[List.Commands[At].Texture];
            }
            InState.TexturesGeneration = InState.Generation;
            InState.TexturesEpoch      = P->UIImageEpoch;
        }

        SCOPED_DRAW_EVENT("UI");
        P->EnsureInitialised();
        GDISetViewport(0, 0, InWidth, InHeight);
        GDISetColorMask(EColM_RGBA);
        GDISetDepthState(IDepthStateDesc());
        GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
        P->Bind(Vertex, Pixel);
        GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &P->Scene, uint(sizeof(P->Scene)));

        /* Pixels from the top-left to clip space: NDC +y is up, the target's
         * top row, on every backend (GDI.h). */
        EngineCore::Matrix4x4 Ortho;
        Ortho.InitIdentity();
        Ortho.Data[0][0] = 2.0f / float(InWidth);
        Ortho.Data[0][3] = -1.0f;
        Ortho.Data[1][1] = -2.0f / float(InHeight);
        Ortho.Data[1][3] = 1.0f;
        P->View3D->Camera->Projection = Ortho;
        P->View3D->Camera->ViewMat    = EngineCore::Matrix4x4().InitIdentity();
        GDITransform* Transform = P->UIObject->Transform;
        Transform->Translate = EngineCore::Vector3(0, 0, 0);
        Transform->Rotate    = EngineCore::Vector3(0, 0, 0);
        Transform->Scale     = EngineCore::Vector3(1, 1, 1);
        P->UIObject->Mesh = InState.Mesh;

        UIClipRect Clip;   // none
        GDITextureResource* Bound = nullptr;
        bool bFirst = true;
        for(uint32_t At = 0; At < List.CommandCount; ++At)
        {
            const UIDrawCmd& Command = List.Commands[At];
            GDITextureResource* Texture = InState.CommandTextures[At];
            if(Command.IndexCount == 0 || !Texture)
            {
                continue;   // a missing texture draws nothing rather than white
            }
            if(bFirst || Command.Clip != Clip)
            {
                Clip = Command.Clip;
                if(Clip.IsNone())
                {
                    GDIClearScissorRect();
                }
                else
                {
                    GDISetScissorRect(uint(std::max(Clip.X, 0)), uint(std::max(Clip.Y, 0)), uint(std::max(Clip.W, 0)), uint(std::max(Clip.H, 0)));
                }
            }
            if(bFirst || Texture != Bound)
            {
                GDISetMaterialTexture(0, Texture);
                Bound = Texture;
            }
            bFirst = false;
            InState.Mesh->SetDrawRange(Command.FirstIndex, Command.IndexCount);
            GDIDrawObject(P->UIObject);
            P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        GDIClearScissorRect();
        GDISetMaterialTexture(0, nullptr);
        InState.Mesh->SetDrawRange(0, 0);
        P->UIObject->Mesh = nullptr;
    }

    void RenderContext::SetTemporalJitter(const float InNdcX, const float InNdcY)
    {
        P->TemporalJitter[0] = InNdcX;
        P->TemporalJitter[1] = InNdcY;
    }

    void RenderContext::SetPostProcess(const PostProcessSettings& InSettings, const EngineCore::Matrix4x4* InPreviousViewProjection)
    {
        P->PostSettings           = InSettings;
        P->bHasPrevViewProjection = InPreviousViewProjection != nullptr;
        if(InPreviousViewProjection)
        {
            P->PrevViewProjection = *InPreviousViewProjection;
        }
    }

    bool RenderContext::SubmitPostProcess(const char* InMaterial, const uint8_t InStage)
    {
        const std::string Name = InMaterial ? std::string(InMaterial) : std::string();
        Material& Found = P->FindMaterial(Name);
        if(!Found.Pixel)
        {
            return false;   // warned
        }
        if(Found.Description.Domain != EMaterialDomain::PostProcess)
        {
            P->WarnOnce(Name + "|not-post", "material '" + Name + "' is " + GetMaterialDomainName(Found.Description.Domain)
                        + ", not PostProcess: its Post Process component is skipped");
            return false;
        }

        DrawCommand Item;
        Item.Kind      = DrawCommand::EKind::Fullscreen;
        Item.Mat       = &Found;
        Item.PostStage = InStage;
        P->Submit(Item, ERenderStage::PostProcess);
        return true;
    }

    void RenderContext::ReloadMaterial(const char* InName)
    {
        if(!InName || InName[0] == '\0')
        {
            P->RetireMaterials();
            return;
        }

        /* Every name for the same files: its path, and a bare name from an older world or a mesh. */
        std::string Stem;
        if(!GetMaterialStem(InName, Stem))
        {
            return;
        }
        for(auto Found = P->Materials.begin(); Found != P->Materials.end();)
        {
            std::string Each;
            if(!GetMaterialStem(Found->first, Each) || Each != Stem)
            {
                ++Found;
                continue;
            }

            Material& M = Found->second;
            for(GDIShader* Shader : { M.Pixel, M.Vertex[0], M.Vertex[1], M.Vertex[2], M.Vertex[3] })
            {
                if(!Shader) { continue; }
                if(P->BoundPixel == Shader)  { P->BoundPixel = nullptr; }
                if(P->BoundVertex == Shader) { P->BoundVertex = nullptr; }
                P->RetiredShaders.push_back(Shader);
            }

            /* Forget the warnings about it, so a fixed material says so if it fails again. */
            const std::string Prefix = "material '" + Found->first + "'|";
            for(auto It = P->Warned.begin(); It != P->Warned.end();)
            {
                It = It->compare(0, Prefix.size(), Prefix) == 0 ? P->Warned.erase(It) : std::next(It);
            }
            Found = P->Materials.erase(Found);
            ++P->ContentGeneration;
        }
    }

    bool RenderContext::MountShaderCache(const char* InModule)
    {
        const std::string Module = InModule ? InModule : "";
        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        if(Module.empty() || !Provider || P->Backend == ERenderBackend::Null)
        {
            return false;
        }

        const std::string Path = GetShaderCachePath(Module, P->Backend);
        std::vector<uint8_t> Bytes;
        if(!Provider->ReadAll(Path, Bytes))
        {
            RENDERCORE_LOG("shader cache: none for '" + Module + "' at " + Path + "; its materials load from loose files")
            return false;
        }

        ShaderCacheContents Contents;
        std::string Error;
        if(!ReadShaderCache(std::move(Bytes), Contents, Error))
        {
            RENDERCORE_LOG_WARN("shader cache " + Path + ": " + Error + "; not mounted")
            return false;
        }
        if(Contents.Platform != GetShaderPlatformName(P->Backend) || Contents.TemplateVersion != MATERIAL_TEMPLATE_VERSION)
        {
            RENDERCORE_LOG_WARN("shader cache " + Path + " was cooked for " + Contents.Platform + ", templates v"
                                + std::to_string(Contents.TemplateVersion) + "; this build needs " + GetShaderPlatformName(P->Backend)
                                + ", v" + std::to_string(MATERIAL_TEMPLATE_VERSION) + " -- recook it; not mounted")
            return false;
        }

        /* A Shipping cache holds its paths as hashes only (CorePack.h). */
        const size_t FileCount = Contents.Files.size() + Contents.HashedFiles.size();
        std::shared_ptr<EngineCore::IFileProvider> Layer;
        if(!Contents.HashedFiles.empty())
        {
            Layer = std::make_shared<EngineCore::HashedMemoryProvider>(std::move(Contents.HashedFiles));
        }
        else
        {
            Layer = std::make_shared<EngineCore::MemoryFileProvider>(std::move(Contents.Files));
        }
        if(!Provider->MountLayer(GetShaderCacheLayerName(Module), Layer))
        {
            RENDERCORE_LOG_WARN("shader cache " + Path + ": the installed file provider takes no layers; not mounted")
            return false;
        }

        /* Materials that failed (or loaded from elsewhere) load again, through it. */
        P->RetireMaterials();
        RENDERCORE_LOG("shader cache: mounted '" + Module + "' (" + std::to_string(FileCount) + " files)")
        return true;
    }

    void RenderContext::UnmountShaderCache(const char* InModule)
    {
        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        if(!InModule || !Provider)
        {
            return;
        }
        if(Provider->UnmountLayer(GetShaderCacheLayerName(InModule)))
        {
            P->RetireMaterials();
            RENDERCORE_LOG(std::string("shader cache: unmounted '") + InModule + "'")
        }
    }

#if WITH_EDITOR
    void RenderContext::RenderMaterialPreview(const MaterialPreviewRequest& InRequest)
    {
        std::unique_ptr<GDIReadback> Readback;
        std::string Error;
        if(!DrawMaterialPreview(InRequest, Readback, Error))
        {
            if(InRequest.Mailbox) { InRequest.Mailbox->Deliver(InRequest.Request, EngineCore::Image(), std::move(Error)); }
            return;
        }
        Impl::PendingPreview Pending;
        Pending.Readback = std::move(Readback);
        Pending.Request  = InRequest.Request;
        Pending.Mailbox  = InRequest.Mailbox;
        P->PendingPreviews.push_back(std::move(Pending));
        DeliverMaterialPreviews();   // a backend that read back at once
    }

    void RenderContext::DeliverMaterialPreviews()
    {
        auto& Pending = P->PendingPreviews;
        for(size_t At = 0; At < Pending.size();)
        {
            if(!Pending[At].Readback->IsReady())
            {
                ++At;
                continue;
            }
            EngineCore::Image Pixels = Pending[At].Readback->Take();
            std::string       Error;
            if(Pixels.GetWidth() == 0)
            {
                Error = "this backend reads nothing back (the Null backend draws nothing)";
            }
            if(Pending[At].Mailbox) { Pending[At].Mailbox->Deliver(Pending[At].Request, std::move(Pixels), std::move(Error)); }
            Pending.erase(Pending.begin() + std::ptrdiff_t(At));
        }
    }

    bool RenderContext::DrawMaterialPreview(const MaterialPreviewRequest& InRequest, std::unique_ptr<GDIReadback>& OutReadback, std::string& OutError)
    {
        const std::string Name(InRequest.Material, strnlen(InRequest.Material, sizeof(InRequest.Material)));
        const std::string Mesh(InRequest.Mesh, strnlen(InRequest.Mesh, sizeof(InRequest.Mesh)));
        const auto Clamp = [](const int InValue) { return InValue < 16 ? 16 : (InValue > 1024 ? 1024 : InValue); };
        const int Width  = Clamp(InRequest.Width);
        const int Height = Clamp(InRequest.Height);

        /* Every level of its textures: a preview is drawn once and kept. */
        struct BypassStreaming
        {
            bool& Flag;
            explicit BypassStreaming(bool& InFlag) : Flag(InFlag) { Flag = true; }
            ~BypassStreaming() { Flag = false; }
        } Bypass(P->bStreamBypass);

        /* Loaded (or failed) exactly as the viewport's draw would. */
        Material* Found = Name.empty() ? nullptr : &P->FindMaterial(Name);
        if(Found && !Found->Pixel)
        {
            OutError = "material '" + Name + "' could not be loaded -- see the log";
            return false;
        }
        if(!Found && Mesh.empty())
        {
            OutError = "a preview needs a material or a mesh";
            return false;
        }
        const bool bPostProcess = Found && Found->Description.Domain == EMaterialDomain::PostProcess;
        const bool bMesh        = !Mesh.empty() && !bPostProcess;

        std::shared_ptr<const MeshData> Source;
        if(bMesh)
        {
            Source = MeshAssetCache::Find(Mesh);
            if(!Source)
            {
                OutError = "mesh '" + Mesh + "' could not be loaded -- see the log";
                return false;
            }
        }

        P->EnsureInitialised();
        if(!P->PreviewTargets.Allocate(uint(Width), uint(Height)))
        {
            OutError = "could not create the preview render target";
            return false;
        }

        /* The view and lighting, restored afterwards. */
        /* A preview is lit by its studio light alone: no scene's probes or sky. */
        struct WithoutSceneIBL
        {
            Impl* Owner; const IBLResources* Saved; bool bSavedSky;
            explicit WithoutSceneIBL(Impl* InOwner) : Owner(InOwner), Saved(InOwner->IBL), bSavedSky(InOwner->bForceSkyBackground)
            {
                Owner->IBL = nullptr;
                Owner->bForceSkyBackground = false;
            }
            ~WithoutSceneIBL() { Owner->IBL = Saved; Owner->bForceSkyBackground = bSavedSky; }
        } NoIBL(P);
        const RenderView     SavedView     = P->View;
        const RenderLighting SavedLighting = P->Lighting;
        BeginSceneDraws();

        if(!bMesh)
        {
            /* Pixel-space view over the target. The eye sits at the quad's
             * depth: with ZNear -1, pulling it back would clip. For lighting,
             * though, the eye is a quad's width in front of it. */
            P->View = RenderView::MakeOrthographic(EngineCore::Size(Width, Height), EngineCore::Vector3(0.0, 0.0, 0.0), -1.0f, 1000.0f);
            const EngineCore::Vector3 Eye(double(Width) * 0.5, double(Height) * 0.5, -double(std::max(Width, Height)));
            P->UpdateScene(&Eye);

            /* An Opaque Lit material goes through the G-buffer; a PostProcess
             * one runs over the clear. */
            if(bPostProcess)
            {
                SubmitPostProcess(Name.c_str());
            }
            else
            {
                RenderTransform Quad;
                Quad.Position = EngineCore::Vector3(double(Width) * 0.5, double(Height) * 0.5, 0.0);
                DrawQuadWithMaterial(Quad, float(Width), float(Height), InRequest.Tint, Name.c_str());
            }
        }
        else
        {
            /* Orbit the bounds' centre at a distance that frames their sphere. */
            constexpr float FovY = 40.0f;
            const double Pi = 3.14159265358979;
            EngineCore::Vector3 Centre, Extent;
            Centre = EngineCore::Vector3((Source->BoundsMin[0] + Source->BoundsMax[0]) * 0.5, (Source->BoundsMin[1] + Source->BoundsMax[1]) * 0.5,
                                         (Source->BoundsMin[2] + Source->BoundsMax[2]) * 0.5);
            Extent = EngineCore::Vector3(Source->BoundsMax[0] - Source->BoundsMin[0], Source->BoundsMax[1] - Source->BoundsMin[1],
                                         Source->BoundsMax[2] - Source->BoundsMin[2]);
            const double Radius = std::max(Extent.GetLength() * 0.5, 0.01);

            const double Yaw   = double(InRequest.Yaw) * Pi / 180.0;
            const double Pitch = double(std::clamp(InRequest.Pitch, -85.0f, 85.0f)) * Pi / 180.0;
            const double Zoom  = double(std::clamp(InRequest.Zoom, 0.1f, 20.0f));
            /* +Y is up, so a camera above looks down along -Y. */
            const EngineCore::Vector3 Forward(std::sin(Yaw) * std::cos(Pitch), -std::sin(Pitch), std::cos(Yaw) * std::cos(Pitch));
            const double Distance = Radius / std::sin(double(FovY) * 0.5 * Pi / 180.0) * 1.1 / Zoom;
            const EngineCore::Vector3 Eye = Centre - Forward * Distance;
            const float Near = float(std::max(Distance - Radius * 1.5, Distance * 0.01));
            const float Far  = float(Distance + Radius * 2.0);
            P->View = RenderView::MakePerspective(EngineCore::Size(Width, Height), Eye, Forward, FovY, Near, Far);

            /* A studio light from over the camera's shoulder, the same
             * whatever the scene's; the default sky. */
            const EngineCore::Vector3 Down(0.0, -1.0, 0.0);
            EngineCore::Vector3 Side = Forward ^ Down;
            const double SideLength = Side.GetLength();
            Side = SideLength > 1e-6 ? Side * (1.0 / SideLength) : EngineCore::Vector3(1.0, 0.0, 0.0);
            RenderLighting Studio;
            Studio.LightDirection = Forward + Down * 0.7 + Side * 0.4;
            Studio.LightIntensity = 2.2f;
            P->Lighting = Studio;
            P->UpdateScene();

            /* The material in every section, else the request's slots. */
            const char* Slots[RENDER_PREVIEW_SLOTS];
            char SlotText[RENDER_PREVIEW_SLOTS][sizeof(InRequest.Slots[0])];
            for(size_t Slot = 0; Slot < RENDER_PREVIEW_SLOTS; ++Slot)
            {
                std::memcpy(SlotText[Slot], InRequest.Slots[Slot], sizeof(SlotText[Slot]));
                SlotText[Slot][sizeof(SlotText[Slot]) - 1] = '\0';
                Slots[Slot] = SlotText[Slot];
            }
            /* A skinned mesh in the pose the request carries, else its reference pose. */
            MaterialParameterOverrides Skin;
            if(InRequest.SkinBones > 0)
            {
                Skin.SkinPalette = InRequest.SkinPalette;
                Skin.SkinBones   = InRequest.SkinBones;
            }
            const char* Every[1] = { Name.c_str() };
            const bool bDrawn = Found ? DrawMeshSections(RenderTransform(), Mesh.c_str(), InRequest.Tint, Every, 1, /*bInEvery*/ true, Skin)
                                      : DrawMeshSections(RenderTransform(), Mesh.c_str(), InRequest.Tint, Slots, uint32_t(RENDER_PREVIEW_SLOTS),
                                                         /*bInEvery*/ false, Skin);
            if(!bDrawn)
            {
                RenderSceneStages(P->PreviewTargets, EngineCore::Color(38, 40, 46, 255));   // drop the queued draws
                P->View     = SavedView;
                P->Lighting = SavedLighting;
                P->UpdateScene();
                OutError = "this backend cannot draw mesh '" + Mesh + "'";
                return false;
            }
        }
        /* A channel of the surface instead of its lit colour (Scene_View). */
        P->Scene.View[0] = float(InRequest.Channel);
        RenderSceneStages(P->PreviewTargets, EngineCore::Color(38, 40, 46, 255));   // the editor viewport's clear
        P->Scene.View[0] = 0.0f;

        /* The copy joins the frame; the pixels are taken when it is done. */
        GDIRenderTarget* Final = P->FinalTarget ? P->FinalTarget : P->PreviewTargets.SceneColor;
        OutReadback.reset(Final->BeginReadback());
        P->View     = SavedView;
        P->Lighting = SavedLighting;
        P->UpdateScene();
        return true;
    }
#endif // WITH_EDITOR

    void RenderContext::ReleaseGPUResources()
    {
#if WITH_EDITOR
        P->PendingPreviews.clear();
#endif
        P->PreviewTargets.Release();
        P->ReleaseRenderTargets();
        P->ReleaseShadowAtlases();
        P->Shadows.ViewCount = 0;
        P->FinalTarget = nullptr;

        for(auto& Each : P->Meshes) { delete Each.second.Gpu; }
        P->Meshes.clear();
        delete P->OcclusionBox;
        P->OcclusionBox = nullptr;
        P->ForgetOcclusion();

        for(uint Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
        for(auto& Each : P->Textures)
        {
            if(Each.second.Gpu) { P->RetiredTextures.push_back(Each.second.Gpu); }
        }
        P->Textures.clear();
        P->Streamer.ReleaseAll();
        ++P->StreamEpoch;
        P->FreeRetiredStreamed(true);
        if(P->OverlayFont) { P->RetiredTextures.push_back(P->OverlayFont); }
        P->OverlayFont = nullptr;
        if(P->UIWhite) { P->RetiredTextures.push_back(P->UIWhite); }
        P->UIWhite = nullptr;
        for(auto& Each : P->UIImages)
        {
            if(Each.second) { P->RetiredTextures.push_back(Each.second); }
        }
        P->UIImages.clear();
        ++P->UIImageEpoch;
        for(GDITextureResource* Each : P->RetiredTextures)
        {
            Each->Release();
            delete Each;
        }
        P->RetiredTextures.clear();

        if(P->ScreenObject)
        {
            delete P->ScreenObject->Mesh;
            P->ScreenObject->Mesh = nullptr;
        }

        for(auto& Pools : P->PassPools)
        {
            for(auto& Each : Pools) { P->ReleasePassResources(Each.second); }
            Pools.clear();
        }
        for(auto& Each : P->PassShaders) { if(Each.second) { P->RetiredShaders.push_back(Each.second); } }
        P->PassShaders.clear();

        P->RetireMaterials();
        for(GDIShader* Each : P->RetiredShaders) { delete Each; }   // ~GDIShader releases its resource
        P->RetiredShaders.clear();
        P->BoundVertex = nullptr;
        P->BoundPixel  = nullptr;
        for(uint Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
    }
}
