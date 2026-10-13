#include "Public/AICore/NavMesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "Recast.h"

#include "Private/AICore/NavInternal.h"
#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreLog.h"

/*
 * NavMesh: Recast's tiled build into DetourTileCache layers, after
 * Sample_TempObstacles.cpp (RecastDemo). NavMesh.h has the pipeline and the
 * file format.
 */
namespace AICore
{
    uint64_t NavBuildSettings::Hash() const
    {
        uint64_t Hash = NavDetail::HASH_SEED;
        const float Floats[] = { CellSize, CellHeight, AgentHeight, AgentRadius, AgentMaxClimb, AgentMaxSlope,
                                 RegionMinSize, RegionMergeSize, EdgeMaxLength, EdgeMaxError };
        NavDetail::HashBytes(Hash, Floats, sizeof(Floats));
        const int32_t Ints[] = { TileSize, LayerMask };
        NavDetail::HashBytes(Hash, Ints, sizeof(Ints));
        return Hash;
    }

    namespace
    {
        constexpr char     MAGIC[4]       = { 'N', 'M', 'N', 'V' };
        constexpr uint32_t BINARY_VERSION = 1;
        constexpr int      LAYERS_PER_TILE = 4;     // expected; a tile may have more (stairs over stairs)
        constexpr int      MAX_LAYERS      = 32;
        constexpr int      MAX_OBSTACLES   = 256;
        constexpr int      VERTS_PER_POLY  = 6;

        constexpr uint32_t Tag(const char InA, const char InB, const char InC, const char InD)
        {
            return uint32_t(uint8_t(InA)) | (uint32_t(uint8_t(InB)) << 8) | (uint32_t(uint8_t(InC)) << 16) | (uint32_t(uint8_t(InD)) << 24);
        }
        constexpr uint32_t TAG_HEADER = Tag('H', 'E', 'A', 'D');
        constexpr uint32_t TAG_LAYER  = Tag('L', 'A', 'Y', 'R');

        /* The layers as built: no compression. A level's layers are small,
         * and a pak compresses them anyway. */
        struct PassthroughCompressor : public dtTileCacheCompressor
        {
            int maxCompressedSize(const int InBufferSize) override { return InBufferSize; }
            dtStatus compress(const unsigned char* InBuffer, const int InBufferSize, unsigned char* OutCompressed,
                              const int InMaxCompressedSize, int* OutCompressedSize) override
            {
                if(InBufferSize > InMaxCompressedSize) { return DT_FAILURE | DT_BUFFER_TOO_SMALL; }
                std::memcpy(OutCompressed, InBuffer, size_t(InBufferSize));
                *OutCompressedSize = InBufferSize;
                return DT_SUCCESS;
            }
            dtStatus decompress(const unsigned char* InCompressed, const int InCompressedSize, unsigned char* OutBuffer,
                                const int InMaxBufferSize, int* OutBufferSize) override
            {
                if(InCompressedSize > InMaxBufferSize) { return DT_FAILURE | DT_BUFFER_TOO_SMALL; }
                std::memcpy(OutBuffer, InCompressed, size_t(InCompressedSize));
                *OutBufferSize = InCompressedSize;
                return DT_SUCCESS;
            }
        };

        /* Every polygon of a walkable area is walkable; the area keeps its
         * id for the filter's cost. */
        struct MeshProcess : public dtTileCacheMeshProcess
        {
            void process(dtNavMeshCreateParams* InParams, unsigned char* InOutAreas, unsigned short* OutFlags) override
            {
                for(int Index = 0; Index < InParams->polyCount; ++Index)
                {
                    OutFlags[Index] = InOutAreas[Index] != NAV_AREA_BLOCKED ? NAV_FLAG_WALK : 0;
                }
            }
        };

        int NextPow2(int InValue)
        {
            int Result = 1;
            while(Result < InValue) { Result <<= 1; }
            return Result;
        }

        int Log2(int InValue)
        {
            int Result = 0;
            while((1 << (Result + 1)) <= InValue) { ++Result; }
            return Result;
        }

