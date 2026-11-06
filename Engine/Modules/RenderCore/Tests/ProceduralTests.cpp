/*
 * RenderCoreProceduralTests -- splines and the Procedural Spawner's placement
 * without a window (PROCEDURAL.md): GameCore's SplineCurve and Spline
 * component, then GenerateProcedural over hand-made terrains and meshes.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/Spline.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/ProceduralGeneration.h"
#include "Private/RenderCore/ProceduralSystem.h"

using namespace RenderCore;
using EngineCore::Vector3;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static bool Near(const float A, const float B, const float Tolerance = 1e-2f) { return std::fabs(A - B) < Tolerance; }

static bool NearV(const Vector3& A, const Vector3& B, const float Tolerance = 1e-2f)
{
    return Near(float(A.GetX()), float(B.GetX()), Tolerance) && Near(float(A.GetY()), float(B.GetY()), Tolerance)
        && Near(float(A.GetZ()), float(B.GetZ()), Tolerance);
}

static EngineCore::Matrix4x4 Translation(const float X, const float Y, const float Z)
{
    EngineCore::Matrix4x4 M;
    M.InitTranslation(Vector3(X, Y, Z));
    return M;
}

/* Every instance of a result, flattened. */
static std::vector<RenderInstance> AllInstances(const ProceduralResult& InResult)
{
    std::vector<RenderInstance> Out;
    for(const ProceduralCell& Cell : InResult.Cells)
    {
        for(const ProceduralBatch& Batch : Cell.Batches) { Out.insert(Out.end(), Batch.Instances.begin(), Batch.Instances.end()); }
    }
    return Out;
}

/* Inputs with one 100-unit box mesh in entry 0, and a neat grid (Jitter 0). */
static ProceduralInputs BaseInputs()
{
    ProceduralInputs In;
    In.Settings.Source  = PGS_Box;
    In.Settings.Project = PGP_None;
    In.Settings.Spacing = 100.0f;
    In.Settings.Jitter  = 0.0f;
    In.Settings.BoxExtent = Vector3(500.0f, 500.0f, 500.0f);
    In.Settings.ScaleMin  = 1.0f;
    In.Settings.ScaleMax  = 1.0f;
    std::strcpy(In.Settings.Meshes[0].Mesh, "Test/Box.nmmesh");
    In.Entries[0].bValid = true;
    In.Entries[0].BoundsMin[0] = -50.0f; In.Entries[0].BoundsMin[1] = 0.0f;   In.Entries[0].BoundsMin[2] = -50.0f;
    In.Entries[0].BoundsMax[0] =  50.0f; In.Entries[0].BoundsMax[1] = 100.0f; In.Entries[0].BoundsMax[2] =  50.0f;
    return In;
}

/* A 6400-unit terrain, 65 samples a side, rising 0.25 per unit along X; layer 1 painted where X > 3200. */
static std::shared_ptr<GameCore::TerrainData> MakeSlope()
{
    auto Data = std::make_shared<GameCore::TerrainData>();
    Data->Reset(65, 100.0f, 0.0f);
    GameCore::TerrainRect All;
    for(int32_t Z = 0; Z < 65; ++Z)
    {
        for(int32_t X = 0; X < 65; ++X)
        {
            Data->SetHeight(X, Z, float(X) * 100.0f * 0.25f);
            uint8_t* Weight = Data->GetWeights() + (size_t(Z) * 65 + size_t(X)) * GameCore::TERRAIN_LAYERS;
            const bool bEast = X > 32;
            Weight[0] = bEast ? 0 : 255;
            Weight[1] = bEast ? 255 : 0;
            Weight[2] = Weight[3] = 0;
            All.Add(X, Z);
        }
    }
    Data->MarkDirty(All);
    return Data;
}

