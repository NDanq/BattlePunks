/*
 * RenderCoreFractureTests -- Voronoi fracture without a window
 * (MeshFracture.h): hand-made closed meshes cut into pieces that must each
 * be closed, add up to the source's volume, and come out the same every
 * time for the same seed.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/MeshFracture.h"

using namespace RenderCore;

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

struct P3 { float X, Y, Z; };

/* One flat-shaded triangle, wound so (B-A)x(C-A) is its normal. */
static void AddTri(MeshData& InOutMesh, const P3& A, const P3& B, const P3& C)
{
    const float UX = B.X - A.X, UY = B.Y - A.Y, UZ = B.Z - A.Z;
    const float VX = C.X - A.X, VY = C.Y - A.Y, VZ = C.Z - A.Z;
    float NX = UY * VZ - UZ * VY, NY = UZ * VX - UX * VZ, NZ = UX * VY - UY * VX;
    const float Len = std::sqrt(NX * NX + NY * NY + NZ * NZ);
    NX /= Len; NY /= Len; NZ /= Len;
    for(const P3& P : { A, B, C })
    {
        InOutMesh.Indices.push_back(InOutMesh.GetVertexCount());
        InOutMesh.Positions.insert(InOutMesh.Positions.end(), { P.X, P.Y, P.Z });
        InOutMesh.Normals.insert(InOutMesh.Normals.end(), { NX, NY, NZ });
        InOutMesh.UV0.insert(InOutMesh.UV0.end(), { 0.0f, 0.0f });
    }
}

static void AddQuad(MeshData& InOutMesh, const P3& A, const P3& B, const P3& C, const P3& D)
{
    AddTri(InOutMesh, A, B, C);
    AddTri(InOutMesh, A, C, D);
}

/* An axis-aligned box facing out, or in (a cavity) when bInward. */
static void AddBox(MeshData& InOutMesh, const P3& Lo, const P3& Hi, const bool bInward)
{
    const P3 C[8] = {
        { Lo.X, Lo.Y, Lo.Z }, { Hi.X, Lo.Y, Lo.Z }, { Hi.X, Hi.Y, Lo.Z }, { Lo.X, Hi.Y, Lo.Z },
        { Lo.X, Lo.Y, Hi.Z }, { Hi.X, Lo.Y, Hi.Z }, { Hi.X, Hi.Y, Hi.Z }, { Lo.X, Hi.Y, Hi.Z },
    };
    const int Faces[6][4] = {
        { 0, 3, 2, 1 }, { 4, 5, 6, 7 },   // -Z, +Z
        { 0, 1, 5, 4 }, { 3, 7, 6, 2 },   // -Y, +Y
        { 0, 4, 7, 3 }, { 1, 2, 6, 5 },   // -X, +X
    };
    for(const auto& F : Faces)
    {
        if(bInward) { AddQuad(InOutMesh, C[F[0]], C[F[3]], C[F[2]], C[F[1]]); }
        else        { AddQuad(InOutMesh, C[F[0]], C[F[1]], C[F[2]], C[F[3]]); }
    }
}

static MeshData Finish(MeshData InMesh)
{
    MeshSection All;
    All.Name       = "Surface";
    All.Material   = "Materials/Crate.material.nmgraph";
    All.IndexCount = uint32_t(InMesh.Indices.size());
    InMesh.Sections.push_back(All);
    ComputeMeshBounds(InMesh);
    return InMesh;
}

static MeshData MakeBox(const float InSize)
{
    MeshData Mesh;
    AddBox(Mesh, { -InSize / 2, -InSize / 2, -InSize / 2 }, { InSize / 2, InSize / 2, InSize / 2 }, false);
    return Finish(Mesh);
}

/* A box with a box-shaped cavity: cuts through it cap with holes. */
static MeshData MakeHollowBox()
{
    MeshData Mesh;
    AddBox(Mesh, { -100, -100, -100 }, { 100, 100, 100 }, false);
    AddBox(Mesh, { -50, -50, -50 }, { 50, 50, 50 }, true);
    return Finish(Mesh);
}