        /* The four corners of an area box, turned, for rcMarkConvexPolyArea. */
        void AreaCorners(const NavAreaBox& InBox, float (&OutVerts)[12])
        {
            const float Yaw = InBox.Yaw * 3.14159265358979f / 180.0f;
            const float C = std::cos(Yaw), S = std::sin(Yaw);
            const float HX = float(InBox.HalfExtents.GetX()), HZ = float(InBox.HalfExtents.GetZ());
            const float CX = float(InBox.Center.GetX()), CY = float(InBox.Center.GetY()), CZ = float(InBox.Center.GetZ());
            const float Local[4][2] = { { -HX, -HZ }, { HX, -HZ }, { HX, HZ }, { -HX, HZ } };
            for(int Index = 0; Index < 4; ++Index)
            {
                /* A turn about +Y by Yaw: x' = x cos + z sin, z' = -x sin + z cos. */
                OutVerts[Index * 3 + 0] = CX + Local[Index][0] * C + Local[Index][1] * S;
                OutVerts[Index * 3 + 1] = CY;
                OutVerts[Index * 3 + 2] = CZ - Local[Index][0] * S + Local[Index][1] * C;
            }
        }

        struct RasterContext
        {
            rcHeightfield*            Solid   = nullptr;
            rcCompactHeightfield*     Compact = nullptr;
            rcHeightfieldLayerSet*    Layers  = nullptr;
            ~RasterContext()
            {
                rcFreeHeightField(Solid);
                rcFreeCompactHeightfield(Compact);
                rcFreeHeightfieldLayerSet(Layers);
            }
        };
    }

    struct NavMesh::Impl
    {
        NavBuildSettings Settings;
        GameCore::AABB   Bounds;
        uint64_t         GeometryHash = 0;
        int32_t          TilesX = 0;
        int32_t          TilesZ = 0;
        uint64_t         Revision = 1;
        double           LastBuildMs = 0.0;
        int32_t          InputTriangles = 0;

        dtNavMesh*   Detour = nullptr;
        dtTileCache* Cache  = nullptr;
        dtTileCacheAlloc     Allocator;
        PassthroughCompressor Compressor;
        MeshProcess          Process;

        ~Impl() { Free(); }

        void Free()
        {
            dtFreeNavMesh(Detour);
            dtFreeTileCache(Cache);
            Detour = nullptr;
            Cache  = nullptr;
            TilesX = TilesZ = 0;
        }

        float TileWorldSize() const { return float(Settings.TileSize) * Settings.CellSize; }

        /* The Recast config every tile shares; its bounds are set per tile. */
        rcConfig BaseConfig() const
        {
            rcConfig Config;
            std::memset(&Config, 0, sizeof(Config));
            Config.cs = Settings.CellSize;
            Config.ch = Settings.CellHeight;
            Config.walkableSlopeAngle = Settings.AgentMaxSlope;
            Config.walkableHeight = int(std::ceil(Settings.AgentHeight / Config.ch));
            Config.walkableClimb  = int(std::floor(Settings.AgentMaxClimb / Config.ch));
            Config.walkableRadius = int(std::ceil(Settings.AgentRadius / Config.cs));
            Config.maxEdgeLen = int(Settings.EdgeMaxLength / Config.cs);
            Config.maxSimplificationError = Settings.EdgeMaxError;
            Config.minRegionArea   = int(Settings.RegionMinSize * Settings.RegionMinSize);
            Config.mergeRegionArea = int(Settings.RegionMergeSize * Settings.RegionMergeSize);
            Config.maxVertsPerPoly = VERTS_PER_POLY;
            Config.tileSize   = Settings.TileSize;
            Config.borderSize = Config.walkableRadius + 3;
            Config.width  = Config.tileSize + Config.borderSize * 2;
            Config.height = Config.tileSize + Config.borderSize * 2;
            Config.detailSampleDist     = Config.cs * 6.0f;
            Config.detailSampleMaxError = Config.ch * 1.0f;
            return Config;
        }