/* A flat quad Size a side at height InY, facing up, centred on the origin. */
static std::shared_ptr<MeshData> MakeQuad(const float InSize, const float InY, const bool bInWall = false)
{
    auto Mesh = std::make_shared<MeshData>();
    const float H = 0.5f * InSize;
    if(bInWall)
    {
        Mesh->Positions = { -H, 0.0f, 0.0f,   H, 0.0f, 0.0f,   H, InSize, 0.0f,   -H, InSize, 0.0f };
        Mesh->Normals   = { 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f };
    }
    else
    {
        Mesh->Positions = { -H, InY, -H,   H, InY, -H,   H, InY, H,   -H, InY, H };
        Mesh->Normals   = { 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f };
    }
    Mesh->Indices = { 0, 2, 1,   0, 3, 2 };
    ComputeMeshBounds(*Mesh);
    return Mesh;
}

// === Splines ====================================================================

static void TestSplineCurve()
{
    std::printf("SplineCurve\n");
    const Vector3 Line[2] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 0.0f) };
    const GameCore::SplineCurve Straight(Line, 2, false, GameCore::ST_Curve);
    Expect(Near(Straight.GetLength(), 1000.0f, 0.5f), "a two-point curve is as long as its chord");
    const GameCore::SplineSample Quarter = Straight.SampleAtDistance(250.0f);
    Expect(NearV(Quarter.Position, Vector3(250.0f, 0.0f, 0.0f), 0.5f), "sampled by distance, a quarter of the way is a quarter of the way");
    Expect(NearV(Quarter.Tangent, Vector3(1.0f, 0.0f, 0.0f), 1e-3f), "its tangent runs along it");
    Expect(NearV(Straight.SampleAtDistance(5000.0f).Position, Line[1], 0.5f), "past the end, an open curve clamps to its end");

    const Vector3 Bend[4] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(100.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 300.0f), Vector3(1100.0f, 50.0f, 900.0f) };
    const GameCore::SplineCurve Curve(Bend, 4, false, GameCore::ST_Curve);
    bool bThrough = true, bRising = true;
    for(uint32_t Index = 0; Index < 4; ++Index)
    {
        const uint32_t Segment = Index < 3 ? Index : 2;
        const Vector3  At      = Curve.Evaluate(Segment, Index < 3 ? 0.0f : 1.0f);
        bThrough = bThrough && NearV(At, Bend[Index], 1e-2f);
        if(Index > 0) { bRising = bRising && Curve.GetDistanceAtPoint(Index) > Curve.GetDistanceAtPoint(Index - 1); }
    }
    Expect(bThrough, "a Catmull-Rom curve passes through every point, the closely spaced ones too");
    Expect(bRising, "distance along it grows from point to point");
    float Chords = 0.0f;
    for(uint32_t Index = 1; Index < 4; ++Index) { Chords += float((Bend[Index] - Bend[Index - 1]).GetLength()); }
    Expect(Curve.GetLength() >= Chords - 0.5f, "a curve is no shorter than its chords");

    /* Evenly spaced by distance: equal steps are equal arcs. */
    float Previous = 0.0f;
    bool  bEven = true;
    for(int Step = 1; Step <= 10; ++Step)
    {
        const float D = Curve.GetLength() * float(Step) / 10.0f;
        const float Arc = float((Curve.SampleAtDistance(D).Position - Curve.SampleAtDistance(D - Curve.GetLength() / 10.0f).Position).GetLength());
        if(Step > 1 && std::fabs(Arc - Previous) > 0.05f * Previous) { bEven = false; }
        Previous = Arc;
    }
    Expect(bEven, "equal steps of distance cover near equal chords");

    const Vector3 Square[4] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 1000.0f), Vector3(0.0f, 0.0f, 1000.0f) };
    const GameCore::SplineCurve Loop(Square, 4, true, GameCore::ST_Linear);
    Expect(Loop.IsClosed() && Near(Loop.GetLength(), 4000.0f, 0.5f), "a closed linear square is its perimeter long");
    Expect(Loop.ContainsXZ(500.0f, 500.0f) && !Loop.ContainsXZ(1500.0f, 500.0f) && !Loop.ContainsXZ(-1.0f, 500.0f),
           "inside and outside a closed curve, from above");
    Expect(NearV(Loop.SampleAtDistance(4250.0f).Position, Vector3(250.0f, 0.0f, 0.0f), 0.5f), "a closed curve's distance wraps");
    GameCore::SplineSample Nearest;
    float Distance = 0.0f;
    Expect(Loop.NearestToXZ(Vector3(500.0f, 900.0f, -200.0f), Nearest, Distance) && Near(Distance, 200.0f, 0.5f)
           && Near(Nearest.Distance, 500.0f, 0.5f), "the nearest point on the curve, from above, and how far along it is");

    const Vector3 Two[2] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f) };
    Expect(!GameCore::SplineCurve(Two, 2, true).IsClosed(), "two points cannot close");
    Expect(!GameCore::SplineCurve(Two, 1, false).IsValid(), "one point is no curve");

    const GameCore::SplineCurve Moved = Straight.Transformed(Translation(0.0f, 0.0f, 500.0f));
    Expect(NearV(Moved.SampleAtDistance(0.0f).Position, Vector3(0.0f, 0.0f, 500.0f)), "a curve through a matrix moves with it");
}

