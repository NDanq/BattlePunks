#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include "Public/RenderCore/ProceduralGeneration.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderView.h"
#include "Private/RenderCore/ProceduralSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/* ProceduralSpawnerComponent: registration, its proxy, and what it exposes to
 * saves, Lua and the editor. Placement is ProceduralGeneration.cpp; when it
 * runs, ProceduralSystem.cpp. */

namespace RenderCore
{
    namespace
    {
        /* Whether any corner of a local box, through InClip, is in the frustum (as the terrain's chunks). */
        bool CellInFrustum(const EngineCore::Matrix4x4& InClip, const float (&InMin)[3], const float (&InMax)[3])
        {
            int Outside[5] = {0, 0, 0, 0, 0};
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const float P[3] = { (Corner & 1) ? InMax[0] : InMin[0],
                                     (Corner & 2) ? InMax[1] : InMin[1],
                                     (Corner & 4) ? InMax[2] : InMin[2] };
                float C[4];
                for(int Row = 0; Row < 4; ++Row)
                {
                    C[Row] = float(InClip.Data[Row][0]) * P[0] + float(InClip.Data[Row][1]) * P[1]
                           + float(InClip.Data[Row][2]) * P[2] + float(InClip.Data[Row][3]);
                }
                if(C[0] >  C[3]) { ++Outside[0]; }
                if(C[0] < -C[3]) { ++Outside[1]; }
                if(C[1] >  C[3]) { ++Outside[2]; }
                if(C[1] < -C[3]) { ++Outside[3]; }
                if(C[3] <= 0.0f) { ++Outside[4]; }
            }
            for(const int Each : Outside)
            {
                if(Each == 8) { return false; }
            }
            return true;
        }

        /*
         * The render-thread side: the latest finished generation
         * (ProceduralResultStore), one instance buffer per cell and entry,
         * swapped in whole when a new one is published -- the old instances
         * draw until then. Cells are culled per view and thinned by distance.
         */
        class ProceduralSpawnerProxy final : public RenderProxy
        {
        public:
            ProceduralSpawnerProxy(const GameCore::EntityID InOwner, const ProceduralSpawnerComponent& InInit)
                : RenderProxy(InOwner)
                , WorldKey(InInit.Owner.OwningWorld)
            {
                ApplyComponent(&InInit);
            }

            bool AllowsStaticDraws() const override { return false; }

            void ApplyComponent(const void* InComponentBytes) override
            {
                const auto& C = *static_cast<const ProceduralSpawnerComponent*>(InComponentBytes);
                SetVisible(C.bVisible != 0);
                bCastShadows = C.bCastShadows != 0;
                CellSize     = std::max(C.CellSize, 100.0f);
                LODDistance  = std::max(C.LODDistance, 0.0f);
                CullDistance = std::max(C.CullDistance, 0.0f);
                Density[0]   = 1.0f;
                Density[1]   = std::clamp(C.LOD1Density, 0.0f, 1.0f);
                Density[2]   = std::clamp(C.LOD2Density, 0.0f, 1.0f);
                for(uint32_t Entry = 0; Entry < PROCEDURAL_MESHES_MAX; ++Entry)
                {
                    const ProceduralMeshEntry& In = C.Meshes[Entry];
                    if(Entries[Entry].Mesh != In.Mesh)         { Entries[Entry].Mesh.assign(In.Mesh); }
                    if(Entries[Entry].FarMesh != In.FarMesh)   { Entries[Entry].FarMesh.assign(In.FarMesh); }
                    if(Entries[Entry].Material != In.Material) { Entries[Entry].Material.assign(In.Material); }
                }
            }