/* An L in XY (counter-clockwise from +Z), 100 deep: not convex. */
static MeshData MakeLPrism()
{
    const float L[6][2] = { { 0, 0 }, { 200, 0 }, { 200, 100 }, { 100, 100 }, { 100, 200 }, { 0, 200 } };
    const int   Fan[4][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 3, 4 }, { 0, 4, 5 } };
    MeshData Mesh;
    for(const auto& T : Fan)
    {
        AddTri(Mesh, { L[T[0]][0], L[T[0]][1], 100 }, { L[T[1]][0], L[T[1]][1], 100 }, { L[T[2]][0], L[T[2]][1], 100 });
        AddTri(Mesh, { L[T[0]][0], L[T[0]][1], 0 }, { L[T[2]][0], L[T[2]][1], 0 }, { L[T[1]][0], L[T[1]][1], 0 });
    }
    for(int I = 0; I < 6; ++I)
    {
        const int J = (I + 1) % 6;
        AddQuad(Mesh, { L[I][0], L[I][1], 0 }, { L[J][0], L[J][1], 0 }, { L[J][0], L[J][1], 100 }, { L[I][0], L[I][1], 100 });
    }
    return Finish(Mesh);
}

/* Signed volume from the triangles: positive when they face out. */
static double MeshVolume(const MeshData& InMesh)
{
    double Volume = 0.0;
    for(size_t I = 0; I + 2 < InMesh.Indices.size(); I += 3)
    {
        const float* A = &InMesh.Positions[size_t(InMesh.Indices[I]) * 3];
        const float* B = &InMesh.Positions[size_t(InMesh.Indices[I + 1]) * 3];
        const float* C = &InMesh.Positions[size_t(InMesh.Indices[I + 2]) * 3];
        Volume += (double(A[0]) * (double(B[1]) * C[2] - double(B[2]) * C[1])
                 - double(A[1]) * (double(B[0]) * C[2] - double(B[2]) * C[0])
                 + double(A[2]) * (double(B[0]) * C[1] - double(B[1]) * C[0])) / 6.0;
    }
    return Volume;
}

/* Closed: every edge (by position) is used once each way. */
static bool IsClosed(const MeshData& InMesh, const float InWeld)
{
    using Key = std::tuple<long long, long long, long long>;
    auto KeyOf = [&](const uint32_t Index)
    {
        const float* P = &InMesh.Positions[size_t(Index) * 3];
        return Key{ std::llround(P[0] / InWeld), std::llround(P[1] / InWeld), std::llround(P[2] / InWeld) };
    };
    std::map<std::pair<Key, Key>, int> Edges;
    for(size_t I = 0; I + 2 < InMesh.Indices.size(); I += 3)
    {
        for(int K = 0; K < 3; ++K)
        {
            const Key A = KeyOf(InMesh.Indices[I + K]), B = KeyOf(InMesh.Indices[I + (K + 1) % 3]);
            if(A != B) { ++Edges[{ A, B }]; }
        }
    }
    int Unmatched = 0;
    for(const auto& Each : Edges)
    {
        const auto Back = Edges.find({ Each.first.second, Each.first.first });
        if(Back == Edges.end() || Back->second != Each.second)
        {
            if(++Unmatched <= 3)
            {
                const Key& A = Each.first.first;
                const Key& B = Each.first.second;
                std::printf("    open edge (%.3f %.3f %.3f) -> (%.3f %.3f %.3f) x%d\n",
                            std::get<0>(A) * InWeld, std::get<1>(A) * InWeld, std::get<2>(A) * InWeld,
                            std::get<0>(B) * InWeld, std::get<1>(B) * InWeld, std::get<2>(B) * InWeld, Each.second);
            }
        }
    }
    if(Unmatched > 0) { std::printf("    %d open edges of %zu\n", Unmatched, Edges.size()); }
    return Unmatched == 0;
}