static void TestSplineComponent()
{
    std::printf("Spline component\n");
    GameCore::World World;
    GameCore::Entity Road = World.CreateEntity();
    GameCore::TransformComponent Transform;
    Transform.Position = Vector3(100.0f, 0.0f, 0.0f);
    Road.AddComponent(Transform);
    GameCore::SplineComponent* Spline = Road.AddComponent<GameCore::SplineComponent>();
    Expect(Spline && Spline->GetPointCount() == 0, "a new Spline has no points");
    if(!Spline) { return; }

    Expect(Spline->AddPoint(Vector3(0.0f, 0.0f, 0.0f)) && Spline->GetPointCount() == 1, "a point on the origin is kept (nudged off it)");
    Expect(Spline->AddPoint(Vector3(500.0f, 0.0f, 0.0f)) && Spline->GetPointCount() == 2, "AddPoint appends");
    Expect(Spline->InsertPoint(1, Vector3(250.0f, 0.0f, 100.0f)) && Spline->GetPointCount() == 3
           && NearV(Spline->GetPoint(1), Vector3(250.0f, 0.0f, 100.0f)), "InsertPoint shifts the rest along");
    Expect(Spline->RemovePoint(1) && Spline->GetPointCount() == 2 && NearV(Spline->GetPoint(1), Vector3(500.0f, 0.0f, 0.0f)),
           "RemovePoint closes the gap");
    Expect(NearV(Spline->GetLocationAtDistance(0.0f), Vector3(100.0f, 0.01f, 0.0f), 0.05f), "queries are in world space");
    Expect(Near(Spline->GetLength(), 500.0f, 0.5f), "and so is its length");
    Expect(Near(Spline->FindDistanceClosestTo(Vector3(400.0f, 50.0f, 30.0f)), 300.0f, 0.5f), "the distance along it nearest a point");
    Expect(Spline->RemovePoint(1) && Spline->GetPointCount() == 1, "the last point can go");
    Spline->ClearPoints();
    Expect(Spline->GetPointCount() == 0, "ClearPoints empties it");
}

// === Placement ===================================================================

