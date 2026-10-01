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
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/ShaderCache.h"
#include "Private/RenderCore/RenderCoreInternal.h"

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
 * PASSES. Inside a scene (BeginSceneDraws/FlushSceneDraws) draws are queued
 * and issued opaque first -- Opaque, Masked, plain quads and tinted meshes,
 * in submission order -- then Translucent, back to front. Outside one (a
 * preview, a test) they are issued at once.
 */

namespace RenderCore
{
    namespace
    {
        const char* const MATERIAL_FOLDER      = "Materials/";
        const char* const DEFAULT_LIT_MATERIAL = "DefaultLit";   // Engine/Content/Materials: a mesh with none

        /* A material's shader: no name-based uniforms, the material binding layout. */
        class MaterialShader final : public GDIShader
        {
        public:
            explicit MaterialShader(const std::string& InName) : GDIShader(InName) {}
            void InitUniformBindings() override {}
        };

        struct Material
        {
            MaterialDescription  Description;
            GDIShader*           Pixel = nullptr;      // null: failed to load, drawn as the fallback
            GDIShader*           Vertex[2] = {};       // its own Quad and Mesh shaders, with an offset only
            std::vector<uint8_t> Block;                // the defaults; the tint is written per draw
        };

        /* SceneUniforms.glsl's Scene_Block, byte for byte: every member a vec4. */
        struct SceneUniforms
        {
            float CameraPosition[4] = {};
            float Time[4]           = {};
            float LightDirection[4] = {};
            float LightColor[4]     = {};
            float SkyColor[4]       = {};
            float GroundColor[4]    = {};
        };
        static_assert(sizeof(SceneUniforms) == 96, "SceneUniforms must match Scene_Block (SceneUniforms.glsl)");
        static_assert(offsetof(SceneUniforms, Time) == 16 && offsetof(SceneUniforms, LightDirection) == 32
                      && offsetof(SceneUniforms, GroundColor) == 80, "Scene_Block's members are vec4s in this order");

        enum EPass : uint8_t
        {
            PASS_Opaque      = 0,   // Opaque, Masked, plain quads, tinted meshes
            PASS_Translucent = 1,
        };

        struct DrawItem
        {
            enum class EKind : uint8_t { Quad, MaterialQuad, MeshSection };

            EKind             Kind  = EKind::Quad;
            uint8_t           Pass  = PASS_Opaque;
            float             Depth = 0.0f;          // view depth, for the translucent sort
            RenderTransform   Transform;
            float             Width  = 1.0f;
            float             Height = 1.0f;
            EngineCore::Color Tint{255, 255, 255};
            Material*         Mat   = nullptr;       // MaterialQuad; MeshSection (null: Mesh_PS)
            GDIMesh*          Mesh  = nullptr;
            uint32_t          First = 0;
            uint32_t          Count = 0;             // 0: the whole mesh
        };

        void PutColor(float (&OutValue)[4], const EngineCore::Color InColor, const float InAlpha)
        {
            OutValue[0] = InColor.Rf();
            OutValue[1] = InColor.Gf();
            OutValue[2] = InColor.Bf();
            OutValue[3] = InAlpha;
        }

        /* A shader file's cooked path, as GDI resolves it (GetCookedShaderPath). */
        std::string CookedPath(const std::string& InFile, const ERenderBackend InBackend)
        {
            const std::string File = std::string(MATERIAL_FOLDER) + InFile;
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

        /* Skips redundant rebinds. Reset each frame. */
        GDIShader*     BoundVertex = nullptr;
        GDIShader*     BoundPixel  = nullptr;

        std::unordered_map<std::string, Material> Materials;   // node-based: a Material* survives inserts
        GDIShader*              DefaultVertex[3] = {};          // MaterialDefault, by EMaterialVertexInput
        bool                    bDefaultVertexTried[3] = {};
        std::vector<GDIShader*> RetiredShaders;                 // see RenderContext::ReloadMaterial
        std::set<std::string>   Warned;                         // one warning per material and reason

        /* The scene block, refreshed by SetView and SetLighting. */
        RenderLighting                        Lighting;
        SceneUniforms                         Scene;
        std::chrono::steady_clock::time_point Start     = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point LastFrame = Start;
        float                                 FrameDelta = 0.0f;

        bool                  bDeferring = false;
        std::vector<DrawItem> Items;

        /* RenderMaterialPreview's target, square, resized as asked. */
        GDIRenderTarget* PreviewTarget = nullptr;
        int              PreviewSize   = 0;

        std::atomic<uint64_t> DrawCallCount{0};

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
            ViewObject = new GDIViewObject(View3D);

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

        void WarnOnce(const std::string& InKey, const std::string& InMessage)
        {
            if(Warned.insert(InKey).second)
            {
                RENDERCORE_LOG_WARN(InMessage)
            }
        }

        // === Shaders and materials ===========================================

        /* A cooked permutation, or null (warned) if this backend has none. */
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
            Shader->InitResource(InType, std::string(MATERIAL_FOLDER) + InFile);
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
                DefaultVertex[Index] = LoadShader(GetPermutationFileName(MATERIAL_DEFAULT_VERTEX, GetVertexPermutationKey(InInput), true),
                                                  ST_Vertex, "the shared material vertex shaders", false);
            }
            return DefaultVertex[Index];
        }