            void Simulate(RenderContext& InContext) override
            {
                std::shared_ptr<const ProceduralResult> Latest = ProceduralResultStore::Find(WorldKey, GetOwner());
                if(Latest == Shown) { return; }
                Shown = std::move(Latest);
                Release(InContext);
                if(!Shown) { SetLocalBounds(GameCore::AABB()); MarkChanged(); return; }
                if(!InContext.SupportsInstancing())
                {
                    if(!bWarnedInstancing)
                    {
                        bWarnedInstancing = true;
                        RENDERCORE_LOG_WARN("this backend draws no instanced meshes: Procedural Spawners are not drawn")
                    }
                    return;
                }

                float Min[3] = {  1e30f,  1e30f,  1e30f };
                float Max[3] = { -1e30f, -1e30f, -1e30f };
                Cells.reserve(Shown->Cells.size());
                for(size_t Index = 0; Index < Shown->Cells.size(); ++Index)
                {
                    const ProceduralCell& In = Shown->Cells[Index];
                    Cell Out;
                    std::memcpy(Out.Min, In.Min, sizeof(Out.Min));
                    std::memcpy(Out.Max, In.Max, sizeof(Out.Max));
                    for(const ProceduralBatch& Batch : In.Batches)
                    {
                        if(Batch.Instances.empty()) { continue; }
                        char Name[64];
                        std::snprintf(Name, sizeof(Name), "Procedural cell %zu entry %u", Index, unsigned(Batch.Entry));
                        CellDraw Each;
                        Each.Entry  = Batch.Entry;
                        Each.Count  = uint32_t(Batch.Instances.size());
                        Each.Buffer = InContext.CreateInstanceBuffer(Batch.Instances.data(), Each.Count, Name);
                        if(Each.Buffer) { Out.Draws.push_back(Each); }
                    }
                    if(Out.Draws.empty()) { continue; }
                    for(int Axis = 0; Axis < 3; ++Axis)
                    {
                        Min[Axis] = std::min(Min[Axis], Out.Min[Axis]);
                        Max[Axis] = std::max(Max[Axis], Out.Max[Axis]);
                    }
                    Cells.push_back(std::move(Out));
                }
                SetLocalBounds(Cells.empty() ? GameCore::AABB()
                                             : GameCore::AABB(EngineCore::Vector3(Min[0], Min[1], Min[2]), EngineCore::Vector3(Max[0], Max[1], Max[2])));
                MarkChanged();
            }

            void Draw(RenderContext& InContext) override
            {
                const RenderView& View = InContext.GetView();
                const EngineCore::Matrix4x4 Clip = View.Projection * View.View * GetLocalToWorld();
                ChooseLODs(View.Origin);
                for(const Cell& Each : Cells)
                {
                    if(Each.bCulled || !CellInFrustum(Clip, Each.Min, Each.Max)) { continue; }
                    DrawCell(InContext, Each, false);
                }
            }

            /* Every cell the last view kept, at the LOD it chose (the shadow view culls the proxy, not its cells). */
            void DrawShadow(RenderContext& InContext) override
            {
                if(!bCastShadows) { return; }
                for(const Cell& Each : Cells)
                {
                    if(!Each.bCulled) { DrawCell(InContext, Each, true); }
                }
            }

            void ReleaseRenderResources(RenderContext& InContext) override
            {
                Release(InContext);
                Shown.reset();
            }

        private:
            struct CellDraw
            {
                uint8_t               Entry  = 0;
                uint32_t              Count  = 0;
                RenderInstanceBuffer* Buffer = nullptr;
            };

            struct Cell
            {
                float             Min[3] = { 0.0f, 0.0f, 0.0f };
                float             Max[3] = { 0.0f, 0.0f, 0.0f };
                std::vector<CellDraw> Draws;
                uint32_t          Lod     = 0;
                bool              bCulled = false;   // past Cull Distance, as of the last view
            };

            struct EntryNames
            {
                std::string Mesh;
                std::string FarMesh;
                std::string Material;
            };