static void TestBoxGrid()
{
    std::printf("Box source\n");
    ProceduralInputs In = BaseInputs();
    ProceduralResult A, B;
    GenerateProcedural(In, A);
    Expect(A.Problem == nullptr && A.InstanceCount == 100, "a neat 1000-unit box at Spacing 100 holds 10 x 10");

    In.Settings.Jitter = 1.0f;
    GenerateProcedural(In, A);
    GenerateProcedural(In, B);
    const std::vector<RenderInstance> First = AllInstances(A), Second = AllInstances(B);
    Expect(First.size() == Second.size() && std::memcmp(First.data(), Second.data(), First.size() * sizeof(RenderInstance)) == 0,
           "the same rules and seed place the same instances");
    In.Settings.Seed = 7;
    GenerateProcedural(In, B);
    const std::vector<RenderInstance> Third = AllInstances(B);
    Expect(First.size() != Third.size() || std::memcmp(First.data(), Third.data(), First.size() * sizeof(RenderInstance)) != 0,
           "another seed places them elsewhere");

    bool bInside = true;
    for(const RenderInstance& Each : Third)
    {
        bInside = bInside && std::fabs(Each.Transform[12]) <= 500.0f && std::fabs(Each.Transform[14]) <= 500.0f && Each.Transform[13] == 0.0f;
    }
    Expect(bInside, "with nothing to drop onto, every one stays inside the box, on its plane");

    In = BaseInputs();
    In.Settings.Density = 0.5f;
    GenerateProcedural(In, A);
    Expect(A.InstanceCount > 30 && A.InstanceCount < 70, "Density 0.5 keeps about half");
    In = BaseInputs();
    In.Settings.Spacing = 20.0f;
    In.Settings.NoiseScale = 300.0f;
    In.Settings.NoiseThreshold = 0.5f;
    GenerateProcedural(In, A);
    Expect(A.InstanceCount > 2500 / 5 && A.InstanceCount < 2500 * 4 / 5, "a noise mask at threshold 0.5 leaves patches, neither all nor none");

    In = BaseInputs();
    In.Settings.MaxInstances = 10;
    GenerateProcedural(In, A);
    Expect(A.InstanceCount == 10 && A.bCapped, "Max Instances stops it, and says so");

    In = BaseInputs();
    In.Settings.CellSize = 400.0f;
    GenerateProcedural(In, A);
    size_t Sum = 0;
    bool bBoxes = true;
    for(const ProceduralCell& Cell : A.Cells)
    {
        for(const ProceduralBatch& Batch : Cell.Batches)
        {
            Sum += Batch.Instances.size();
            for(const RenderInstance& Each : Batch.Instances)
            {
                bBoxes = bBoxes && Each.Transform[12] >= Cell.Min[0] && Each.Transform[12] <= Cell.Max[0]
                                && Each.Transform[14] >= Cell.Min[2] && Each.Transform[14] <= Cell.Max[2];
            }
        }
    }
    Expect(A.Cells.size() >= 9 && Sum == A.InstanceCount, "instances are bucketed into Cell Size cells, none lost");
    Expect(bBoxes, "each cell's box holds its instances");

    In = BaseInputs();
    In.Entries[0].bValid = false;
    GenerateProcedural(In, A);
    Expect(A.InstanceCount == 0 && A.Problem != nullptr, "with no mesh to place, nothing is placed, and it says why");

    /* Two entries by weight. */
    In = BaseInputs();
    In.Settings.Spacing = 20.0f;
    std::strcpy(In.Settings.Meshes[1].Mesh, "Test/Other.nmmesh");
    In.Settings.Meshes[1].Weight = 3.0f;
    In.Entries[1] = In.Entries[0];
    GenerateProcedural(In, A);
    size_t PerEntry[2] = { 0, 0 };
    for(const ProceduralCell& Cell : A.Cells)
    {
        for(const ProceduralBatch& Batch : Cell.Batches) { if(Batch.Entry < 2) { PerEntry[Batch.Entry] += Batch.Instances.size(); } }
    }
    const float Share = float(PerEntry[1]) / float(std::max<size_t>(PerEntry[0] + PerEntry[1], 1));
    Expect(Share > 0.7f && Share < 0.8f, "entries are picked by Weight (3:1 gives about three quarters)");
}