        /* Loaded on first use; failures are remembered and warned once.
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
            const std::string DataPath = std::string(MATERIAL_FOLDER) + InName + ".material";
            const std::string Who      = "material '" + InName + "'";

            std::string Text, Error;
            if(InName.empty() || !Provider || !Provider->ReadAllText(DataPath, Text))
            {
                if(!bQuietIfMissing && !InName.empty())
                {
                    RENDERCORE_LOG_WARN(Who + ": cannot read " + DataPath + "; drawing the plain tint")
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
            GDIShader* Pixel = LoadShader(GetPermutationFileName(InName, PixelKey, false), ST_Pixel, Who, false);
            if(!Pixel)
            {
                return Loaded;   // warned; drawn as the fallback
            }

            if(Desc.bHasOffset && Desc.Domain != EMaterialDomain::PostProcess)
            {
                for(const EMaterialVertexInput Input : { EMaterialVertexInput::Quad, EMaterialVertexInput::Mesh })
                {
                    GDIShader* Vertex = LoadShader(GetPermutationFileName(InName, GetVertexPermutationKey(Input), true), ST_Vertex, Who, false);
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

        /* Binds InMaterial's shaders for InInput and its blocks with InTint.
         * False (nothing bound) if a shader is missing. */
        bool BindMaterial(Material& InMaterial, const EMaterialVertexInput InInput, const EngineCore::Color InTint)
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
            GDISetUniformBlock(GDI_MATERIAL_BLOCK_BINDING, Block.data(), uint(Block.size()));
            GDISetUniformBlock(GDI_SCENE_BLOCK_BINDING, &Scene, uint(sizeof(Scene)));
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
        }

        // === Issuing ============================================================

        void IssueQuad(const DrawItem& InItem)
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

        void IssueMaterialQuad(const DrawItem& InItem)
        {
            EnsureInitialised();
            if(!BindMaterial(*InItem.Mat, EMaterialVertexInput::Quad, InItem.Tint))
            {
                DrawItem Plain = InItem;
                IssueQuad(Plain);
                return;
            }
            PlaceQuad(InItem.Transform, InItem.Width, InItem.Height);
            GDIDrawObject(ViewObject);
            DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        void IssueMeshSection(const DrawItem& InItem)
        {
            EnsureInitialised();
            Place(MeshObject, InItem.Transform, 1.0f, 1.0f);
            MeshObject->Mesh = InItem.Mesh;

            if(!InItem.Mat || !BindMaterial(*InItem.Mat, EMaterialVertexInput::Mesh, InItem.Tint))
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

        void Issue(const DrawItem& InItem)
        {
            switch(InItem.Kind)
            {
                case DrawItem::EKind::Quad:         IssueQuad(InItem);        break;
                case DrawItem::EKind::MaterialQuad: IssueMaterialQuad(InItem); break;
                case DrawItem::EKind::MeshSection:  IssueMeshSection(InItem); break;
            }
        }

        void Submit(DrawItem InItem)
        {
            if(!bDeferring)
            {
                Issue(InItem);
                return;
            }
            const EngineCore::Vector3 Offset = InItem.Transform.Position - View.Origin;
            InItem.Depth = float(Offset.Dot(ViewForward(View)));
            Items.push_back(InItem);
        }

        static uint8_t PassOf(const Material& InMaterial)
        {
            return InMaterial.Description.Domain == EMaterialDomain::Translucent ? PASS_Translucent : PASS_Opaque;
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
        DrawItem Item;
        Item.Kind      = DrawItem::EKind::Quad;
        Item.Transform = InTransform;
        Item.Width     = InWidth;
        Item.Height    = InHeight;
        Item.Tint      = InTint;
        P->Submit(Item);
    }

    void RenderContext::DrawQuadWithMaterial(const RenderTransform& InTransform, const float InWidth, const float InHeight,
                                             const EngineCore::Color InTint, const char* InMaterial)
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

        DrawItem Item;
        Item.Kind      = DrawItem::EKind::MaterialQuad;
        Item.Pass      = Impl::PassOf(Found);
        Item.Transform = InTransform;
        Item.Width     = InWidth;
        Item.Height    = InHeight;
        Item.Tint      = InTint;
        Item.Mat       = &Found;
        P->Submit(Item);
    }

    bool RenderContext::DrawMesh(const RenderTransform& InTransform, const char* InMeshPath,
                                 const EngineCore::Color InTint, const char* InMaterial)
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

        const bool        bOverride = InMaterial && InMaterial[0] != '\0';
        const std::string Override  = bOverride ? std::string(InMaterial) : std::string();

        /* Usable, and drawable on a mesh. */
        const auto Usable = [](Material& InFound) -> Material*
        {
            return InFound.Pixel && InFound.Description.Domain != EMaterialDomain::PostProcess ? &InFound : nullptr;
        };

        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];

            /* Override, else section material, else DefaultLit, else Mesh_PS (lit tint). */
            Material* Chosen = nullptr;
            if(bOverride)
            {
                Chosen = Usable(P->FindMaterial(Override));
            }
            else if(!Section.Material.empty())
            {
                Chosen = Usable(P->FindMaterial(Section.Material, /*bQuietIfMissing*/ true));
            }
            if(!Chosen)
            {
                Chosen = Usable(P->FindMaterial(DEFAULT_LIT_MATERIAL, /*bQuietIfMissing*/ true));
            }

            DrawItem Item;
            Item.Kind      = DrawItem::EKind::MeshSection;
            Item.Pass      = Chosen ? Impl::PassOf(*Chosen) : uint8_t(PASS_Opaque);
            Item.Transform = InTransform;
            Item.Tint      = InTint;
            Item.Mat       = Chosen;
            Item.Mesh      = Mesh;
            Item.First     = Section.FirstIndex;
            Item.Count     = Sections.empty() ? 0u : Section.IndexCount;
            P->Submit(Item);
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
        P->Items.clear();
        P->bDeferring = true;
    }

    void RenderContext::FlushSceneDraws()
    {
        P->bDeferring = false;

        /* Opaque first in submission order; translucent back to front, ties
         * in submission order (coplanar 2D quads keep their layering).
         * Indices are sorted, not the items: a DrawItem's SIMD math is
         * over-aligned, which std::stable_sort's buffer refuses on MSVC. */
        std::vector<uint32_t> Order(P->Items.size());
        for(uint32_t Index = 0; Index < uint32_t(Order.size()); ++Index) { Order[Index] = Index; }
        const std::vector<DrawItem>& Items = P->Items;
        std::stable_sort(Order.begin(), Order.end(), [&Items](const uint32_t InA, const uint32_t InB)
        {
            const DrawItem& A = Items[InA];
            const DrawItem& B = Items[InB];
            if(A.Pass != B.Pass) { return A.Pass < B.Pass; }
            return A.Pass == PASS_Translucent && A.Depth > B.Depth;
        });

        BeginDrawEvent("Opaque");
        bool bTranslucent = false;
        for(const uint32_t Index : Order)
        {
            const DrawItem& Each = Items[Index];
            if(Each.Pass == PASS_Translucent && !bTranslucent)
            {
                EndDrawEvent();
                BeginDrawEvent("Translucent");
                bTranslucent = true;
            }
            P->Issue(Each);
        }
        EndDrawEvent();
        P->Items.clear();
    }

    bool RenderContext::DrawPostProcessPass(const char* InMaterial)
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

        P->EnsureInitialised();
        if(!P->BindMaterial(Found, EMaterialVertexInput::Fullscreen, EngineCore::Color(255, 255, 255, 255)))
        {
            return false;
        }
        GDIDrawObject(P->ScreenObject);
        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void RenderContext::ReloadMaterial(const char* InName)
    {
        if(!InName || InName[0] == '\0')
        {
            P->RetireMaterials();
            return;
        }

        const auto Found = P->Materials.find(InName);
        if(Found != P->Materials.end())
        {
            Material& M = Found->second;
            for(GDIShader* Shader : { M.Pixel, M.Vertex[0], M.Vertex[1] })
            {
                if(!Shader) { continue; }
                if(P->BoundPixel == Shader)  { P->BoundPixel = nullptr; }
                if(P->BoundVertex == Shader) { P->BoundVertex = nullptr; }
                P->RetiredShaders.push_back(Shader);
            }
            P->Materials.erase(Found);
        }

        /* Forget the warnings about it, so a fixed material says so if it fails again. */
        const std::string Prefix = std::string(InName) + "|";
        for(auto It = P->Warned.begin(); It != P->Warned.end();)
        {
            It = It->compare(0, Prefix.size(), Prefix) == 0 ? P->Warned.erase(It) : std::next(It);
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
    bool RenderContext::RenderMaterialPreview(const char* InMaterial, const int InSize, const EngineCore::Color InTint,
                                              EngineCore::Image& OutImage, std::string& OutError)
    {
        const std::string Name = InMaterial ? InMaterial : "";
        const int Size = InSize < 16 ? 16 : (InSize > 1024 ? 1024 : InSize);

        /* Loaded (or failed) exactly as the viewport's draw would. */
        Material& Found = P->FindMaterial(Name);
        if(!Found.Pixel)
        {
            OutError = "material '" + Name + "' could not be loaded -- see the log";
            return false;
        }

        P->EnsureInitialised();
        if(!P->PreviewTarget)
        {
            P->PreviewTarget = GDICreateRenderTarget("RenderCore.MaterialPreview", uint(Size), uint(Size));
        }
        else if(P->PreviewSize != Size)
        {
            P->PreviewTarget->Resize(uint(Size), uint(Size));
        }
        P->PreviewSize = Size;
        if(!P->PreviewTarget)
        {
            OutError = "could not create the preview render target";
            return false;
        }

        /* Pixel-space view over the target, restored afterwards. The eye sits
         * at the quad's depth: with ZNear -1, pulling it back would clip. For
         * lighting, though, the eye is a quad's width in front of it. */
        const RenderView Saved = P->View;
        P->View = RenderView::MakeOrthographic(EngineCore::Size(Size, Size), EngineCore::Vector3(0.0, 0.0, 0.0), -1.0f, 1000.0f);
        const EngineCore::Vector3 Eye(double(Size) * 0.5, double(Size) * 0.5, -double(Size));
        P->UpdateScene(&Eye);

        GDISetRenderTarget(P->PreviewTarget);
        GDISetViewport(0, 0, uint(Size), uint(Size));
        GDISetColorMask(EColM_RGBA);   // see RenderScene::Render: nothing reaches a target without it
        GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
        GDISetDepthState(IDepthStateDesc());
        GDIClear(CM_Color | CM_Depth, EngineCore::Color(38, 40, 46, 255));   // the editor viewport's clear

        if(Found.Description.Domain == EMaterialDomain::PostProcess)
        {
            /* Over the clear colour's own black: slot 0 has no scene here. */
            GDISetMaterialTexture(0, nullptr);
            DrawPostProcessPass(Name.c_str());
        }
        else
        {
            RenderTransform Quad;
            Quad.Position = EngineCore::Vector3(double(Size) * 0.5, double(Size) * 0.5, 0.0);
            DrawQuadWithMaterial(Quad, float(Size), float(Size), InTint, Name.c_str());
        }

        /* Flushes, reads back, resumes the frame. */
        OutImage = P->PreviewTarget->ToImage();
        P->View  = Saved;
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
        if(P->PreviewTarget)
        {
            GDIReleaseRenderTarget(P->PreviewTarget);
            delete P->PreviewTarget;
            P->PreviewTarget = nullptr;
            P->PreviewSize   = 0;
        }

        for(auto& Each : P->Meshes) { delete Each.second.Gpu; }
        P->Meshes.clear();

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