        bool Init(std::string* OutError)
        {
            const auto Fail = [OutError](const char* InWhy) { if(OutError) { *OutError = InWhy; } return false; };
            Free();

            const rcConfig Config = BaseConfig();
            float BMin[3], BMax[3];
            NavDetail::ToFloat3(Bounds.Min, BMin);
            NavDetail::ToFloat3(Bounds.Max, BMax);
            int GridW = 0, GridH = 0;
            rcCalcGridSize(BMin, BMax, Config.cs, &GridW, &GridH);
            TilesX = (GridW + Config.tileSize - 1) / Config.tileSize;
            TilesZ = (GridH + Config.tileSize - 1) / Config.tileSize;
            if(TilesX <= 0 || TilesZ <= 0) { return Fail("the bounds are empty"); }

            dtTileCacheParams CacheParams;
            std::memset(&CacheParams, 0, sizeof(CacheParams));
            dtVcopy(CacheParams.orig, BMin);
            CacheParams.cs = Config.cs;
            CacheParams.ch = Config.ch;
            CacheParams.width  = Config.tileSize;
            CacheParams.height = Config.tileSize;
            CacheParams.walkableHeight = Settings.AgentHeight;
            CacheParams.walkableRadius = Settings.AgentRadius;
            CacheParams.walkableClimb  = Settings.AgentMaxClimb;
            CacheParams.maxSimplificationError = Settings.EdgeMaxError;
            CacheParams.maxTiles     = TilesX * TilesZ * LAYERS_PER_TILE;
            CacheParams.maxObstacles = MAX_OBSTACLES;

            Cache = dtAllocTileCache();
            if(!Cache) { return Fail("out of memory (tile cache)"); }
            if(dtStatusFailed(Cache->init(&CacheParams, &Allocator, &Compressor, &Process))) { return Fail("could not set up the tile cache"); }

            const int TileBits = std::min(Log2(NextPow2(TilesX * TilesZ * LAYERS_PER_TILE)), 14);
            const int PolyBits = 22 - TileBits;
            dtNavMeshParams MeshParams;
            std::memset(&MeshParams, 0, sizeof(MeshParams));
            dtVcopy(MeshParams.orig, BMin);
            MeshParams.tileWidth  = TileWorldSize();
            MeshParams.tileHeight = TileWorldSize();
            MeshParams.maxTiles = 1 << TileBits;
            MeshParams.maxPolys = 1 << PolyBits;

            Detour = dtAllocNavMesh();
            if(!Detour) { return Fail("out of memory (navmesh)"); }
            if(dtStatusFailed(Detour->init(&MeshParams))) { return Fail("could not set up the navmesh"); }
            return true;
        }

