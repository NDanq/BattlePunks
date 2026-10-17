#include "Public/RenderCore/Components/TerrainComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Foliage.h"

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
#include <utility>
#include <vector>

/* TerrainComponent: registration, its proxy, and what it exposes to saves,
 * Lua and the editor. */

namespace RenderCore
{
    std::string GetTerrainChunkMeshPath(const GameCore::EntityID InOwner, const uint32_t InX, const uint32_t InZ, const uint32_t InLod)
    {
        char Buffer[96];
        if(InLod == 0)
        {
            std::snprintf(Buffer, sizeof(Buffer), "@terrain/%llx/%u_%u", static_cast<unsigned long long>(InOwner.Pack()), InX, InZ);
        }
        else
        {
            std::snprintf(Buffer, sizeof(Buffer), "@terrain/%llx/%u_%u_lod%u", static_cast<unsigned long long>(InOwner.Pack()), InX, InZ, InLod);
        }
        return Buffer;
    }

    bool BuildTerrainChunkMesh(const GameCore::TerrainData& InData, const uint32_t InX, const uint32_t InZ,
                               const float InUVTileSize, MeshData& OutMesh, const uint32_t InLod, const bool bInSkirts,
                               const float InSkirtDepth)
    {
        std::lock_guard<std::mutex> Guard(InData.GetLock());
        const GameCore::TerrainRect Rect = InData.GetChunkSamples(InX, InZ);
        if(Rect.IsEmpty())
        {
            return false;
        }

        /* The samples this LOD takes along each axis: every Step-th, and the last always. */
        const int32_t Step = int32_t(1u << std::min<uint32_t>(InLod, TERRAIN_LODS - 1));
        const auto Pick = [Step](const int32_t InFirst, const int32_t InLast)
        {
            std::vector<int32_t> Out;
            for(int32_t At = InFirst; At < InLast; At += Step) { Out.push_back(At); }
            Out.push_back(InLast);
            return Out;
        };
        const std::vector<int32_t> Xs = Pick(Rect.X0, Rect.X1);
        const std::vector<int32_t> Zs = Pick(Rect.Z0, Rect.Z1);
        const uint32_t Columns = uint32_t(Xs.size());
        const uint32_t Rows    = uint32_t(Zs.size());
        const float    Spacing = InData.GetSpacing();
        const float    Tile    = InUVTileSize > 1e-3f ? InUVTileSize : 400.0f;

        OutMesh = MeshData();
        const size_t Reserve = size_t(Columns) * Rows + (bInSkirts ? size_t(Columns + Rows) * 2 : 0);
        OutMesh.Positions.reserve(Reserve * 3);
        OutMesh.Normals.reserve(Reserve * 3);
        OutMesh.UV0.reserve(Reserve * 2);
        OutMesh.Colors.reserve(Reserve * 4);

        const auto AddVertex = [&](const int32_t InSampleX, const int32_t InSampleZ, const float InDrop)
        {
            const float PX = float(InSampleX) * Spacing;
            const float PZ = float(InSampleZ) * Spacing;
            OutMesh.Positions.insert(OutMesh.Positions.end(), { PX, InData.GetHeight(InSampleX, InSampleZ) - InDrop, PZ });
            float Normal[3];
            InData.GetSmoothNormal(InSampleX, InSampleZ, Normal);
            OutMesh.Normals.insert(OutMesh.Normals.end(), { Normal[0], Normal[1], Normal[2] });
            OutMesh.UV0.insert(OutMesh.UV0.end(), { PX / Tile, PZ / Tile });
            const uint8_t* Weight = InData.GetWeight(InSampleX, InSampleZ);
            for(uint32_t Layer = 0; Layer < 4; ++Layer) { OutMesh.Colors.push_back(float(Weight[Layer]) / 255.0f); }
            return uint32_t(OutMesh.Positions.size() / 3 - 1);
        };

        for(uint32_t Z = 0; Z < Rows; ++Z)
        {
            for(uint32_t X = 0; X < Columns; ++X) { AddVertex(Xs[X], Zs[Z], 0.0f); }
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

        /* Skirts: each edge's vertices again, dropped, joined to the edge by a strip. */
        if(bInSkirts && InSkirtDepth > 0.0f)
        {
            const auto Skirt = [&](const std::vector<uint32_t>& InEdge, const std::vector<std::pair<int32_t, int32_t>>& InSamples)
            {
                std::vector<uint32_t> Dropped;
                Dropped.reserve(InEdge.size());
                for(const auto& Each : InSamples) { Dropped.push_back(AddVertex(Each.first, Each.second, InSkirtDepth)); }
                for(size_t Each = 0; Each + 1 < InEdge.size(); ++Each)
                {
                    OutMesh.Indices.insert(OutMesh.Indices.end(), { InEdge[Each], Dropped[Each], Dropped[Each + 1],
                                                                    InEdge[Each], Dropped[Each + 1], InEdge[Each + 1] });
                }
            };
            std::vector<uint32_t> Edge;
            std::vector<std::pair<int32_t, int32_t>> Samples;
            const auto Gather = [&](const bool bInAlongX, const uint32_t InFixed)
            {
                Edge.clear();
                Samples.clear();
                const uint32_t Count = bInAlongX ? Columns : Rows;
                for(uint32_t Each = 0; Each < Count; ++Each)
                {
                    const uint32_t X = bInAlongX ? Each : InFixed;
                    const uint32_t Z = bInAlongX ? InFixed : Each;
                    Edge.push_back(Z * Columns + X);
                    Samples.emplace_back(Xs[X], Zs[Z]);
                }
                Skirt(Edge, Samples);
            };
            Gather(true, 0);
            Gather(true, Rows - 1);
            Gather(false, 0);
            Gather(false, Columns - 1);
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
        /* What a Terrain with no Material draws with: flat layer colours mixed by its weights. */
        const char* const DEFAULT_TERRAIN_MATERIAL = "Engine/Content/Materials/Terrain.material.nmgraph";

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
         *
         * Its foliage (GameCore's Foliage.h) rides the same chunks: an
         * instance buffer per chunk and type, sorted so its first N are the
         * instances a coarser LOD keeps; rebuilt when the chunk's foliage or
         * its ground changes; drawn when the chunk is in view and nearer than
         * the type's cull distance, as many as the chunk's LOD keeps, with
         * that LOD's mesh.
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
                Material.assign(C.Material[0] != '\0' ? C.Material : DEFAULT_TERRAIN_MATERIAL);
                bCastShadows = C.bCastShadows != 0;
                bOccluder    = C.bOccluder != 0;
                bShowTerrain = C.bVisible != 0;
                bShowFoliage = C.bFoliageVisible != 0;
                SetVisible(bShowTerrain || bShowFoliage);
                if(Path != C.Terrain)
                {
                    Path.assign(C.Terrain);
                    Data.reset();
                    CacheGeneration = ~uint64_t(0);   // look it up in the next Simulate
                }
                if(FoliagePath != C.Foliage)
                {
                    FoliagePath.assign(C.Foliage);
                    Foliage.reset();
                    FoliageGeneration = ~uint64_t(0);
                }
                if(C.UVTileSize != UVTileSize)
                {
                    UVTileSize = C.UVTileSize;
                    bRebuildAll = true;
                }
                LODDistance = std::max(C.LODDistance, 0.0f);
            }

            void Simulate(RenderContext& InContext) override
            {
                Refresh(InContext);
                RefreshFoliage(InContext);
            }

            void Draw(RenderContext& InContext) override
            {
                const RenderView& View = InContext.GetView();
                const EngineCore::Matrix4x4 Clip = View.Projection * View.View * GetLocalToWorld();
                ChooseLODs(View.Origin);
                const char* Slots[1] = { Material.c_str() };
                for(const Chunk& Each : Chunks)
                {
                    if(!Each.bBuilt) { continue; }
                    if(bShowTerrain && BoxInFrustum(Clip, Each.Min, Each.Max))
                    {
                        InContext.DrawMesh(GetTransform(), Each.Paths[Each.Lod].c_str(), EngineCore::Color(255, 255, 255, 255), Slots, 1);
                    }
                    if(bShowFoliage && !Each.Foliage.empty())
                    {
                        const float Max[3] = { Each.Max[0], Each.Max[1] + Each.FoliageTop, Each.Max[2] };
                        if(BoxInFrustum(Clip, Each.Min, Max)) { DrawFoliage(InContext, Each, false); }
                    }
                }
            }

            /* Every chunk (the shadow view culls the proxy, not its chunks), at the LOD the view last chose. */
            void DrawShadow(RenderContext& InContext) override
            {
                const char* Slots[1] = { Material.c_str() };
                for(const Chunk& Each : Chunks)
                {
                    if(!Each.bBuilt) { continue; }
                    if(bShowTerrain && bCastShadows) { InContext.DrawMeshShadow(GetTransform(), Each.Paths[Each.Lod].c_str(), Slots, 1); }
                    if(bShowFoliage && !Each.Foliage.empty()) { DrawFoliage(InContext, Each, true); }
                }
            }

            void DrawOccluder(RenderContext& InContext) override
            {
                if(!bShowTerrain) { return; }
                const RenderView& View = InContext.GetView();
                const EngineCore::Matrix4x4 Clip = View.Projection * View.View * GetLocalToWorld();
                ChooseLODs(View.Origin);
                const char* Slots[1] = { Material.c_str() };
                for(const Chunk& Each : Chunks)
                {
                    if(!Each.bBuilt || !BoxInFrustum(Clip, Each.Min, Each.Max)) { continue; }
                    InContext.DrawMeshOccluder(GetTransform(), Each.Paths[Each.Lod].c_str(), Slots, 1);
                }
            }

            float GetBoundsPadding(RenderContext& InContext) override
            {
                (void)InContext;
                return 0.0f;   // the foliage's height is in the local bounds
            }

            void ReleaseRenderResources(RenderContext& InContext) override
            {
                for(Chunk& Each : Chunks)
                {
                    for(const std::string& Path : Each.Paths)
                    {
                        InContext.ReleaseMesh(Path.c_str());
                        MeshAssetCache::Remove(Path);
                    }
                    ReleaseFoliage(InContext, Each);
                }
                Chunks.clear();
            }

        private:
            /* One foliage type's instances in one chunk. */
            struct FoliageDraw
            {
                uint16_t              Type   = 0;
                RenderInstanceBuffer* Buffer = nullptr;
                uint32_t              Count  = 0;
            };

            struct Chunk
            {
                std::string Paths[TERRAIN_LODS];
                uint32_t    Lod      = 0;      // what the last view chose
                float       Distance = 0.0f;   // and how far it was, terrain units
                uint64_t    Revision = 0;      // the data's chunk revision it was built from
                bool        bBuilt   = false;
                float       Min[3]   = {0.0f, 0.0f, 0.0f};
                float       Max[3]   = {0.0f, 0.0f, 0.0f};

                std::vector<FoliageDraw> Foliage;
                uint64_t                 FoliageRevision = 0;   // the bucket's, as built
                uint64_t                 FoliageGround   = 0;   // the chunk revision it stood on
                float                    FoliageTop      = 0.0f;   // how far above the ground it reaches
            };

            /* Type InDraw's instances in InChunk: as many as its LOD keeps, with that LOD's mesh. */
            void DrawFoliage(RenderContext& InContext, const Chunk& InChunk, const bool bInShadow)
            {
                for(const FoliageDraw& Each : InChunk.Foliage)
                {
                    if(Each.Type >= FoliageTypes.size() || !Each.Buffer) { continue; }
                    const GameCore::FoliageType& Type = FoliageTypes[Each.Type];
                    if(bInShadow && !Type.bCastShadows) { continue; }
                    if(Type.CullDistance > 0.0f && InChunk.Distance * Scale > Type.CullDistance) { continue; }
                    const uint32_t Lod   = std::min<uint32_t>(InChunk.Lod, GameCore::FOLIAGE_LODS - 1);
                    const float Density  = std::clamp(Type.LODDensity[Lod], 0.0f, 1.0f);
                    const uint32_t Count = uint32_t(std::ceil(float(Each.Count) * Density));
                    if(Count == 0) { continue; }
                    const std::string* Mesh = nullptr;
                    for(int32_t Level = int32_t(Lod); Level >= 0 && !Mesh; --Level)
                    {
                        if(!Type.Mesh[Level].empty()) { Mesh = &Type.Mesh[Level]; }
                    }
                    if(!Mesh) { continue; }

                    MaterialParameterOverrides Instanced;
                    Instanced.Instances     = Each.Buffer;
                    Instanced.InstanceCount = Count;
                    const char* Every[RENDER_MATERIAL_SLOTS];
                    for(const char*& Slot : Every) { Slot = Type.Material.c_str(); }
                    const char* const* Slots = Type.Material.empty() ? nullptr : Every;
                    const uint32_t SlotCount = Type.Material.empty() ? 0u : uint32_t(RENDER_MATERIAL_SLOTS);
                    if(bInShadow) { InContext.DrawMeshShadow(GetTransform(), Mesh->c_str(), Slots, SlotCount, Instanced); }
                    else          { InContext.DrawMesh(GetTransform(), Mesh->c_str(), EngineCore::Color(255, 255, 255, 255), Slots, SlotCount, Instanced); }
                }
            }

            void ReleaseFoliage(RenderContext& InContext, Chunk& InChunk)
            {
                for(FoliageDraw& Each : InChunk.Foliage) { InContext.ReleaseInstanceBuffer(Each.Buffer); }
                InChunk.Foliage.clear();
                InChunk.FoliageRevision = 0;
                InChunk.FoliageGround   = 0;
                InChunk.FoliageTop      = 0.0f;
            }

            /* Picks up a new terrain (path or cache), then rebuilds the chunks whose samples moved. */
            void Refresh(RenderContext& InContext)
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
                    if(!Chunks.empty()) { DropChunks(InContext); }
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
                    DropChunks(InContext);
                    ChunksPerSide = Side;
                    Chunks.resize(Revisions.size());
                    for(uint32_t Z = 0; Z < Side; ++Z)
                    {
                        for(uint32_t X = 0; X < Side; ++X)
                        {
                            for(uint32_t Lod = 0; Lod < TERRAIN_LODS; ++Lod)
                            {
                                Chunks[size_t(Z) * Side + X].Paths[Lod] = GetTerrainChunkMeshPath(GetOwner(), X, Z, Lod);
                            }
                        }
                    }
                }

                for(uint32_t Z = 0; Z < Side; ++Z)
                {
                    for(uint32_t X = 0; X < Side; ++X)
                    {
                        Chunk& Each = Chunks[size_t(Z) * Side + X];
                        const uint64_t Wanted = Revisions[size_t(Z) * Side + X];
                        if(Each.bBuilt && Each.Revision == Wanted && !bRebuildAll) { continue; }
                        /* Skirts deep enough to hide where a coarser neighbour cuts a corner. */
                        const float SkirtDepth = Data->GetSpacing() * 4.0f;
                        bool bAll = true;
                        for(uint32_t Lod = 0; Lod < TERRAIN_LODS && bAll; ++Lod)
                        {
                            auto Mesh = std::make_shared<MeshData>();
                            if(!BuildTerrainChunkMesh(*Data, X, Z, UVTileSize, *Mesh, Lod, true, SkirtDepth)) { bAll = false; break; }
                            if(Lod == 0)
                            {
                                std::memcpy(Each.Min, Mesh->BoundsMin, sizeof(Each.Min));
                                std::memcpy(Each.Max, Mesh->BoundsMax, sizeof(Each.Max));
                            }
                            MeshAssetCache::Store(Each.Paths[Lod], std::move(Mesh));
                        }
                        if(!bAll) { continue; }
                        Each.Revision = Wanted;
                        Each.bBuilt   = true;
                    }
                }
                bRebuildAll  = false;
                SeenRevision = Revision;
                TerrainBounds = GameCore::AABB(EngineCore::Vector3(0.0, double(Low), 0.0),
                                               EngineCore::Vector3(double(Size), double(High), double(Size)));
                UpdateBounds();
                MarkChanged();
            }

            /* The foliage: picks up a new file, then rebuilds the chunks whose bucket or ground moved. */
            void RefreshFoliage(RenderContext& InContext)
            {
                const uint64_t Generation = GameCore::FoliageAssetCache::GetGeneration();
                if(Generation != FoliageGeneration)
                {
                    FoliageGeneration = Generation;
                    std::shared_ptr<GameCore::FoliageData> Next = FoliagePath.empty() ? nullptr : GameCore::FoliageAssetCache::Find(FoliagePath);
                    if(Next != Foliage)
                    {
                        Foliage = std::move(Next);
                        SeenFoliageRevision = 0;
                        for(Chunk& Each : Chunks) { ReleaseFoliage(InContext, Each); }
                    }
                }
                if(!Foliage || !Data || Chunks.empty())
                {
                    if(!Foliage) { for(Chunk& Each : Chunks) { if(!Each.Foliage.empty()) { ReleaseFoliage(InContext, Each); } } }
                    return;
                }
                const uint64_t Revision = Foliage->GetRevision();
                if(Revision == SeenFoliageRevision && SeenFoliageGround == SeenRevision) { return; }

                if(!InContext.SupportsInstancing())
                {
                    if(!bWarnedInstancing)
                    {
                        bWarnedInstancing = true;
                        RENDERCORE_LOG_WARN("this backend draws no instanced meshes: terrain foliage is not drawn")
                    }
                    SeenFoliageRevision = Revision;
                    SeenFoliageGround   = SeenRevision;
                    return;
                }

                /* Lock order: the foliage, then the terrain (as the brushes take them). */
                std::vector<std::vector<GameCore::FoliageInstance>> Rebuild(Chunks.size());
                std::vector<uint64_t> BucketRevisions(Chunks.size(), 0);
                std::vector<bool> bChanged(Chunks.size(), false);
                {
                    std::lock_guard<std::mutex> Guard(Foliage->GetLock());
                    FoliageTypes = Foliage->GetTypes();
                    const bool bFits = Foliage->GetBucketsPerSide() == ChunksPerSide;
                    if(!bFits && !bWarnedGrid)
                    {
                        bWarnedGrid = true;
                        RENDERCORE_LOG_WARN("foliage '" + FoliagePath + "' was made for a terrain of another size: it is not drawn")
                    }
                    for(uint32_t Z = 0; Z < ChunksPerSide && bFits; ++Z)
                    {
                        for(uint32_t X = 0; X < ChunksPerSide; ++X)
                        {
                            const size_t Index = size_t(Z) * ChunksPerSide + X;
                            const uint64_t Bucket = Foliage->GetBucketRevision(X, Z);
                            BucketRevisions[Index] = Bucket;
                            if(Chunks[Index].FoliageRevision == Bucket && Chunks[Index].FoliageGround == Chunks[Index].Revision) { continue; }
                            bChanged[Index] = true;
                            Rebuild[Index]  = Foliage->GetBucket(X, Z);
                        }
                    }
                }

                /* Each type's tallest point above its pivot, at scale 1 (its LOD 0 mesh's box). */
                std::vector<float> Tops(FoliageTypes.size(), 0.0f);
                for(size_t Type = 0; Type < FoliageTypes.size(); ++Type)
                {
                    if(const std::shared_ptr<const MeshData> Mesh = FoliageTypes[Type].Mesh[0].empty() ? nullptr
                                                                    : MeshAssetCache::Find(FoliageTypes[Type].Mesh[0]))
                    {
                        Tops[Type] = std::max(Mesh->BoundsMax[1], 0.0f);
                    }
                }

                std::lock_guard<std::mutex> TerrainGuard(Data->GetLock());
                for(size_t Index = 0; Index < Chunks.size(); ++Index)
                {
                    if(!bChanged[Index]) { continue; }
                    Chunk& Each = Chunks[Index];
                    ReleaseFoliage(InContext, Each);

                    /* By type; within a type, in thinning order: its first N are what a coarser LOD keeps. */
                    std::vector<GameCore::FoliageInstance>& Instances = Rebuild[Index];
                    std::sort(Instances.begin(), Instances.end(), [](const GameCore::FoliageInstance& A, const GameCore::FoliageInstance& B)
                    {
                        if(A.Type != B.Type) { return A.Type < B.Type; }
                        return GameCore::FoliageSeedFraction(A.Seed) < GameCore::FoliageSeedFraction(B.Seed);
                    });
                    std::vector<RenderInstance> Packed;
                    size_t First = 0;
                    while(First < Instances.size())
                    {
                        const uint16_t Type = Instances[First].Type;
                        size_t Last = First;
                        while(Last < Instances.size() && Instances[Last].Type == Type) { ++Last; }
                        if(Type < FoliageTypes.size())
                        {
                            const GameCore::FoliageType& Kind = FoliageTypes[Type];
                            Packed.assign(Last - First, RenderInstance());
                            float Highest = 0.0f;
                            for(size_t Each = First; Each < Last; ++Each)
                            {
                                RenderInstance& Out = Packed[Each - First];
                                GameCore::GetFoliageTransform(*Data, Kind, Instances[Each], Out.Transform);
                                const float Shade = 1.0f - std::clamp(Kind.ColorVariation, 0.0f, 1.0f)
                                                         * GameCore::FoliageSeedFraction(Instances[Each].Seed * 7919u + 13u);
                                Out.Color[0] = Out.Color[1] = Out.Color[2] = Shade;
                                Out.Color[3] = 1.0f;
                                Highest = std::max(Highest, Out.Transform[13] - Data->SampleHeight(Instances[Each].X, Instances[Each].Z)
                                                            + Tops[Type] * Instances[Each].Scale);
                            }
                            char Name[96];
                            std::snprintf(Name, sizeof(Name), "Foliage %s chunk %zu", Kind.Name.c_str(), Index);
                            FoliageDraw Draw;
                            Draw.Type   = Type;
                            Draw.Count  = uint32_t(Packed.size());
                            Draw.Buffer = InContext.CreateInstanceBuffer(Packed.data(), Draw.Count, Name);
                            if(Draw.Buffer)
                            {
                                Each.Foliage.push_back(Draw);
                                Each.FoliageTop = std::max(Each.FoliageTop, Highest);
                            }
                        }
                        First = Last;
                    }
                    Each.FoliageRevision = BucketRevisions[Index];
                    Each.FoliageGround   = Each.Revision;
                }
                SeenFoliageRevision = Revision;
                SeenFoliageGround   = SeenRevision;
                UpdateBounds();
                MarkChanged();
            }

            /* The terrain's box, raised by the tallest foliage. */
            void UpdateBounds()
            {
                float Top = 0.0f;
                for(const Chunk& Each : Chunks) { Top = std::max(Top, Each.FoliageTop); }
                GameCore::AABB Box = TerrainBounds;
                if(!Box.IsEmpty() && Top > 0.0f)
                {
                    Box = GameCore::AABB(Box.Min, EngineCore::Vector3(Box.Max.GetX(), Box.Max.GetY() + double(Top), Box.Max.GetZ()));
                }
                SetLocalBounds(Box);
            }

            /* Each chunk's LOD for a view from InEye: by the distance from the eye to its box, in
             * terrain units (LODDistance is the world's; the entity's ground scale converts). */
            void ChooseLODs(const EngineCore::Vector3& InEye)
            {
                if(Chunks.empty() || !Data) { return; }
                const EngineCore::Matrix4x4& World = GetLocalToWorld();
                const EngineCore::Matrix4x4 Inverse = EngineCore::Matrix4x4::Inverse(World);
                float Eye[3];
                for(int Row = 0; Row < 3; ++Row)
                {
                    Eye[Row] = float(double(Inverse.Data[Row][0]) * InEye.GetX() + double(Inverse.Data[Row][1]) * InEye.GetY()
                                     + double(Inverse.Data[Row][2]) * InEye.GetZ() + double(Inverse.Data[Row][3]));
                }
                const double ScaleX = std::sqrt(double(World.Data[0][0]) * World.Data[0][0] + double(World.Data[1][0]) * World.Data[1][0]
                                                + double(World.Data[2][0]) * World.Data[2][0]);
                const double ScaleZ = std::sqrt(double(World.Data[0][2]) * World.Data[0][2] + double(World.Data[1][2]) * World.Data[1][2]
                                                + double(World.Data[2][2]) * World.Data[2][2]);
                Scale = float(std::max(0.5 * (ScaleX + ScaleZ), 1e-6));
                const float Near = LODDistance > 0.0f ? LODDistance / Scale
                                                      : 1.5f * float(GameCore::TERRAIN_CHUNK_QUADS) * Data->GetSpacing();
                for(Chunk& Each : Chunks)
                {
                    float Distance2 = 0.0f;
                    for(int Axis = 0; Axis < 3; ++Axis)
                    {
                        const float Top = Axis == 1 ? Each.Max[1] + Each.FoliageTop : Each.Max[Axis];
                        const float Out = std::max({ Each.Min[Axis] - Eye[Axis], 0.0f, Eye[Axis] - Top });
                        Distance2 += Out * Out;
                    }
                    Each.Distance = std::sqrt(Distance2);
                    Each.Lod = Each.Distance < Near ? 0u : (Each.Distance < Near * 2.0f ? 1u : 2u);
                }
            }

            void DropChunks(RenderContext& InContext)
            {
                for(Chunk& Each : Chunks)
                {
                    for(const std::string& Path : Each.Paths) { MeshAssetCache::Remove(Path); }
                    ReleaseFoliage(InContext, Each);
                }
                Chunks.clear();
                ChunksPerSide = 0;
            }

            std::string                            Path;
            std::string                            Material;
            std::shared_ptr<GameCore::TerrainData> Data;
            std::vector<Chunk>                     Chunks;
            GameCore::AABB                         TerrainBounds;
            uint32_t                               ChunksPerSide   = 0;
            uint64_t                               SeenRevision    = 0;
            uint64_t                               CacheGeneration = ~uint64_t(0);
            float                                  UVTileSize      = 400.0f;
            float                                  LODDistance     = 0.0f;
            float                                  Scale           = 1.0f;   // world units per terrain unit, as of the last view
            bool                                   bRebuildAll     = true;
            bool                                   bCastShadows    = true;
            bool                                   bOccluder       = true;
            bool                                   bShowTerrain    = true;
            bool                                   bShowFoliage    = true;

            std::string                            FoliagePath;
            std::shared_ptr<GameCore::FoliageData> Foliage;
            std::vector<GameCore::FoliageType>     FoliageTypes;          // as of the last rebuild
            uint64_t                               FoliageGeneration   = ~uint64_t(0);
            uint64_t                               SeenFoliageRevision = 0;
            uint64_t                               SeenFoliageGround   = 0;
            bool                                   bWarnedInstancing   = false;
            bool                                   bWarnedGrid         = false;
        };
    }

    RENDERCORE_DEFINE_PROXY_COMPONENT(TerrainComponent, "Terrain", TerrainProxy)

    CreateProxyFn GetTerrainComponentFactory()
    {
        return &TerrainComponent_CreateProxy;
    }

    /* Version 2 adds the LOD distance; 3, the foliage. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(TerrainComponent, 3)
    {
        Ar.Field("Terrain",       C.Terrain);
        Ar.Field("Material",      C.Material);
        Ar.Flag("Visible",        C.bVisible);
        Ar.Flag("CastShadows",    C.bCastShadows);
        Ar.Flag("Occluder",       C.bOccluder);
        Ar.Flag("Walkable",       C.bWalkable);
        Ar.Field("UVTileSize",    C.UVTileSize);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("LODDistance", C.LODDistance);
        }
        if(Ar.GetVersion() >= 3)
        {
            Ar.Field("Foliage",        C.Foliage);
            Ar.Flag("FoliageVisible",  C.bFoliageVisible);
        }
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
        Bind.Field("LODDistance", &TerrainComponent::LODDistance);
        Bind.Field("Foliage",     &TerrainComponent::Foliage);
        Bind.Flag("FoliageVisible", &TerrainComponent::bFoliageVisible);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(TerrainComponent)
    {
        Props.TypeCategory("Rendering");
        Props.Field("Terrain",       &TerrainComponent::Terrain, GameCore::PF_AssetTerrain);
        Props.Field("Material",      &TerrainComponent::Material, GameCore::PF_AssetMaterial);
        Props.Flag("Visible",        &TerrainComponent::bVisible);
        Props.Field("UV Tile Size",  &TerrainComponent::UVTileSize);
        Props.Category("Foliage");
        Props.Field("Foliage",       &TerrainComponent::Foliage, GameCore::PF_AssetFoliage);
        Props.Flag("Foliage Visible", &TerrainComponent::bFoliageVisible);
        Props.Category("Collision");
        Props.Flag("Walkable",       &TerrainComponent::bWalkable);
        Props.Category("Shadows");
        Props.Flag("Cast Shadows",   &TerrainComponent::bCastShadows);
        Props.Category("Culling");
        Props.Flag("Occluder",       &TerrainComponent::bOccluder);
        Props.Field("LOD Distance",  &TerrainComponent::LODDistance);
    }
#endif
}
