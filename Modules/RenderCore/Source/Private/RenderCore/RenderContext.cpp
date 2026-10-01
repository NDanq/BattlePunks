#include "Public/RenderCore/RenderContext.h"

#include <atomic>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreImage.h"
#include "Public/GDI/Shaders/Classes/TestbedShaders.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/RenderCoreInternal.h"

/*
 * DrawQuad's whole job: one 1x1 quad mesh, the Basic_VS/Basic_PS pair every
 * backend already registers in InitializeGDI (GDI.cpp), and a GDIView/
 * GDIViewObject pair whose Camera projection/view matrices are overwritten
 * from RenderView every SetView() -- so the shaders see exactly what a
 * GDICamera would have produced, without RenderContext owning one itself
 * (GDICamera's own UpdateDimensions recomputes from a width/height it does
 * not have here; RenderView already carries the finished matrices).
 *
 * DrawQuadWithMaterial swaps in a material's pixel shader for Basic_PS -- the
 * vertex side is Basic_VS either way, whose interpolants (Common.glsl) are
 * what a material graph's inputs read.
 *
 * DrawMesh is the same with Mesh_VS, which writes those interpolants too (and
 * a normal), so any material draws on a mesh as it does on a quad. Its GPU
 * meshes are built from MeshAssetCache's, one per path, and rebuilt when the
 * cache hands back a different mesh -- which is how a reimport shows.
 */

namespace RenderCore
{
    namespace
    {
        const char* const MATERIAL_FOLDER  = "Materials/";
        const char* const MATERIAL_UNIFORM = "MaterialParams";
        constexpr int     MATERIAL_SLOTS   = 3;   // columns 0-2; column 3 is the tint

        /* A material's pixel shader: GDI's registry keys shaders by C++
         * type, one instance per type, so a material -- one of any number,
         * named at run time -- is created and owned here instead. */
        class MaterialPixelShader final : public GDIShader
        {
        public:
            explicit MaterialPixelShader(const std::string& InName) : GDIShader(InName) {}

            void InitUniformBindings() override
            {
                RegisterUniform(MATERIAL_UNIFORM, EngineCore::Matrix4x4());
            }
        };

        struct Material
        {
            GDIShader* Pixel = nullptr;           // null: failed to load, drawn as DrawQuad
            float      Slots[MATERIAL_SLOTS][4] = {};
        };

        /* Where GDI's own cooked-path rule (GDIShader.cpp's GetCookedShaderPath)
         * puts a material's shader -- checked before InitResource, which on
         * OpenGL treats a missing file as a hard error. */
        std::string CookedMaterialPath(const std::string& InName, const ERenderBackend InBackend)
        {
            const std::string File = std::string(MATERIAL_FOLDER) + InName + ".fsh";
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

        /* Materials/<Name>.material: "Param <Name> <Slot> <Type> x y z w"
         * lines (NodeGraph's MaterialSchema.cpp writes them). False if the
         * file is not a material. */
        bool ParseMaterial(const std::string& InText, Material& OutMaterial, std::string& OutError)
        {
            std::istringstream Lines(InText);
            std::string Line;
            bool bHeader = false;
            while(std::getline(Lines, Line))
            {
                const size_t Hash = Line.find('#');
                if(Hash != std::string::npos) { Line.resize(Hash); }

                std::istringstream Words(Line);
                std::string Keyword;
                if(!(Words >> Keyword)) { continue; }

                if(Keyword == "NimbleMaterial")
                {
                    int Version = 0;
                    Words >> Version;
                    if(Version != 1)
                    {
                        OutError = "unsupported material version " + std::to_string(Version);
                        return false;
                    }
                    bHeader = true;
                }
                else if(Keyword == "Param")
                {
                    std::string Name, Type;
                    int Slot = -1;
                    float Values[4] = {};
                    Words >> Name >> Slot >> Type >> Values[0] >> Values[1] >> Values[2] >> Values[3];
                    if(Words.fail() || Slot < 0 || Slot >= MATERIAL_SLOTS)
                    {
                        OutError = "bad parameter line '" + Line + "'";
                        return false;
                    }
                    for(int Lane = 0; Lane < 4; ++Lane) { OutMaterial.Slots[Slot][Lane] = Values[Lane]; }
                }
            }
            if(!bHeader) { OutError = "no NimbleMaterial header"; }
            return bHeader;
        }
    }

    struct RenderContext::Impl
    {
        ERenderBackend Backend      = ERenderBackend::Null;
        uint64_t       FrameNumber  = 0;
        RenderView     View;

        GDIView*       View3D    = nullptr;   // owns the GDICamera DrawQuad overwrites per SetView()
        GDIViewObject* ViewObject = nullptr;   // owns the shared unit quad mesh

