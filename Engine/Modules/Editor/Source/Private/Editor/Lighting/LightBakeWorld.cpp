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

        float ToLinear(const float InDisplay) { return std::pow(std::max(InDisplay, 0.0f), 2.2f); }

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

        /* A material's BaseColor and Metallic, from its cooked description and
         * InComponent's overrides for InSlot: what it would draw with, as far
         * as a constant can say. */
        Vec3 AlbedoFor(const RenderCore::RenderComponent& InComponent, const int InSlot, const std::string& InMaterial,
                       std::vector<std::string>& OutWarnings)
        {
            float Base[3] = { -1.0f, -1.0f, -1.0f };
            float Metallic = 0.0f;

            std::string Stem;
            RenderCore::MaterialDescription Description;
            std::string Text, Error;
            EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
            if(RenderCore::GetMaterialStem(InMaterial, Stem) && Provider && Provider->ReadAllText(Stem + ".material", Text)
               && RenderCore::ParseMaterialDescription(Text, Description, Error))
            {
                if(const RenderCore::MaterialParameter* Param = Description.FindParameter("BaseColor"))
                {
                    std::copy_n(Param->Default, 3, Base);
                }
                if(const RenderCore::MaterialParameter* Param = Description.FindParameter("Metallic"))
                {
                    Metallic = Param->Default[0];
                }
            }
            else if(!InMaterial.empty())
            {
                OutWarnings.push_back("material '" + InMaterial + "' could not be read (is it cooked?); it bakes grey");
            }

            for(const RenderCore::RenderMaterialParameter& Each : InComponent.Parameters)
            {
                if(Each.Name[0] == '\0' || int(Each.Slot) != InSlot) { continue; }
                if(std::strcmp(Each.Name, "BaseColor") == 0) { std::copy_n(Each.Value, 3, Base); }
                if(std::strcmp(Each.Name, "Metallic") == 0)  { Metallic = Each.Value[0]; }
            }

            const float Diffuse = 1.0f - std::clamp(Metallic, 0.0f, 1.0f);
            if(Base[0] < 0.0f)
            {
                return Vec3(FALLBACK_ALBEDO, FALLBACK_ALBEDO, FALLBACK_ALBEDO) * Diffuse;
            }
            return Vec3(std::min(ToLinear(Base[0]), MAX_ALBEDO), std::min(ToLinear(Base[1]), MAX_ALBEDO),
                        std::min(ToLinear(Base[2]), MAX_ALBEDO)) * Diffuse;
        }

        void AddMesh(const GameCore::Entity& InEntity, const RenderCore::RenderComponent& InComponent, Scene& InOutScene,
                     GatherReport& InOutReport)
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
                std::string Material = InComponent.GetMaterial(Slot);
                if(Material.empty()) { Material = Section.Material; }
                if(Material.empty()) { Material = DEFAULT_LIT; }
                const Vec3 Albedo = AlbedoFor(InComponent, Slot, Material, InOutReport.Warnings);

                for(uint32_t At = Section.FirstIndex; At + 2 < Section.FirstIndex + Section.IndexCount && At + 2 < Mesh->Indices.size(); At += 3)
                {
                    Triangle T;
                    T.Instance = InstanceIndex;
                    T.Albedo   = Albedo;
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

    void GatherScene(GameCore::World& InWorld, Scene& OutScene, GatherReport& OutReport)
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
            if(!E.IsAlive() || !E.IsStatic()) { continue; }
            const GameCore::TransformPose Pose = E.GetWorldPose();

            if(const auto* Render = E.GetComponent<RenderCore::RenderComponent>())
            {
                if(Render->bVisible && Render->Mesh[0] != '\0') { AddMesh(E, *Render, OutScene, OutReport); }
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
                    S.SkyColor    = ToLinear(SkyLight->SkyColor) * SkyLight->Intensity;
                    S.GroundColor = ToLinear(SkyLight->GroundColor) * SkyLight->Intensity;
                    S.Rotation    = SkyLight->EnvironmentRotation * 3.14159265f / 180.0f;
                    if(SkyLight->Environment[0] != '\0'
                       && !LoadEnvironment(SkyLight->Environment, std::max(SkyLight->EnvironmentIntensity, 0.0f), S))
                    {
                        OutReport.Warnings.push_back(std::string("the sky's HDRI ") + SkyLight->Environment
                                                     + " cannot be loaded; its colours light the bake");
                    }
                    OutReport.bSky = true;
                }
            }
        }

        OutScene.Finish();
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

        for(size_t Index = 0; Index < InResult.Atlases.size(); ++Index)
        {
            const Atlas& A = InResult.Atlases[Index];
            RenderCore::TextureData Texture;
            Texture.Pixels = EngineCore::Image(A.Size, A.Size, EngineCore::EImageFormat::RGBA8);
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
