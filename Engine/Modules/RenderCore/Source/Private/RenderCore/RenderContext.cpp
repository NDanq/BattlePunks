#include "Public/RenderCore/RenderContext.h"

#include <algorithm>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreImage.h"
#include "Public/GDI/Shaders/Classes/TestbedShaders.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderPipeline.h"
#include "Public/RenderCore/RenderScene.h"
#include "Public/RenderCore/ShaderCache.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Public/RenderCore/RenderTargetAsset.h"
#include "Private/RenderCore/DrawPolicy.h"
#include "Private/RenderCore/OverlayFont.h"
#include "Private/RenderCore/UIRenderer.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/SceneTargets.h"
#include "Private/RenderCore/ShadowSetup.h"

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

namespace RenderCore
{
    /* A loaded material: DrawCommand points at one (DrawPolicy.h). */
    struct RenderMaterial
    {
        MaterialDescription  Description;
        GDIShader*           Pixel = nullptr;      // null: failed to load, drawn as the fallback
        GDIShader*           Vertex[2] = {};       // its own Quad and Mesh shaders, with an offset only
        std::vector<uint8_t> Block;                // the defaults; the tint is written per draw
    };

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
            PP_PassCount
        };
        const char* const POST_PASS_SHADERS[PP_PassCount] = {
            "PPDofPrepare.fsh", "PPDofBlur.fsh", "PPDofComposite.fsh", "PPMotionBlur.fsh",
            "PPBloomDownsample.fsh", "PPBloomUpsample.fsh", "PPBloomComposite.fsh", "PPFxaa.fsh",
            "PPAmbientOcclusion.fsh", "PPAmbientBlur.fsh", "StatsOverlay.fsh", "ClearGradient.fsh", "CaptureCopy.fsh", "Tonemap.fsh",
            "IBLSky.fsh", "IBLPrefilter.fsh", "IBLLighting.fsh", "Sky.fsh", "ViewTarget.fsh", "UI.fsh",
        };
        /* Each pass's draw event (DrawPost). */
        const char* const POST_PASS_NAMES[PP_PassCount] = {
            "DofPrepare", "DofBlur", "DofComposite", "MotionBlur",
            "BloomDownsample", "BloomUpsample", "BloomComposite", "Fxaa",
            "AmbientOcclusion", "AmbientBlur", "StatsOverlay", "ClearGradient", "CaptureCopy", "Tonemap",
            "IBLSky", "IBLPrefilter", "IBLLighting", "Sky", "ViewTarget", "UI",
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
        static_assert(sizeof(SceneUniforms) == 3856 && sizeof(SceneUniforms) <= GDI_SCENE_BLOCK_MAX_BYTES,
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
        };
        std::unordered_map<std::string, GpuMesh> Meshes;

        /* A material's textures, keyed by path and the sampler settings they
         * resolve to; rebuilt when TextureAssetCache's pointer differs from
         * Source. Replaced ones are kept until ReleaseGPUResources: a frame in
         * flight may still sample them. */
        struct GpuTexture
        {
            std::shared_ptr<const TextureData> Source;
            GDITextureResource*                Gpu = nullptr;
        };
        std::unordered_map<std::string, GpuTexture> Textures;
        std::vector<GDITextureResource*>            RetiredTextures;

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
        GDIShader*              DefaultVertex[3] = {};          // MaterialDefault, by EMaterialVertexInput
        bool                    bDefaultVertexTried[3] = {};
        std::vector<GDIShader*> RetiredShaders;                 // see RenderContext::ReloadMaterial
        std::vector<uint8_t>    OverrideBlock;                  // BindMaterial's: a block with an entity's overrides
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
        GDITextureResource*   PostSceneDepth = nullptr;
        GDIShader*            PostShaders[PP_PassCount] = {};
        bool                  bPostShaderTried[PP_PassCount] = {};
        const EngineCore::Matrix4x4* PlaceProjection = nullptr;
        const EngineCore::Matrix4x4* PlaceView       = nullptr;

        /* RenderMaterialPreview's targets, square, resized as asked; and the
         * target the last RenderSceneStages finished in. */
        SceneTargets     PreviewTargets{"RenderCore.MaterialPreview"};
        GDIRenderTarget* FinalTarget = nullptr;

        std::atomic<uint64_t> DrawCallCount{0};

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

            /* The vertices and UVs every backend's own screen quad uses. */
            static const float Verts[] = { -1.f, -1.f, 0.f,  -1.f, 1.f, 0.f,  1.f, 1.f, 0.f,
                                            1.f,  1.f, 0.f,   1.f, -1.f, 0.f, -1.f, -1.f, 0.f };
            static const float UVs[]   = { 0.f, 0.f,  0.f, 1.f,  1.f, 1.f,  1.f, 1.f,  1.f, 0.f,  0.f, 0.f };
            ScreenObject = new GDIViewObject();
            ScreenObject->Init(View3D);
            ScreenObject->Mesh = GDICreateQuadMesh(Verts, UVs, 18, 12, false);

            UIObject = new GDIViewObject();
            UIObject->Init(View3D);
        }

        void Bind(GDIShader* InVertex, GDIShader* InPixel)
        {
            if(InVertex != BoundVertex || InPixel != BoundPixel)
            {
                GDISetBoundShader(InVertex, InPixel);
                BoundVertex = InVertex;
                BoundPixel  = InPixel;
            }
        }

        /* Camera, and InTransform with scale times InScaleX/Y (1 for a mesh). */
        void Place(GDIViewObject* InObject, const RenderTransform& InTransform, const float InScaleX, const float InScaleY)
        {
            /* Here rather than in SetView(), so a RenderContext without a
             * RenderScene (tests) still draws. */
            View3D->Camera->Projection = PlaceProjection ? *PlaceProjection : View.Projection;
            View3D->Camera->ViewMat    = PlaceView ? *PlaceView : View.View;

            GDITransform* Transform = InObject->Transform;
            Transform->Translate = InTransform.Position;
            Transform->Rotate    = InTransform.Rotation;
            Transform->Scale     = EngineCore::Vector3(InTransform.Scale.GetX() * double(InScaleX),
                                                       InTransform.Scale.GetY() * double(InScaleY),
                                                       InTransform.Scale.GetZ());
        }

        /* The camera and the quad's placement, shared by both quad draws. */
        void PlaceQuad(const RenderTransform& InTransform, const float InWidth, const float InHeight)
        {
            Place(ViewObject, InTransform, InWidth, InHeight);
        }

        /* Null if unloadable or unbuildable; OutSource is set if found. */
        GDIMesh* FindMesh(const std::string& InPath, std::shared_ptr<const MeshData>& OutSource)
        {
            OutSource = MeshAssetCache::Find(InPath);
            GpuMesh& Entry = Meshes[InPath];
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
            Desc.NumVertices = OutSource->GetVertexCount();
            Desc.Indices     = OutSource->Indices.data();
            Desc.NumIndices  = uint32_t(OutSource->Indices.size());
            Entry.Gpu = GDICreateMesh(Desc);
            if(!Entry.Gpu)
            {
                RENDERCORE_LOG_WARN("mesh '" + InPath + "': this backend cannot draw it; drawing a quad")
            }
            return Entry.Gpu;
        }

        /* A material texture's GPU copy with its sampler: the node's settings
         * where it overrides them, else the asset's. Null (warned by the
         * cache) if the file cannot be loaded: the slot reads transparent. */
        GDITextureResource* FindTexture(const MaterialTexture& InTexture)
        {
            /* A render target (a scene capture), not a file. */
            if(IsRenderTargetPath(InTexture.Path))
            {
                return RenderTargetTexture(InTexture.Path);
            }
            std::shared_ptr<const TextureData> Source = TextureAssetCache::Find(InTexture.Path);
            if(!Source)
            {
                return nullptr;
            }

            const ETextureFilter Filter = InTexture.bFilter ? InTexture.Filter : Source->Filter;
            const ETextureWrap   Wrap   = InTexture.bWrap ? InTexture.Wrap : Source->Wrap;
            const std::string    Key    = InTexture.Path + "|" + std::to_string(int(Filter)) + std::to_string(int(Wrap));

            GpuTexture& Entry = Textures[Key];
            if(Entry.Source == Source)
            {
                return Entry.Gpu;
            }
            if(Entry.Gpu) { RetiredTextures.push_back(Entry.Gpu); }
            Entry.Source = Source;

            const EGDITextureBorder Border = Wrap == ETextureWrap::Clamp  ? GDI_TB_Clamp_To_Edge
                                           : Wrap == ETextureWrap::Mirror ? GDI_TB_Mirrored_Repeat : GDI_TB_Repeat;
            const EGDITextureFilter GpuFilter = Filter == ETextureFilter::Nearest ? GDI_TF_Nearest : GDI_TF_Linear;
            if(Source->Mips.empty())
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
            return Entry.Gpu;
        }

        void WarnOnce(const std::string& InKey, const std::string& InMessage)
        {
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

        /*
         * One full-screen pass of the stack: InPass's shader into InTarget
         * (InWidth x InHeight, no depth), InTextures as material slots 0..,
         * InBlock as PP_Block. Additive blends into what InTarget holds. False
         * (warned by LoadShader) if a shader is missing.
         */
        bool DrawPost(const EPostPass InPass, GDIRenderTarget* InTarget, const uint InWidth, const uint InHeight,
                      std::initializer_list<GDITextureResource*> InTextures, const PostBlock& InBlock, const bool bInAdditive = false,
                      const uint InX = 0)
        {
            GDIShader* Pixel  = GetPostShader(InPass);
            GDIShader* Vertex = GetDefaultVertex(EMaterialVertexInput::Fullscreen);
            if(!Pixel || !Vertex || !InTarget)
            {
                return false;
            }
            SCOPED_DRAW_EVENT(POST_PASS_NAMES[InPass]);
            GDISetRenderTargets(&InTarget, 1, nullptr);
            GDISetViewport(InX, 0, InWidth, InHeight);   // InX: a cube face's place in its strip
            GDISetColorMask(EColM_RGBA);

            uint32_t Slot = 0;
            for(GDITextureResource* Each : InTextures)
            {
                GDISetMaterialTexture(Slot++, Each);
            }
            EnsureInitialised();
            Bind(Vertex, Pixel);
            /* These blocks carry their matrix first; the others start at A. */
            if(InPass == PP_MotionBlur || InPass == PP_AmbientOcclusion || InPass == PP_IBLSky || InPass == PP_Sky)
            {
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, &InBlock, uint(sizeof(InBlock)));
            }
            else
            {
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, InBlock.A, uint(sizeof(InBlock) - sizeof(InBlock.Matrix)));
            }
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));
            GDISetDepthState(IDepthStateDesc());
            GDISetBlendState(bInAdditive ? IBlendStateDesc(true, BM_One, BM_One) : IBlendStateDesc());
            GDIDrawObject(ScreenObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
            for(uint32_t Each = 0; Each < Slot; ++Each)
            {
                GDISetMaterialTexture(Each, nullptr);
            }
            return true;
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
        GDITextureResource* OverlayFont      = nullptr;   // OverlayFont.h's glyphs side by side, made on first use

        /* DrawUI's: its vertex shader (UI.fsh is PP_UI), the white texture a
         * list's texture 0 is, and Renderer::SetUIImage's images by key --
         * UIImageEpoch counts their changes, for each scene's resolved textures. */
        GDIShader*                                           UIVertexShader = nullptr;
        bool                                                 bUIVertexTried = false;
        GDITextureResource*                                  UIWhite        = nullptr;
        std::unordered_map<std::string, GDITextureResource*> UIImages;
        uint64_t                                             UIImageEpoch   = 1;

        /* "stat frame": the scene timing this GDI block, and the last block's. */
        bool        bTimeStages = false;
        const void* TimedOwner  = nullptr;

        /* InTimer's start (or end) timestamp: indices 2t and 2t + 1. */
        void StageTimestamp(const ERenderGpuTimer InTimer, const bool bInEnd) const
        {
            if(bTimeStages) { GDIWriteGPUTimestamp(uint32_t(InTimer) * 2 + (bInEnd ? 1 : 0)); }
        }
        bool                bGBufferThisFrame = false;   // RenderSceneStages wrote the G-buffer: Bloom may read its emissive

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
            return Lightmap.Path.empty() ? nullptr : FindTexture(Lightmap);
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
                Environment = FindTexture(Hdri);
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
            const uint32_t PixelKey = GetPixelPermutationKey(Desc.Domain, Desc.Shading);
            GDIShader* Pixel = LoadShader(GetPermutationFileName(Stem, PixelKey, false), ST_Pixel, Who, false);
            if(!Pixel)
            {
                return Loaded;   // warned; drawn as the fallback
            }

            if(Desc.bHasOffset && Desc.Domain != EMaterialDomain::PostProcess)
            {
                for(const EMaterialVertexInput Input : { EMaterialVertexInput::Quad, EMaterialVertexInput::Mesh })
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
            const std::vector<uint8_t>& Block = InMaterial.Block;
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
        bool BindMaterial(Material& InMaterial, const EMaterialVertexInput InInput, const EngineCore::Color InTint,
                          const DrawCommand* InItem = nullptr)
        {
            GDIShader* Vertex = VertexFor(InMaterial, InInput);
            if(!Vertex || !InMaterial.Pixel)
            {
                return false;
            }

            Bind(Vertex, InMaterial.Pixel);

            std::vector<uint8_t>& Block = InMaterial.Block;
            const float Tint[4] = { InTint.Rf(), InTint.Gf(), InTint.Bf(), InTint.Af() };
            std::memcpy(Block.data(), Tint, sizeof(Tint));

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

            const std::vector<uint8_t>& Upload = BlockWithOverrides(InMaterial, InItem);
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Upload.data(), uint(Upload.size()));
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));

            /* Its textures, every other slot cleared; slot 0 (a PostProcess
             * pass's scene) is the caller's. */
            for(uint32_t Slot = MATERIAL_TEXTURE_SLOT_FIRST; Slot < MATERIAL_TEXTURE_SLOTS; ++Slot)
            {
                GDISetMaterialTexture(Slot, nullptr);
            }
            for(const MaterialTexture& Each : InMaterial.Description.Textures)
            {
                GDISetMaterialTexture(Each.Slot, FindTexture(Each));
            }
            /* Slot 0, its own outside PostProcess (MaterialTemplate_PS.glsl): a Base
             * Pass material's lightmap, a forward Lit material's probe atlas. */
            if(InMaterial.Description.Domain != EMaterialDomain::PostProcess && InMaterial.Description.Shading == EMaterialShading::Lit)
            {
                const bool bBasePass = InMaterial.Description.Domain == EMaterialDomain::Opaque
                                    || InMaterial.Description.Domain == EMaterialDomain::Masked;
                if(bBasePass)
                {
                    GDISetMaterialTexture(0, bLightmap ? LightmapTexture(Baked->Atlas) : nullptr);
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

            /* No shadow until PrepareShadows draws some. */
            Scene.ShadowParams[0] = 0.0f;
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

        /* The frame's shadow views from View and Lighting, into the scene block. */
        void PrepareShadows()
        {
            BuildShadowFrame(Lighting, View, Shadows);
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

            /* Local views follow the sun's, in Locals order. */
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

        /* One caster section into the bound atlas tile, with its material's
         * vertex shader (World Position Offset) and ShadowDepth.fsh. */
        void IssueShadowCaster(const DrawCommand& InItem)
        {
            EnsureInitialised();
            GDIShader* Pixel  = GetShadowDepthShader();
            GDIShader* Vertex = InItem.Mat ? VertexFor(*InItem.Mat, EMaterialVertexInput::Mesh) : GetDefaultVertex(EMaterialVertexInput::Mesh);
            if(!Pixel || !Vertex)
            {
                return;   // warned by LoadShader
            }

            Place(MeshObject, InItem.Transform, 1.0f, 1.0f);
            MeshObject->Mesh = InItem.Mesh;
            Bind(Vertex, Pixel);

            static const uint8_t NoMaterial[MATERIAL_ENGINE_BLOCK_BYTES] = {};
            if(InItem.Mat && !InItem.Mat->Block.empty())
            {
                /* The entity's overrides too: a parameter may drive its World Position Offset. */
                const std::vector<uint8_t>& Block = BlockWithOverrides(*InItem.Mat, &InItem);
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Block.data(), uint(Block.size()));
            }
            else
            {
                GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, NoMaterial, uint(sizeof(NoMaterial)));
            }
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));

            IDepthStateDesc Depth;
            Depth.bDepthTest = true;
            Depth.bDepthMask = true;
            Depth.DepthMode  = CTM_LEQUAL;
            GDISetDepthState(Depth);
            GDISetBlendState(IBlendStateDesc());

            InItem.Mesh->SetDrawRange(InItem.First, InItem.Count);
            GDIDrawObject(MeshObject);
            InItem.Mesh->SetDrawRange(0, 0);
            MeshObject->Mesh = nullptr;
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
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
             * each run of one view's casters is a draw event of its own. */
            const std::vector<uint32_t> Order = Policy.GetIssueOrder();
            int32_t BoundAtlas = -1;
            for(size_t Start = 0; Start < Order.size();)
            {
                const int32_t ViewIndex = Policy[Order[Start]].ShadowView;
                size_t End = Start + 1;
                while(End < Order.size() && Policy[Order[End]].ShadowView == ViewIndex) { ++End; }
                if(ViewIndex >= 0 && uint32_t(ViewIndex) < Shadows.ViewCount)
                {
                    const ShadowView& View = Shadows.Views[ViewIndex];
                    if(int32_t(View.Atlas) != BoundAtlas)
                    {
                        BoundAtlas = int32_t(View.Atlas);
                        GDIRenderTarget* Target = ShadowAtlas[View.Atlas];
                        GDISetRenderTargets(&Target, 1, Target);
                        GDISetColorMask(EColM_None);
                    }
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
            PlaceProjection = nullptr;
            PlaceView       = nullptr;
        }

        /* "Sun", "Sun Cascade 1", "Spot 2", "Point 0 +X": what a shadow view
         * is of, for its draw event. */
        std::string GetShadowViewName(const int32_t InView) const
        {
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
            PlaceQuad(InItem.Transform, InItem.Width, InItem.Height);
            GDIDrawObject(ViewObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        void IssueMeshSection(const DrawCommand& InItem)
        {
            EnsureInitialised();
            Place(MeshObject, InItem.Transform, 1.0f, 1.0f);
            MeshObject->Mesh = InItem.Mesh;

            if(!InItem.Mat || !BindMaterial(*InItem.Mat, EMaterialVertexInput::Mesh, InItem.Tint, &InItem))
            {
                GDIShader* Pixel = GDIGetShader<Mesh_PS>();
                Bind(GDIGetShader<Mesh_VS>(), Pixel);
                ApplyPlainState();
                if(Pixel)
                {
                    Pixel->SetUniform("Tint", EngineCore::Vector3(InItem.Tint.Rf(), InItem.Tint.Gf(), InItem.Tint.Bf()));
                }
            }

            InItem.Mesh->SetDrawRange(InItem.First, InItem.Count);
            GDIDrawObject(MeshObject);
            InItem.Mesh->SetDrawRange(0, 0);
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

        void Issue(const DrawCommand& InItem)
        {
            switch(InItem.Kind)
            {
                case DrawCommand::EKind::Quad:         IssueQuad(InItem);         break;
                case DrawCommand::EKind::MaterialQuad: IssueMaterialQuad(InItem); break;
                case DrawCommand::EKind::MeshSection:  IssueMeshSection(InItem);  break;
                case DrawCommand::EKind::Fullscreen:   IssueFullscreen(InItem);   break;
            }
        }

        /* Into InStage's policy inside a scene; at once outside one. */
        void Submit(DrawCommand InItem, const ERenderStage InStage)
        {
            if(Capture)
            {
                Capture->push_back({ InItem, InStage });
                return;
            }
            if(!bDeferring)
            {
                Issue(InItem);
                return;
            }
            const EngineCore::Vector3 Offset = InItem.Transform.Position - View.Origin;
            InItem.Depth = float(Offset.Dot(ViewForward(View)));
            PolicyFor(InStage).Add(InItem);
        }

        /* A usable material's stage; Unlit for the plain fallbacks. */
        static ERenderStage StageOf(const Material* InMaterial)
        {
            return InMaterial ? GetMaterialRenderStage(InMaterial->Description.Domain, InMaterial->Description.Shading)
                              : ERenderStage::Unlit;
        }

        /* InPolicy's draws [InBegin, InEnd) of an issue order: the static draw
         * lists' first, then the rest, each under its own draw event. Only for
         * a stage that neither blends nor depends on order within itself. */
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
         * as material texture 4, after GBuffer0-3. False (warned) if this
         * backend cannot.
         */
        bool IssueLighting(SceneTargets& InTargets)
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

            /* The shadow atlases this frame drew, after the scene depth; then
             * this frame's ambient occlusion, if it has any. */
            for(uint32_t Atlas = 0; Atlas < SA_Count; ++Atlas)
            {
                GDITexture2D* Map = Shadows.ViewCount > 0 && ShadowAtlas[Atlas] ? ShadowAtlas[Atlas]->ToDepthTexture2D() : nullptr;
                GDISetMaterialTexture(RENDER_GBUFFER_TARGETS + 1 + Atlas, Map ? Map->Resource : nullptr);
            }
            const uint32_t OcclusionSlot = RENDER_GBUFFER_TARGETS + 1 + SA_Count;
            GDISetMaterialTexture(OcclusionSlot, AmbientOcclusion);

            /* Lighting_Block: x, whether the occlusion texture is this frame's. */
            float LightingBlock[MATERIAL_ENGINE_BLOCK_BYTES / sizeof(float)] = {};
            LightingBlock[0] = AmbientOcclusion ? 1.0f : 0.0f;
            EnsureInitialised();
            Bind(Vertex, Pixel);
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, LightingBlock, uint(sizeof(LightingBlock)));
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));
            GDISetDepthState(IDepthStateDesc());   // no depth test: there is no depth bound
            GDISetBlendState(IBlendStateDesc());
            GDIDrawObject(ScreenObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);

            /* Unbound again: the depth is about to be an attachment. */
            for(uint32_t Index = 0; Index <= OcclusionSlot; ++Index)
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
                for(GDIShader* Shader : { M.Pixel, M.Vertex[0], M.Vertex[1] })
                {
                    if(Shader) { RetiredShaders.push_back(Shader); }
                }
            }
            Materials.clear();
            for(int Index = 0; Index < 3; ++Index)
            {
                if(DefaultVertex[Index]) { RetiredShaders.push_back(DefaultVertex[Index]); }
                DefaultVertex[Index] = nullptr;
                bDefaultVertexTried[Index] = false;
            }
            if(LightingShader) { RetiredShaders.push_back(LightingShader); }
            LightingShader = nullptr;
            bLightingTried = false;
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
        }
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
                if(IssueFullscreen(Materials[Index]))
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
            Sizes(Block, Width, Height);
            GDIRenderTarget* Dest = Next();
            if(DrawPost(PP_MotionBlur, Dest, Width, Height, { Texture(Source), Depth }, Block))
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

        /* Antialiasing, last: FXAA over everything before it. */
        if(bAa)
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

        std::shared_ptr<const MeshData> Source;
        GDIMesh* Mesh = P->FindMesh(InMeshPath, Source);
        if(!Mesh || !Source)
        {
            return false;
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
            Item.First     = Section.FirstIndex;
            Item.Count     = Sections.empty() ? 0u : Section.IndexCount;
            Item.Overrides     = InOverrides.Items;
            Item.OverrideCount = InOverrides.Count;
            Item.Baked         = InOverrides.Baked;
            Item.MaterialSlot  = uint8_t(SlotIndex);   // the section's slot, whichever material it fell back to
            P->Submit(Item, Impl::StageOf(Chosen));
        }
        return true;
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
        P->bDeferring        = true;
        P->Shadows.ViewCount = 0;   // a preview has none; a scene's PrepareShadows adds them
        P->CasterView        = -1;
    }

    uint32_t RenderContext::PrepareShadows()
    {
        P->PrepareShadows();
        return P->Shadows.ViewCount;
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

    bool RenderContext::DrawMeshShadow(const RenderTransform& InTransform, const char* InMeshPath,
                                       const char* const* InSlots, const uint32_t InSlotCount,
                                       const MaterialParameterOverrides& InOverrides)
    {
        if(P->CasterView < 0 || !P->bDeferring || !InMeshPath || InMeshPath[0] == '\0')
        {
            return false;
        }

        std::shared_ptr<const MeshData> Source;
        GDIMesh* Mesh = P->FindMesh(InMeshPath, Source);
        if(!Mesh || !Source)
        {
            return false;
        }

        static const MeshSection Everything;
        const std::vector<MeshSection>& Sections = Source->Sections;
        const size_t SectionCount = Sections.empty() ? 1 : Sections.size();
        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];
            const char* Slot = InSlots && Index < InSlotCount && InSlots[Index] && InSlots[Index][0] != '\0' ? InSlots[Index] : nullptr;
            Material* Chosen = P->SectionMaterial(Slot, Section.Material);

            /* Only what writes depth casts: Opaque and Masked (Masked whole,
             * for now -- its clip is not run here). The plain fallback is opaque. */
            if(Chosen && Chosen->Description.Domain != EMaterialDomain::Opaque && Chosen->Description.Domain != EMaterialDomain::Masked)
            {
                continue;
            }

            DrawCommand Item;
            Item.Kind       = DrawCommand::EKind::MeshSection;
            Item.Transform  = InTransform;
            Item.Mat        = Chosen;
            Item.Mesh       = Mesh;
            Item.First      = Section.FirstIndex;
            Item.Count      = Sections.empty() ? 0u : Section.IndexCount;
            Item.ShadowView    = P->CasterView;
            Item.Overrides     = InOverrides.Items;
            Item.OverrideCount = InOverrides.Count;
            Item.MaterialSlot  = uint8_t(Index);
            if(P->Capture) { P->Capture->push_back({ Item, ERenderStage::Shadows }); }
            else           { P->PolicyFor(ERenderStage::Shadows).Add(Item); }
        }
        return true;
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
        const void* Previous = P->TimedOwner;
        P->TimedOwner = bInTime && CanTimeGpu() ? InOwner : nullptr;
        for(float& Each : OutGpuMs) { Each = -1.0f; }
        if(!Previous)
        {
            return nullptr;
        }
        double         Stamps[GDI_GPU_TIMESTAMP_MAX] = {};
        const uint64_t Written = GDIGetPreviousGPUTimestamps(Stamps);
        for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
        {
            const uint64_t Both = (uint64_t(1) << (Timer * 2)) | (uint64_t(1) << (Timer * 2 + 1));
            if((Written & Both) == Both)
            {
                OutGpuMs[Timer] = float(std::max(Stamps[Timer * 2 + 1] - Stamps[Timer * 2], 0.0));
            }
        }
        return Written ? Previous : nullptr;
    }

    void RenderContext::WriteGpuTimestamp(const ERenderGpuTimer InTimer, const bool bInEnd)
    {
        if(P->TimedOwner) { GDIWriteGPUTimestamp(uint32_t(InTimer) * 2 + (bInEnd ? 1 : 0)); }
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
            const char* const GBufferNames[RENDER_GBUFFER_TARGETS] = { "GBuffer0 (base colour, AO)", "GBuffer1 (normal)",
                                                                       "GBuffer2 (metallic, roughness, specular)", "GBuffer3 (emissive)" };
            for(uint32_t Index = 0; Index < RENDER_GBUFFER_TARGETS; ++Index) { Add(InTargets.GBuffer[Index], GBufferNames[Index]); }
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

    void RenderContext::RenderSceneStages(SceneTargets& InTargets, const EngineCore::Color InClearColor, const bool bInTimed,
                                          const EngineCore::Color* InClearBottom)
    {
        /* Timed only for the block's owning scene: a preview's stages in the
         * same block would write the same indices. */
        struct TimedScope
        {
            Impl* Owner;
            TimedScope(Impl* InOwner, const bool bInTimed) : Owner(InOwner) { Owner->bTimeStages = bInTimed && Owner->TimedOwner; }
            ~TimedScope() { Owner->bTimeStages = false; }
        } Timed(P, bInTimed);

        P->bDeferring  = false;
        P->FinalTarget = nullptr;
        InTargets.Final = nullptr;
        for(uint32_t& Count : P->StageDrawCounts) { Count = 0; }

        const auto Finish = [this]()
        {
            for(DrawPolicy& Policy : P->Policies) { Policy.Clear(); }
        };

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

        /* PrePass: RenderScene culled before the draws were queued. */
        P->StageTimestamp(RGT_PrePass, false);
        P->IssueStage(ERenderStage::PrePass);
        P->StageTimestamp(RGT_PrePass, true);

        /* Base Pass: the G-buffer, created the first time something needs it. */
        const DrawPolicy& Base = P->PolicyFor(ERenderStage::BasePass);
        const bool bDeferred = !Base.IsEmpty() && InTargets.EnsureGBuffer();
        P->bGBufferThisFrame = bDeferred;
        if(bDeferred)
        {
            P->StageTimestamp(RGT_BasePass, false);
            BindTargets(InTargets.GBuffer, RENDER_GBUFFER_TARGETS, SceneColor);
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
            BindTargets(&SceneColor, 1, nullptr);
            SCOPED_DRAW_EVENT(GetRenderStageName(ERenderStage::Lighting));
            {
                SCOPED_DRAW_EVENT("DeferredLighting");
                if(P->IssueLighting(InTargets))
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
                PostBlock Block;
                Block.A[0] = P->AmbientOcclusion ? 1.0f : 0.0f;
                P->DrawPost(PP_IBLLighting, SceneColor, Width, Height,
                            { Texture(InTargets.GBuffer[0]), Texture(InTargets.GBuffer[1]), Texture(InTargets.GBuffer[2]),
                              Depth ? Depth->Resource : nullptr, P->AmbientOcclusion, P->IBLAtlasTexture() },
                            Block, /*bInAdditive*/ true);
                for(uint32_t Slot = 0; Slot < 6; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
            }
            P->AmbientOcclusion = nullptr;
            P->StageTimestamp(RGT_Lighting, true);
        }

        /* Forward, over the lit scene and against its depth. */
        P->StageTimestamp(RGT_Forward, false);
        BindTargets(&SceneColor, 1, SceneColor);
        P->IssueStage(ERenderStage::Unlit);
        P->IssueStage(ERenderStage::TranslucentLit);
        P->IssueStage(ERenderStage::TranslucentUnlit);
        P->StageTimestamp(RGT_Forward, true);

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
                GDITexture2D*    Linear = SceneColor->ToGDITexture2D();
                PostBlock Block;
                if(Mapped && Linear && P->DrawPost(PP_Tonemap, Mapped, Width, Height, { Linear->Resource }, Block))
                {
                    Display = Mapped;
                }
                GDISetMaterialTexture(0, nullptr);
            }
            Source = P->RunPostProcessStack(InTargets, Post, bHasPrev, Display);
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
                    ByTexture[At] = P->FindTexture(Texture);
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

        /* Pixels from the top-left to clip space, through the view's projection
         * so every backend's own flip applies (Metal's PackViewParamsUBO, the
         * D3D12 shaders' --flip-vert-y): NDC +y is up everywhere. */
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
            for(GDIShader* Shader : { M.Pixel, M.Vertex[0], M.Vertex[1] })
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

        const size_t FileCount = Contents.Files.size();
        auto Layer = std::make_shared<EngineCore::MemoryFileProvider>(std::move(Contents.Files));
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
    bool RenderContext::RenderMaterialPreview(const MaterialPreviewRequest& InRequest, EngineCore::Image& OutImage, std::string& OutError)
    {
        const std::string Name(InRequest.Material, strnlen(InRequest.Material, sizeof(InRequest.Material)));
        const std::string Mesh(InRequest.Mesh, strnlen(InRequest.Mesh, sizeof(InRequest.Mesh)));
        const auto Clamp = [](const int InValue) { return InValue < 16 ? 16 : (InValue > 1024 ? 1024 : InValue); };
        const int Width  = Clamp(InRequest.Width);
        const int Height = Clamp(InRequest.Height);

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
            const bool bDrawn = Found ? DrawMesh(RenderTransform(), Mesh.c_str(), InRequest.Tint, Name.c_str())
                                      : DrawMesh(RenderTransform(), Mesh.c_str(), InRequest.Tint, Slots, uint32_t(RENDER_PREVIEW_SLOTS));
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

        /* Flushes, reads back, resumes the frame. */
        GDIRenderTarget* Final = P->FinalTarget ? P->FinalTarget : P->PreviewTargets.SceneColor;
        OutImage    = Final->ToImage();
        P->View     = SavedView;
        P->Lighting = SavedLighting;
        P->UpdateScene();

        if(OutImage.GetWidth() == 0)
        {
            OutError = "this backend reads nothing back (the Null backend draws nothing)";
            return false;
        }
        return true;
    }
#endif // WITH_EDITOR

    void RenderContext::ReleaseGPUResources()
    {
        P->PreviewTargets.Release();
        P->ReleaseRenderTargets();
        P->ReleaseShadowAtlases();
        P->Shadows.ViewCount = 0;
        P->FinalTarget = nullptr;

        for(auto& Each : P->Meshes) { delete Each.second.Gpu; }
        P->Meshes.clear();

        for(uint Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
        for(auto& Each : P->Textures)
        {
            if(Each.second.Gpu) { P->RetiredTextures.push_back(Each.second.Gpu); }
        }
        P->Textures.clear();
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

        P->RetireMaterials();
        for(GDIShader* Each : P->RetiredShaders) { delete Each; }   // ~GDIShader releases its resource
        P->RetiredShaders.clear();
        P->BoundVertex = nullptr;
        P->BoundPixel  = nullptr;
        for(uint Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
    }
}
