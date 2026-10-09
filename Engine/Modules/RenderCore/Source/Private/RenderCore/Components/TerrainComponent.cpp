#include "Public/RenderCore/Components/TerrainComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/* TerrainComponent: registration, its proxy, and what it exposes to saves,
 * Lua and the editor. */

namespace RenderCore
{
    std::string GetTerrainChunkMeshPath(const GameCore::EntityID InOwner, const uint32_t InX, const uint32_t InZ)
    {
        char Buffer[96];
        std::snprintf(Buffer, sizeof(Buffer), "@terrain/%llx/%u_%u",
                      static_cast<unsigned long long>(InOwner.Pack()), InX, InZ);
        return Buffer;
    }

    bool BuildTerrainChunkMesh(const GameCore::TerrainData& InData, const uint32_t InX, const uint32_t InZ,
                               const float InUVTileSize, MeshData& OutMesh)
    {
        std::lock_guard<std::mutex> Guard(InData.GetLock());
        const GameCore::TerrainRect Rect = InData.GetChunkSamples(InX, InZ);
        if(Rect.IsEmpty())
        {
            return false;
        }

        const uint32_t Columns = uint32_t(Rect.X1 - Rect.X0 + 1);
        const uint32_t Rows    = uint32_t(Rect.Z1 - Rect.Z0 + 1);
        const uint32_t Count   = Columns * Rows;
        const float    Spacing = InData.GetSpacing();
        const float    Tile    = InUVTileSize > 1e-3f ? InUVTileSize : 400.0f;

        OutMesh = MeshData();
        OutMesh.Positions.resize(size_t(Count) * 3);
        OutMesh.Normals.resize(size_t(Count) * 3);
        OutMesh.UV0.resize(size_t(Count) * 2);
        OutMesh.Colors.resize(size_t(Count) * 4);

        size_t Vertex = 0;
        for(int32_t Z = Rect.Z0; Z <= Rect.Z1; ++Z)
        {
            for(int32_t X = Rect.X0; X <= Rect.X1; ++X, ++Vertex)
            {
                const float PX = float(X) * Spacing;
                const float PZ = float(Z) * Spacing;
                OutMesh.Positions[Vertex * 3 + 0] = PX;
                OutMesh.Positions[Vertex * 3 + 1] = InData.GetHeight(X, Z);
                OutMesh.Positions[Vertex * 3 + 2] = PZ;

                float Normal[3];
                InData.GetSmoothNormal(X, Z, Normal);
                OutMesh.Normals[Vertex * 3 + 0] = Normal[0];
                OutMesh.Normals[Vertex * 3 + 1] = Normal[1];
                OutMesh.Normals[Vertex * 3 + 2] = Normal[2];

                OutMesh.UV0[Vertex * 2 + 0] = PX / Tile;
                OutMesh.UV0[Vertex * 2 + 1] = PZ / Tile;

                const uint8_t* Weight = InData.GetWeight(X, Z);
                for(uint32_t Layer = 0; Layer < 4; ++Layer)
                {
                    OutMesh.Colors[Vertex * 4 + Layer] = float(Weight[Layer]) / 255.0f;
                }
            }
        }

        /* Each cell along the (X, Z) -> (X + 1, Z + 1) diagonal, as TerrainData samples it. */
        OutMesh.Indices.reserve(size_t(Columns - 1) * (Rows - 1) * 6);
        for(uint32_t Z = 0; Z + 1 < Rows; ++Z)
        {
            for(uint32_t X = 0; X + 1 < Columns; ++X)
            {
                const uint32_t I00 = Z * Columns + X;
                const uint32_t I10 = I00 + 1;
                const uint32_t I01 = I00 + Columns;
                const uint32_t I11 = I01 + 1;
                OutMesh.Indices.insert(OutMesh.Indices.end(), { I00, I01, I11, I00, I11, I10 });
            }
        }

        MeshSection Section;
        Section.Name       = "Terrain";
        Section.FirstIndex = 0;
        Section.IndexCount = uint32_t(OutMesh.Indices.size());
        OutMesh.Sections.push_back(Section);
        ComputeMeshBounds(OutMesh);
        return true;
    }

