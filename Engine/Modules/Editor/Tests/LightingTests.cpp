/*
 * NimbleEditorLightingTests -- the light baker (Private/Editor/Lighting),
 * headless: its BVH against brute force, its atlas layout, and small bakes
 * whose answers are known -- a plane in the sun with a box's shadow on it,
 * bounced light, and probes that know when they are inside something.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "Private/Editor/Lighting/LightBakeBVH.h"
#include "Private/Editor/Lighting/LightBaker.h"
#include "Private/Editor/Lighting/LightBakeScene.h"

using namespace LightBake;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bInCondition, const char* InWhat)
{
    ++GChecks;
    if(!bInCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", InWhat);
    }
}

static constexpr float PI = 3.14159265358979f;

// === Scenes =================================================================

/* A square of side InSize at height InY facing +Y, UV1 over all of it, as
 * two triangles of instance InInstance. */
static void AddFloor(Scene& InOutScene, const float InSize, const float InY, const uint32_t InInstance, const Vec3& InAlbedo)
{
    const float H = InSize * 0.5f;
    const Vec3 Corners[4] = { Vec3(-H, InY, -H), Vec3(H, InY, -H), Vec3(H, InY, H), Vec3(-H, InY, H) };
    const float UV[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
    const int Tris[2][3] = { { 0, 2, 1 }, { 0, 3, 2 } };   // wound to face +Y: (P1-P0) x (P2-P0) points up
    for(const auto& Tri : Tris)
    {
        Triangle T;
        T.Instance = InInstance;
        T.Albedo   = InAlbedo;
        for(int C = 0; C < 3; ++C)
        {
            T.P[C] = Corners[Tri[C]];
            T.N[C] = Vec3(0.0f, 1.0f, 0.0f);
            T.UV1[C][0] = UV[Tri[C]][0];
            T.UV1[C][1] = UV[Tri[C]][1];
        }
        InOutScene.Triangles.push_back(T);
    }
}

/* An axis-aligned box, faces outward, no UV1 (not lightmapped). */
static void AddBox(Scene& InOutScene, const Vec3& InMin, const Vec3& InMax, const uint32_t InInstance, const Vec3& InAlbedo)
{
    for(int Axis = 0; Axis < 3; ++Axis)
    {
        for(int Side = 0; Side < 2; ++Side)
        {
            Vec3 N;
            if(Axis == 0) { N = Vec3(Side ? 1.0f : -1.0f, 0, 0); }
            if(Axis == 1) { N = Vec3(0, Side ? 1.0f : -1.0f, 0); }
            if(Axis == 2) { N = Vec3(0, 0, Side ? 1.0f : -1.0f); }
            const int U = (Axis + 1) % 3, V = (Axis + 2) % 3;
            Vec3 Q[4];
            for(int Corner = 0; Corner < 4; ++Corner)
            {
                float P[3];
                P[Axis] = Side ? InMax[Axis] : InMin[Axis];
                P[U] = (Corner == 1 || Corner == 2) ? InMax[U] : InMin[U];
                P[V] = (Corner >= 2) ? InMax[V] : InMin[V];
                Q[Corner] = Vec3(P[0], P[1], P[2]);
            }
            const int Tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
            for(const auto& Tri : Tris)
            {
                Triangle T;
                T.Instance = InInstance;
                T.Albedo   = InAlbedo;
                for(int C = 0; C < 3; ++C) { T.P[C] = Q[Tri[C]]; T.N[C] = N; }
                /* Facing out: flip the winding if the edges' cross points in. */
                if(Dot(Cross(T.P[1] - T.P[0], T.P[2] - T.P[0]), N) < 0.0f) { std::swap(T.P[1], T.P[2]); }
                InOutScene.Triangles.push_back(T);
            }
        }
    }
}

static void FinishInstances(Scene& InOutScene, const std::vector<bool>& InLightmapped)
{
    InOutScene.Instances.assign(InLightmapped.size(), Instance());
    for(size_t Index = 0; Index < InLightmapped.size(); ++Index)
    {
        InOutScene.Instances[Index].GuidLo       = Index + 1;
        InOutScene.Instances[Index].bLightmapped = InLightmapped[Index];
        InOutScene.Instances[Index].First        = std::numeric_limits<uint32_t>::max();
    }
    for(uint32_t Tri = 0; Tri < InOutScene.Triangles.size(); ++Tri)
    {
        Instance& Each = InOutScene.Instances[InOutScene.Triangles[Tri].Instance];
        Each.First = std::min(Each.First, Tri);
        ++Each.Count;
    }
    InOutScene.Finish();
}

/* The baked value at world point InX, InZ of a floor instance (UV1 = its xz over InSize). */
static Vec3 FloorTexel(const Result& InResult, const uint32_t InInstance, const float InSize, const float InX, const float InZ)
{
    for(const Placement& Place : InResult.Placements)
    {
        if(Place.Instance != InInstance) { continue; }
        const Atlas& A = InResult.Atlases[Place.Atlas];
        const float U = InX / InSize + 0.5f, V = InZ / InSize + 0.5f;
        const int X = std::clamp(int(U * float(Place.Resolution)) + Place.X, 0, A.Size - 1);
        const int Y = std::clamp(int(V * float(Place.Resolution)) + Place.Y, 0, A.Size - 1);
        const float* T = &A.Texels[(size_t(Y) * size_t(A.Size) + size_t(X)) * 3];
        return Vec3(T[0], T[1], T[2]);
    }
    return Vec3(-1.0f, -1.0f, -1.0f);
}

// === Tests ==================================================================

static void TestBVH()
{
    std::printf("[bvh]\n");
    Scene S;
    AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.5f, 0.5f, 0.5f));
    for(int Index = 0; Index < 40; ++Index)
    {
        const float X = float((Index * 37) % 900) - 450.0f, Z = float((Index * 53) % 900) - 450.0f;
        AddBox(S, Vec3(X, 0.0f, Z), Vec3(X + 30.0f, 20.0f + float(Index % 7) * 15.0f, Z + 30.0f), 1, Vec3(0.5f, 0.5f, 0.5f));
    }
    FinishInstances(S, { true, false });
    BVH Tree;
    Tree.Build(S.Triangles);

    /* Brute force, the same test the BVH runs. */
    const auto Brute = [&S](const Vec3& O, const Vec3& D, float& OutT) -> bool
    {
        bool bHit = false;
        OutT = 1e30f;
        for(const Triangle& T : S.Triangles)
        {
            const Vec3 E1 = T.P[1] - T.P[0], E2 = T.P[2] - T.P[0];
            const Vec3 PV = Cross(D, E2);
            const float Det = Dot(E1, PV);
            if(std::fabs(Det) < 1e-12f) { continue; }
            const Vec3 TV = O - T.P[0];
            const float U = Dot(TV, PV) / Det;
            if(U < 0.0f || U > 1.0f) { continue; }
            const Vec3 QV = Cross(TV, E1);
            const float V = Dot(D, QV) / Det;
            if(V < 0.0f || U + V > 1.0f) { continue; }
            const float Dist = Dot(E2, QV) / Det;
            if(Dist > 0.0f && Dist < OutT) { OutT = Dist; bHit = true; }
        }
        return bHit;
    };

    int Agree = 0, Rays = 2000, Hits = 0;
    uint32_t Seed = 12345;
    const auto Next = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return float(Seed >> 8) / 16777216.0f; };
    for(int Ray = 0; Ray < Rays; ++Ray)
    {
        const Vec3 O(Next() * 1200.0f - 600.0f, Next() * 300.0f + 1.0f, Next() * 1200.0f - 600.0f);
        const Vec3 D = Normalize(Vec3(Next() * 2.0f - 1.0f, Next() * 2.0f - 1.0f, Next() * 2.0f - 1.0f));
        float BruteT = 0.0f;
        const bool bBrute = Brute(O, D, BruteT);
        Hit H;
        const bool bTree = Tree.Intersect(O, D, 0.0f, 1e30f, H);
        const bool bOccluded = Tree.Occluded(O, D, 0.0f, 1e30f);
        if(bBrute == bTree && bTree == bOccluded && (!bBrute || std::fabs(BruteT - H.T) < 1e-2f)) { ++Agree; }
        Hits += bBrute ? 1 : 0;
    }
    Expect(Hits > 200 && Hits < Rays, "the random rays both hit and miss");
    Expect(Agree == Rays, "every ray's nearest hit and occlusion match brute force");
}