static void TestTerrainProjection()
{
    std::printf("Terrain projection\n");
    std::shared_ptr<GameCore::TerrainData> Slope = MakeSlope();
    ProceduralInputs In = BaseInputs();
    In.Settings.Project   = PGP_Terrain;
    In.Settings.BoxExtent = Vector3(1000.0f, 3000.0f, 1000.0f);
    In.Settings.Jitter    = 1.0f;
    In.ToWorld = Translation(3200.0f, 1000.0f, 3200.0f);
    ProceduralTerrainInput Terrain;
    Terrain.Data    = Slope;
    Terrain.ToWorld = EngineCore::Matrix4x4();
    Terrain.ToLocal = EngineCore::Matrix4x4();
    In.Terrains.push_back(Terrain);

    ProceduralResult Out;
    GenerateProcedural(In, Out);
    bool bOnGround = true;
    for(const RenderInstance& Each : AllInstances(Out))
    {
        const float X = Each.Transform[12] + 3200.0f, Y = Each.Transform[13] + 1000.0f, Z = Each.Transform[14] + 3200.0f;
        bOnGround = bOnGround && Near(Y, Slope->SampleHeight(X, Z), 0.5f);
    }
    Expect(Out.InstanceCount == 400 && bOnGround, "dropped onto a terrain, each stands on its ground, in the entity's space");

    In.Settings.AlignToNormal = 1.0f;
    GenerateProcedural(In, Out);
    float Normal[3];
    Slope->SampleNormal(3200.0f, 3200.0f, Normal);
    bool bAligned = true;
    for(const RenderInstance& Each : AllInstances(Out))
    {
        const float Length = std::sqrt(Each.Transform[4] * Each.Transform[4] + Each.Transform[5] * Each.Transform[5] + Each.Transform[6] * Each.Transform[6]);
        bAligned = bAligned && Near(Each.Transform[4] / Length, Normal[0], 1e-3f) && Near(Each.Transform[5] / Length, Normal[1], 1e-3f);
    }
    Expect(bAligned, "Align to Normal 1 stands each one along the ground's normal");

    In.Settings.MaxSlope = 10.0f;   // the slope is 14 degrees
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 0, "Max Slope keeps them off steeper ground");
    In.Settings.MaxSlope = 20.0f;
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 400, "and lets them onto gentler ground");

    In.Settings.MinHeight = 800.0f;
    GenerateProcedural(In, Out);
    bool bHigh = Out.InstanceCount > 0;
    for(const RenderInstance& Each : AllInstances(Out)) { bHigh = bHigh && Each.Transform[13] + 1000.0f >= 800.0f; }
    Expect(bHigh && Out.InstanceCount < 400, "Min Height keeps the low ones out");
    In.Settings.MinHeight = -1.0e6f;

    In.Settings.TerrainLayer   = 2;   // layer 1: painted east of X 3200
    In.Settings.LayerThreshold = 0.5f;
    GenerateProcedural(In, Out);
    bool bEast = Out.InstanceCount > 0;
    for(const RenderInstance& Each : AllInstances(Out)) { bEast = bEast && Each.Transform[12] + 3200.0f > 3150.0f; }
    Expect(bEast && Out.InstanceCount > 120 && Out.InstanceCount < 280, "Terrain Layer keeps them where that layer is painted");

    In.Settings.TerrainLayer = PROCEDURAL_ANY_LAYER;
    In.ToWorld = Translation(30000.0f, 1000.0f, 3200.0f);
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 0, "with no terrain under them, points are dropped");
}