        /* Tile (InX, InZ)'s layers from InGeometry, added to the cache. How
         * many layers. */
        int RasterizeTile(const NavInputGeometry& InGeometry, const std::vector<float>& InTriBounds, const int InX, const int InZ)
        {
            rcContext Context(false);
            rcConfig Config = BaseConfig();

            const float TileSize = TileWorldSize();
            Config.bmin[0] = float(Bounds.Min.GetX()) + float(InX) * TileSize;
            Config.bmin[1] = float(Bounds.Min.GetY());
            Config.bmin[2] = float(Bounds.Min.GetZ()) + float(InZ) * TileSize;
            Config.bmax[0] = float(Bounds.Min.GetX()) + float(InX + 1) * TileSize;
            Config.bmax[1] = float(Bounds.Max.GetY());
            Config.bmax[2] = float(Bounds.Min.GetZ()) + float(InZ + 1) * TileSize;
            Config.bmin[0] -= float(Config.borderSize) * Config.cs;
            Config.bmin[2] -= float(Config.borderSize) * Config.cs;
            Config.bmax[0] += float(Config.borderSize) * Config.cs;
            Config.bmax[2] += float(Config.borderSize) * Config.cs;

            /* The triangles that reach this tile (with its border). */
            std::vector<int> Tris;
            const size_t TriangleCount = InGeometry.GetTriangleCount();
            for(size_t Tri = 0; Tri < TriangleCount; ++Tri)
            {
                const float* B = &InTriBounds[Tri * 4];   // minx, minz, maxx, maxz
                if(B[2] < Config.bmin[0] || B[0] > Config.bmax[0] || B[3] < Config.bmin[2] || B[1] > Config.bmax[2]) { continue; }
                Tris.push_back(InGeometry.Indices[Tri * 3 + 0]);
                Tris.push_back(InGeometry.Indices[Tri * 3 + 1]);
                Tris.push_back(InGeometry.Indices[Tri * 3 + 2]);
            }
            if(Tris.empty()) { return 0; }

            RasterContext Raster;
            Raster.Solid = rcAllocHeightfield();
            if(!Raster.Solid || !rcCreateHeightfield(&Context, *Raster.Solid, Config.width, Config.height, Config.bmin, Config.bmax, Config.cs, Config.ch))
            {
                return 0;
            }

            const int NumTris  = int(Tris.size() / 3);
            const int NumVerts = int(InGeometry.GetVertexCount());
            std::vector<unsigned char> Areas(size_t(NumTris), 0);
            rcMarkWalkableTriangles(&Context, Config.walkableSlopeAngle, InGeometry.Vertices.data(), NumVerts, Tris.data(), NumTris, Areas.data());
            if(!rcRasterizeTriangles(&Context, InGeometry.Vertices.data(), NumVerts, Tris.data(), Areas.data(), NumTris, *Raster.Solid, Config.walkableClimb))
            {
                return 0;
            }

            rcFilterLowHangingWalkableObstacles(&Context, Config.walkableClimb, *Raster.Solid);
            rcFilterLedgeSpans(&Context, Config.walkableHeight, Config.walkableClimb, *Raster.Solid);
            rcFilterWalkableLowHeightSpans(&Context, Config.walkableHeight, *Raster.Solid);

            Raster.Compact = rcAllocCompactHeightfield();
            if(!Raster.Compact || !rcBuildCompactHeightfield(&Context, Config.walkableHeight, Config.walkableClimb, *Raster.Solid, *Raster.Compact))
            {
                return 0;
            }
            if(!rcErodeWalkableArea(&Context, Config.walkableRadius, *Raster.Compact)) { return 0; }

            /* The areas, in order: a later box over an earlier one wins. */
            for(const NavAreaBox& Box : InGeometry.Areas)
            {
                float Verts[12];
                AreaCorners(Box, Verts);
                const float MinY = float(Box.Center.GetY() - Box.HalfExtents.GetY());
                const float MaxY = float(Box.Center.GetY() + Box.HalfExtents.GetY());
                rcMarkConvexPolyArea(&Context, Verts, 4, MinY, MaxY, Box.Area, *Raster.Compact);
            }

            Raster.Layers = rcAllocHeightfieldLayerSet();
            if(!Raster.Layers || !rcBuildHeightfieldLayers(&Context, *Raster.Compact, Config.borderSize, Config.walkableHeight, *Raster.Layers))
            {
                return 0;
            }

            int Added = 0;
            for(int Layer = 0; Layer < std::min(Raster.Layers->nlayers, MAX_LAYERS); ++Layer)
            {
                const rcHeightfieldLayer& Source = Raster.Layers->layers[Layer];
                dtTileCacheLayerHeader Header;
                std::memset(&Header, 0, sizeof(Header));
                Header.magic   = DT_TILECACHE_MAGIC;
                Header.version = DT_TILECACHE_VERSION;
                Header.tx = InX;
                Header.ty = InZ;
                Header.tlayer = Layer;
                dtVcopy(Header.bmin, Source.bmin);
                dtVcopy(Header.bmax, Source.bmax);
                Header.width  = (unsigned char)Source.width;
                Header.height = (unsigned char)Source.height;
                Header.minx = (unsigned char)Source.minx;
                Header.maxx = (unsigned char)Source.maxx;
                Header.miny = (unsigned char)Source.miny;
                Header.maxy = (unsigned char)Source.maxy;
                Header.hmin = (unsigned short)Source.hmin;
                Header.hmax = (unsigned short)Source.hmax;

                unsigned char* Data = nullptr;
                int DataSize = 0;
                if(dtStatusFailed(dtBuildTileCacheLayer(&Compressor, &Header, Source.heights, Source.areas, Source.cons, &Data, &DataSize)))
                {
                    continue;
                }
                if(dtStatusFailed(Cache->addTile(Data, DataSize, DT_COMPRESSEDTILE_FREE_DATA, nullptr)))
                {
                    dtFree(Data);
                    continue;
                }
                ++Added;
            }
            return Added;
        }