/* The checks every fracture must pass. */
static void ExpectSound(const MeshData& InSource, const FractureResult& InResult, const char* InWhat)
{
    char What[256];
    const double Source = MeshVolume(InSource);
    double Sum = 0.0;
    bool bClosed = true, bVolumes = true, bInterior = true, bSections = true, bNoUV1 = true;
    for(const FracturePiece& Each : InResult.Pieces)
    {
        Sum += Each.Volume;
        bClosed   = bClosed && IsClosed(Each.Mesh, 1e-5f);
        bVolumes  = bVolumes && std::fabs(MeshVolume(Each.Mesh) - Each.Volume) <= 1e-3 * Source;
        bSections = bSections && Each.Mesh.Sections.size() == InSource.Sections.size() + 1;
        bInterior = bInterior && !Each.Mesh.Sections.empty() && Each.Mesh.Sections.back().Name == "Interior"
                    && Each.Mesh.Sections.back().IndexCount > 0;
        bNoUV1    = bNoUV1 && Each.Mesh.UV1.empty() && Each.Mesh.Normals.size() == Each.Mesh.Positions.size();
    }
    std::snprintf(What, sizeof(What), "%s: pieces were made", InWhat);
    Expect(InResult.Pieces.size() >= 2, What);
    std::snprintf(What, sizeof(What), "%s: every cut closed (%u open)", InWhat, InResult.OpenCaps);
    Expect(InResult.OpenCaps == 0, What);
    std::snprintf(What, sizeof(What), "%s: every piece is closed", InWhat);
    Expect(bClosed, What);
    std::snprintf(What, sizeof(What), "%s: piece volumes add up to the source's (%.1f of %.1f)", InWhat, Sum, Source);
    Expect(std::fabs(Sum - Source) <= 1e-3 * Source, What);
    std::snprintf(What, sizeof(What), "%s: each piece's Volume matches its triangles", InWhat);
    Expect(bVolumes, What);
    std::snprintf(What, sizeof(What), "%s: sections are the source's plus Interior, with caps in it", InWhat);
    Expect(bSections && bInterior, What);
    std::snprintf(What, sizeof(What), "%s: normals for every vertex, no lightmap UVs", InWhat);
    Expect(bNoUV1, What);

    bool bPairs = true;
    std::vector<int> Touches(InResult.Pieces.size(), 0);
    for(const auto& Pair : InResult.Neighbours)
    {
        bPairs = bPairs && Pair.first < Pair.second && Pair.second < InResult.Pieces.size();
        if(bPairs) { ++Touches[Pair.first]; ++Touches[Pair.second]; }
    }
    bool bAllTouch = true;
    for(const int Count : Touches) { bAllTouch = bAllTouch && Count > 0; }
    std::snprintf(What, sizeof(What), "%s: neighbours are valid pairs and every piece has one", InWhat);
    Expect(bPairs && bAllTouch, What);
}

static void TestSeeds()
{
    std::printf("Seeds\n");
    const MeshData Box = MakeBox(200);
    FractureSettings Settings;
    Settings.PieceCount = 20;
    for(const uint8_t Pattern : { FP_Uniform, FP_Clustered, FP_Splinter, FP_Radial })
    {
        Settings.Pattern = Pattern;
        std::vector<float> Seeds;
        MakeFractureSeeds(Box, Settings, Seeds);
        bool bInside = true;
        for(const float Each : Seeds) { bInside = bInside && Each >= -100.0f && Each <= 100.0f; }
        Expect(Seeds.size() == 60 && bInside, "one seed per piece, all inside the bounds");
    }

    Settings.Pattern = FP_Clustered;
    Settings.bHasImpact = true;
    Settings.ImpactPoint[0] = 90; Settings.ImpactPoint[1] = 90; Settings.ImpactPoint[2] = 90;
    std::vector<float> Seeds;
    MakeFractureSeeds(Box, Settings, Seeds);
    int Near = 0;
    for(size_t I = 0; I < Seeds.size(); I += 3)
    {
        const float DX = Seeds[I] - 90, DY = Seeds[I + 1] - 90, DZ = Seeds[I + 2] - 90;
        Near += std::sqrt(DX * DX + DY * DY + DZ * DZ) < 100.0f ? 1 : 0;
    }
    Expect(Near >= 10, "clustered seeds gather near the hit");
}

