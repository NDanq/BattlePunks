/*
 * NimbleEngineTerrainTests -- heightmap terrain without a renderer: sampling
 * on the shared triangulation, raycasts, the .nmterrain file both ways, the
 * brushes, chunk revisions and meshes, and the ground queries a game walks
 * on (SampleGround, RaycastTerrain, PhysicsRaycast's terrain fallback).
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Foliage.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/TerrainBrush.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/RenderCore/Components/TerrainComponent.h"
#include "Public/RenderCore/MeshAsset.h"

using namespace GameCore;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    bool Near(const double InA, const double InB, const double InTolerance = 1e-3)
    {
        return std::fabs(InA - InB) <= InTolerance;
    }

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.4f", InValue);
        return Buffer;
    }

    /* A deterministic bumpy field. */
    float Bumpy(const int32_t InX, const int32_t InZ)
    {
        return 120.0f * std::sin(float(InX) * 0.37f) + 80.0f * std::cos(float(InZ) * 0.23f) + float(InX * InZ % 7) * 3.0f;
    }

    void FillBumpy(TerrainData& InData)
    {
        std::lock_guard<std::mutex> Guard(InData.GetLock());
        const int32_t N = int32_t(InData.GetResolution());
        TerrainRect All;
        for(int32_t Z = 0; Z < N; ++Z)
        {
            for(int32_t X = 0; X < N; ++X)
            {
                InData.SetHeight(X, Z, Bumpy(X, Z));
                All.Add(X, Z);
            }
        }
        InData.MarkDirty(All);
    }

    void TestSampling()
    {
        TerrainData Data;
        Data.Reset(65, 50.0f, 25.0f);
        Expect(Data.GetResolution() == 65 && Near(Data.GetSize(), 3200.0f), "65 samples 50 apart span 3200");
        Expect(Data.GetChunksPerSide() == 1, "64 cells are one chunk a side");
        Expect(Near(Data.SampleHeight(1234.0f, 777.0f), 25.0f), "a flat terrain samples its height everywhere");
        Expect(Near(Data.SampleHeight(-500.0f, 99999.0f), 25.0f), "outside the grid: the edge's height");

        /* A plane is reproduced exactly by any triangulation, normal included. */
        {
            std::lock_guard<std::mutex> Guard(Data.GetLock());
            TerrainRect All;
            for(int32_t Z = 0; Z < 65; ++Z)
            {
                for(int32_t X = 0; X < 65; ++X)
                {
                    Data.SetHeight(X, Z, 0.5f * float(X) * 50.0f - 0.25f * float(Z) * 50.0f);
                    All.Add(X, Z);
                }
            }
            Data.MarkDirty(All);
        }
        Expect(Near(Data.SampleHeight(333.0f, 1111.0f), 0.5f * 333.0f - 0.25f * 1111.0f),
               "a sloped plane samples exactly: " + Num(Data.SampleHeight(333.0f, 1111.0f)));
        float Normal[3];
        Data.SampleNormal(333.0f, 1111.0f, Normal);
        const float Len = std::sqrt(0.5f * 0.5f + 1.0f + 0.25f * 0.25f);
        Expect(Near(Normal[0], -0.5f / Len) && Near(Normal[1], 1.0f / Len) && Near(Normal[2], 0.25f / Len),
               "the plane's normal is (-dh/dx, 1, -dh/dz) normalised");
        Expect(Near(Data.GetMinHeight(), 0.5f * 0.0f - 0.25f * 3200.0f) && Near(Data.GetMaxHeight(), 0.5f * 3200.0f),
               "MarkDirty refreshed the height range");

        /* One cell with only the far corner raised: the (X,Z)-(X+1,Z+1) diagonal decides who sees it. */
        TerrainData Cell;
        Cell.Reset(2, 100.0f, 0.0f);
        {
            std::lock_guard<std::mutex> Guard(Cell.GetLock());
            Cell.SetHeight(1, 1, 100.0f);
            TerrainRect R; R.Add(1, 1);
            Cell.MarkDirty(R);
        }
        /* Above the diagonal (z > x): triangle (0,0),(0,1),(1,1): h = fx * 100. */
        Expect(Near(Cell.SampleHeight(25.0f, 75.0f), 25.0f), "triangle (X,Z),(X,Z+1),(X+1,Z+1) interpolates: "
               + Num(Cell.SampleHeight(25.0f, 75.0f)));
        /* Below it (x > z): triangle (0,0),(1,1),(1,0): h = fz * 100. */
        Expect(Near(Cell.SampleHeight(75.0f, 25.0f), 25.0f), "triangle (X,Z),(X+1,Z+1),(X+1,Z) interpolates: "
               + Num(Cell.SampleHeight(75.0f, 25.0f)));
        Expect(Near(Cell.SampleHeight(50.0f, 50.0f), 50.0f), "on the diagonal both agree");
    }

    void TestRaycast()
    {
        TerrainData Data;
        Data.Reset(129, 25.0f);
        FillBumpy(Data);

        int Misses = 0, OffSurface = 0;
        for(int Each = 0; Each < 200; ++Each)
        {
            const float X = 13.0f + float(Each * 37 % 3150);
            const float Z = 7.0f + float(Each * 53 % 3150);
            const float Origin[3]    = { X, 5000.0f, Z };
            const float Direction[3] = { 0.0f, -1.0f, 0.0f };
            float Distance = 0.0f, Normal[3];
            if(!Data.Raycast(Origin, Direction, 10000.0f, Distance, Normal)) { ++Misses; continue; }
            if(!Near(5000.0f - Distance, Data.SampleHeight(X, Z), 0.05)) { ++OffSurface; }
        }
        Expect(Misses == 0, "every downward ray over the terrain hits it (" + std::to_string(Misses) + " missed)");
        Expect(OffSurface == 0, "downward hits land on SampleHeight (" + std::to_string(OffSurface) + " off)");

        /* Slanted rays aimed at a point on the surface walk the cells and hit at or before it, on the surface. */
        int SlantMisses = 0, SlantOff = 0, SlantLate = 0;
        for(int Each = 0; Each < 100; ++Each)
        {
            const float TX = 200.0f + float(Each * 31 % 2800);
            const float TZ = 150.0f + float(Each * 47 % 2900);
            const float Target[3]    = { TX, Data.SampleHeight(TX, TZ), TZ };
            const float Direction[3] = { 0.8f - float(Each % 7) * 0.25f, -0.3f - float(Each % 5) * 0.1f, 0.5f - float(Each % 3) * 0.4f };
            const float Back = 1500.0f;
            const float Origin[3] = { Target[0] - Direction[0] * Back, Target[1] - Direction[1] * Back, Target[2] - Direction[2] * Back };
            float Distance = 0.0f, Normal[3];
            if(!Data.Raycast(Origin, Direction, 1.0e6f, Distance, Normal)) { ++SlantMisses; continue; }
            if(Distance > Back + 0.05f) { ++SlantLate; }
            const float HX = Origin[0] + Direction[0] * Distance;
            const float HY = Origin[1] + Direction[1] * Distance;
            const float HZ = Origin[2] + Direction[2] * Distance;
            if(!Near(HY, Data.SampleHeight(HX, HZ), 0.1)) { ++SlantOff; }
        }
        Expect(SlantMisses == 0, "slanted rays at the surface hit it (" + std::to_string(SlantMisses) + " missed)");
        Expect(SlantLate == 0, "never past the point they were aimed at (" + std::to_string(SlantLate) + " late)");
        Expect(SlantOff == 0, "slanted hits lie on the surface (" + std::to_string(SlantOff) + " off)");

        const float Up[3] = { 0.0f, 1.0f, 0.0f };
        const float Above[3] = { 100.0f, 5000.0f, 100.0f };
        float Distance = 0.0f, Normal[3];
        Expect(!Data.Raycast(Above, Up, 1.0e6f, Distance, Normal), "a ray pointing away misses");
        const float Down[3] = { 0.0f, -1.0f, 0.0f };
        Expect(!Data.Raycast(Above, Down, 100.0f, Distance, Normal), "a ray too short misses");
        const float Outside[3] = { -100.0f, 5000.0f, 100.0f };
        Expect(!Data.Raycast(Outside, Down, 1.0e6f, Distance, Normal), "a ray beside the terrain misses");
    }

    void TestFile()
    {
        std::error_code Error;
        const std::filesystem::path Folder = std::filesystem::temp_directory_path(Error) / "NimbleTerrainTests";
        std::filesystem::remove_all(Folder, Error);
        const std::string Path = (Folder / "Levels" / "Hills" / "Terrain" / "Ground.nmterrain").string();

        TerrainData Data;
        Data.Reset(129, 100.0f);
        FillBumpy(Data);
        {
            std::lock_guard<std::mutex> Guard(Data.GetLock());
            uint8_t* W = Data.GetWeights();
            W[(5 * 129 + 7) * TERRAIN_LAYERS + 0] = 55;
            W[(5 * 129 + 7) * TERRAIN_LAYERS + 2] = 200;
        }
        Data.LayerNames[0] = "Grass";
        Data.LayerNames[3] = "Snow";
        Expect(SaveTerrainFile(Path, Data), "SaveTerrainFile writes " + Path);
        Expect(!std::filesystem::exists(Path + ".tmp"), "no temporary left behind");

        TerrainData Loaded;
        Expect(LoadTerrainFile(Path, Loaded), "LoadTerrainFile reads it back");
        Expect(Loaded.GetResolution() == 129 && Near(Loaded.GetSpacing(), 100.0f), "resolution and spacing round-trip");
        const float Step = (Data.GetMaxHeight() - Data.GetMinHeight()) / 65535.0f;
        double Worst = 0.0;
        for(int32_t Z = 0; Z < 129; ++Z)
        {
            for(int32_t X = 0; X < 129; ++X) { Worst = std::max(Worst, double(std::fabs(Loaded.GetHeight(X, Z) - Data.GetHeight(X, Z)))); }
        }
        Expect(Worst <= Step * 0.51 + 1e-4, "heights round-trip within half a 16-bit step (" + Num(Worst) + " <= " + Num(Step * 0.5) + ")");
        const uint8_t* W = Loaded.GetWeight(7, 5);
        Expect(W[0] == 55 && W[2] == 200, "paint weights round-trip exactly");
        Expect(Loaded.LayerNames[0] == "Grass" && Loaded.LayerNames[3] == "Snow" && Loaded.LayerNames[1].empty(), "layer names round-trip");

        /* Garbage is refused, leaving a usable (tiny) terrain. */
        const std::string Bad = (Folder / "Bad.nmterrain").string();
        if(FILE* File = std::fopen(Bad.c_str(), "wb")) { std::fputs("not a terrain at all", File); std::fclose(File); }
        TerrainData Garbage;
        Expect(!LoadTerrainFile(Bad, Garbage), "a file that is not a terrain does not load");

        /* The cache: loaded once, shared, Store replaces, Invalidate forgets. */
        std::shared_ptr<TerrainData> A = TerrainAssetCache::Find(Path);
        std::shared_ptr<TerrainData> B = TerrainAssetCache::Find(Path);
        Expect(A && A == B, "TerrainAssetCache shares one copy");
        auto Made = std::make_shared<TerrainData>();
        Made->Reset(3, 10.0f);
        const uint64_t Generation = TerrainAssetCache::GetGeneration();
        TerrainAssetCache::Store(Path, Made);
        Expect(TerrainAssetCache::Find(Path) == Made && TerrainAssetCache::GetGeneration() != Generation, "Store replaces and bumps the generation");
        TerrainAssetCache::Invalidate(Path);
        Expect(TerrainAssetCache::FindLoaded(Path) == nullptr, "Invalidate forgets");
        std::filesystem::remove_all(Folder, Error);
    }

    void TestBrushes()
    {
        TerrainData Data;
        Data.Reset(257, 100.0f);
        Expect(Data.GetChunksPerSide() == 4, "256 cells are four chunks a side");
        uint64_t Before[16];
        for(uint32_t Z = 0; Z < 4; ++Z) { for(uint32_t X = 0; X < 4; ++X) { Before[Z * 4 + X] = Data.GetChunkRevision(X, Z); } }

        TerrainBrush Brush;
        Brush.X = 3000.0f; Brush.Z = 3000.0f; Brush.Radius = 800.0f; Brush.Strength = 1.0f;
        TerrainBrushOp Raise;
        Raise.Tool = TT_Raise; Raise.Rate = 600.0f; Raise.DeltaSeconds = 0.5f;
        const TerrainRect Changed = ApplyTerrainBrush(Data, Brush, Raise);
        Expect(!Changed.IsEmpty(), "a raise dab changes samples");
        Expect(Near(Data.GetHeight(30, 30), 300.0f), "the centre rises Rate * Strength * dt: " + Num(Data.GetHeight(30, 30)));
        Expect(Data.GetHeight(30, 39) == 0.0f && Data.GetHeight(39, 30) == 0.0f, "samples at or past the radius do not move");
        Expect(Data.GetHeight(30, 34) > 0.0f && Data.GetHeight(30, 34) < 300.0f, "falloff: partway out rises partway");

        int Bumped = 0, Wrong = 0;
        for(uint32_t Z = 0; Z < 4; ++Z)
        {
            for(uint32_t X = 0; X < 4; ++X)
            {
                const bool bMoved = Data.GetChunkRevision(X, Z) != Before[Z * 4 + X];
                const bool bShould = X == 0 && Z == 0;   // samples 22..38 lie in chunk 0 (0..64)
                Bumped += bMoved ? 1 : 0;
                Wrong  += bMoved != bShould ? 1 : 0;
            }
        }
        Expect(Bumped == 1 && Wrong == 0, "only the chunk under the brush is marked (" + std::to_string(Bumped) + " bumped)");

        /* A dab on a chunk edge marks both neighbours (their meshes share the edge samples and normals). */
        for(uint32_t Z = 0; Z < 4; ++Z) { for(uint32_t X = 0; X < 4; ++X) { Before[Z * 4 + X] = Data.GetChunkRevision(X, Z); } }
        Brush.X = 6400.0f; Brush.Z = 1000.0f; Brush.Radius = 150.0f;
        ApplyTerrainBrush(Data, Brush, Raise);
        Expect(Data.GetChunkRevision(0, 0) != Before[0] && Data.GetChunkRevision(1, 0) != Before[1],
               "a dab on the edge at X = 64 marks chunks 0 and 1");

        /* Flatten eases toward the target; Set Height gets there. */
        TerrainBrushOp Flatten;
        Flatten.Tool = TT_Flatten; Flatten.TargetHeight = 100.0f; Flatten.DeltaSeconds = 1.0f;
        Brush.X = 3000.0f; Brush.Z = 3000.0f; Brush.Radius = 800.0f;
        ApplyTerrainBrush(Data, Brush, Flatten);
        Expect(Near(Data.GetHeight(30, 30), 100.0f, 1.0), "Flatten at full strength for a second reaches the target: " + Num(Data.GetHeight(30, 30)));

        /* Smooth lowers a spike. */
        {
            std::lock_guard<std::mutex> Guard(Data.GetLock());
            Data.SetHeight(100, 100, 1000.0f);
            TerrainRect R; R.Add(100, 100);
            Data.MarkDirty(R);
        }
        TerrainBrushOp Smooth;
        Smooth.Tool = TT_Smooth; Smooth.DeltaSeconds = 0.2f;
        Brush.X = 10000.0f; Brush.Z = 10000.0f; Brush.Radius = 300.0f;
        ApplyTerrainBrush(Data, Brush, Smooth);
        Expect(Data.GetHeight(100, 100) < 700.0f && Data.GetHeight(101, 100) > 0.0f, "Smooth spreads a spike: "
               + Num(Data.GetHeight(100, 100)));

        /* Paint keeps weights summing to 255, and moves toward the layer. */
        TerrainBrushOp Paint;
        Paint.Tool = TT_Paint; Paint.Layer = 2; Paint.DeltaSeconds = 0.1f;
        Brush.X = 5000.0f; Brush.Z = 5000.0f; Brush.Radius = 500.0f; Brush.Strength = 0.7f;
        for(int Dab = 0; Dab < 5; ++Dab) { ApplyTerrainBrush(Data, Brush, Paint); }
        int BadSums = 0;
        for(int32_t Z = 40; Z <= 60; ++Z)
        {
            for(int32_t X = 40; X <= 60; ++X)
            {
                const uint8_t* W = Data.GetWeight(X, Z);
                if(int(W[0]) + W[1] + W[2] + W[3] != 255) { ++BadSums; }
            }
        }
        Expect(BadSums == 0, "painted weights still sum to 255 (" + std::to_string(BadSums) + " do not)");
        Expect(Data.GetWeight(50, 50)[2] > 200 && Data.GetWeight(50, 50)[0] < 55, "the centre is mostly layer 2 now");
        Expect(Data.GetWeight(56, 50)[2] == 0, "outside the brush is untouched");

        Expect(FindTerrainTool("Smooth") == TT_Smooth && std::string(GetTerrainToolName(TT_Paint)) == "Paint", "tool names round-trip");
        Expect(FindTerrainFalloff("Tip") == TF_Tip && FindTerrainFalloff("nope") == TF_Count, "falloff names round-trip");
        TerrainBrush Edge;
        Edge.Radius = 100.0f;
        int Monotonic = 1;
        for(uint8_t F = 0; F < TF_Count; ++F)
        {
            Edge.Falloff = F;
            Monotonic &= Near(GetTerrainBrushWeight(Edge, 0.0f), 1.0f) && GetTerrainBrushWeight(Edge, 100.0f) == 0.0f
                         && GetTerrainBrushWeight(Edge, 30.0f) >= GetTerrainBrushWeight(Edge, 60.0f) ? 1 : 0;
        }
        Expect(Monotonic != 0, "every falloff is 1 at the centre, 0 at the radius, and falls");
    }

    void TestChunkMesh()
    {
        TerrainData Data;
        Data.Reset(129, 50.0f);
        FillBumpy(Data);
        RenderCore::MeshData Mesh;
        Expect(RenderCore::BuildTerrainChunkMesh(Data, 1, 0, 200.0f, Mesh), "chunk (1, 0) builds");
        Expect(Mesh.GetVertexCount() == 65 * 65 && Mesh.Indices.size() == size_t(64) * 64 * 6, "65 x 65 vertices, 64 x 64 cells");
        Expect(Mesh.Colors.size() == size_t(65) * 65 * 4 && Mesh.Sections.size() == 1, "colours carry the weights; one section");
        /* First vertex is sample (64, 0). */
        Expect(Near(Mesh.Positions[0], 64.0f * 50.0f) && Near(Mesh.Positions[1], Data.GetHeight(64, 0)) && Near(Mesh.Positions[2], 0.0f),
               "vertices sit on the samples, in terrain space");
        Expect(Near(Mesh.UV0[0], 64.0f * 50.0f / 200.0f), "UV0 tiles every UVTileSize units");
        /* First cell's triangles split along (X,Z)-(X+1,Z+1). */
        Expect(Mesh.Indices[0] == 0 && Mesh.Indices[1] == 65 && Mesh.Indices[2] == 66
               && Mesh.Indices[3] == 0 && Mesh.Indices[4] == 66 && Mesh.Indices[5] == 1, "cells split along the shared diagonal");
        Expect(!RenderCore::BuildTerrainChunkMesh(Data, 2, 0, 200.0f, Mesh), "chunk out of range does not build");

        /* Coarser LODs take every 2nd / 4th sample; skirts add a dropped copy of each edge. */
        RenderCore::MeshData Lod2;
        Expect(RenderCore::BuildTerrainChunkMesh(Data, 1, 0, 200.0f, Lod2, 2) && Lod2.GetVertexCount() == 17 * 17
               && Lod2.Indices.size() == size_t(16) * 16 * 6, "LOD 2: 17 x 17 vertices");
        Expect(Near(Lod2.Positions[3], 68.0f * 50.0f) && Near(Lod2.Positions[4], Data.GetHeight(68, 0)), "on every fourth sample");
        RenderCore::MeshData Skirted;
        Expect(RenderCore::BuildTerrainChunkMesh(Data, 1, 0, 200.0f, Skirted, 1, true, 200.0f)
               && Skirted.GetVertexCount() == 33 * 33 + 4 * 33 && Skirted.Indices.size() == size_t(32) * 32 * 6 + 4 * 32 * 6,
               "skirts: four edges of 33 more vertices, 32 quads each");
        Expect(Near(Skirted.BoundsMin[1], std::min(Data.GetMinHeight(), Mesh.BoundsMin[1]) - 200.0f, 400.0f), "hanging below the edge");
        Expect(RenderCore::GetTerrainChunkMeshPath(EntityID(3, 1), 2, 5, 2) != RenderCore::GetTerrainChunkMeshPath(EntityID(3, 1), 2, 5),
               "each LOD has its own path");
        Expect(RenderCore::GetTerrainChunkMeshPath(EntityID(3, 1), 2, 5).rfind("@terrain/", 0) == 0, "chunk meshes have virtual paths");

        /* MeshAssetCache::Store: generated meshes are found, survive Invalidate, and go with Remove. */
        auto Generated = std::make_shared<RenderCore::MeshData>(Mesh);
        RenderCore::MeshAssetCache::Store("@terrain/test/0_0", Generated);
        Expect(RenderCore::MeshAssetCache::Find("@terrain/test/0_0") == Generated, "a stored mesh is found");
        RenderCore::MeshAssetCache::Invalidate("");
        Expect(RenderCore::MeshAssetCache::Find("@terrain/test/0_0") == Generated, "Invalidate leaves stored meshes");
        RenderCore::MeshAssetCache::Remove("@terrain/test/0_0");
    }

    void TestGround()
    {
        World TheWorld;
        auto Data = std::make_shared<TerrainData>();
        Data->Reset(65, 100.0f);
        {
            std::lock_guard<std::mutex> Guard(Data->GetLock());
            TerrainRect All;
            for(int32_t Z = 0; Z < 65; ++Z)
            {
                for(int32_t X = 0; X < 65; ++X) { Data->SetHeight(X, Z, 2.0f * float(X)); All.Add(X, Z); }
            }
            Data->MarkDirty(All);
        }
        TerrainAssetCache::Store("Content/Levels/Test/Terrain/Ground.nmterrain", Data);

        Entity Ground = TheWorld.CreateEntity("Ground");
        TransformComponent T;
        T.Position = EngineCore::Vector3(-1000.0, 50.0, 200.0);
        T.Scale    = EngineCore::Vector3(1.0, 2.0, 1.0);
        Ground.AddComponent(T);
        RenderCore::TerrainComponent C;
        std::snprintf(C.Terrain, sizeof(C.Terrain), "%s", "Content/Levels/Test/Terrain/Ground.nmterrain");
        Ground.AddComponent(C);

        /* World (x, z) -> local (x + 1000, z - 200); height 2 * localX / 100, scaled by 2, raised 50. */
        float Height = 0.0f;
        EngineCore::Vector3 Normal;
        Expect(NimbleEngine::SampleGround(TheWorld, EngineCore::Vector3(500.0, 0.0, 1000.0), Height, &Normal),
               "SampleGround finds the terrain under a point");
        const double Expected = 50.0 + 2.0 * (2.0 * (500.0 + 1000.0) / 100.0);
        Expect(Near(Height, Expected, 0.01), "through the Transform's offset and scale: " + Num(Height) + " vs " + Num(Expected));
        Expect(Normal.GetY() > 0.99 && Normal.GetX() < 0.0, "the normal leans away from the uphill side");
        Expect(!NimbleEngine::SampleGround(TheWorld, EngineCore::Vector3(-2000.0, 0.0, 1000.0), Height),
               "nothing beside the terrain");
        Expect(NimbleEngine::SampleGround(TheWorld, EngineCore::Vector3(500.0, -400.0, 1000.0), Height) && Near(Height, Expected, 0.01),
               "a point under the ground is still found from above");

        NimbleEngine::TerrainRaycastHit Hit;
        Expect(NimbleEngine::RaycastTerrain(TheWorld, EngineCore::Vector3(0.0, 1000.0, 1000.0), EngineCore::Vector3(0.0, -3.0, 0.0),
                                            5000.0f, Hit)
               && Hit.Entity == Ground.GetID() && Near(Hit.Distance, 1000.0 - (50.0 + 2.0 * 2.0 * 1000.0 / 100.0), 0.01),
               "RaycastTerrain: distance in world units for any direction length");

        NimbleEngine::PhysicsRaycastHit PHit;
        const bool bPhysics = NimbleEngine::PhysicsRaycast(TheWorld, EngineCore::Vector3(0.0, 1000.0, 1000.0),
                                                           EngineCore::Vector3(0.0, -1.0, 0.0), 5000.0f, PHit);
        if(!NimbleEngine::IsWorldSimulated(TheWorld))
        {
            Expect(bPhysics && PHit.Entity == Ground.GetID() && PHit.Normal.GetY() > 0.9,
                   "PhysicsRaycast without physics hits walkable terrain, with a normal");
        }

        Ground.GetComponent<RenderCore::TerrainComponent>()->bWalkable = 0;
        Expect(!NimbleEngine::SampleGround(TheWorld, EngineCore::Vector3(500.0, 0.0, 1000.0), Height), "not walkable: not ground");

        /* The provider hands physics the same data. */
        const TerrainProvider* Provider = GetTerrainProvider();
        Expect(Provider && Provider->FindTerrain && Provider->FindTerrain(TheWorld, Ground.GetID(), Provider->UserData) == Data,
               "the TerrainProvider finds an entity's terrain for physics");
        TerrainAssetCache::Invalidate("Content/Levels/Test/Terrain/Ground.nmterrain");
    }

    void TestWalking()
    {
        /* Flat for X < 3000, then a cliff rising 3 for every 1 (72 degrees) to 3000 high, then flat again. */
        World TheWorld;
        auto Data = std::make_shared<TerrainData>();
        Data->Reset(65, 100.0f);
        {
            std::lock_guard<std::mutex> Guard(Data->GetLock());
            TerrainRect All;
            for(int32_t Z = 0; Z < 65; ++Z)
            {
                for(int32_t X = 0; X < 65; ++X)
                {
                    const float Rise = std::clamp(float(X - 30) * 300.0f, 0.0f, 3000.0f);
                    Data->SetHeight(X, Z, Rise + float(X) * 2.0f);   // and a gentle 1-in-50 everywhere
                    All.Add(X, Z);
                }
            }
            Data->MarkDirty(All);
        }
        const char* Path = "Content/Levels/Test/Terrain/Cliff.nmterrain";
        TerrainAssetCache::Store(Path, Data);
        Entity Ground = TheWorld.CreateEntity("Cliff");
        Ground.AddComponent(TransformComponent());
        RenderCore::TerrainComponent C;
        std::snprintf(C.Terrain, sizeof(C.Terrain), "%s", Path);
        Ground.AddComponent(C);

        NimbleEngine::GroundMoveSettings Walk;
        Walk.RideHeight = 170.0f;

        /* Up the gentle slope: the eye follows the ground. */
        EngineCore::Vector3 To(1500.0, 0.0, 3200.0);
        Expect(NimbleEngine::MoveOnGround(TheWorld, EngineCore::Vector3(1400.0, 170.0, 3200.0), To, Walk)
               && Near(To.GetX(), 1500.0) && Near(To.GetY(), 30.0 + 170.0, 0.01), "a walk on gentle ground rides 170 above it: y " + Num(To.GetY()));

        /* Into the cliff: stopped at its foot. */
        EngineCore::Vector3 At(2950.0, 230.0, 3200.0);
        int Steps = 0;
        for(; Steps < 40; ++Steps)
        {
            EngineCore::Vector3 Next(At.GetX() + 20.0, At.GetY(), At.GetZ());
            NimbleEngine::MoveOnGround(TheWorld, At, Next, Walk);
            At = Next;
        }
        Expect(At.GetX() < 3060.0 && At.GetY() < 170.0 + 300.0, "the cliff stops a walk up it: x " + Num(At.GetX()) + " y " + Num(At.GetY()));

        /* A diagonal into it slides along its foot instead. */
        EngineCore::Vector3 Slide(3020.0, 0.0, 3300.0);
        NimbleEngine::MoveOnGround(TheWorld, EngineCore::Vector3(3000.0, 230.0, 3200.0), Slide, Walk);
        Expect(Slide.GetZ() > 3250.0 && Slide.GetX() <= 3020.0, "a diagonal into the cliff slides along it: z " + Num(Slide.GetZ()));

        /* Down it is always allowed. */
        EngineCore::Vector3 Down(3000.0, 0.0, 3200.0);
        Expect(NimbleEngine::MoveOnGround(TheWorld, EngineCore::Vector3(4100.0, 3300.0, 3200.0), Down, Walk) && Near(Down.GetX(), 3000.0),
               "stepping off the top is never blocked");

        /* A low ledge is a step: a 1-sample rise of 30 at a steep angle. */
        NimbleEngine::GroundMoveSettings Steep = Walk;
        Steep.MaxSlopeDegrees = 5.0f;
        EngineCore::Vector3 Small(1510.0, 0.0, 3200.0);
        Expect(NimbleEngine::MoveOnGround(TheWorld, EngineCore::Vector3(1500.0, 200.0, 3200.0), Small, Steep) && Near(Small.GetX(), 1510.0),
               "a rise under StepHeight is walked up whatever its slope");

        EngineCore::Vector3 Off(9000.0, 50.0, 3200.0);
        Expect(!NimbleEngine::MoveOnGround(TheWorld, EngineCore::Vector3(6300.0, 50.0, 3200.0), Off, Walk) && Near(Off.GetX(), 9000.0)
               && Near(Off.GetY(), 50.0), "off the terrain: false, the move left alone");
        TerrainAssetCache::Invalidate(Path);
    }
}