        /* DrawMesh's: its own Transform, and a Mesh pointed at whichever
         * GDI mesh is drawing -- never owned by it. */
        GDIViewObject* MeshObject = nullptr;

        /* A mesh's GPU copy, keyed by path. Source is what it was built from;
         * a different pointer from MeshAssetCache means rebuild. Gpu null
         * with Source set: this backend could not build it (drawn as a quad). */
        struct GpuMesh
        {
            std::shared_ptr<const MeshData> Source;
            GDIMesh*                        Gpu = nullptr;
        };
        std::unordered_map<std::string, GpuMesh> Meshes;

        /* What GDISetBoundShader last got from here -- rebinding is skipped
         * when a draw wants the same pair. Forgotten at every frame start,
         * in case something outside this class bound others in between. */
        GDIShader*     BoundVertex = nullptr;
        GDIShader*     BoundPixel  = nullptr;

        std::unordered_map<std::string, Material> Materials;
        std::vector<GDIShader*>                   RetiredShaders;   // see RenderContext::ReloadMaterial

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

        /* The camera and InObject's placement: InTransform, its scale times
         * InScaleX/Y -- a quad's size, or 1 for a mesh. */
        void Place(GDIViewObject* InObject, const RenderTransform& InTransform, const float InScaleX, const float InScaleY)
        {
            /* Camera's matrices are the finished RenderView's, not recomputed
             * from a width/height GDICamera would otherwise need -- overwritten
             * here rather than in SetView() so a RenderContext used with no
             * RenderScene at all (a hand-built scene in a test) still draws
             * correctly as long as SetView() was called at least once. */
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

        /* InMaterial's slots, with InTint in column 3, into its MaterialParams. */
        static void SetMaterialParams(const Material& InMaterial, const EngineCore::Color InTint)
        {
            /* Column c of the shader's mat4 is Matrix4x4 column c: every backend
             * transposes Matrix4x4's rows into the column-major upload. */
            EngineCore::Matrix4x4 Params;
            for(int Column = 0; Column < MATERIAL_SLOTS; ++Column)
            {
                for(int Row = 0; Row < 4; ++Row) { Params.Data[Row][Column] = InMaterial.Slots[Column][Row]; }
            }
            Params.Data[0][3] = InTint.Rf();
            Params.Data[1][3] = InTint.Gf();
            Params.Data[2][3] = InTint.Bf();
            Params.Data[3][3] = InTint.Af();
            InMaterial.Pixel->SetUniform(MATERIAL_UNIFORM, Params);
        }

        /* InPath's GPU mesh, built (or rebuilt) from MeshAssetCache's. Null if
         * the mesh cannot be loaded or this backend cannot build it; OutSource
         * is the mesh either way it was found. */
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

        /* The material, loaded on first use. A failed load is remembered (as
         * a Material with no shader) so it is warned about once, not per frame.
         * bQuietIfMissing: say nothing if there is no such material at all --
         * for a mesh section's imported material name, which usually has none. */
        Material& FindMaterial(const std::string& InName, const bool bQuietIfMissing = false)
        {
            const auto Found = Materials.find(InName);
            if(Found != Materials.end())
            {
                return Found->second;
            }

            Material& Loaded = Materials[InName];
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            const std::string DataPath   = std::string(MATERIAL_FOLDER) + InName + ".material";
            const std::string ShaderPath = CookedMaterialPath(InName, Backend);

            std::string Text, Error;
            if(!Provider || !Provider->ReadAllText(DataPath, Text))
            {
                if(!bQuietIfMissing)
                {
                    RENDERCORE_LOG_WARN("material '" + InName + "': cannot read " + DataPath + "; drawing the plain tint")
                }
                return Loaded;
            }
            if(!ParseMaterial(Text, Loaded, Error))
            {
                RENDERCORE_LOG_WARN("material '" + InName + "': " + DataPath + ": " + Error + "; drawing the plain tint")
                return Loaded;
            }
            if(!ShaderPath.empty() && !Provider->Exists(ShaderPath))
            {
                RENDERCORE_LOG_WARN("material '" + InName + "': no cooked shader at " + ShaderPath
                                    + " -- cook it for this backend; drawing the plain tint")
                return Loaded;
            }

            auto* Shader = new MaterialPixelShader("Material:" + InName);
            Shader->InitResource(ST_Pixel, std::string(MATERIAL_FOLDER) + InName + ".fsh");
            Shader->InitUniformBindings();
            Loaded.Pixel = Shader;
            RENDERCORE_LOG("material '" + InName + "' loaded")
            return Loaded;
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
        P->EnsureInitialised();
        P->Bind(GDIGetShader<Basic_VS>(), GDIGetShader<Basic_PS>());
        P->PlaceQuad(InTransform, InWidth, InHeight);

        /* Basic_PS::InitUniformBindings (TestbedShaders.h) registers
         * "TestColor" as a Vector3 -- overwritten here, per draw, with
         * InTint's normalised channels, so two proxies with different tints
         * drawn in the same frame each see their own colour rather than
         * whichever one bound the shader last. */
        if(GDIShader* Pixel = GDIGetShader<Basic_PS>())
        {
            Pixel->SetUniform("TestColor", EngineCore::Vector3(InTint.Rf(), InTint.Gf(), InTint.Bf()));
        }

        GDIDrawObject(P->ViewObject);

        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderContext::DrawQuadWithMaterial(const RenderTransform& InTransform, const float InWidth, const float InHeight,
                                             const EngineCore::Color InTint, const char* InMaterial)
    {
        const Material& Found = P->FindMaterial(InMaterial ? std::string(InMaterial) : std::string());
        if(!Found.Pixel)
        {
            DrawQuad(InTransform, InWidth, InHeight, InTint);
            return;
        }

        P->EnsureInitialised();
        P->Bind(GDIGetShader<Basic_VS>(), Found.Pixel);
        P->PlaceQuad(InTransform, InWidth, InHeight);
        Impl::SetMaterialParams(Found, InTint);

        GDIDrawObject(P->ViewObject);

        P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
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

        P->EnsureInitialised();
        P->Place(P->MeshObject, InTransform, 1.0f, 1.0f);
        P->MeshObject->Mesh = Mesh;

        /* A section per material; a mesh with none is one section of everything. */
        static const MeshSection Everything;
        const std::vector<MeshSection>& Sections = Source->Sections;
        const size_t SectionCount = Sections.empty() ? 1 : Sections.size();

        const bool        bOverride = InMaterial && InMaterial[0] != '\0';
        const std::string Override  = bOverride ? std::string(InMaterial) : std::string();

        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            const MeshSection& Section = Sections.empty() ? Everything : Sections[Index];

            /* The override if it loads; else the section's own material if
             * there is one by that name; else Mesh_PS, the tint lit. */
            const Material* Chosen = nullptr;
            if(bOverride)
            {
                const Material& Found = P->FindMaterial(Override);
                if(Found.Pixel) { Chosen = &Found; }
            }
            else if(!Section.Material.empty())
            {
                const Material& Found = P->FindMaterial(Section.Material, /*bQuietIfMissing*/ true);
                if(Found.Pixel) { Chosen = &Found; }
            }

            if(Chosen)
            {
                P->Bind(GDIGetShader<Mesh_VS>(), Chosen->Pixel);
                Impl::SetMaterialParams(*Chosen, InTint);
            }
            else
            {
                GDIShader* Pixel = GDIGetShader<Mesh_PS>();
                P->Bind(GDIGetShader<Mesh_VS>(), Pixel);
                if(Pixel)
                {
                    Pixel->SetUniform("Tint", EngineCore::Vector3(InTint.Rf(), InTint.Gf(), InTint.Bf()));
                }
            }

            Mesh->SetDrawRange(Section.FirstIndex, Sections.empty() ? 0u : Section.IndexCount);
            GDIDrawObject(P->MeshObject);
            P->DrawCallCount.fetch_add(1, std::memory_order_relaxed);
        }

        Mesh->SetDrawRange(0, 0);
        P->MeshObject->Mesh = nullptr;
        return true;
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

        /*
         * World units per pixel at the segment's depth, read back from the
         * projection so any view works: Data[1][1] is the Y focal scale in
         * both MakeOrthographic and MakePerspective. An orthographic
         * projection has no w-divide (bottom row 0 0 0 1), so its scale is
         * the same at every depth; a perspective one grows with view depth.
         * The view's own row 2 is its forward axis (Matrix4x4::LookAt).
         */
        const EngineCore::Matrix4x4& Proj = View.Projection;
        const Vector3 Forward(View.View.Data[2][0], View.View.Data[2][1], View.View.Data[2][2]);
        const bool    bOrthographic = Proj.Data[3][2] == 0.0f && Proj.Data[3][3] != 0.0f;
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

        /* The ribbon's width axis: across the segment and across the line of
         * sight, so the flat quad faces the camera. End-on, there is none. */
        const Vector3 Sight = bOrthographic ? Forward : (Mid - View.Origin);
        Vector3 Side = Along ^ Sight;
        const double SideLength = Side.GetLength();
        if(SideLength < 1e-9 * Sight.GetLength())
        {
            return;
        }
        Side = Side * (1.0 / SideLength);
        const Vector3 Normal = Along ^ Side;

        /* Unit quad (+-0.5 in X and Y, z = 0) -> the ribbon: X along the
         * segment (capped by half a width at each end), Y across it. Columns
         * are the images of the quad's axes; translation is the midpoint. */
        const Vector3 AxisX = Along * (Length + Width);
        const Vector3 AxisY = Side * Width;

        EngineCore::Matrix4x4 Segment;
        Segment.Data[0][0] = AxisX.GetX(); Segment.Data[0][1] = AxisY.GetX(); Segment.Data[0][2] = Normal.GetX(); Segment.Data[0][3] = Mid.GetX();
        Segment.Data[1][0] = AxisX.GetY(); Segment.Data[1][1] = AxisY.GetY(); Segment.Data[1][2] = Normal.GetY(); Segment.Data[1][3] = Mid.GetY();
        Segment.Data[2][0] = AxisX.GetZ(); Segment.Data[2][1] = AxisY.GetZ(); Segment.Data[2][2] = Normal.GetZ(); Segment.Data[2][3] = Mid.GetZ();
        Segment.Data[3][0] = 0.0f;         Segment.Data[3][1] = 0.0f;         Segment.Data[3][2] = 0.0f;          Segment.Data[3][3] = 1.0f;

        P->EnsureInitialised();
        P->Bind(GDIGetShader<Basic_VS>(), GDIGetShader<Basic_PS>());

        /* The shader computes Projection * View * Model * v; with Model the
         * identity, View * Segment places the quad. Both uniforms are
         * uploaded per draw (GDIShader::UpdateCoreUniforms), so the next
         * DrawQuad's own View is unaffected. */
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
    }

    void RenderContext::SetView(const RenderView& InView)
    {
        P->View = InView;
    }

    void RenderContext::ReloadMaterial(const char* InName)
    {
        const auto Retire = [this](Material& InMaterial)
        {
            if(InMaterial.Pixel)
            {
                if(P->BoundPixel == InMaterial.Pixel) { P->BoundPixel = nullptr; }
                P->RetiredShaders.push_back(InMaterial.Pixel);
                InMaterial.Pixel = nullptr;
            }
        };

        if(!InName || InName[0] == '\0')
        {
            for(auto& Each : P->Materials) { Retire(Each.second); }
            P->Materials.clear();
            return;
        }

        const auto Found = P->Materials.find(InName);
        if(Found != P->Materials.end())
        {
            Retire(Found->second);
            P->Materials.erase(Found);
        }
    }

#if WITH_EDITOR
    bool RenderContext::RenderMaterialPreview(const char* InMaterial, const int InSize, const EngineCore::Color InTint,
                                              EngineCore::Image& OutImage, std::string& OutError)
    {
        const std::string Name = InMaterial ? InMaterial : "";
        const int Size = InSize < 16 ? 16 : (InSize > 1024 ? 1024 : InSize);

        /* Loaded (or failed) exactly as the viewport's draw would. */
        if(!P->FindMaterial(Name).Pixel)
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

        /* A pixel-space view over exactly the target, and the quad filling
         * it -- the scene's own view is put back afterwards. The eye sits at
         * the quad's own depth, as a game's camera does: with ZNear -1 the
         * orthographic volume admits view-space z up to +1 only, so an eye
         * pulled back along -z would clip the quad away. */
        const RenderView Saved = P->View;
        P->View = RenderView::MakeOrthographic(EngineCore::Size(Size, Size), EngineCore::Vector3(0.0, 0.0, 0.0), -1.0f, 1000.0f);

        GDISetRenderTarget(P->PreviewTarget);
        GDISetViewport(0, 0, uint(Size), uint(Size));
        GDISetColorMask(EColM_RGBA);   // see RenderScene::Render: nothing reaches a target without it
        GDISetBlendState(IBlendStateDesc(true, BM_Src_Alpha, BM_One_Minus_Src_Alpha));
        GDISetDepthState(IDepthStateDesc());
        GDIClear(CM_Color | CM_Depth, EngineCore::Color(38, 40, 46, 255));   // the editor viewport's clear

        RenderTransform Quad;
        Quad.Position = EngineCore::Vector3(double(Size) * 0.5, double(Size) * 0.5, 0.0);
        DrawQuadWithMaterial(Quad, float(Size), float(Size), InTint, Name.c_str());

        /* Flushes the frame so far, reads back, and resumes it (every backend's
         * ToImage does); the caller binds its own target next. */
        OutImage = P->PreviewTarget->ToImage();
        P->View  = Saved;

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

        ReloadMaterial(nullptr);
        for(GDIShader* Each : P->RetiredShaders) { delete Each; }   // ~GDIShader releases its resource
        P->RetiredShaders.clear();
        P->BoundVertex = nullptr;
        P->BoundPixel  = nullptr;
    }
}