static void TestSplineSources()
{
    std::printf("Spline sources\n");
    ProceduralInputs In = BaseInputs();
    In.Settings.Source = PGS_SplinePath;
    ProceduralResult Out;
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 0 && Out.Problem != nullptr, "Spline Path without a Spline says so");

    const Vector3 Line[2] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 0.0f) };
    In.bHasSpline = true;
    In.Spline.Build(Line, 2, false, GameCore::ST_Linear);
    In.Settings.bAlignToPath = 1;
    GenerateProcedural(In, Out);
    bool bFacing = Out.InstanceCount > 0, bOnLine = true;
    for(const RenderInstance& Each : AllInstances(Out))
    {
        bFacing = bFacing && Near(Each.Transform[8], 1.0f, 1e-2f) && Near(Each.Transform[10], 0.0f, 1e-2f);   // +Z along +X
        bOnLine = bOnLine && Near(Each.Transform[14], 0.0f, 1e-2f);
    }
    Expect(Out.InstanceCount == 11, "along a 1000-unit path at Spacing 100: one at each end and every 100 between");
    Expect(bFacing && bOnLine, "Align to Path faces each along the curve, on it");

    In.SplineWidth = 400.0f;
    GenerateProcedural(In, Out);
    bool bWithin = true;
    for(const RenderInstance& Each : AllInstances(Out)) { bWithin = bWithin && std::fabs(Each.Transform[14]) <= 200.0f + 1e-3f; }
    Expect(Out.InstanceCount == 44 && bWithin, "a Width fills lanes Spacing apart across it, within half the width each side");

    In.SplineWidth = 0.0f;
    In.Settings.PathOffset = 300.0f;
    In.Settings.bBothSides = 1;
    GenerateProcedural(In, Out);
    size_t Left = 0, Right = 0;
    for(const RenderInstance& Each : AllInstances(Out))
    {
        Left  += Near(Each.Transform[14],  300.0f, 1e-2f) ? 1u : 0u;   // the curve runs +X: its left is +Z
        Right += Near(Each.Transform[14], -300.0f, 1e-2f) ? 1u : 0u;
    }
    Expect(Out.InstanceCount == 22 && Left == 11 && Right == 11, "Path Offset with Both Sides makes a row each side of the curve");
    In.Settings.PathOffset = 0.0f;
    In.Settings.bBothSides = 0;

    In.Settings.Source = PGS_SplineArea;
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 0 && Out.Problem != nullptr, "Spline Area needs a closed spline");
    const Vector3 Square[4] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 0.0f), Vector3(1000.0f, 0.0f, 1000.0f), Vector3(0.0f, 0.0f, 1000.0f) };
    In.Spline.Build(Square, 4, true, GameCore::ST_Linear);
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 100, "a closed 1000-unit square at Spacing 100 fills 10 x 10");

    /* An exclusion: a closed square over the area's south-west quarter, and a road across its east. */
    ProceduralExclusionInput Pond;
    const Vector3 Quarter[4] = { Vector3(-10.0f, 0.0f, -10.0f), Vector3(500.0f, 0.0f, -10.0f), Vector3(500.0f, 0.0f, 500.0f), Vector3(-10.0f, 0.0f, 500.0f) };
    Pond.Curve.Build(Quarter, 4, true, GameCore::ST_Linear);
    In.Exclusions.push_back(Pond);
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 75, "a closed exclusion clears what is inside it");
    ProceduralExclusionInput Road;
    const Vector3 Across[2] = { Vector3(850.0f, 0.0f, -100.0f), Vector3(850.0f, 0.0f, 1100.0f) };
    Road.Curve.Build(Across, 2, false, GameCore::ST_Linear);
    Road.HalfWidth = 100.0f;
    In.Exclusions.push_back(Road);
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 75 - 10, "an open exclusion clears a band half its Width each side");
}