namespace
{
    void TestFoliage()
    {
        TerrainData Ground;
        Ground.Reset(129, 100.0f);   // 2 x 2 chunks, 12800 a side
        {
            /* A cliff along X = 6400..7000 (steep), flat elsewhere. */
            std::lock_guard<std::mutex> Guard(Ground.GetLock());
            TerrainRect All;
            for(int32_t Z = 0; Z < 129; ++Z)
            {
                for(int32_t X = 0; X < 129; ++X)
                {
                    Ground.SetHeight(X, Z, std::clamp(float(X - 64) * 250.0f, 0.0f, 1500.0f));
                    All.Add(X, Z);
                }
            }
            Ground.MarkDirty(All);
        }

        FoliageData Foliage;
        Foliage.Reset(Ground);
        Expect(Foliage.GetBucketsPerSide() == 2 && Near(Foliage.GetBucketSize(), 6400.0f), "a bucket per terrain chunk");
        FoliageType Grass;
        Grass.Name = "Grass"; Grass.Mesh[0] = "Content/Meshes/Grass.nmmesh"; Grass.Spacing = 200.0f; Grass.MaxSlope = 30.0f;
        FoliageType Tree;
        Tree.Name = "Tree"; Tree.Mesh[0] = "Content/Meshes/Tree.nmmesh"; Tree.Spacing = 800.0f; Tree.AlignToNormal = 0.0f;
        Tree.LODDensity[1] = 1.0f; Tree.LODDensity[2] = 1.0f; Tree.CullDistance = 30000.0f;
        Foliage.GetTypes() = { Grass, Tree };

        /* Paint grass for two seconds on the flat. */
        TerrainBrush Brush;
        Brush.X = 3000.0f; Brush.Z = 3000.0f; Brush.Radius = 2000.0f; Brush.Strength = 1.0f;
        FoliageBrushOp Paint;
        Paint.Tool = FT_Paint; Paint.Types = { 0 }; Paint.DeltaSeconds = 1.0f / 30.0f;
        const uint64_t Before   = Foliage.GetBucketRevision(0, 0);
        const uint64_t Before11 = Foliage.GetBucketRevision(1, 1);
        uint32_t Added = 0;
        for(int Dab = 0; Dab < 60; ++Dab) { Added += ApplyFoliageBrush(Foliage, Ground, Brush, Paint).Added; }
        const size_t Grasses = Foliage.GetInstanceCount(0);
        const double Ideal = 3.14159 * 2000.0 * 2000.0 / (200.0 * 200.0);
        Expect(Added == Grasses && Grasses > size_t(Ideal * 0.25) && Grasses < size_t(Ideal * 1.2),
               "painting fills toward the spacing: " + std::to_string(Grasses) + " of ~" + std::to_string(int(Ideal)));
        Expect(Foliage.GetInstanceCount(1) == 0, "only the chosen type");
        Expect(Foliage.GetBucketRevision(0, 0) != Before && Foliage.GetBucketRevision(1, 1) == Before11,
               "the touched bucket's revision moves, an untouched one's does not");

        /* None closer than the spacing, none outside the brush. */
        double Closest = 1e9, Farthest = 0.0;
        const std::vector<FoliageInstance>& Bucket = Foliage.GetBucket(0, 0);
        for(size_t A = 0; A < Bucket.size(); ++A)
        {
            Farthest = std::max(Farthest, std::hypot(double(Bucket[A].X) - 3000.0, double(Bucket[A].Z) - 3000.0));
            for(size_t B = A + 1; B < Bucket.size(); ++B)
            {
                Closest = std::min(Closest, std::hypot(double(Bucket[A].X - Bucket[B].X), double(Bucket[A].Z - Bucket[B].Z)));
            }
        }
        Expect(Closest >= 200.0 - 1e-3 && Farthest <= 2000.0, "spacing kept (" + Num(Closest) + "), inside the brush (" + Num(Farthest) + ")");

        /* The cliff is too steep for grass; flat ground beyond it is not. */
        Brush.X = 6700.0f; Brush.Radius = 250.0f;
        for(int Dab = 0; Dab < 60; ++Dab) { ApplyFoliageBrush(Foliage, Ground, Brush, Paint); }
        int OnCliff = 0;
        for(const FoliageInstance& Each : Foliage.GetBucket(1, 0)) { OnCliff += (Each.X > 6450.0f && Each.X < 6950.0f) ? 1 : 0; }
        Expect(OnCliff == 0, "nothing on ground steeper than MaxSlope (" + std::to_string(OnCliff) + ")");

        /* Erase takes the chosen types out from under the brush. */
        FoliageBrushOp Erase;
        Erase.Tool = FT_Erase; Erase.DeltaSeconds = 1.0f / 30.0f;
        Brush.X = 3000.0f; Brush.Radius = 800.0f;
        uint32_t Removed = 0;
        for(int Dab = 0; Dab < 120; ++Dab) { Removed += ApplyFoliageBrush(Foliage, Ground, Brush, Erase).Removed; }
        int Left = 0;
        for(const FoliageInstance& Each : Foliage.GetBucket(0, 0)) { Left += std::hypot(Each.X - 3000.0f, Each.Z - 3000.0f) < 400.0f ? 1 : 0; }
        Expect(Removed > 0 && Left == 0, "erase clears the brush's middle (" + std::to_string(Removed) + " removed)");

        /* Transforms stand on the ground, upright or along the normal, sunk, scaled. */
        FoliageInstance One;
        One.Type = 1; One.X = 6500.0f; One.Z = 1000.0f; One.Yaw = 90.0f; One.Scale = 2.0f;
        float M[16];
        Tree.Sink = 10.0f;
        GetFoliageTransform(Ground, Tree, One, M);
        Expect(Near(M[12], 6500.0) && Near(M[13], Ground.SampleHeight(6500.0f, 1000.0f) - 20.0, 0.01) && Near(M[14], 1000.0),
               "upright: at the ground, sunk Sink x Scale: y " + Num(M[13]));
        Expect(Near(M[5], 2.0) && Near(std::hypot(M[0], M[2]), 2.0) && Near(M[1], 0.0) && Near(M[15], 1.0), "scaled, yawed about Y only");
        Tree.AlignToNormal = 1.0f;
        GetFoliageTransform(Ground, Tree, One, M);
        float Normal[3];
        Ground.SampleNormal(6500.0f, 1000.0f, Normal);
        Expect(Near(M[4] / 2.0, Normal[0], 1e-3) && Near(M[5] / 2.0, Normal[1], 1e-3), "aligned: its up is the ground's normal");

        /* It follows the ground when sculpted. */
        {
            std::lock_guard<std::mutex> Guard(Ground.GetLock());
            for(int32_t Z = 0; Z < 129; ++Z) { for(int32_t X = 0; X < 129; ++X) { Ground.SetHeight(X, Z, Ground.GetHeight(X, Z) + 300.0f); } }
        }
        Tree.AlignToNormal = 0.0f;
        Tree.Sink = 0.0f;
        GetFoliageTransform(Ground, Tree, One, M);
        Expect(Near(M[13], Ground.SampleHeight(6500.0f, 1000.0f), 0.01), "raised ground raises it");

        /* The file both ways; out-of-grid instances dropped on load. */
        std::error_code Error;
        const std::filesystem::path Folder = std::filesystem::temp_directory_path(Error) / "NimbleFoliageTests";
        std::filesystem::remove_all(Folder, Error);
        std::filesystem::create_directories(Folder, Error);
        const std::string Path = (Folder / "Ground.nmfoliage").string();
        Foliage.GetTypes()[1].CullDistance = 12345.0f;
        Expect(SaveFoliageFile(Path, Foliage), "SaveFoliageFile writes it");
        FoliageData Loaded;
        Expect(LoadFoliageFile(Path, Loaded), "LoadFoliageFile reads it");
        Expect(Loaded.GetTypes().size() == 2 && Loaded.GetTypes()[0].Name == "Grass" && Loaded.GetTypes()[1].Mesh[0] == "Content/Meshes/Tree.nmmesh"
               && Near(Loaded.GetTypes()[1].CullDistance, 12345.0) && Near(Loaded.GetTypes()[1].LODDensity[2], 1.0),
               "types round-trip");
        Expect(Loaded.GetInstanceCount() == Foliage.GetInstanceCount() && Loaded.GetBucket(0, 0).size() == Foliage.GetBucket(0, 0).size(),
               "instances round-trip into the same buckets");
        const FoliageInstance& A = Foliage.GetBucket(0, 0).front();
        const FoliageInstance& B = Loaded.GetBucket(0, 0).front();
        Expect(A.X == B.X && A.Z == B.Z && A.Yaw == B.Yaw && A.Scale == B.Scale && A.Seed == B.Seed && A.Type == B.Type, "exactly");

        /* RemoveType renumbers. */
        Loaded.RemoveType(0);
        Expect(Loaded.GetTypes().size() == 1 && Loaded.GetTypes()[0].Name == "Tree" && Loaded.GetInstanceCount() == Loaded.GetInstanceCount(0),
               "removing a type removes its instances and renumbers the rest");
        std::filesystem::remove_all(Folder, Error);

        /* Thinning is stable: a fraction from the seed alone. */
        Expect(FoliageSeedFraction(12345u) == FoliageSeedFraction(12345u) && FoliageSeedFraction(1u) != FoliageSeedFraction(2u)
               && FoliageSeedFraction(0xFFFFFFFFu) < 1.0f, "seed fractions are stable, in [0, 1)");
    }
}

int main()
{
    std::printf("NimbleEngineTerrainTests\n");
    TestSampling();
    TestRaycast();
    TestFile();
    TestBrushes();
    TestChunkMesh();
    TestGround();
    TestWalking();
    TestFoliage();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