static void TestLayout()
{
    std::printf("[layout]\n");
    Scene S;
    for(uint32_t Index = 0; Index < 30; ++Index)
    {
        AddFloor(S, 50.0f + float(Index) * 40.0f, float(Index) * 10.0f, Index, Vec3(0.5f, 0.5f, 0.5f));
    }
    FinishInstances(S, std::vector<bool>(30, true));

    Settings Config;
    Config.TexelsPerUnit   = 0.25f;
    Config.MaxAtlasSize    = 256;
    Config.SamplesPerTexel = 1;
    Config.Bounces         = 0;
    Config.ProbeSpacing    = 10000.0f;
    Result R;
    std::string Error;
    Expect(Bake(S, Config, R, Error) == EBakeStatus::Done, "a bake of 30 floors finishes");
    Expect(R.Placements.size() == 30, "every lightmapped instance is placed");
    Expect(R.Atlases.size() > 1, "more than one atlas when they do not fit in one");

    bool bInside = true, bApart = true, bScaled = true;
    for(size_t A = 0; A < R.Placements.size(); ++A)
    {
        const Placement& P = R.Placements[A];
        const int Size = R.Atlases[P.Atlas].Size;
        bInside &= P.X >= 1 && P.Y >= 1 && P.X + P.Resolution < Size && P.Y + P.Resolution < Size;
        bScaled &= std::fabs(P.ScaleOffset[0] * float(Size) - float(P.Resolution)) < 1e-3f
                && std::fabs(P.ScaleOffset[2] * float(Size) - float(P.X)) < 1e-3f;
        for(size_t B = A + 1; B < R.Placements.size(); ++B)
        {
            const Placement& Q = R.Placements[B];
            if(Q.Atlas != P.Atlas) { continue; }
            /* A texel of border each side: squares at least one texel apart. */
            const bool bSeparate = P.X + P.Resolution + 1 <= Q.X || Q.X + Q.Resolution + 1 <= P.X
                                || P.Y + P.Resolution + 1 <= Q.Y || Q.Y + Q.Resolution + 1 <= P.Y;
            bApart &= bSeparate;
        }
    }
    Expect(bInside, "every square sits inside its atlas, a texel in from the edge");
    Expect(bApart, "no two squares overlap or touch");
    Expect(bScaled, "scale and offset take UV1 to the square");
}