        /* Removes every layer and navmesh tile at (InX, InZ). */
        void RemoveTilesAt(const int InX, const int InZ)
        {
            dtCompressedTileRef Refs[MAX_LAYERS];
            const int Count = Cache->getTilesAt(InX, InZ, Refs, MAX_LAYERS);
            for(int Index = 0; Index < Count; ++Index)
            {
                unsigned char* Data = nullptr;
                int DataSize = 0;
                Cache->removeTile(Refs[Index], &Data, &DataSize);
                dtFree(Data);
            }
            for(int Layer = 0; Layer < MAX_LAYERS; ++Layer)
            {
                const dtTileRef Tile = Detour->getTileRefAt(InX, InZ, Layer);
                if(Tile) { Detour->removeTile(Tile, nullptr, nullptr); }
            }
        }

        static std::vector<float> TriangleBounds(const NavInputGeometry& InGeometry)
        {
            const size_t Count = InGeometry.GetTriangleCount();
            std::vector<float> Result(Count * 4);
            for(size_t Tri = 0; Tri < Count; ++Tri)
            {
                float MinX = 1e30f, MinZ = 1e30f, MaxX = -1e30f, MaxZ = -1e30f;
                for(int Corner = 0; Corner < 3; ++Corner)
                {
                    const float* V = &InGeometry.Vertices[size_t(InGeometry.Indices[Tri * 3 + size_t(Corner)]) * 3];
                    MinX = std::min(MinX, V[0]); MaxX = std::max(MaxX, V[0]);
                    MinZ = std::min(MinZ, V[2]); MaxZ = std::max(MaxZ, V[2]);
                }
                Result[Tri * 4 + 0] = MinX;
                Result[Tri * 4 + 1] = MinZ;
                Result[Tri * 4 + 2] = MaxX;
                Result[Tri * 4 + 3] = MaxZ;
            }
            return Result;
        }

        int PolygonCount() const
        {
            if(!Detour) { return 0; }
            int Polys = 0;
            const dtNavMesh* Const = Detour;
            for(int Index = 0; Index < Const->getMaxTiles(); ++Index)
            {
                const dtMeshTile* Tile = Const->getTile(Index);
                if(Tile && Tile->header) { Polys += Tile->header->polyCount; }
            }
            return Polys;
        }

        int NavTileCount() const
        {
            if(!Detour) { return 0; }
            int Tiles = 0;
            const dtNavMesh* Const = Detour;
            for(int Index = 0; Index < Const->getMaxTiles(); ++Index)
            {
                const dtMeshTile* Tile = Const->getTile(Index);
                if(Tile && Tile->header && Tile->dataSize > 0) { ++Tiles; }
            }
            return Tiles;
        }
    };

    NavMesh::NavMesh() : P(new Impl()) {}
    NavMesh::~NavMesh() { delete P; }

    void NavMesh::Clear()
    {
        P->Free();
        P->GeometryHash = 0;
        ++P->Revision;
    }

    bool NavMesh::IsValid() const { return P->Detour != nullptr && P->Cache != nullptr; }

    bool NavMesh::Build(const NavInputGeometry& InGeometry, const NavBuildSettings& InSettings, const GameCore::AABB& InBounds,
                        NavBuildStats* OutStats, std::string* OutError)
    {
        const auto Started = std::chrono::steady_clock::now();
        const auto Fail = [this, OutError](const std::string& InWhy) { if(OutError) { *OutError = InWhy; } P->Free(); ++P->Revision; return false; };

        if(InGeometry.IsEmpty()) { return Fail("there is no geometry to build from"); }

        P->Settings = InSettings;
        P->Settings.CellSize   = std::max(P->Settings.CellSize, 1.0f);
        P->Settings.CellHeight = std::max(P->Settings.CellHeight, 1.0f);
        /* A layer's side is a byte: the tile and its border must fit. */
        P->Settings.TileSize = std::clamp(P->Settings.TileSize, 16, 200);

        GameCore::AABB Bounds = InBounds.IsEmpty() ? InGeometry.GetBounds() : InBounds;
        /* The geometry's height, whatever the volume says: Recast clips spans to the bounds. */
        const GameCore::AABB GeometryBounds = InGeometry.GetBounds();
        Bounds.Min.SetY(std::min(float(Bounds.Min.GetY()), float(GeometryBounds.Min.GetY())) - P->Settings.CellHeight);
        Bounds.Max.SetY(std::max(float(Bounds.Max.GetY()), float(GeometryBounds.Max.GetY())) + P->Settings.AgentHeight);
        P->Bounds = Bounds;

        if(!P->Init(OutError)) { P->Free(); ++P->Revision; return false; }

        const std::vector<float> TriBounds = Impl::TriangleBounds(InGeometry);
        int Layers = 0;
        for(int Z = 0; Z < P->TilesZ; ++Z)
        {
            for(int X = 0; X < P->TilesX; ++X)
            {
                Layers += P->RasterizeTile(InGeometry, TriBounds, X, Z);
                P->Cache->buildNavMeshTilesAt(X, Z, P->Detour);
            }
        }
        if(Layers == 0) { return Fail("nothing in the bounds is walkable"); }

        P->GeometryHash   = InGeometry.Hash() ^ P->Settings.Hash();
        P->InputTriangles = int32_t(InGeometry.GetTriangleCount());
        P->LastBuildMs    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
        ++P->Revision;

        if(OutStats)
        {
            *OutStats = GetStats();
        }
        return true;
    }

