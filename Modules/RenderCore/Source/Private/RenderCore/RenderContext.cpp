#include "Public/RenderCore/RenderContext.h"

#include <algorithm>
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
#include "Private/RenderCore/DrawPolicy.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/SceneTargets.h"

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
        constexpr size_t  RENDER_PREVIEW_SLOTS = 8;                       // MaterialPreviewRequest::Slots

        /* A material's shader: no name-based uniforms, the material binding layout. */
        class MaterialShader final : public GDIShader
        {
        public:
            explicit MaterialShader(const std::string& InName) : GDIShader(InName) {}
            void InitUniformBindings() override {}
        };

        using Material = RenderMaterial;

        /* SceneUniforms.glsl's Scene_Block, byte for byte: vec4s, then a mat4
         * (column-major, as GDI uploads matrices). */
        struct SceneUniforms
        {
            float CameraPosition[4]     = {};
            float Time[4]               = {};
            float LightDirection[4]     = {};
            float LightColor[4]         = {};
            float SkyColor[4]           = {};
            float GroundColor[4]        = {};
            float InvViewProjection[16] = {};
        };
        static_assert(sizeof(SceneUniforms) == 160, "SceneUniforms must match Scene_Block (SceneUniforms.glsl)");
        static_assert(offsetof(SceneUniforms, Time) == 16 && offsetof(SceneUniforms, LightDirection) == 32
                      && offsetof(SceneUniforms, GroundColor) == 80 && offsetof(SceneUniforms, InvViewProjection) == 96,
                      "Scene_Block's members are in this order");

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
        uint32_t   StageDrawCounts[size_t(ERenderStage::Count)] = {};   // the last RenderSceneStages'

        /* DeferredLighting.fsh, loaded on first use; null if it cannot be (warned). */
        GDIShader* LightingShader = nullptr;
        bool       bLightingTried = false;

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
            View3D->Camera->Projection = View.Projection;
            View3D->Camera->ViewMat    = View.View;

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
            Entry.Gpu = GDICreateTextureResource(Source->Pixels, Border, Filter == ETextureFilter::Nearest ? GDI_TF_Nearest : GDI_TF_Linear);
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

            /* Overrides go into a copy: the material's own block keeps its
             * defaults for every other entity drawing with it. */
            const std::vector<uint8_t>* Upload = &Block;
            if(InItem && InItem->OverrideCount > 0)
            {
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
                if(bAny)
                {
                    Upload = &OverrideBlock;
                }
            }
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Upload->data(), uint(Upload->size()));
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

            /* Clip back to world, for the Lighting stage: the inverse of
             * Projection * View, column-major as GDI uploads a mat4. */
            const EngineCore::Matrix4x4 InvViewProjection = EngineCore::Matrix4x4::Inverse(View.Projection * View.View);
            for(int Row = 0; Row < 4; ++Row)
            {
                for(int Col = 0; Col < 4; ++Col)
                {
                    Scene.InvViewProjection[Col * 4 + Row] = float(InvViewProjection.Data[Row][Col]);
                }
            }
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

        /* Issues InStage's policy in its order, under a draw event. */
        void IssueStage(const ERenderStage InStage)
        {
            const DrawPolicy& Policy = PolicyFor(InStage);
            StageDrawCounts[size_t(InStage)] = uint32_t(Policy.Num());
            if(Policy.IsEmpty())
            {
                return;
            }
            GDIBeginDrawEvent(GetRenderStageName(InStage));
            for(const uint32_t Index : Policy.GetIssueOrder())
            {
                Issue(Policy[Index]);
            }
            GDIEndDrawEvent();
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

            EnsureInitialised();
            Bind(Vertex, Pixel);
            static const uint8_t NoMaterial[MATERIAL_ENGINE_BLOCK_BYTES] = {};
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, NoMaterial, uint(sizeof(NoMaterial)));
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));
            GDISetDepthState(IDepthStateDesc());   // no depth test: there is no depth bound
            GDISetBlendState(IBlendStateDesc());
            GDIDrawObject(ScreenObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);

            /* Unbound again: the depth is about to be an attachment. */
            for(uint32_t Index = 0; Index <= RENDER_GBUFFER_TARGETS; ++Index)
            {
                GDISetMaterialTexture(Index, nullptr);
            }
            return true;
        }

        /* Retires every material and the shared vertex shaders. */
        void RetireMaterials()
        {
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
            Warned.clear();
            BoundVertex = nullptr;
            BoundPixel  = nullptr;
        }
    };

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

        /* Usable, and drawable on a mesh. */
        const auto Usable = [](Material& InFound) -> Material*
        {
            return InFound.Pixel && InFound.Description.Domain != EMaterialDomain::PostProcess ? &InFound : nullptr;
        };

        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];

            /* The slot, else the section's material, else DefaultLit, else Mesh_PS (lit tint). */
            const size_t SlotIndex = bInEvery ? 0 : Index;
            const char*  Slot      = InSlots && SlotIndex < InSlotCount && InSlots[SlotIndex] && InSlots[SlotIndex][0] != '\0'
                                   ? InSlots[SlotIndex] : nullptr;
            Material* Chosen = nullptr;
            if(Slot)
            {
                Chosen = Usable(P->FindMaterial(Slot));
            }
            if(!Chosen && !Section.Material.empty())
            {
                Chosen = Usable(P->FindMaterial(Section.Material, /*bQuietIfMissing*/ true));
            }
            if(!Chosen)
            {
                Chosen = Usable(P->FindMaterial(DEFAULT_LIT_MATERIAL, /*bQuietIfMissing*/ true));
            }

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
        P->bDeferring = true;
    }

    void RenderContext::RenderSceneStages(SceneTargets& InTargets, const EngineCore::Color InClearColor)
    {
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

        BindTargets(&SceneColor, 1, SceneColor);
        GDIClear(CM_Color | CM_Depth, InClearColor);

        /* PrePass: RenderScene culled before the draws were queued. */
        P->IssueStage(ERenderStage::PrePass);

        /* Base Pass: the G-buffer, created the first time something needs it. */
        const DrawPolicy& Base = P->PolicyFor(ERenderStage::BasePass);
        const bool bDeferred = !Base.IsEmpty() && InTargets.EnsureGBuffer();
        if(bDeferred)
        {
            BindTargets(InTargets.GBuffer, RENDER_GBUFFER_TARGETS, SceneColor);
            GDIClear(CM_Color, EngineCore::Color(0, 0, 0, 0));   // alpha 0 in GBuffer1: no lit surface
            P->IssueStage(ERenderStage::BasePass);
        }
        else if(!Base.IsEmpty())
        {
            P->StageDrawCounts[size_t(ERenderStage::BasePass)] = uint32_t(Base.Num());
            P->WarnOnce("|gbuffer", "the Base Pass: this backend could not create the G-buffer; "
                                    "Opaque and Masked Lit materials draw nothing");
        }

        /* Lighting: Shadows (none yet), then the resolve into the scene colour. */
        P->IssueStage(ERenderStage::Shadows);
        if(bDeferred)
        {
            BindTargets(&SceneColor, 1, nullptr);
            BeginDrawEvent(GetRenderStageName(ERenderStage::Lighting));
            if(P->IssueLighting(InTargets))
            {
                P->StageDrawCounts[size_t(ERenderStage::Lighting)] = 1;
            }
            EndDrawEvent();
        }

        /* Forward, over the lit scene and against its depth. */
        BindTargets(&SceneColor, 1, SceneColor);
        P->IssueStage(ERenderStage::Unlit);
        P->IssueStage(ERenderStage::TranslucentLit);
        P->IssueStage(ERenderStage::TranslucentUnlit);

        /* PostProcess: ping-pong between the scene colour and a second target. */
        GDIRenderTarget*  Source = SceneColor;
        const DrawPolicy& Post   = P->PolicyFor(ERenderStage::PostProcess);
        P->StageDrawCounts[size_t(ERenderStage::PostProcess)] = uint32_t(Post.Num());
        GDIRenderTarget* Dest = Post.IsEmpty() ? nullptr : InTargets.EnsurePostProcess();
        if(Dest)
        {
            BeginDrawEvent(GetRenderStageName(ERenderStage::PostProcess));
            for(const uint32_t Index : Post.GetIssueOrder())
            {
                GDITexture2D* Scene = Source->ToGDITexture2D();
                BindTargets(&Dest, 1, Dest);
                GDISetMaterialTexture(0, Scene ? Scene->Resource : nullptr);
                if(P->IssueFullscreen(Post[Index]))
                {
                    std::swap(Source, Dest);
                }
                else
                {
                    BindTargets(&Source, 1, Source);   // a skipped pass leaves the result where it was
                }
            }
            GDISetMaterialTexture(0, nullptr);
            EndDrawEvent();
        }

        P->FinalTarget  = Source;
        InTargets.Final = Source;
        Finish();
    }

    bool RenderContext::SubmitPostProcess(const char* InMaterial)
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
        Item.Kind = DrawCommand::EKind::Fullscreen;
        Item.Mat  = &Found;
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
        RenderSceneStages(P->PreviewTargets, EngineCore::Color(38, 40, 46, 255));   // the editor viewport's clear

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
        P->FinalTarget = nullptr;

        for(auto& Each : P->Meshes) { delete Each.second.Gpu; }
        P->Meshes.clear();

        for(uint Slot = 0; Slot < GDI_MATERIAL_TEXTURE_SLOTS; ++Slot) { GDISetMaterialTexture(Slot, nullptr); }
        for(auto& Each : P->Textures)
        {
            if(Each.second.Gpu) { P->RetiredTextures.push_back(Each.second.Gpu); }
        }
        P->Textures.clear();
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