    namespace
    {
        /* Whether any of a local box's corners, through InClip (projection * view * world), is in the frustum:
         * a box is out when every corner is past the same side. Conservative: corners may straddle. */
        bool BoxInFrustum(const EngineCore::Matrix4x4& InClip, const float (&InMin)[3], const float (&InMax)[3])
        {
            int Outside[5] = {0, 0, 0, 0, 0};   // past +X, -X, +Y, -Y, behind
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
         * The render-thread side: a mesh per chunk, kept in MeshAssetCache
         * under the entity's own paths, rebuilt in Simulate whenever the
         * chunk's samples change (an editor brush) -- so it never captures
         * static draws, and culls its chunks per view in Draw.
         */
        class TerrainProxy final : public RenderProxy
        {
        public:
            TerrainProxy(const GameCore::EntityID InOwner, const TerrainComponent& InInit)
                : RenderProxy(InOwner)
            {
                ApplyComponent(&InInit);
            }

            bool AllowsStaticDraws() const override { return false; }
            bool IsOccluder() const override { return bOccluder; }

            void ApplyComponent(const void* InComponentBytes) override
            {
                const auto& C = *static_cast<const TerrainComponent*>(InComponentBytes);
                Material.assign(C.Material);
                bCastShadows = C.bCastShadows != 0;
                bOccluder    = C.bOccluder != 0;
                SetVisible(C.bVisible != 0);
                if(Path != C.Terrain)
                {
                    Path.assign(C.Terrain);
                    Data.reset();
                    CacheGeneration = ~uint64_t(0);   // look it up in the next Simulate
                }
                if(C.UVTileSize != UVTileSize)
                {
                    UVTileSize = C.UVTileSize;
                    bRebuildAll = true;
                }
            }

            void Simulate(RenderContext& InContext) override
            {
                (void)InContext;
                Refresh();
            }

            void Draw(RenderContext& InContext) override
            {
                const RenderView& View = InContext.GetView();
                const EngineCore::Matrix4x4 Clip = View.Projection * View.View * GetLocalToWorld();
                const char* Slots[1] = { Material.c_str() };
                for(const Chunk& Each : Chunks)
                {
                    if(!Each.bBuilt || !BoxInFrustum(Clip, Each.Min, Each.Max)) { continue; }
                    InContext.DrawMesh(GetTransform(), Each.Path.c_str(), EngineCore::Color(255, 255, 255, 255), Slots, 1);
                }
            }

            void DrawShadow(RenderContext& InContext) override
            {
                if(!bCastShadows) { return; }
                const char* Slots[1] = { Material.c_str() };
                for(const Chunk& Each : Chunks)
                {
                    if(Each.bBuilt) { InContext.DrawMeshShadow(GetTransform(), Each.Path.c_str(), Slots, 1); }
                }
            }

            void DrawOccluder(RenderContext& InContext) override
            {
                const RenderView& View = InContext.GetView();
                const EngineCore::Matrix4x4 Clip = View.Projection * View.View * GetLocalToWorld();
                const char* Slots[1] = { Material.c_str() };
                for(const Chunk& Each : Chunks)
                {
                    if(!Each.bBuilt || !BoxInFrustum(Clip, Each.Min, Each.Max)) { continue; }
                    InContext.DrawMeshOccluder(GetTransform(), Each.Path.c_str(), Slots, 1);
                }
            }

            void ReleaseRenderResources(RenderContext& InContext) override
            {
                for(const Chunk& Each : Chunks)
                {
                    InContext.ReleaseMesh(Each.Path.c_str());
                    MeshAssetCache::Remove(Each.Path);
                }
                Chunks.clear();
            }

        private:
            struct Chunk
            {
                std::string Path;
                uint64_t    Revision = 0;      // the data's chunk revision it was built from
                bool        bBuilt   = false;
                float       Min[3]   = {0.0f, 0.0f, 0.0f};
                float       Max[3]   = {0.0f, 0.0f, 0.0f};
            };

            /* Picks up a new terrain (path or cache), then rebuilds the chunks whose samples moved. */
            void Refresh()
            {
                const uint64_t Generation = GameCore::TerrainAssetCache::GetGeneration();
                if(Generation != CacheGeneration)
                {
                    CacheGeneration = Generation;
                    std::shared_ptr<GameCore::TerrainData> Next = Path.empty() ? nullptr : GameCore::TerrainAssetCache::Find(Path);
                    if(Next != Data)
                    {
                        Data = std::move(Next);
                        SeenRevision = 0;
                        bRebuildAll  = true;
                    }
                }
                if(!Data)
                {
                    if(!Chunks.empty()) { DropChunks(); }
                    SetLocalBounds(GameCore::AABB());
                    return;
                }

                const uint64_t Revision = Data->GetRevision();
                if(Revision == SeenRevision && !bRebuildAll) { return; }

                /* Which chunks moved, read under the lock (a Reset may replace the table). */
                uint32_t Side = 0;
                std::vector<uint64_t> Revisions;
                float Size = 0.0f, Low = 0.0f, High = 0.0f;
                {
                    std::lock_guard<std::mutex> Guard(Data->GetLock());
                    Side = Data->GetChunksPerSide();
                    Revisions.resize(size_t(Side) * Side);
                    for(uint32_t Z = 0; Z < Side; ++Z)
                    {
                        for(uint32_t X = 0; X < Side; ++X) { Revisions[size_t(Z) * Side + X] = Data->GetChunkRevision(X, Z); }
                    }
                    Size = Data->GetSize();
                    Low  = Data->GetMinHeight();
                    High = Data->GetMaxHeight();
                }
                if(Chunks.size() != Revisions.size() || Side != ChunksPerSide)
                {
                    DropChunks();
                    ChunksPerSide = Side;
                    Chunks.resize(Revisions.size());
                    for(uint32_t Z = 0; Z < Side; ++Z)
                    {
                        for(uint32_t X = 0; X < Side; ++X) { Chunks[size_t(Z) * Side + X].Path = GetTerrainChunkMeshPath(GetOwner(), X, Z); }
                    }
                }

                for(uint32_t Z = 0; Z < Side; ++Z)
                {
                    for(uint32_t X = 0; X < Side; ++X)
                    {
                        Chunk& Each = Chunks[size_t(Z) * Side + X];
                        const uint64_t Wanted = Revisions[size_t(Z) * Side + X];
                        if(Each.bBuilt && Each.Revision == Wanted && !bRebuildAll) { continue; }
                        auto Mesh = std::make_shared<MeshData>();
                        if(!BuildTerrainChunkMesh(*Data, X, Z, UVTileSize, *Mesh)) { continue; }
                        std::memcpy(Each.Min, Mesh->BoundsMin, sizeof(Each.Min));
                        std::memcpy(Each.Max, Mesh->BoundsMax, sizeof(Each.Max));
                        Each.Revision = Wanted;
                        Each.bBuilt   = true;
                        MeshAssetCache::Store(Each.Path, std::move(Mesh));
                    }
                }
                bRebuildAll  = false;
                SeenRevision = Revision;
                SetLocalBounds(GameCore::AABB(EngineCore::Vector3(0.0, double(Low), 0.0),
                                              EngineCore::Vector3(double(Size), double(High), double(Size))));
                MarkChanged();
            }

            void DropChunks()
            {
                for(const Chunk& Each : Chunks) { MeshAssetCache::Remove(Each.Path); }
                Chunks.clear();
                ChunksPerSide = 0;
            }

            std::string                            Path;
            std::string                            Material;
            std::shared_ptr<GameCore::TerrainData> Data;
            std::vector<Chunk>                     Chunks;
            uint32_t                               ChunksPerSide   = 0;
            uint64_t                               SeenRevision    = 0;
            uint64_t                               CacheGeneration = ~uint64_t(0);
            float                                  UVTileSize      = 400.0f;
            bool                                   bRebuildAll     = true;
            bool                                   bCastShadows    = true;
            bool                                   bOccluder       = true;
        };
    }