static void TestMeshes()
{
    std::printf("Meshes\n");
    /* Mesh Surface: a 1000-unit quad, so about 100 at Spacing 100. */
    ProceduralInputs In = BaseInputs();
    In.Settings.Source = PGS_MeshSurface;
    In.Settings.AlignToNormal = 1.0f;
    In.Surface.Mesh    = MakeQuad(1000.0f, 50.0f);
    In.Surface.ToWorld = EngineCore::Matrix4x4();
    ProceduralResult Out;
    GenerateProcedural(In, Out);
    bool bOn = true;
    for(const RenderInstance& Each : AllInstances(Out)) { bOn = bOn && Near(Each.Transform[13], 50.0f, 1e-2f) && Near(Each.Transform[5], 1.0f, 1e-3f); }
    Expect(Out.InstanceCount >= 98 && Out.InstanceCount <= 102 && bOn, "Mesh Surface: one per Spacing squared of area, on its triangles, along their normal");

    In.Surface.Mesh = MakeQuad(1000.0f, 0.0f, true);   // a wall
    GenerateProcedural(In, Out);
    bool bSideways = Out.InstanceCount > 0;
    for(const RenderInstance& Each : AllInstances(Out)) { bSideways = bSideways && Near(Each.Transform[6], 1.0f, 1e-3f); }
    Expect(bSideways, "on a wall, aligned instances stand out of it");
    In.Settings.MaxSlope = 45.0f;
    GenerateProcedural(In, Out);
    Expect(Out.InstanceCount == 0, "and Max Slope keeps them off it");

    /* Static meshes under a box: a platform at 300 over half of it. */
    In = BaseInputs();
    In.Settings.Project = PGP_Meshes;
    ProceduralMeshInput Platform;
    Platform.Mesh    = MakeQuad(500.0f, 300.0f);
    Platform.ToWorld = Translation(260.0f, 0.0f, 10.0f);   // X 10..510, Z -240..260: 5 x 5 grid points
    In.Meshes.push_back(Platform);
    GenerateProcedural(In, Out);
    bool bUp = Out.InstanceCount > 0;
    for(const RenderInstance& Each : AllInstances(Out)) { bUp = bUp && Near(Each.Transform[13], 300.0f, 1e-2f) && Each.Transform[12] >= 0.0f; }
    Expect(Out.InstanceCount == 25 && bUp, "dropped onto static meshes, only where one is under them, on its top");

    /* Both: the higher wins. */
    In.Settings.Project = PGP_All;
    ProceduralMeshInput Floor;
    Floor.Mesh    = MakeQuad(2000.0f, 0.0f);
    Floor.ToWorld = EngineCore::Matrix4x4();
    In.Meshes.push_back(Floor);
    GenerateProcedural(In, Out);
    size_t High = 0;
    for(const RenderInstance& Each : AllInstances(Out)) { High += Each.Transform[13] > 150.0f ? 1u : 0u; }
    Expect(Out.InstanceCount == 100 && High == 25, "where two surfaces are under a point, it lands on the higher");
}

/* What PROCEDURAL.md shows from Lua: a Vector3 is three numbers, queries are in world space. */
static void TestScripts()
{
    std::printf("Lua\n");
    GameCore::World World;
    GameCore::ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "a script engine starts");
    Scripts.BindWorld(&World);
    GameCore::Entity Road = World.CreateEntity("Road");
    GameCore::TransformComponent Transform;
    Transform.Position = Vector3(100.0f, 0.0f, 0.0f);
    Road.AddComponent(Transform);
    GameCore::SplineComponent Spline;
    Spline.AddPoint(Vector3(0.0f, 0.0f, 0.0f));
    Spline.AddPoint(Vector3(500.0f, 0.0f, 0.0f));
    Road.AddComponent(Spline);
    Road.AddComponent<ProceduralSpawnerComponent>();

    Expect(Scripts.RunString(
        "local s = World.FindEntity('Road'):GetComponent('Spline')\n"
        "assert(s:GetPointCount() == 2, 'count')\n"
        "assert(math.abs(s:GetLength() - 500) < 0.5, 'length')\n"
        "local x, y, z = s:GetLocationAtDistance(250)\n"
        "assert(math.abs(x - 350) < 0.5 and math.abs(z) < 0.5, 'location in world space')\n"
        "local dx, dy, dz = s:GetDirectionAtDistance(250)\n"
        "assert(math.abs(dx - 1) < 1e-3, 'direction')\n"
        "assert(math.abs(s:FindDistanceClosestTo(400, 80, 30) - 300) < 0.5, 'closest')\n"
        "assert(s:AddPoint(500, 0, 400) and s:GetPointCount() == 3, 'add')\n"
        "s:SetClosed(true)\n"
        "assert(s:IsInside(450, 0, 100), 'inside')\n"
        "local p = World.FindEntity('Road'):GetComponent('Procedural Spawner')\n"
        "p:SetDensity(0.5)\n"
        "assert(math.abs(p:GetDensity() - 0.5) < 1e-6 and p:GetInstanceCount() == 0, 'spawner fields')\n"
        "p:Regenerate()\n",
        "ProceduralTests"), "the Spline and Procedural Spawner Lua APIs work as documented");
    Scripts.UnbindWorld();
    Scripts.Shutdown();
}