    int32_t NavMesh::RebuildTiles(const NavInputGeometry& InGeometry, const GameCore::AABB& InRegion)
    {
        if(!IsValid() || InRegion.IsEmpty()) { return 0; }
        const float TileSize = P->TileWorldSize();
        const int X0 = std::max(0, int(std::floor((float(InRegion.Min.GetX()) - float(P->Bounds.Min.GetX())) / TileSize)));
        const int Z0 = std::max(0, int(std::floor((float(InRegion.Min.GetZ()) - float(P->Bounds.Min.GetZ())) / TileSize)));
        const int X1 = std::min(P->TilesX - 1, int(std::floor((float(InRegion.Max.GetX()) - float(P->Bounds.Min.GetX())) / TileSize)));
        const int Z1 = std::min(P->TilesZ - 1, int(std::floor((float(InRegion.Max.GetZ()) - float(P->Bounds.Min.GetZ())) / TileSize)));
        if(X1 < X0 || Z1 < Z0) { return 0; }

        const std::vector<float> TriBounds = Impl::TriangleBounds(InGeometry);
        int32_t Rebuilt = 0;
        for(int Z = Z0; Z <= Z1; ++Z)
        {
            for(int X = X0; X <= X1; ++X)
            {
                P->RemoveTilesAt(X, Z);
                P->RasterizeTile(InGeometry, TriBounds, X, Z);
                P->Cache->buildNavMeshTilesAt(X, Z, P->Detour);
                ++Rebuilt;
            }
        }
        ++P->Revision;
        return Rebuilt;
    }

    // === Files ================================================================

    std::vector<uint8_t> NavMesh::Save() const
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        char Magic[4];
        std::memcpy(Magic, MAGIC, 4);
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = BINARY_VERSION;
        Ar << Version;
        if(!IsValid()) { return Ar.GetBytes(); }