    RENDERCORE_DEFINE_PROXY_COMPONENT(TerrainComponent, "Terrain", TerrainProxy)

    CreateProxyFn GetTerrainComponentFactory()
    {
        return &TerrainComponent_CreateProxy;
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(TerrainComponent, 1)
    {
        Ar.Field("Terrain",       C.Terrain);
        Ar.Field("Material",      C.Material);
        Ar.Flag("Visible",        C.bVisible);
        Ar.Flag("CastShadows",    C.bCastShadows);
        Ar.Flag("Occluder",       C.bOccluder);
        Ar.Flag("Walkable",       C.bWalkable);
        Ar.Field("UVTileSize",    C.UVTileSize);
    }

    /* Lua sees the same names (e:GetComponent("Terrain").Walkable). */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(TerrainComponent)
    {
        Bind.Field("Terrain",     &TerrainComponent::Terrain);
        Bind.Field("Material",    &TerrainComponent::Material);
        Bind.Flag("Visible",      &TerrainComponent::bVisible);
        Bind.Flag("CastShadows",  &TerrainComponent::bCastShadows);
        Bind.Flag("Occluder",     &TerrainComponent::bOccluder);
        Bind.Flag("Walkable",     &TerrainComponent::bWalkable);
        Bind.Field("UVTileSize",  &TerrainComponent::UVTileSize);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(TerrainComponent)
    {
        Props.Field("Terrain",       &TerrainComponent::Terrain, GameCore::PF_AssetTerrain);
        Props.Field("Material",      &TerrainComponent::Material, GameCore::PF_AssetMaterial);
        Props.Flag("Visible",        &TerrainComponent::bVisible);
        Props.Field("UV Tile Size",  &TerrainComponent::UVTileSize);
        Props.Category("Collision");
        Props.Flag("Walkable",       &TerrainComponent::bWalkable);
        Props.Category("Shadows");
        Props.Flag("Cast Shadows",   &TerrainComponent::bCastShadows);
        Props.Category("Culling");
        Props.Flag("Occluder",       &TerrainComponent::bOccluder);
    }
#endif
}