static void TestBox()
{
    std::printf("Box\n");
    const MeshData Box = MakeBox(200);
    FractureSettings Settings;
    Settings.Seed = 7;
    Settings.PieceCount = 12;
    Settings.InteriorMaterial = "Materials/ConcreteInside.material.nmgraph";

    FractureResult Result;
    Expect(FractureMesh(Box, Settings, Result), "a box fractures");
    ExpectSound(Box, Result, "box, uniform");
    Expect(Result.Pieces.size() == 12, "a seed inside a box always gives a piece");
    Expect(!Result.Pieces.empty() && Result.Pieces[0].Mesh.Sections.back().Material == Settings.InteriorMaterial,
           "the Interior section takes the interior material");
    Expect(!Result.Pieces.empty() && Result.Pieces[0].Mesh.Sections[0].Material == Box.Sections[0].Material,
           "the source's sections keep their materials");

    /* Each piece's origin is its centre of mass. */
    bool bCentred = true;
    for(const FracturePiece& Each : Result.Pieces)
    {
        double MX = 0, MY = 0, MZ = 0, Volume = 0;
        const MeshData& M = Each.Mesh;
        for(size_t I = 0; I + 2 < M.Indices.size(); I += 3)
        {
            const float* A = &M.Positions[size_t(M.Indices[I]) * 3];
            const float* B = &M.Positions[size_t(M.Indices[I + 1]) * 3];
            const float* C = &M.Positions[size_t(M.Indices[I + 2]) * 3];
            const double Tet = (double(A[0]) * (double(B[1]) * C[2] - double(B[2]) * C[1])
                              - double(A[1]) * (double(B[0]) * C[2] - double(B[2]) * C[0])
                              + double(A[2]) * (double(B[0]) * C[1] - double(B[1]) * C[0])) / 6.0;
            Volume += Tet;
            MX += Tet * (A[0] + B[0] + C[0]) / 4; MY += Tet * (A[1] + B[1] + C[1]) / 4; MZ += Tet * (A[2] + B[2] + C[2]) / 4;
        }
        bCentred = bCentred && std::fabs(MX / Volume) < 0.05 && std::fabs(MY / Volume) < 0.05 && std::fabs(MZ / Volume) < 0.05;
    }
    Expect(bCentred, "each piece is centred on its centre of mass");

    /* The same seed, the same pieces; another seed, others. */
    FractureResult Again;
    FractureMesh(Box, Settings, Again);
    bool bSame = Again.Pieces.size() == Result.Pieces.size();
    for(size_t I = 0; bSame && I < Result.Pieces.size(); ++I)
    {
        bSame = Again.Pieces[I].Mesh.Positions == Result.Pieces[I].Mesh.Positions
             && Again.Pieces[I].Mesh.Indices == Result.Pieces[I].Mesh.Indices;
    }
    Expect(bSame, "the same seed gives the same pieces, bit for bit");
    Settings.Seed = 8;
    FractureResult Other;
    FractureMesh(Box, Settings, Other);
    Expect(Other.Pieces.empty() || Result.Pieces.empty() || Other.Pieces[0].Mesh.Positions != Result.Pieces[0].Mesh.Positions,
           "another seed gives other pieces");
}

