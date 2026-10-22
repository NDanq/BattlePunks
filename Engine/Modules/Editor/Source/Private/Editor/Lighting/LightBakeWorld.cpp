#if WITH_EDITOR

#include "Private/Editor/Lighting/LightBakeWorld.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "Public/Core/CoreFileProvider.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/BakedLightingAsset.h"
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace LightBake
{
    namespace
    {
        constexpr const char* DEFAULT_LIT     = "Engine/Content/Materials/DefaultLit.material.nmgraph";
        constexpr float       FALLBACK_ALBEDO = 0.5f;                           // linear, without a BaseColor
        constexpr float       MAX_ALBEDO      = 0.9f;                           // so bounces lose energy
        constexpr int         MAX_SKY_WIDTH   = 256;

        float ToLinear(const float InDisplay) { return RenderCore::SrgbToLinear(InDisplay); }

        /* A copy beside the executable, where the editor's renderer reads
         * runtime paths from, as every other asset the editor writes. */
        void Stage(const std::string& InFile)
        {
            const QString Source = QString::fromStdString(InFile);
            const QString Staged = EditorGraphs::GetStagedPath(Source);
            if(Staged.isEmpty()) { return; }
            QDir().mkpath(QFileInfo(Staged).absolutePath());
            QFile::remove(Staged);
            QFile::copy(Source, Staged);
        }

        Vec3 ToLinear(const EngineCore::Color& InColor)
        {
            return Vec3(ToLinear(InColor.Rf()), ToLinear(InColor.Gf()), ToLinear(InColor.Bf()));
        }

        Vec3 Transform(const EngineCore::Matrix4x4& M, const float* InP)
        {
            return Vec3(float(M.Data[0][0] * InP[0] + M.Data[0][1] * InP[1] + M.Data[0][2] * InP[2] + M.Data[0][3]),
                        float(M.Data[1][0] * InP[0] + M.Data[1][1] * InP[1] + M.Data[1][2] * InP[2] + M.Data[1][3]),
                        float(M.Data[2][0] * InP[0] + M.Data[2][1] * InP[1] + M.Data[2][2] * InP[2] + M.Data[2][3]));
        }

        /* The upper 3x3's cofactors: the inverse transpose up to scale, which
         * keeps normals perpendicular under any scale (and its sign, any
         * mirror's flip). */
        void NormalMatrix(const EngineCore::Matrix4x4& M, float (&Out)[3][3])
        {
            const auto A = [&M](const int R, const int C) { return float(M.Data[R][C]); };
            Out[0][0] = A(1, 1) * A(2, 2) - A(1, 2) * A(2, 1);
            Out[0][1] = A(1, 2) * A(2, 0) - A(1, 0) * A(2, 2);
            Out[0][2] = A(1, 0) * A(2, 1) - A(1, 1) * A(2, 0);
            Out[1][0] = A(0, 2) * A(2, 1) - A(0, 1) * A(2, 2);
            Out[1][1] = A(0, 0) * A(2, 2) - A(0, 2) * A(2, 0);
            Out[1][2] = A(0, 1) * A(2, 0) - A(0, 0) * A(2, 1);
            Out[2][0] = A(0, 1) * A(1, 2) - A(0, 2) * A(1, 1);
            Out[2][1] = A(0, 2) * A(1, 0) - A(0, 0) * A(1, 2);
            Out[2][2] = A(0, 0) * A(1, 1) - A(0, 1) * A(1, 0);
        }

        /* What a section's material is: the slot's, else the section's own,
         * else DefaultLit -- as the renderer picks it. */
        std::string MaterialFor(const RenderCore::RenderComponent& InComponent, const RenderCore::MeshSection& InSection, const int InSlot)
        {
            std::string Material = InComponent.GetMaterial(InSlot);
            if(Material.empty()) { Material = InSection.Material; }
            if(Material.empty()) { Material = DEFAULT_LIT; }
            return Material;
        }

        /* A section's diffuse albedo and glow: its material as measured if it
         * was, else its BaseColor parameter, else grey; InComponent's
         * BaseColor and Metallic overrides for InSlot win. */
        void SurfaceFor(const RenderCore::RenderComponent& InComponent, const int InSlot, const std::string& InMaterial,
                        const MeasuredMaterials* InMeasured, Vec3& OutAlbedo, Vec3& OutEmissive, std::vector<std::string>& OutWarnings)
        {
            Vec3  Base(-1.0f, -1.0f, -1.0f);   // linear; < 0 while unknown
            float Metallic = 0.0f;
            OutEmissive = Vec3();

            if(InMeasured)
            {
                const auto Found = InMeasured->find(InMaterial);
                if(Found != InMeasured->end())
                {
                    Base        = Found->second.BaseColor;
                    OutEmissive = Found->second.Emissive;
                }
            }

            std::string Stem;
            RenderCore::MaterialDescription Description;
            std::string Text, Error;
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            if(RenderCore::GetMaterialStem(InMaterial, Stem) && Provider && Provider->ReadAllText(Stem + ".material", Text)
               && RenderCore::ParseMaterialDescription(Text, Description, Error))
            {
                const RenderCore::MaterialParameter* Param = Description.FindParameter("BaseColor");
                if(Param && Base.X < 0.0f)
                {
                    Base = Vec3(ToLinear(Param->Default[0]), ToLinear(Param->Default[1]), ToLinear(Param->Default[2]));
                }
                if(const RenderCore::MaterialParameter* MetallicParam = Description.FindParameter("Metallic"))
                {
                    Metallic = MetallicParam->Default[0];
                }
            }
            else if(!InMaterial.empty() && Base.X < 0.0f)
            {
                OutWarnings.push_back("material '" + InMaterial + "' could not be read (is it cooked?); it bakes grey");
            }

            for(const RenderCore::RenderMaterialParameter& Each : InComponent.Parameters)
            {
                if(Each.Name[0] == '\0' || int(Each.Slot) != InSlot) { continue; }
                if(std::strcmp(Each.Name, "BaseColor") == 0) { Base = Vec3(ToLinear(Each.Value[0]), ToLinear(Each.Value[1]), ToLinear(Each.Value[2])); }
                if(std::strcmp(Each.Name, "Metallic") == 0)  { Metallic = Each.Value[0]; }
            }

            const float Diffuse = 1.0f - std::clamp(Metallic, 0.0f, 1.0f);
            if(Base.X < 0.0f) { Base = Vec3(FALLBACK_ALBEDO, FALLBACK_ALBEDO, FALLBACK_ALBEDO); }
            OutAlbedo = Vec3(std::min(Base.X, MAX_ALBEDO), std::min(Base.Y, MAX_ALBEDO), std::min(Base.Z, MAX_ALBEDO)) * Diffuse;
        }

        void AddMesh(const GameCore::Entity& InEntity, const RenderCore::RenderComponent& InComponent, Scene& InOutScene,
                     GatherReport& InOutReport, const MeasuredMaterials* InMeasured)
        {
            const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(InComponent.Mesh);
            if(!Mesh)
            {
                InOutReport.Warnings.push_back(std::string("'") + InEntity.GetName() + "': its mesh " + InComponent.Mesh
                                               + " cannot be loaded; it is left out");
                return;
            }

            const EngineCore::Matrix4x4 M = InEntity.GetWorldMatrix();
            float NM[3][3];
            NormalMatrix(M, NM);
            const bool bUV1 = Mesh->UV1.size() == size_t(Mesh->GetVertexCount()) * 2;

            Instance Inst;
            const GameCore::EntityGuid Guid = InEntity.GetGuid();
            Inst.GuidHi       = Guid.Hi;
            Inst.GuidLo       = Guid.Lo;
            Inst.First        = uint32_t(InOutScene.Triangles.size());
            Inst.bLightmapped = bUV1;
            const uint32_t InstanceIndex = uint32_t(InOutScene.Instances.size());

            /* No sections: one, every triangle, as the renderer draws it. */
            std::vector<RenderCore::MeshSection> Sections = Mesh->Sections;
            if(Sections.empty())
            {
                RenderCore::MeshSection All;
                All.IndexCount = uint32_t(Mesh->Indices.size());
                Sections.push_back(All);
            }
            for(size_t SectionIndex = 0; SectionIndex < Sections.size(); ++SectionIndex)
            {
                const RenderCore::MeshSection& Section = Sections[SectionIndex];
                const int Slot = int(SectionIndex);
                Vec3 Albedo, Emissive;
                SurfaceFor(InComponent, Slot, MaterialFor(InComponent, Section, Slot), InMeasured, Albedo, Emissive, InOutReport.Warnings);

                for(uint32_t At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount && At + 2 < Mesh->Indices.size(); At += 3)
                {
                    Triangle T;
                    T.Instance = InstanceIndex;
                    T.Albedo   = Albedo;
                    T.Emissive = Emissive;
                    for(int Corner = 0; Corner < 3; ++Corner)
                    {
                        const uint32_t V = Mesh->Indices[At + uint32_t(Corner)];
                        T.P[Corner] = Transform(M, &Mesh->Positions[size_t(V) * 3]);
                        const float* N = &Mesh->Normals[size_t(V) * 3];
                        T.N[Corner] = Normalize(Vec3(NM[0][0] * N[0] + NM[0][1] * N[1] + NM[0][2] * N[2],
                                                     NM[1][0] * N[0] + NM[1][1] * N[1] + NM[1][2] * N[2],
                                                     NM[2][0] * N[0] + NM[2][1] * N[1] + NM[2][2] * N[2]));
                        if(bUV1)
                        {
                            T.UV1[Corner][0] = Mesh->UV1[size_t(V) * 2];
                            T.UV1[Corner][1] = Mesh->UV1[size_t(V) * 2 + 1];
                        }
                    }
                    InOutScene.Triangles.push_back(T);
                }
            }
            Inst.Count = uint32_t(InOutScene.Triangles.size()) - Inst.First;
            InOutScene.Instances.push_back(Inst);
            ++InOutReport.Meshes;
            if(bUV1) { ++InOutReport.Lightmapped; }
            else
            {
                InOutReport.Warnings.push_back(std::string("'") + InEntity.GetName() + "': " + InComponent.Mesh
                                               + " has no lightmap UVs (reimport it); it is lit by the probes");
            }
        }

        /* An HDRI, decoded and boxed down to MAX_SKY_WIDTH: the sky only lights
         * diffuse surfaces here, so detail would only be noise. */
        bool LoadEnvironment(const char* InPath, const float InIntensity, Sky& OutSky)
        {
            RenderCore::TextureData Texture;
            if(!RenderCore::LoadTextureFile(InPath, Texture) || Texture.Pixels.GetWidth() <= 0) { return false; }
            const int SourceW = Texture.Pixels.GetWidth(), SourceH = Texture.Pixels.GetHeight();
            const int Step = std::max(1, SourceW / MAX_SKY_WIDTH);
            OutSky.Width  = std::max(1, SourceW / Step);
            OutSky.Height = std::max(1, SourceH / Step);
            OutSky.Environment.assign(size_t(OutSky.Width) * size_t(OutSky.Height) * 3, 0.0f);
            const uint8_t* Bytes = Texture.Pixels.GetData();
            for(int Y = 0; Y < OutSky.Height; ++Y)
            {
                for(int X = 0; X < OutSky.Width; ++X)
                {
                    float Sum[3] = {};
                    for(int SY = 0; SY < Step; ++SY)
                    {
                        for(int SX = 0; SX < Step; ++SX)
                        {
                            const int PX = std::min(X * Step + SX, SourceW - 1), PY = std::min(Y * Step + SY, SourceH - 1);
                            const uint8_t* Texel = Bytes + (size_t(PY) * size_t(SourceW) + size_t(PX)) * 4;
                            float Rgb[3];
                            if(Texture.Format == RenderCore::ETexturePixelFormat::RGBE8) { RenderCore::DecodeRgbe(Texel, Rgb); }
                            else
                            {
                                for(int C = 0; C < 3; ++C) { Rgb[C] = ToLinear(float(Texel[C]) / 255.0f); }
                            }
                            for(int C = 0; C < 3; ++C) { Sum[C] += Rgb[C]; }
                        }
                    }
                    for(int C = 0; C < 3; ++C)
                    {
                        OutSky.Environment[(size_t(Y) * size_t(OutSky.Width) + size_t(X)) * 3 + size_t(C)] = Sum[C] / float(Step * Step) * InIntensity;
                    }
                }
            }
            return true;
        }
    }

    namespace
    {
        std::vector<GameCore::EntityID> EntitiesOf(GameCore::World& InWorld)
        {
            std::vector<GameCore::EntityID> Entities;
            InWorld.ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
            {
                static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
            }, &Entities);
            return Entities;
        }

        /* FNV-1a, 64-bit, for HashWorld. */
        struct Fnv
        {
            uint64_t Value = 1469598103934665603ull;
            void Bytes(const void* InData, const size_t InSize)
            {
                const auto* Data = static_cast<const uint8_t*>(InData);
                for(size_t At = 0; At < InSize; ++At) { Value ^= Data[At]; Value *= 1099511628211ull; }
            }
            template<typename T> void Add(const T& InValue) { Bytes(&InValue, sizeof(T)); }
            void Text(const char* InText) { Bytes(InText, std::strlen(InText) + 1); }
            void Color(const EngineCore::Color& InColor) { Add(InColor.R); Add(InColor.G); Add(InColor.B); Add(InColor.A); }
        };
    }

    std::vector<std::string> ListBakeMaterials(GameCore::World& InWorld)
    {
        std::vector<std::string> Out;
        for(const GameCore::EntityID Id : EntitiesOf(InWorld))
        {
            const GameCore::Entity E = InWorld.GetEntity(Id);
            const auto* Render = E.IsAlive() && E.IsStatic() ? E.GetComponent<RenderCore::RenderComponent>() : nullptr;
            if(!Render || !Render->bVisible || Render->Mesh[0] == '\0') { continue; }
            const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(Render->Mesh);
            if(!Mesh) { continue; }
            const size_t Count = std::max<size_t>(Mesh->Sections.size(), 1);
            for(size_t Section = 0; Section < Count; ++Section)
            {
                const RenderCore::MeshSection Each = Section < Mesh->Sections.size() ? Mesh->Sections[Section] : RenderCore::MeshSection();
                const std::string Material = MaterialFor(*Render, Each, int(Section));
                if(std::find(Out.begin(), Out.end(), Material) == Out.end()) { Out.push_back(Material); }
            }
        }
        return Out;
    }

    uint64_t HashWorld(GameCore::World& InWorld)
    {
        const GameCore::WorldLightingSettings& Settings = InWorld.GetSettings().Lighting;
        Fnv World;
        World.Add(Settings.TexelsPerUnit);
        World.Add(Settings.Bounces);
        World.Add(Settings.SamplesPerTexel);
        World.Add(Settings.ProbeSpacing);
        World.Add(Settings.MaxAtlasSize);

        /* Each entity's own hash, summed: the order entities load in does not matter. */
        uint64_t Entities = 0;
        std::unordered_map<std::string, uint64_t> MaterialTexts;
        for(const GameCore::EntityID Id : EntitiesOf(InWorld))
        {
            const GameCore::Entity E = InWorld.GetEntity(Id);
            if(!E.IsAlive() || (!E.IsStatic() && E.GetMobility() != GameCore::EMobility::Stationary)) { continue; }
            Fnv H;
            const GameCore::EntityGuid Guid = E.GetGuid();
            H.Add(Guid.Hi);
            H.Add(Guid.Lo);
            H.Add(uint8_t(E.GetMobility()));
            const EngineCore::Matrix4x4 M = E.GetWorldMatrix();
            for(int Row = 0; Row < 3; ++Row) { for(int Col = 0; Col < 4; ++Col) { H.Add(float(M.Data[Row][Col])); } }
            bool bCounts = false;

            if(const auto* Render = E.IsStatic() ? E.GetComponent<RenderCore::RenderComponent>() : nullptr)
            {
                bCounts = true;
                H.Add(Render->bVisible);
                H.Text(Render->Mesh);
                for(const auto& Material : Render->Materials) { H.Text(Material); }
                for(const RenderCore::RenderMaterialParameter& Each : Render->Parameters)
                {
                    H.Text(Each.Name);
                    H.Add(Each.Slot);
                    for(const float Value : Each.Value) { H.Add(Value); }
                }
                /* The mesh as loaded, and its sections' materials' descriptions. */
                if(const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(Render->Mesh))
                {
                    H.Add(Mesh->GetVertexCount());
                    H.Add(Mesh->GetTriangleCount());
                    for(const float Bound : Mesh->BoundsMin) { H.Add(Bound); }
                    for(const float Bound : Mesh->BoundsMax) { H.Add(Bound); }
                    H.Add(uint64_t(Mesh->UV1.size()));
                    const size_t Count = std::max<size_t>(Mesh->Sections.size(), 1);
                    for(size_t Section = 0; Section < Count; ++Section)
                    {
                        const RenderCore::MeshSection Each = Section < Mesh->Sections.size() ? Mesh->Sections[Section] : RenderCore::MeshSection();
                        const std::string Material = MaterialFor(*Render, Each, int(Section));
                        auto Found = MaterialTexts.find(Material);
                        if(Found == MaterialTexts.end())
                        {
                            Fnv Text;
                            std::string Stem, Contents;
                            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
                            if(RenderCore::GetMaterialStem(Material, Stem) && Provider) { Provider->ReadAllText(Stem + ".material", Contents); }
                            Text.Bytes(Contents.data(), Contents.size());
                            Found = MaterialTexts.emplace(Material, Text.Value).first;
                        }
                        H.Add(Found->second);
                    }
                }
            }
            if(const auto* Sun = E.GetComponent<RenderCore::DirectionalLightComponent>())
            {
                bCounts = true;
                H.Color(Sun->Color); H.Add(Sun->Intensity); H.Add(Sun->bActive); H.Add(Sun->SourceAngle);
            }
            if(const auto* Point = E.GetComponent<RenderCore::PointLightComponent>())
            {
                bCounts = true;
                H.Color(Point->Color); H.Add(Point->Intensity); H.Add(Point->Radius); H.Add(Point->bActive); H.Add(Point->SourceRadius);
            }
            if(const auto* Spot = E.GetComponent<RenderCore::SpotLightComponent>())
            {
                bCounts = true;
                H.Color(Spot->Color); H.Add(Spot->Intensity); H.Add(Spot->Range); H.Add(Spot->InnerConeAngle); H.Add(Spot->OuterConeAngle);
                H.Add(Spot->bActive); H.Add(Spot->SourceRadius);
            }
            if(const auto* SkyLight = E.GetComponent<RenderCore::SkyLightComponent>())
            {
                bCounts = true;
                H.Color(SkyLight->SkyColor); H.Color(SkyLight->GroundColor); H.Add(SkyLight->Intensity); H.Add(SkyLight->bActive);
                H.Text(SkyLight->Environment); H.Add(SkyLight->EnvironmentIntensity); H.Add(SkyLight->EnvironmentRotation);
            }
            if(bCounts) { Entities += H.Value; }
        }
        World.Add(Entities);
        return World.Value;
    }

    void GatherScene(GameCore::World& InWorld, Scene& OutScene, GatherReport& OutReport, const MeasuredMaterials* InMaterials)
    {
        OutScene  = Scene();
        OutReport = GatherReport();

        std::vector<GameCore::EntityID> Entities;
        InWorld.ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
        }, &Entities);

        for(const GameCore::EntityID Id : Entities)
        {
            const GameCore::Entity E = InWorld.GetEntity(Id);
            const bool bStatic     = E.IsAlive() && E.IsStatic();
            const bool bStationary = E.IsAlive() && E.GetMobility() == GameCore::EMobility::Stationary;
            if(!bStatic && !bStationary) { continue; }
            const GameCore::TransformPose Pose = E.GetWorldPose();

            /* Stationary: only a light's bounce; anything else Stationary is not baked. */
            if(const auto* Render = bStatic ? E.GetComponent<RenderCore::RenderComponent>() : nullptr)
            {
                if(Render->bVisible && Render->Mesh[0] != '\0') { AddMesh(E, *Render, OutScene, OutReport, InMaterials); }
            }
            if(const auto* Sun = E.GetComponent<RenderCore::DirectionalLightComponent>())
            {
                if(Sun->bActive && Sun->Intensity > 0.0f)
                {
                    Light L;
                    L.Type       = ELightType::Directional;
                    const EngineCore::Vector3 Forward = RenderCore::LightForward(Pose.Rotation);
                    L.Direction  = Normalize(Vec3(float(Forward.GetX()), float(Forward.GetY()), float(Forward.GetZ())));
                    L.Radiance   = ToLinear(Sun->Color) * Sun->Intensity;
                    L.SourceSize = std::clamp(Sun->SourceAngle, 0.0f, 30.0f) * 3.14159265f / 180.0f;
                    L.bBakeDirect = bStatic;
                    OutScene.Lights.push_back(L);
                    ++OutReport.Lights;
                }
            }
            if(const auto* Point = E.GetComponent<RenderCore::PointLightComponent>())
            {
                if(Point->bActive && Point->Intensity > 0.0f && Point->Radius > 0.0f)
                {
                    Light L;
                    L.Type       = ELightType::Point;
                    L.Position   = Vec3(float(Pose.Position.GetX()), float(Pose.Position.GetY()), float(Pose.Position.GetZ()));
                    L.Radiance   = ToLinear(Point->Color) * Point->Intensity;
                    L.Range      = Point->Radius;
                    L.SourceSize = std::max(Point->SourceRadius, 0.0f);
                    L.bBakeDirect = bStatic;
                    OutScene.Lights.push_back(L);
                    ++OutReport.Lights;
                }
            }
            if(const auto* Spot = E.GetComponent<RenderCore::SpotLightComponent>())
            {
                if(Spot->bActive && Spot->Intensity > 0.0f && Spot->Range > 0.0f)
                {
                    /* As WorldBridge clamps and RenderContext packs them. */
                    constexpr float Degrees = 3.14159265f / 180.0f;
                    const float Outer = std::clamp(Spot->OuterConeAngle, 0.5f, 89.0f);
                    const float Inner = std::clamp(Spot->InnerConeAngle, 0.0f, Outer);
                    const EngineCore::Vector3 Forward = RenderCore::LightForward(Pose.Rotation);
                    Light L;
                    L.Type       = ELightType::Spot;
                    L.Position   = Vec3(float(Pose.Position.GetX()), float(Pose.Position.GetY()), float(Pose.Position.GetZ()));
                    L.Direction  = Normalize(Vec3(float(Forward.GetX()), float(Forward.GetY()), float(Forward.GetZ())));
                    L.Radiance   = ToLinear(Spot->Color) * Spot->Intensity;
                    L.Range      = Spot->Range;
                    L.CosOuter   = std::cos(Outer * Degrees);
                    L.CosInner   = std::cos(std::min(Inner, Outer - 0.01f) * Degrees);
                    L.SourceSize = std::max(Spot->SourceRadius, 0.0f);
                    L.bBakeDirect = bStatic;
                    OutScene.Lights.push_back(L);
                    ++OutReport.Lights;
                }
            }
            if(const auto* SkyLight = E.GetComponent<RenderCore::SkyLightComponent>())
            {
                if(SkyLight->bActive && !OutScene.SkyLight.bPresent)
                {
                    Sky& S = OutScene.SkyLight;
                    S.bPresent    = true;
                    S.bBakeDirect = bStatic;
                    S.SkyColor    = ToLinear(SkyLight->SkyColor) * SkyLight->Intensity;
                    S.GroundColor = ToLinear(SkyLight->GroundColor) * SkyLight->Intensity;
                    S.Rotation    = SkyLight->EnvironmentRotation * 3.14159265f / 180.0f;
                    if(SkyLight->Environment[0] != '\0'
                       && !LoadEnvironment(SkyLight->Environment, std::max(SkyLight->EnvironmentIntensity, 0.0f), S))
                    {
                        OutReport.Warnings.push_back(std::string("the sky's HDRI ") + SkyLight->Environment
                                                     + " cannot be loaded; its colours light the bake");
                    }
                    OutReport.bSky    = bStatic;
                    OutReport.bAnySky = true;
                }
            }
        }

        OutScene.Finish();
        OutScene.Hash = HashWorld(InWorld);
    }

    bool SaveBake(const std::string& InWorldFile, const Scene& InScene, const Result& InResult, const bool bInSkyBaked,
                  std::string& OutBakedData, std::string& OutError)
    {
        const std::string Folder = RenderCore::BakedLightingFolderFor(InWorldFile);
        if(Folder.empty())
        {
            OutError = "the world is not a .nmworld file";
            return false;
        }
        if(!QDir().mkpath(QString::fromStdString(Folder)))
        {
            OutError = "cannot create " + Folder;
            return false;
        }

        RenderCore::BakedLightingData Data;
        Data.InputHash = InScene.Hash;
        Data.bSkyBaked = bInSkyBaked;
        Data.bDirectional = !InResult.Atlases.empty();
        for(const Atlas& A : InResult.Atlases)
        {
            Data.bDirectional &= A.Directions.size() == size_t(A.Size) * size_t(A.Size) * 4;
        }

        for(size_t Index = 0; Index < InResult.Atlases.size(); ++Index)
        {
            const Atlas& A = InResult.Atlases[Index];
            RenderCore::TextureData Texture;
            /* Directional: twice as tall -- the light above, its direction below (BakedLightingAsset.h). */
            Texture.Pixels = EngineCore::Image(A.Size, Data.bDirectional ? A.Size * 2 : A.Size, EngineCore::EImageFormat::RGBA8);
            Texture.Format = RenderCore::ETexturePixelFormat::RGBE8;
            Texture.bSRGB  = false;
            Texture.Filter = RenderCore::ETextureFilter::Nearest;   // RGBE: the shader filters it
            Texture.Wrap   = RenderCore::ETextureWrap::Clamp;
            uint8_t* Bytes = Texture.Pixels.GetData();
            for(size_t Texel = 0; Texel < size_t(A.Size) * size_t(A.Size); ++Texel)
            {
                const float Rgb[3] = { std::max(A.Texels[Texel * 3], 0.0f), std::max(A.Texels[Texel * 3 + 1], 0.0f),
                                       std::max(A.Texels[Texel * 3 + 2], 0.0f) };
                RenderCore::EncodeRgbe(Rgb, Bytes + Texel * 4);
            }
            if(Data.bDirectional)
            {
                uint8_t* Below = Bytes + size_t(A.Size) * size_t(A.Size) * 4;
                const auto Byte = [](const float InValue) { return uint8_t(std::clamp(InValue, 0.0f, 1.0f) * 255.0f + 0.5f); };
                for(size_t Texel = 0; Texel < size_t(A.Size) * size_t(A.Size); ++Texel)
                {
                    const float* Way = &A.Directions[Texel * 4];
                    Below[Texel * 4]     = Byte(Way[0] * 0.5f + 0.5f);
                    Below[Texel * 4 + 1] = Byte(Way[1] * 0.5f + 0.5f);
                    Below[Texel * 4 + 2] = Byte(Way[2] * 0.5f + 0.5f);
                    Below[Texel * 4 + 3] = Byte(Way[3]);
                }
            }
            const std::string File = Folder + "Lightmap_" + std::to_string(Index) + ".nmtexture";
            if(!RenderCore::SaveTextureFile(File, Texture))
            {
                OutError = "cannot write " + File;
                return false;
            }
            const std::string Runtime = EditorContent::ToRuntimePath(QString::fromStdString(File)).toStdString();
            if(Runtime.empty())
            {
                OutError = File + " is outside the project";
                return false;
            }
            Stage(File);
            RenderCore::TextureAssetCache::Invalidate(Runtime);
            Data.Atlases.push_back(Runtime);
        }

        /* A smaller bake leaves fewer atlases: the rest go. */
        for(size_t Index = InResult.Atlases.size();; ++Index)
        {
            const QString Stale = QString::fromStdString(Folder + "Lightmap_" + std::to_string(Index) + ".nmtexture");
            if(!QFileInfo::exists(Stale)) { break; }
            QFile::remove(Stale);
            QFile::remove(EditorGraphs::GetStagedPath(Stale));
        }

        for(const Placement& Place : InResult.Placements)
        {
            RenderCore::BakedLightmapEntry Entry;
            Entry.GuidHi = InScene.Instances[Place.Instance].GuidHi;
            Entry.GuidLo = InScene.Instances[Place.Instance].GuidLo;
            Entry.Atlas  = Place.Atlas;
            std::copy(std::begin(Place.ScaleOffset), std::end(Place.ScaleOffset), Entry.ScaleOffset);
            Data.Entries.push_back(Entry);
        }
        Data.Probes = InResult.Probes;

        const std::string File = Folder + "Lighting.nmlighting";
        if(!RenderCore::SaveBakedLightingFile(File, Data))
        {
            OutError = "cannot write " + File;
            return false;
        }
        Stage(File);
        OutBakedData = EditorContent::ToRuntimePath(QString::fromStdString(File)).toStdString();
        return true;
    }
}

#endif // WITH_EDITOR