            void DrawCell(RenderContext& InContext, const Cell& InCell, const bool bInShadow)
            {
                for(const CellDraw& Each : InCell.Draws)
                {
                    const EntryNames& Names = Entries[Each.Entry];
                    const std::string& Mesh = InCell.Lod > 0 && !Names.FarMesh.empty() ? Names.FarMesh : Names.Mesh;
                    if(Mesh.empty() || !Each.Buffer) { continue; }
                    const uint32_t Count = uint32_t(std::ceil(float(Each.Count) * Density[std::min<uint32_t>(InCell.Lod, PROCEDURAL_LODS - 1)]));
                    if(Count == 0) { continue; }

                    MaterialParameterOverrides Instanced;
                    Instanced.Instances     = Each.Buffer;
                    Instanced.InstanceCount = std::min(Count, Each.Count);
                    const char* Every[RENDER_MATERIAL_SLOTS];
                    for(const char*& Slot : Every) { Slot = Names.Material.c_str(); }
                    const char* const* Slots = Names.Material.empty() ? nullptr : Every;
                    const uint32_t SlotCount = Names.Material.empty() ? 0u : uint32_t(RENDER_MATERIAL_SLOTS);
                    if(bInShadow) { InContext.DrawMeshShadow(GetTransform(), Mesh.c_str(), Slots, SlotCount, Instanced); }
                    else          { InContext.DrawMesh(GetTransform(), Mesh.c_str(), EngineCore::Color(255, 255, 255, 255), Slots, SlotCount, Instanced); }
                }
            }

            /* Each cell's LOD and culling for a view from InEye, by its box's distance in world units. */
            void ChooseLODs(const EngineCore::Vector3& InEye)
            {
                if(Cells.empty()) { return; }
                const EngineCore::Matrix4x4& World = GetLocalToWorld();
                const EngineCore::Matrix4x4 Inverse = EngineCore::Matrix4x4::Inverse(World);
                float Eye[3];
                for(int Row = 0; Row < 3; ++Row)
                {
                    Eye[Row] = float(double(Inverse.Data[Row][0]) * InEye.GetX() + double(Inverse.Data[Row][1]) * InEye.GetY()
                                     + double(Inverse.Data[Row][2]) * InEye.GetZ() + double(Inverse.Data[Row][3]));
                }
                double Scale = 0.0;
                for(int Col = 0; Col < 3; ++Col)
                {
                    Scale += std::sqrt(double(World.Data[0][Col]) * World.Data[0][Col] + double(World.Data[1][Col]) * World.Data[1][Col]
                                       + double(World.Data[2][Col]) * World.Data[2][Col]);
                }
                const float WorldPerLocal = float(std::max(Scale / 3.0, 1e-6));
                const float Near = LODDistance > 0.0f ? LODDistance : 1.5f * CellSize;
                for(Cell& Each : Cells)
                {
                    float Distance2 = 0.0f;
                    for(int Axis = 0; Axis < 3; ++Axis)
                    {
                        const float Out = std::max({ Each.Min[Axis] - Eye[Axis], 0.0f, Eye[Axis] - Each.Max[Axis] });
                        Distance2 += Out * Out;
                    }
                    const float Distance = std::sqrt(Distance2) * WorldPerLocal;
                    Each.bCulled = CullDistance > 0.0f && Distance > CullDistance;
                    Each.Lod     = Distance < Near ? 0u : (Distance < Near * 2.0f ? 1u : 2u);
                }
            }

            void Release(RenderContext& InContext)
            {
                for(Cell& Each : Cells)
                {
                    for(CellDraw& One : Each.Draws) { InContext.ReleaseInstanceBuffer(One.Buffer); }
                }
                Cells.clear();
            }

            const void*                             WorldKey = nullptr;
            std::shared_ptr<const ProceduralResult> Shown;
            std::vector<Cell>                       Cells;
            EntryNames                              Entries[PROCEDURAL_MESHES_MAX];
            float                                   Density[PROCEDURAL_LODS] = { 1.0f, 0.5f, 0.25f };
            float                                   CellSize     = 4000.0f;
            float                                   LODDistance  = 0.0f;
            float                                   CullDistance = 0.0f;
            bool                                    bCastShadows = true;
            bool                                    bWarnedInstancing = false;
        };

        struct EntryFieldNames
        {
            char Mesh[24], FarMesh[24], Material[24], Weight[24], Scale[24];
        };