static void TestPatterns()
{
    std::printf("Patterns\n");
    const MeshData Box = MakeBox(200);
    FractureSettings Settings;
    Settings.Seed = 3;
    Settings.PieceCount = 16;
    Settings.bHasImpact = true;
    Settings.ImpactPoint[0] = 60; Settings.ImpactPoint[1] = 40; Settings.ImpactPoint[2] = -100;

    FractureResult Result;
    Settings.Pattern = FP_Clustered;
    FractureMesh(Box, Settings, Result);
    ExpectSound(Box, Result, "box, clustered");

    Settings.Pattern = FP_Radial;
    Settings.PieceCount = 24;
    FractureMesh(Box, Settings, Result);
    ExpectSound(Box, Result, "box, radial");

    /* Splinters run along the grain. */
    Settings.Pattern = FP_Splinter;
    Settings.PieceCount = 12;
    Settings.GrainStretch = 4.0f;
    FractureMesh(Box, Settings, Result);
    ExpectSound(Box, Result, "box, splinter");
    double Along = 0.0, Across = 0.0;
    for(const FracturePiece& Each : Result.Pieces)
    {
        Along  += Each.Mesh.BoundsMax[1] - Each.Mesh.BoundsMin[1];
        Across += 0.5 * ((Each.Mesh.BoundsMax[0] - Each.Mesh.BoundsMin[0]) + (Each.Mesh.BoundsMax[2] - Each.Mesh.BoundsMin[2]));
    }
    char What[128];
    std::snprintf(What, sizeof(What), "splinters are longer along the grain (%.2fx)", Across > 0 ? Along / Across : 0.0);
    Expect(Along > 1.5 * Across, What);
}

static void TestNonConvex()
{
    std::printf("Non-convex\n");
    FractureSettings Settings;
    Settings.Seed = 11;
    Settings.PieceCount = 14;

    const MeshData L = MakeLPrism();
    FractureResult Result;
    Expect(FractureMesh(L, Settings, Result), "an L prism fractures");
    ExpectSound(L, Result, "L prism");
    Expect(std::fabs(MeshVolume(L) - 3.0e6) < 1.0, "the L prism is 3,000,000 cubic units");

    /* A cavity: cuts through it cap around a hole. */
    const MeshData Hollow = MakeHollowBox();
    Settings.PieceCount = 10;
    Expect(FractureMesh(Hollow, Settings, Result), "a hollow box fractures");
    ExpectSound(Hollow, Result, "hollow box");
    Expect(std::fabs(MeshVolume(Hollow) - (8.0e6 - 1.0e6)) < 1.0, "the hollow box is 7,000,000 cubic units");

    /* Two pieces: the one cut goes right through the cavity. */
    Settings.PieceCount = 2;
    for(uint32_t Seed = 1; Seed <= 8; ++Seed)
    {
        Settings.Seed = Seed;
        FractureMesh(Hollow, Settings, Result);
        if(Result.Pieces.size() == 2) { break; }
    }
    ExpectSound(Hollow, Result, "hollow box, one cut");
}

static void TestRefusals()
{
    std::printf("Refusals\n");
    FractureResult Result;
    Expect(!FractureMesh(MeshData(), FractureSettings(), Result) && Result.Pieces.empty(), "an empty mesh is refused");

    MeshData Skinned = MakeBox(100);
    Skinned.Joints.assign(size_t(Skinned.GetVertexCount()) * 4, 0);
    Skinned.Weights.assign(size_t(Skinned.GetVertexCount()) * 4, 0.25f);
    Expect(!FractureMesh(Skinned, FractureSettings(), Result), "a skinned mesh is refused");

    /* One seed: the whole mesh, no cuts. */
    FractureSettings One;
    One.PieceCount = 1;
    const MeshData Box = MakeBox(100);
    Expect(FractureMesh(Box, One, Result) && Result.Pieces.size() == 1 && Result.Neighbours.empty()
           && std::fabs(Result.Pieces[0].Volume - 1.0e6f) < 1.0f, "one seed gives the mesh back whole");
}

static void TestTiming()
{
    std::printf("Timing\n");
    const MeshData Box = MakeBox(200);
    FractureSettings Settings;
    Settings.PieceCount = 20;
    FractureResult Result;
    const auto Start = std::chrono::steady_clock::now();
    const int  Runs  = 10;
    for(int I = 0; I < Runs; ++I)
    {
        Settings.Seed = uint32_t(I + 1);
        FractureMesh(Box, Settings, Result);
    }
    const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count() / Runs;
    std::printf("  a 20-piece box takes %.2f ms\n", Ms);
}

int main()
{
    std::printf("RenderCore fracture tests\n");
    TestSeeds();
    TestBox();
    TestPatterns();
    TestNonConvex();
    TestRefusals();
    TestTiming();
    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