/* A 1000-unit floor in a straight-down sun with a 100-unit box on it. */
static void TestSunAndShadow()
{
    std::printf("[sun and shadow]\n");
    Scene S;
    AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.5f, 0.5f, 0.5f));
    AddBox(S, Vec3(-50.0f, 0.0f, -50.0f), Vec3(50.0f, 100.0f, 50.0f), 1, Vec3(0.5f, 0.5f, 0.5f));
    FinishInstances(S, { true, false });

    Light Sun;
    Sun.Type      = ELightType::Directional;
    Sun.Direction = Vec3(0.0f, -1.0f, 0.0f);
    Sun.Radiance  = Vec3(3.0f, 3.0f, 3.0f);
    S.Lights.push_back(Sun);
    S.Finish();

    Settings Config;
    Config.TexelsPerUnit   = 0.064f;   // a 64-texel floor: 15.6 units a texel
    Config.SamplesPerTexel = 64;
    Config.Bounces         = 2;
    Config.ProbeSpacing    = 50.0f;
    Config.MaxAtlasSize    = 128;
    Result R;
    std::string Error;
    Expect(Bake(S, Config, R, Error) == EBakeStatus::Done, "the bake finishes");
    Expect(R.Placements.size() == 1, "only the floor has a lightmap (the box has no UV1)");

    const float Open = 3.0f / PI;   // radiance x cos(0) / pi
    const Vec3 Far   = FloorTexel(R, 0, 1000.0f, 400.0f, 400.0f);
    const Vec3 Under = FloorTexel(R, 0, 1000.0f, 0.0f, 0.0f);
    const Vec3 Beside = FloorTexel(R, 0, 1000.0f, 70.0f, 0.0f);
    std::printf("        open %.3f (expect %.3f), under the box %.3f, beside it %.3f\n", Far.X, Open, Under.X, Beside.X);
    Expect(std::fabs(Far.X - Open) < Open * 0.05f, "an open texel matches the sun's analytic N.L x radiance / pi");
    Expect(Under.X < Open * 0.05f, "the texel under the box is in its shadow");
    Expect(Beside.X > Open * 0.9f, "beside the box is in the sun");
    Expect(std::fabs(Far.X - Far.Y) < 1e-4f && std::fabs(Far.X - Far.Z) < 1e-4f, "grey in, grey out");

    /* Probes: one inside the box, one below the floor, one in the open air. */
    const RenderCore::BakedProbeGrid& G = R.Probes;
    Expect(!G.IsEmpty() && G.Valid.size() == G.Probes.size(), "probes are placed over the scene");
    int Inside = -1, Below = -1, Air = -1;
    for(size_t Index = 0; Index < G.Probes.size(); ++Index)
    {
        const uint32_t I = uint32_t(Index % G.Counts[0]), J = uint32_t((Index / G.Counts[0]) % G.Counts[1]);
        const uint32_t K = uint32_t(Index / (size_t(G.Counts[0]) * G.Counts[1]));
        const float X = G.Origin[0] + float(I) * G.Spacing, Y = G.Origin[1] + float(J) * G.Spacing, Z = G.Origin[2] + float(K) * G.Spacing;
        if(std::fabs(X) < 45.0f && Y > 5.0f && Y < 95.0f && std::fabs(Z) < 45.0f) { Inside = int(Index); }
        if(Y < -5.0f && std::fabs(X) < 300.0f && std::fabs(Z) < 300.0f) { Below = int(Index); }
        if(Y > 10.0f && Y < 60.0f && X > 200.0f && X < 400.0f && std::fabs(Z) < 100.0f) { Air = int(Index); }
    }
    Expect(Inside >= 0 && !G.Valid[size_t(Inside)], "a probe inside the box is left out");
    Expect(Below >= 0 && !G.Valid[size_t(Below)], "a probe under the floor is left out");
    Expect(Air >= 0 && G.Valid[size_t(Air)], "a probe in the open is used");
    if(Air >= 0)
    {
        const RenderCore::BakedAmbientCube& Cube = G.Probes[size_t(Air)];
        const float Up = Cube.Faces[2][0], Down = Cube.Faces[3][0];
        std::printf("        open probe: +Y %.3f (expect %.3f), -Y %.3f (expect about %.3f)\n", Up, Open, Down, Open * 0.5f);
        Expect(std::fabs(Up - Open) < Open * 0.1f, "a probe's upward face sees the sun");
        Expect(Down > Open * 0.35f && Down < Open * 0.6f, "its downward face sees the sunlit floor's bounce (albedo 0.5)");
    }

    float Point[3] = { 300.0f, 30.0f, 0.0f };
    RenderCore::BakedAmbientCube Sampled;
    Expect(G.Sample(Point, Sampled) && Sampled.Faces[2][0] > Open * 0.8f, "sampling the grid in the open blends valid probes");
}