        const EntryFieldNames& EntryFields(const uint32_t InIndex)
        {
            struct Table
            {
                EntryFieldNames Names[PROCEDURAL_MESHES_MAX];
                Table()
                {
                    for(uint32_t Index = 0; Index < PROCEDURAL_MESHES_MAX; ++Index)
                    {
                        EntryFieldNames& N = Names[Index];
                        std::snprintf(N.Mesh,     sizeof(N.Mesh),     "Mesh%uPath",     Index);
                        std::snprintf(N.FarMesh,  sizeof(N.FarMesh),  "Mesh%uFar",      Index);
                        std::snprintf(N.Material, sizeof(N.Material), "Mesh%uMaterial", Index);
                        std::snprintf(N.Weight,   sizeof(N.Weight),   "Mesh%uWeight",   Index);
                        std::snprintf(N.Scale,    sizeof(N.Scale),    "Mesh%uScale",    Index);
                    }
                }
            };
            static const Table Fields;
            return Fields.Names[InIndex];
        }

        const char* ExclusionField(const uint32_t InIndex)
        {
            static const char* const Names[PROCEDURAL_EXCLUSIONS_MAX] = {
                "Exclusion0", "Exclusion1", "Exclusion2", "Exclusion3", "Exclusion4", "Exclusion5", "Exclusion6", "Exclusion7" };
            return Names[InIndex];
        }
    }

    RENDERCORE_DEFINE_PROXY_COMPONENT(ProceduralSpawnerComponent, "Procedural Spawner", ProceduralSpawnerProxy)

    CreateProxyFn GetProceduralSpawnerComponentFactory()
    {
        return &ProceduralSpawnerComponent_CreateProxy;
    }

    /* The rules; never the instances, which are made again on load. Entries
     * and exclusions up to the last in use. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(ProceduralSpawnerComponent, 1)
    {
        Ar.Flag("Visible",          C.bVisible);
        Ar.Field("Source",          C.Source);
        Ar.Field("Project",         C.Project);
        Ar.Field("TerrainLayer",    C.TerrainLayer);
        Ar.Field("Seed",            C.Seed);
        Ar.Field("BoxExtent",       C.BoxExtent);
        Ar.Field("Spacing",         C.Spacing);
        Ar.Field("Jitter",          C.Jitter);
        Ar.Field("ProjectRange",    C.ProjectRange);
        Ar.Field("PathOffset",      C.PathOffset);
        Ar.Flag("BothSides",        C.bBothSides);
        Ar.Field("Density",         C.Density);
        Ar.Field("NoiseScale",      C.NoiseScale);
        Ar.Field("NoiseThreshold",  C.NoiseThreshold);
        Ar.Field("MaxSlope",        C.MaxSlope);
        Ar.Field("MinHeight",       C.MinHeight);
        Ar.Field("MaxHeight",       C.MaxHeight);
        Ar.Field("LayerThreshold",  C.LayerThreshold);
        Ar.Field("ScaleMin",        C.ScaleMin);
        Ar.Field("ScaleMax",        C.ScaleMax);
        Ar.Field("AlignToNormal",   C.AlignToNormal);
        Ar.Field("Sink",            C.Sink);
        Ar.Field("ColorVariation",  C.ColorVariation);
        Ar.Flag("RandomYaw",        C.bRandomYaw);
        Ar.Flag("AlignToPath",      C.bAlignToPath);
        Ar.Flag("CastShadows",      C.bCastShadows);
        Ar.Field("CellSize",        C.CellSize);
        Ar.Field("LODDistance",     C.LODDistance);
        Ar.Field("LOD1Density",     C.LOD1Density);
        Ar.Field("LOD2Density",     C.LOD2Density);
        Ar.Field("CullDistance",    C.CullDistance);
        Ar.Field("MaxInstances",    C.MaxInstances);

        uint32_t Entries = 0;
        for(uint32_t Index = 0; Index < PROCEDURAL_MESHES_MAX; ++Index)
        {
            if(C.Meshes[Index].Mesh[0] != '\0') { Entries = Index + 1; }
        }
        Ar.Field("MeshCount", Entries);
        Entries = std::min<uint32_t>(Entries, PROCEDURAL_MESHES_MAX);
        for(uint32_t Index = 0; Index < Entries; ++Index)
        {
            const EntryFieldNames& Names = EntryFields(Index);
            ProceduralMeshEntry& Entry = C.Meshes[Index];
            Ar.Field(Names.Mesh,     Entry.Mesh);
            Ar.Field(Names.FarMesh,  Entry.FarMesh);
            Ar.Field(Names.Material, Entry.Material);
            Ar.Field(Names.Weight,   Entry.Weight);
            Ar.Field(Names.Scale,    Entry.Scale);
        }

        uint32_t Exclusions = 0;
        for(uint32_t Index = 0; Index < PROCEDURAL_EXCLUSIONS_MAX; ++Index)
        {
            if(C.Exclusions[Index][0] != '\0') { Exclusions = Index + 1; }
        }
        Ar.Field("ExclusionCount", Exclusions);
        Exclusions = std::min<uint32_t>(Exclusions, PROCEDURAL_EXCLUSIONS_MAX);
        for(uint32_t Index = 0; Index < Exclusions; ++Index)
        {
            Ar.Field(ExclusionField(Index), C.Exclusions[Index]);
        }

        if(Ar.IsLoading())
        {
            for(uint32_t Index = Entries; Index < PROCEDURAL_MESHES_MAX; ++Index) { C.Meshes[Index] = ProceduralMeshEntry(); }
            for(uint32_t Index = Exclusions; Index < PROCEDURAL_EXCLUSIONS_MAX; ++Index) { C.Exclusions[Index][0] = '\0'; }
            C.InstanceCount        = 0;
            C.GenerateMilliseconds = 0.0f;
        }
    }

    /* The rules by the same names (e:GetComponent("Procedural Spawner").Density = 0.5);
     * a change makes the instances again a frame later. Regenerate() forces it. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(ProceduralSpawnerComponent)
    {
        Bind.Flag("Visible",        &ProceduralSpawnerComponent::bVisible);
        Bind.Field("Source",        &ProceduralSpawnerComponent::Source);
        Bind.Field("Project",       &ProceduralSpawnerComponent::Project);
        Bind.Field("Seed",          &ProceduralSpawnerComponent::Seed);
        Bind.Field("BoxExtent",     &ProceduralSpawnerComponent::BoxExtent);
        Bind.Field("Spacing",       &ProceduralSpawnerComponent::Spacing);
        Bind.Field("Jitter",        &ProceduralSpawnerComponent::Jitter);
        Bind.Field("Density",       &ProceduralSpawnerComponent::Density);
        Bind.Field("NoiseScale",    &ProceduralSpawnerComponent::NoiseScale);
        Bind.Field("NoiseThreshold", &ProceduralSpawnerComponent::NoiseThreshold);
        Bind.Field("MaxSlope",      &ProceduralSpawnerComponent::MaxSlope);
        Bind.Field("ScaleMin",      &ProceduralSpawnerComponent::ScaleMin);
        Bind.Field("ScaleMax",      &ProceduralSpawnerComponent::ScaleMax);
        Bind.Field("AlignToNormal", &ProceduralSpawnerComponent::AlignToNormal);
        Bind.Field("CullDistance",  &ProceduralSpawnerComponent::CullDistance);
        Bind.ReadOnly("InstanceCount", &ProceduralSpawnerComponent::InstanceCount);
        Bind.Method("Regenerate", [](ProceduralSpawnerComponent& C, GameCore::ScriptCall& Call)
        {
            (void)C; (void)Call;
            RegenerateAllProcedural();
        }, "");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(ProceduralSpawnerComponent)
    {
        Props.Flag("Visible", &ProceduralSpawnerComponent::bVisible);
        Props.Category("Source");
        Props.Choice("Source",  &ProceduralSpawnerComponent::Source, "Spline Path|Spline Area|Box|Mesh Surface");   // EProceduralSource
        Props.Choice("Project", &ProceduralSpawnerComponent::Project, "None|Terrain|Static Meshes|Terrain and Meshes"); // EProceduralProject
        Props.Field("Box Extent",    &ProceduralSpawnerComponent::BoxExtent);
        Props.Field("Spacing",       &ProceduralSpawnerComponent::Spacing);
        Props.Field("Jitter",        &ProceduralSpawnerComponent::Jitter);
        Props.Field("Project Range", &ProceduralSpawnerComponent::ProjectRange);
        Props.Field("Path Offset",   &ProceduralSpawnerComponent::PathOffset);
        Props.Flag("Both Sides",     &ProceduralSpawnerComponent::bBothSides);
        Props.Field("Seed",          &ProceduralSpawnerComponent::Seed);
        Props.Category("Filters");
        Props.Field("Density",         &ProceduralSpawnerComponent::Density);
        Props.Field("Noise Scale",     &ProceduralSpawnerComponent::NoiseScale);
        Props.Field("Noise Threshold", &ProceduralSpawnerComponent::NoiseThreshold);
        Props.Field("Max Slope",       &ProceduralSpawnerComponent::MaxSlope);
        Props.Field("Min Height",      &ProceduralSpawnerComponent::MinHeight);
        Props.Field("Max Height",      &ProceduralSpawnerComponent::MaxHeight);
        Props.Choice("Terrain Layer",  &ProceduralSpawnerComponent::TerrainLayer, "Any|Layer 0|Layer 1|Layer 2|Layer 3");
        Props.Field("Layer Threshold", &ProceduralSpawnerComponent::LayerThreshold);
        Props.Array("Exclusions",      &ProceduralSpawnerComponent::Exclusions);
        Props.Category("Instances");
        Props.StructArray("Meshes", &ProceduralSpawnerComponent::Meshes, [](auto& Member)
        {
            Member.Field("Mesh",     &ProceduralMeshEntry::Mesh, GameCore::PF_AssetMesh);
            Member.Field("Far Mesh", &ProceduralMeshEntry::FarMesh, GameCore::PF_AssetMesh);
            Member.Field("Material", &ProceduralMeshEntry::Material, GameCore::PF_AssetMaterial);
            Member.Field("Weight",   &ProceduralMeshEntry::Weight);
            Member.Field("Scale",    &ProceduralMeshEntry::Scale);
        });
        Props.Field("Scale Min",       &ProceduralSpawnerComponent::ScaleMin);
        Props.Field("Scale Max",       &ProceduralSpawnerComponent::ScaleMax);
        Props.Field("Align to Normal", &ProceduralSpawnerComponent::AlignToNormal);
        Props.Flag("Random Yaw",       &ProceduralSpawnerComponent::bRandomYaw);
        Props.Flag("Align to Path",    &ProceduralSpawnerComponent::bAlignToPath);
        Props.Field("Sink",            &ProceduralSpawnerComponent::Sink);
        Props.Field("Color Variation", &ProceduralSpawnerComponent::ColorVariation);
        Props.Category("Drawing");
        Props.Flag("Cast Shadows",     &ProceduralSpawnerComponent::bCastShadows);
        Props.Field("Cell Size",       &ProceduralSpawnerComponent::CellSize);
        Props.Field("LOD Distance",    &ProceduralSpawnerComponent::LODDistance);
        Props.Field("LOD 1 Density",   &ProceduralSpawnerComponent::LOD1Density);
        Props.Field("LOD 2 Density",   &ProceduralSpawnerComponent::LOD2Density);
        Props.Field("Cull Distance",   &ProceduralSpawnerComponent::CullDistance);
        Props.Field("Max Instances",   &ProceduralSpawnerComponent::MaxInstances);
        Props.Category("State");
        Props.Field("Instance Count",  &ProceduralSpawnerComponent::InstanceCount, GameCore::PF_ReadOnly);
        Props.Field("Generate ms",     &ProceduralSpawnerComponent::GenerateMilliseconds, GameCore::PF_ReadOnly);
    }
#endif
}