/* Everything a generation keeps is in the "Procedural" pool, and goes back when the last handle drops. */
static void TestMemoryPool()
{
    std::printf("Procedural memory pool\n");
    const EngineCore::PoolID Pool = GetProceduralMemoryPool();
    Expect(Pool != EngineCore::POOL_Default && Pool != EngineCore::POOL_Invalid, "a Procedural pool of its own is registered");
    const EngineCore::MemoryPoolStats Before = GetProceduralMemoryStats();
    {
        auto Result = std::allocate_shared<ProceduralResult>(ProceduralAllocator<ProceduralResult>());
        GenerateProcedural(BaseInputs(), *Result);
        const EngineCore::MemoryPoolStats During = GetProceduralMemoryStats();
        Expect(Result->InstanceCount == 100 && During.BytesInUse >= Before.BytesInUse + 100 * sizeof(RenderInstance),
               "a result's instances are counted in the pool");
        Expect(During.LiveAllocations > Before.LiveAllocations + 1, "and so are its cells, batches and the shared handle");
    }
    const EngineCore::MemoryPoolStats After = GetProceduralMemoryStats();
    Expect(After.LiveAllocations == Before.LiveAllocations && After.BytesInUse == Before.BytesInUse,
           "dropping the result returns every byte");

    /* The whole path: a spawner in a World, generated on a worker, published, then forgotten. */
    auto Quad = MakeQuad(100.0f, 0.0f);
    MeshAssetCache::Store("@test/procedural_quad", Quad);
    {
        GameCore::World World;
        GameCore::Entity Field = World.CreateEntity("Field");
        Field.AddComponent(GameCore::TransformComponent());
        ProceduralSpawnerComponent Spawner;
        Spawner.Project = PGP_None;
        Spawner.Spacing = 100.0f;
        Spawner.Jitter  = 0.0f;
        Spawner.BoxExtent = Vector3(500.0f, 500.0f, 500.0f);
        std::strcpy(Spawner.Meshes[0].Mesh, "@test/procedural_quad");
        Field.AddComponent(Spawner);

        ProceduralSystem System;
        const ProceduralSpawnerComponent* Live = Field.GetComponent<ProceduralSpawnerComponent>();
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(Live && Live->InstanceCount == 0 && std::chrono::steady_clock::now() < Deadline)
        {
            System.Update(World);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Expect(Live && Live->InstanceCount == 100, "the system generates a spawner on a worker and counts it back");
        Expect(ProceduralResultStore::Find(&World, Field.GetID()) != nullptr, "and publishes the result for its proxy");
        Expect(GetProceduralMemoryStats().LiveAllocations > Before.LiveAllocations, "the published result lives in the pool");
        System.Reset();
        Expect(ProceduralResultStore::Find(&World, Field.GetID()) == nullptr, "Reset forgets it");
    }
    MeshAssetCache::Remove("@test/procedural_quad");
    const EngineCore::MemoryPoolStats End = GetProceduralMemoryStats();
    Expect(End.LiveAllocations == Before.LiveAllocations && End.BytesInUse == Before.BytesInUse,
           "with every spawner gone the pool is back where it started");
}

int main()
{
    std::printf("RenderCore procedural tests\n");
    TestSplineCurve();
    TestSplineComponent();
    TestBoxGrid();
    TestTerrainProjection();
    TestSplineSources();
    TestMeshes();
    TestScripts();
    TestMemoryPool();
    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