/* No light at all but a white sky over a grey floor: the floor shows the
 * sky's radiance times its view of it -- all of it, open; less by a wall. */
static void TestSkyAndBounce()
{
    std::printf("[sky and bounce]\n");
    Scene S;
    AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.5f, 0.5f, 0.5f));
    AddBox(S, Vec3(-500.0f, 0.0f, -500.0f), Vec3(-480.0f, 400.0f, 500.0f), 1, Vec3(0.8f, 0.0f, 0.0f));
    FinishInstances(S, { true, false });
    S.SkyLight.bPresent    = true;
    S.SkyLight.SkyColor    = Vec3(1.0f, 1.0f, 1.0f);
    S.SkyLight.GroundColor = Vec3(1.0f, 1.0f, 1.0f);
    S.Finish();

    Settings Config;
    Config.TexelsPerUnit   = 0.064f;
    Config.SamplesPerTexel = 256;
    Config.Bounces         = 1;
    Config.ProbeSpacing    = 10000.0f;
    Config.MaxAtlasSize    = 128;
    Result R;
    std::string Error;
    Expect(Bake(S, Config, R, Error) == EBakeStatus::Done, "the bake finishes");

    const Vec3 Open = FloorTexel(R, 0, 1000.0f, 400.0f, 0.0f);
    const Vec3 Wall = FloorTexel(R, 0, 1000.0f, -460.0f, 0.0f);
    std::printf("        open %.3f %.3f %.3f, by the red wall %.3f %.3f %.3f\n", Open.X, Open.Y, Open.Z, Wall.X, Wall.Y, Wall.Z);
    Expect(Open.Y > 0.8f && Open.Y < 1.05f, "an open floor sees about all of a white sky");
    Expect(Wall.Y < Open.Y * 0.85f, "beside a wall the floor sees less sky");
    Expect(Wall.X > Wall.Y * 1.1f, "and picks up the red wall's bounce");

    /* Under the wall the floor is buried: left out and filled from its
     * neighbours, so no black line runs along the wall's foot. */
    const Vec3 Under = FloorTexel(R, 0, 1000.0f, -490.0f, 0.0f);
    std::printf("        under the wall %.3f\n", Under.Y);
    Expect(Under.Y > Wall.Y * 0.5f, "a texel under the wall takes its neighbours' light, not black");
}