        uint16_t ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_HEADER, ChunkVersion))
        {
            NavBuildSettings S = P->Settings;
            Ar << S.CellSize << S.CellHeight << S.AgentHeight << S.AgentRadius << S.AgentMaxClimb << S.AgentMaxSlope;
            Ar << S.TileSize << S.RegionMinSize << S.RegionMergeSize << S.EdgeMaxLength << S.EdgeMaxError << S.LayerMask;
            uint64_t SettingsHash = S.Hash();
            uint64_t GeometryHash = P->GeometryHash;
            Ar << SettingsHash << GeometryHash;
            float Bounds[6] = { float(P->Bounds.Min.GetX()), float(P->Bounds.Min.GetY()), float(P->Bounds.Min.GetZ()),
                                float(P->Bounds.Max.GetX()), float(P->Bounds.Max.GetY()), float(P->Bounds.Max.GetZ()) };
            for(float& Value : Bounds) { Ar << Value; }
            int32_t TilesX = P->TilesX, TilesZ = P->TilesZ, Triangles = P->InputTriangles;
            Ar << TilesX << TilesZ << Triangles;
            Ar.CloseChunk();
        }

        const dtTileCache* Cache = P->Cache;
        for(int Index = 0; Index < Cache->getTileCount(); ++Index)
        {
            const dtCompressedTile* Tile = Cache->getTile(Index);
            if(!Tile || !Tile->header || !Tile->data || Tile->dataSize <= 0) { continue; }
            ChunkVersion = 1;
            if(Ar.OpenChunk(TAG_LAYER, ChunkVersion))
            {
                uint32_t Size = uint32_t(Tile->dataSize);
                Ar << Size;
                Ar.SerializeBytes(Tile->data, size_t(Size));
                Ar.CloseChunk();
            }
        }
        return Ar.GetBytes();
    }

    bool NavMesh::Load(const std::vector<uint8_t>& InBytes, std::string* OutError)
    {
        const auto Fail = [this, OutError](const std::string& InWhy) { if(OutError) { *OutError = InWhy; } P->Free(); ++P->Revision; return false; };
        if(InBytes.size() < 8 || std::memcmp(InBytes.data(), MAGIC, 4) != 0) { return Fail("is not a .nmnav"); }

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        char Magic[4];
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = 0;
        Ar << Version;
        if(Version == 0 || Version > BINARY_VERSION) { return Fail("was written by a newer build (version " + std::to_string(Version) + ")"); }

        bool bHeader = false;
        int32_t Layers = 0;
        while(!Ar.HasError() && !Ar.IsAtEnd())
        {
            uint16_t ChunkVersion = 0;
            if(Ar.OpenChunk(TAG_HEADER, ChunkVersion))
            {
                NavBuildSettings& S = P->Settings;
                Ar << S.CellSize << S.CellHeight << S.AgentHeight << S.AgentRadius << S.AgentMaxClimb << S.AgentMaxSlope;
                Ar << S.TileSize << S.RegionMinSize << S.RegionMergeSize << S.EdgeMaxLength << S.EdgeMaxError << S.LayerMask;
                uint64_t SettingsHash = 0;
                Ar << SettingsHash << P->GeometryHash;
                float Bounds[6] = {};
                for(float& Value : Bounds) { Ar << Value; }
                P->Bounds = GameCore::AABB(EngineCore::Vector3(Bounds[0], Bounds[1], Bounds[2]), EngineCore::Vector3(Bounds[3], Bounds[4], Bounds[5]));
                int32_t TilesX = 0, TilesZ = 0;
                Ar << TilesX << TilesZ << P->InputTriangles;
                Ar.CloseChunk();
                if(Ar.HasError()) { break; }
                if(!P->Init(OutError)) { P->Free(); ++P->Revision; return false; }
                if(P->TilesX != TilesX || P->TilesZ != TilesZ) { return Fail("its tile grid does not match its settings"); }
                bHeader = true;
            }
            else if(Ar.OpenChunk(TAG_LAYER, ChunkVersion))
            {
                uint32_t Size = 0;
                Ar << Size;
                if(!bHeader || Size == 0 || Size > (64u << 20)) { Ar.SetError("a layer out of place"); Ar.CloseChunk(); break; }
                unsigned char* Data = static_cast<unsigned char*>(dtAlloc(int(Size), DT_ALLOC_PERM));
                if(!Data) { Ar.CloseChunk(); return Fail("out of memory"); }
                Ar.SerializeBytes(Data, size_t(Size));
                Ar.CloseChunk();
                if(Ar.HasError()) { dtFree(Data); break; }
                dtCompressedTileRef Ref = 0;
                if(dtStatusFailed(P->Cache->addTile(Data, int(Size), DT_COMPRESSEDTILE_FREE_DATA, &Ref)))
                {
                    dtFree(Data);
                    continue;
                }
                P->Cache->buildNavMeshTile(Ref, P->Detour);
                ++Layers;
            }
            else
            {
                /* A chunk this build does not know: OpenChunk with its own tag steps over it. */
                uint16_t Skip = 0;
                if(Ar.OpenChunk(Ar.PeekChunkTag(), Skip)) { Ar.CloseChunk(); }
                else { break; }
            }
        }
        if(Ar.HasError()) { return Fail("is damaged"); }
        if(!bHeader) { return Fail("has no header"); }
        if(Layers == 0) { return Fail("has no tiles"); }
        ++P->Revision;
        return true;
    }

    bool NavMesh::LoadFile(const std::string& InPath, std::string* OutError)
    {
        std::vector<uint8_t> Bytes;
        /* Through the file provider (paks, loose files), else straight off disk. */
        EngineCore::IFileProvider* Files = EngineCore::GetFileProvider();
        if(!(Files && Files->ReadAll(InPath, Bytes)) && !EngineCore::appFileReadAll(InPath, Bytes))
        {
            if(OutError) { *OutError = "cannot be read"; }
            return false;
        }
        return Load(Bytes, OutError);
    }

    bool NavMesh::SaveFile(const std::string& InDiskPath, std::string* OutError) const
    {
        if(!IsValid())
        {
            if(OutError) { *OutError = "there is no navmesh to save"; }
            return false;
        }
        std::vector<uint8_t> Bytes = Save();
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        Ar.SerializeBytes(Bytes.data(), Bytes.size());
        if(!EngineCore::SaveArchiveToFile(InDiskPath, Ar))
        {
            if(OutError) { *OutError = "could not be written"; }
            return false;
        }
        return true;
    }

    // === Obstacles ============================================================

    uint32_t NavMesh::AddBoxObstacle(const EngineCore::Vector3& InCenter, const EngineCore::Vector3& InHalfExtents, const float InYawDegrees)
    {
        if(!IsValid()) { return 0; }
        float Center[3], Half[3];
        NavDetail::ToFloat3(InCenter, Center);
        NavDetail::ToFloat3(InHalfExtents, Half);
        dtObstacleRef Ref = 0;
        if(dtStatusFailed(P->Cache->addBoxObstacle(Center, Half, InYawDegrees * 3.14159265358979f / 180.0f, &Ref))) { return 0; }
        return uint32_t(Ref);
    }

    uint32_t NavMesh::AddCylinderObstacle(const EngineCore::Vector3& InFeet, const float InRadius, const float InHeight)
    {
        if(!IsValid()) { return 0; }
        float Feet[3];
        NavDetail::ToFloat3(InFeet, Feet);
        dtObstacleRef Ref = 0;
        if(dtStatusFailed(P->Cache->addObstacle(Feet, InRadius, InHeight, &Ref))) { return 0; }
        return uint32_t(Ref);
    }

    void NavMesh::RemoveObstacle(const uint32_t InObstacle)
    {
        if(IsValid() && InObstacle != 0) { P->Cache->removeObstacle(dtObstacleRef(InObstacle)); }
    }

    size_t NavMesh::GetObstacleCount() const
    {
        if(!IsValid()) { return 0; }
        size_t Count = 0;
        const dtTileCache* Cache = P->Cache;
        for(int Index = 0; Index < Cache->getObstacleCount(); ++Index)
        {
            const dtTileCacheObstacle* Obstacle = Cache->getObstacle(Index);
            if(Obstacle && Obstacle->state != DT_OBSTACLE_EMPTY && Obstacle->state != DT_OBSTACLE_REMOVING) { ++Count; }
        }
        return Count;
    }

    bool NavMesh::Update()
    {
        if(!IsValid()) { return true; }
        bool bUpToDate = true;
        /* Several requests a call, so a door's tiles finish in a frame or two. */
        for(int Pass = 0; Pass < 8; ++Pass)
        {
            bool bDone = true;
            P->Cache->update(0.0f, P->Detour, &bDone);
            if(!bDone) { ++P->Revision; }
            bUpToDate = bDone;
            if(bDone) { break; }
        }
        return bUpToDate;
    }

    // === What it is ===========================================================

    const NavBuildSettings& NavMesh::GetSettings() const { return P->Settings; }
    GameCore::AABB          NavMesh::GetBounds() const   { return P->Bounds; }
    uint64_t                NavMesh::GetGeometryHash() const { return P->GeometryHash; }
    uint64_t                NavMesh::GetRevision() const { return P->Revision; }
    dtNavMesh*              NavMesh::GetDetourMesh() const { return P->Detour; }
    dtTileCache*            NavMesh::GetTileCache() const { return P->Cache; }

    NavBuildStats NavMesh::GetStats() const
    {
        NavBuildStats Stats;
        if(!IsValid()) { return Stats; }
        Stats.TilesX = P->TilesX;
        Stats.TilesZ = P->TilesZ;
        Stats.Tiles = P->NavTileCount();
        Stats.Polygons = P->PolygonCount();
        Stats.Triangles = P->InputTriangles;
        Stats.Milliseconds = P->LastBuildMs;
        Stats.Bytes = Save().size();
        return Stats;
    }
}