/* What a floor texel's direction says (Atlas::Directions): xyz, w. */
static void FloorDirection(const Result& InResult, const float InSize, const float InX, const float InZ, float (&Out)[4])
{
    const Placement& Place = InResult.Placements[0];
    const Atlas& A = InResult.Atlases[Place.Atlas];
    const int X = std::clamp(int((InX / InSize + 0.5f) * float(Place.Resolution)) + Place.X, 0, A.Size - 1);
    const int Y = std::clamp(int((InZ / InSize + 0.5f) * float(Place.Resolution)) + Place.Y, 0, A.Size - 1);
    std::copy_n(&A.Directions[(size_t(Y) * size_t(A.Size) + size_t(X)) * 4], 4, Out);
}

/* A Stationary sun bakes no direct light; a Static one does, from above (its direction). */
static void TestStationaryAndDirection()
{
    std::printf("[stationary and direction]\n");
    Settings Config;
    Config.TexelsPerUnit   = 0.064f;
    Config.SamplesPerTexel = 16;
    Config.Bounces         = 1;
    Config.ProbeSpacing    = 10000.0f;
    Config.MaxAtlasSize    = 128;

    for(const bool bStationary : { false, true })
    {
        Scene S;
        AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.5f, 0.5f, 0.5f));
        FinishInstances(S, { true });
        Light Sun;
        Sun.Type        = ELightType::Directional;
        Sun.Direction   = Vec3(0.0f, -1.0f, 0.0f);
        Sun.Radiance    = Vec3(3.0f, 3.0f, 3.0f);
        Sun.bBakeDirect = !bStationary;
        S.Lights.push_back(Sun);
        S.Finish();
        Result R;
        std::string Error;
        Expect(Bake(S, Config, R, Error) == EBakeStatus::Done, "the bake finishes");
        const Vec3 Open = FloorTexel(R, 0, 1000.0f, 300.0f, 300.0f);
        if(bStationary)
        {
            Expect(Open.X < 0.01f, "a Stationary sun bakes no direct light (it stays live)");
            continue;
        }
        Expect(std::fabs(Open.X - 3.0f / PI) < 0.05f, "a Static sun bakes its direct light");
        float Way[4];
        FloorDirection(R, 1000.0f, 300.0f, 300.0f, Way);
        std::printf("        direction %.2f %.2f %.2f, rebalance %.2f\n", Way[0], Way[1], Way[2], Way[3]);
        Expect(Way[1] > 0.95f && std::fabs(Way[0]) < 0.05f && std::fabs(Way[2]) < 0.05f,
               "the light comes from straight above, all from one direction");
        Expect(std::fabs(Way[3] - 1.0f) < 0.05f, "and a flat surface rebalances to the light as baked");
    }
}

/* No light at all, but a glowing panel over the floor: the floor under it is lit by it. */
static void TestEmissive()
{
    std::printf("[emissive]\n");
    Scene S;
    AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.5f, 0.5f, 0.5f));
    AddBox(S, Vec3(-100.0f, 200.0f, -100.0f), Vec3(100.0f, 210.0f, 100.0f), 1, Vec3(0.0f, 0.0f, 0.0f));
    for(Triangle& Each : S.Triangles) { if(Each.Instance == 1) { Each.Emissive = Vec3(10.0f, 5.0f, 1.0f); } }
    FinishInstances(S, { true, false });

    Settings Config;
    Config.TexelsPerUnit   = 0.064f;
    Config.SamplesPerTexel = 128;
    Config.Bounces         = 0;
    Config.ProbeSpacing    = 10000.0f;
    Config.MaxAtlasSize    = 128;
    Result R;
    std::string Error;
    Expect(Bake(S, Config, R, Error) == EBakeStatus::Done, "the bake finishes");
    const Vec3 Under = FloorTexel(R, 0, 1000.0f, 0.0f, 0.0f);
    const Vec3 Far   = FloorTexel(R, 0, 1000.0f, 450.0f, 450.0f);
    std::printf("        under the panel %.3f %.3f %.3f, far from it %.3f\n", Under.X, Under.Y, Under.Z, Far.X);
    Expect(Under.X > 1.0f && Under.X > Under.Y && Under.Y > Under.Z, "the floor under a glowing panel takes its light and colour");
    Expect(Far.X < Under.X * 0.2f, "and less of it farther away");
}

/* A thin wall standing through the floor (overlapping it, down to y -50),
 * thinner than a texel and placed off the texel grid so one column of floor
 * texels has its centre under it. The sun slants in from +X: the wall's +X side
 * is lit, its -X side in shadow. The floor is black (it bounces nothing) and the
 * wall white, so the -X side gets no light at all, direct or bounced: whatever
 * it shows leaked across the wall. */
static void TestOverlapLeaks()
{
    std::printf("[overlap leaks]\n");
    Scene S;
    AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.0f, 0.0f, 0.0f));
    AddBox(S, Vec3(5.0f, -50.0f, -400.0f), Vec3(12.0f, 300.0f, 400.0f), 1, Vec3(1.0f, 1.0f, 1.0f));
    FinishInstances(S, { true, false });

    Light Sun;
    Sun.Type      = ELightType::Directional;
    Sun.Direction = Normalize(Vec3(-1.0f, -1.0f, 0.0f));
    Sun.Radiance  = Vec3(3.0f, 3.0f, 3.0f);
    S.Lights.push_back(Sun);
    S.Finish();

    Settings Config;
    Config.TexelsPerUnit   = 0.064f;   // 15.625 units a texel: texel centres at +-7.8, +-23.4, ...
    Config.SamplesPerTexel = 128;
    Config.Bounces         = 1;
    Config.ProbeSpacing    = 10000.0f;
    Config.MaxAtlasSize    = 128;
    Result R;
    std::string Error;
    Expect(Bake(S, Config, R, Error) == EBakeStatus::Done, "the bake finishes");

    const float Lit = 3.0f * 0.7071f / PI;   // radiance x cos(45) / pi
    const Vec3 Straddled = FloorTexel(R, 0, 1000.0f, 7.8f, 0.0f);    // centre under the wall; most of it on the shadow side
    const Vec3 Shadow    = FloorTexel(R, 0, 1000.0f, -23.4f, 0.0f);  // two texels into the shadow
    const Vec3 Sunlit    = FloorTexel(R, 0, 1000.0f, 39.0f, 0.0f);   // in the sun, beside the wall's lit face
    std::printf("        straddled texel %.3f, shadow side %.4f, sunlit side %.3f (direct %.3f)\n", Straddled.X, Shadow.X, Sunlit.X, Lit);
    Expect(Straddled.X < Lit * 0.1f, "a texel the wall straddles takes the shadow side it mostly lies on, not the sunlit side's light");
    Expect(Shadow.X < Lit * 0.02f, "beside the wall's shadowed face the floor gets no light from the sunlit side, bounced or smoothed");
    Expect(Sunlit.X > Lit * 1.02f, "while the sunlit side has the sun and the lit wall's bounce");
}

static void TestCancel()
{
    std::printf("[cancel]\n");
    Scene S;
    AddFloor(S, 1000.0f, 0.0f, 0, Vec3(0.5f, 0.5f, 0.5f));
    FinishInstances(S, { true });
    Settings Config;
    Config.TexelsPerUnit = 0.5f;
    std::atomic<bool> bCancel{true};
    Result R;
    std::string Error;
    Expect(Bake(S, Config, R, Error, ProgressFn(), &bCancel) == EBakeStatus::Cancelled, "a cancelled bake says so");
}

int main()
{
    std::printf("Editor lighting tests\n");
    TestBVH();
    TestLayout();
    TestSunAndShadow();
    TestSkyAndBounce();
    TestStationaryAndDirection();
    TestEmissive();
    TestOverlapLeaks();
    TestCancel();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
