#if WITH_EDITOR

#include "Private/Editor/Lighting/LightBaker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#include "Private/Editor/Lighting/LightBakeBVH.h"

namespace LightBake
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;

        constexpr int   MIN_RESOLUTION   = 16;
        constexpr int   MIN_ATLAS_SIZE   = 64;
        constexpr int   DILATE_PASSES    = 4;
        constexpr int   FILTER_RADIUS    = 3;
        constexpr int   MAX_PROBES_AXIS  = 64;
        constexpr int   MAX_PROBES       = 65536;
        constexpr float BACKFACE_INSIDE  = 0.25f;   // a probe seeing more backfaces than this is inside

        // === Random numbers =====================================================

        /* PCG32: small, fast, and seeded per texel, so a bake is the same every time. */
        struct Random
        {
            uint64_t State;

            explicit Random(const uint64_t InSeed) : State(InSeed * 6364136223846793005ull + 1442695040888963407ull) { Next(); }

            uint32_t Next()
            {
                const uint64_t Old = State;
                State = Old * 6364136223846793005ull + 1442695040888963407ull;
                const uint32_t Shifted = uint32_t(((Old >> 18u) ^ Old) >> 27u);
                const uint32_t Rotate  = uint32_t(Old >> 59u);
                return (Shifted >> Rotate) | (Shifted << ((32u - Rotate) & 31u));
            }

            /* [0, 1). */
            float Float() { return float(Next() >> 8) * (1.0f / 16777216.0f); }
        };

        uint64_t Mix(uint64_t X)
        {
            X ^= X >> 33; X *= 0xff51afd7ed558ccdull;
            X ^= X >> 33; X *= 0xc4ceb9fe1a85ec53ull;
            return X ^ (X >> 33);
        }

        // === Directions =========================================================

        /* Two unit vectors that, with N, make an orthonormal basis. */
        void Basis(const Vec3& N, Vec3& OutT, Vec3& OutB)
        {
            const float Sign = N.Z >= 0.0f ? 1.0f : -1.0f;
            const float A = -1.0f / (Sign + N.Z);
            const float B = N.X * N.Y * A;
            OutT = Vec3(1.0f + Sign * N.X * N.X * A, Sign * B, -Sign * N.X);
            OutB = Vec3(B, Sign + N.Y * N.Y * A, -N.Y);
        }

        Vec3 CosineSample(const Vec3& N, Random& R)
        {
            const float U1 = R.Float(), U2 = R.Float();
            const float Radius = std::sqrt(U1);
            const float Phi    = 2.0f * PI * U2;
            Vec3 T, B;
            Basis(N, T, B);
            return Normalize(T * (Radius * std::cos(Phi)) + B * (Radius * std::sin(Phi)) + N * std::sqrt(std::max(0.0f, 1.0f - U1)));
        }

        Vec3 SphereSample(Random& R)
        {
            const float Z   = 1.0f - 2.0f * R.Float();
            const float Rad = std::sqrt(std::max(0.0f, 1.0f - Z * Z));
            const float Phi = 2.0f * PI * R.Float();
            return Vec3(Rad * std::cos(Phi), Rad * std::sin(Phi), Z);
        }

        /* A direction within InAngle radians of InAxis, uniform over the cap. */
        Vec3 ConeSample(const Vec3& InAxis, const float InAngle, Random& R)
        {
            const float CosMax = std::cos(InAngle);
            const float CosT   = 1.0f - R.Float() * (1.0f - CosMax);
            const float SinT   = std::sqrt(std::max(0.0f, 1.0f - CosT * CosT));
            const float Phi    = 2.0f * PI * R.Float();
            Vec3 T, B;
            Basis(InAxis, T, B);
            return Normalize(T * (SinT * std::cos(Phi)) + B * (SinT * std::sin(Phi)) + InAxis * CosT);
        }

        float LocalFalloff(const float InDistance, const float InRange)
        {
            const float X      = InDistance / std::max(InRange, 1e-4f);
            const float Window = std::clamp(1.0f - X * X * X * X, 0.0f, 1.0f);
            return Window * Window / (1.0f + 16.0f * X * X);
        }

        float Luminance(const Vec3& InRgb) { return 0.2126f * InRgb.X + 0.7152f * InRgb.Y + 0.0722f * InRgb.Z; }

        float SmoothStep(const float InEdge0, const float InEdge1, const float InX)
        {
            const float T = std::clamp((InX - InEdge0) / std::max(InEdge1 - InEdge0, 1e-6f), 0.0f, 1.0f);
            return T * T * (3.0f - 2.0f * T);
        }

        // === The tracer ===========================================================

        struct Tracer
        {
            const Scene&    S;
            const BVH&      Tree;
            const Settings& Config;
            float           Bias;          // how far a ray starts off its surface, world units
            int             DirectSamples; // per light, for soft shadows

            /* InDirection's light at a point, toward a surface (or probe face)
             * with normal InN (unit; no cosine if null): every light's
             * radiance times cosine over pi, shadowed. bInBakedOnly leaves out
             * Stationary lights, whose direct light stays live. OutDirection,
             * if given, gains each light's direction times its luminance. */
            Vec3 Direct(const Vec3& InPoint, const Vec3* InN, Random& R, const int InSamples, const bool bInBakedOnly,
                        Vec3* OutDirection = nullptr) const
            {
                Vec3 Sum;
                for(const Light& L : S.Lights)
                {
                    if(bInBakedOnly && !L.bBakeDirect) { continue; }
                    Vec3 Light;
                    Vec3 Toward;
                    for(int Sample = 0; Sample < InSamples; ++Sample)
                    {
                        Vec3  ToLight;
                        float Distance = std::numeric_limits<float>::max();
                        float Scale    = 1.0f;
                        if(L.Type == ELightType::Directional)
                        {
                            const Vec3 Axis = Normalize(L.Direction * -1.0f);
                            ToLight = L.SourceSize > 0.0f ? ConeSample(Axis, L.SourceSize, R) : Axis;
                        }
                        else
                        {
                            Vec3 Source = L.Position;
                            if(L.SourceSize > 0.0f) { Source += SphereSample(R) * (L.SourceSize * std::cbrt(R.Float())); }
                            const Vec3 Delta = Source - InPoint;
                            Distance = std::sqrt(Dot(Delta, Delta));
                            if(Distance >= L.Range || Distance < 1e-4f) { continue; }
                            ToLight = Delta * (1.0f / Distance);
                            Scale   = LocalFalloff(Distance, L.Range);
                            if(L.Type == ELightType::Spot)
                            {
                                Scale *= SmoothStep(L.CosOuter, L.CosInner, Dot(ToLight * -1.0f, Normalize(L.Direction)));
                            }
                        }
                        const float Cosine = InN ? Dot(*InN, ToLight) : 1.0f;
                        if(Cosine <= 0.0f || Scale <= 0.0f) { continue; }
                        if(Tree.Occluded(InPoint, ToLight, 0.0f, Distance == std::numeric_limits<float>::max() ? 1e30f : Distance - Bias))
                        {
                            continue;
                        }
                        Light  += L.Radiance * (Cosine * Scale);
                        Toward += ToLight * Luminance(L.Radiance * (Cosine * Scale));
                    }
                    const float Norm = 1.0f / (float(InSamples) * PI);
                    Sum += Light * Norm;
                    if(OutDirection) { *OutDirection += Toward * Norm; }
                }
                return Sum;
            }

            /* Whether a texel at InPoint (normal InN, InTexel world units
             * across) is inside or under something: a short ray along its
             * surface finds the back of one -- a floor texel under a wall.
             * OutExit, if given: the nearest such back face's point, where the
             * texel's surface comes out from under it. */
            bool IsBuried(const Vec3& InPoint, const Vec3& InN, const float InTexel, Vec3* OutExit = nullptr) const
            {
                Vec3 T, B;
                Basis(InN, T, B);
                bool  bBuried = false;
                float Nearest = InTexel;
                for(int Ray = 0; Ray < 8; ++Ray)
                {
                    const float Angle = float(Ray) * (PI / 4.0f);
                    const Vec3  Dir   = Normalize(T * std::cos(Angle) + B * std::sin(Angle) + InN * 0.1f);
                    Hit H;
                    if(!Tree.Intersect(InPoint, Dir, 0.0f, InTexel, H)) { continue; }
                    const Triangle& Tri = S.Triangles[H.Triangle];
                    const float W = 1.0f - H.U - H.V;
                    if(Dot(Normalize(Tri.N[0] * W + Tri.N[1] * H.U + Tri.N[2] * H.V), Dir) <= 0.0f) { continue; }
                    if(!OutExit) { return true; }
                    bBuried = true;
                    if(H.T <= Nearest)
                    {
                        Nearest  = H.T;
                        *OutExit = InPoint + Dir * H.T;
                    }
                }
                return bBuried;
            }

            /* Whether InA and InB, two texels' points, see each other: nothing
             * between them. Smoothing spreads light only between such texels, so
             * none crosses a wall standing on (or through) the surface. */
            bool Sees(const Vec3& InA, const Vec3& InB) const
            {
                const Vec3  Delta  = InB - InA;
                const float Length = std::sqrt(Dot(Delta, Delta));
                return Length <= Bias || !Tree.Occluded(InA, Delta * (1.0f / Length), 0.0f, Length);
            }

            /* The radiance arriving at InOrigin from InDirection: the sky if
             * nothing is hit (not on a first ray, bInPrimary, when the sky is
             * Stationary: its live ambient covers that); a surface's glow,
             * plus its albedo times its own light if InBounces allows one
             * more; black off a backface. */
            Vec3 Radiance(const Vec3& InOrigin, const Vec3& InDirection, const int InBounces, Random& R, bool* OutBackface = nullptr,
                          const bool bInPrimary = false) const
            {
                Hit H;
                if(!Tree.Intersect(InOrigin, InDirection, 0.0f, 1e30f, H))
                {
                    return bInPrimary && !S.SkyLight.bBakeDirect ? Vec3() : S.SkyLight.Radiance(InDirection);
                }
                const Triangle& T = S.Triangles[H.Triangle];
                const float W = 1.0f - H.U - H.V;
                Vec3 N = Normalize(T.N[0] * W + T.N[1] * H.U + T.N[2] * H.V);
                if(Dot(N, InDirection) > 0.0f)
                {
                    if(OutBackface) { *OutBackface = true; }
                    return Vec3();
                }
                if(InBounces <= 0) { return T.Emissive; }

                const Vec3 Point = InOrigin + InDirection * H.T + N * Bias;
                Vec3 Light = Direct(Point, &N, R, 1, /*bInBakedOnly*/ false);
                Light += Radiance(Point, CosineSample(N, R), InBounces - 1, R);
                return T.Emissive + T.Albedo * Light;
            }
        };

        // === Layout ===============================================================

        bool LayOut(const Scene& InScene, const Settings& InSettings, Result& OutResult, std::string& OutError)
        {
            const int AtlasSize = std::max(InSettings.MaxAtlasSize, MIN_ATLAS_SIZE);
            struct Want { uint32_t Instance; int Resolution; };
            std::vector<Want> Wants;
            for(uint32_t Index = 0; Index < InScene.Instances.size(); ++Index)
            {
                const Instance& Each = InScene.Instances[Index];
                if(!Each.bLightmapped || Each.Count == 0 || Each.Area <= 0.0f) { continue; }
                int Resolution = int(std::ceil(std::sqrt(Each.Area) * InSettings.TexelsPerUnit));
                Resolution = std::clamp(Resolution, MIN_RESOLUTION, AtlasSize - 2);
                Wants.push_back({ Index, Resolution });
            }
            std::stable_sort(Wants.begin(), Wants.end(), [](const Want& A, const Want& B) { return A.Resolution > B.Resolution; });

            /* Shelves: a row as tall as its first (tallest) square. */
            int X = 1, Y = 1, Row = 0, UsedX = 0, UsedY = 0;
            const auto Close = [&]()
            {
                int Size = MIN_ATLAS_SIZE;
                while(Size < std::max(UsedX, UsedY) + 1) { Size *= 2; }
                OutResult.Atlases.push_back(Atlas{ std::min(Size, AtlasSize), {} });
                X = Y = 1;
                Row = UsedX = UsedY = 0;
            };
            for(const Want& Each : Wants)
            {
                if(X + Each.Resolution + 1 > AtlasSize)
                {
                    X = 1;
                    Y += Row + 1;
                    Row = 0;
                }
                if(Y + Each.Resolution + 1 > AtlasSize)
                {
                    Close();
                }
                Placement Place;
                Place.Instance   = Each.Instance;
                Place.Atlas      = uint32_t(OutResult.Atlases.size());
                Place.X          = X;
                Place.Y          = Y;
                Place.Resolution = Each.Resolution;
                OutResult.Placements.push_back(Place);
                X    += Each.Resolution + 1;
                Row   = std::max(Row, Each.Resolution);
                UsedX = std::max(UsedX, X);
                UsedY = std::max(UsedY, Y + Row);
            }
            if(!Wants.empty()) { Close(); }

            for(Atlas& Each : OutResult.Atlases)
            {
                Each.Texels.assign(size_t(Each.Size) * size_t(Each.Size) * 3, 0.0f);
            }
            for(Placement& Each : OutResult.Placements)
            {
                const float Size = float(OutResult.Atlases[Each.Atlas].Size);
                Each.ScaleOffset[0] = float(Each.Resolution) / Size;
                Each.ScaleOffset[1] = float(Each.Resolution) / Size;
                Each.ScaleOffset[2] = float(Each.X) / Size;
                Each.ScaleOffset[3] = float(Each.Y) / Size;
            }
            (void)OutError;
            return true;
        }

        // === Texels ===============================================================

        struct Texel
        {
            Vec3     Position;
            Vec3     Normal;
            float    Distance = std::numeric_limits<float>::max();   // texels off its triangle; 0 inside
            float    Size     = 1.0f;                                // world units across, for IsBuried
            uint32_t Instance = 0;
            bool     bUsed    = false;
        };

        /* The closest point to InP on the 2D triangle A B C, as barycentrics. */
        void ClosestBarycentric(const float* A, const float* B, const float* C, const float PX, const float PY,
                                float& OutW0, float& OutW1, float& OutW2, float& OutDistance)
        {
            const auto Inside = [&](float& W0, float& W1, float& W2) -> bool
            {
                const float Det = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1]);
                if(std::fabs(Det) < 1e-12f) { return false; }
                W0 = ((B[1] - C[1]) * (PX - C[0]) + (C[0] - B[0]) * (PY - C[1])) / Det;
                W1 = ((C[1] - A[1]) * (PX - C[0]) + (A[0] - C[0]) * (PY - C[1])) / Det;
                W2 = 1.0f - W0 - W1;
                return true;
            };
            float W0 = 0, W1 = 0, W2 = 0;
            if(!Inside(W0, W1, W2)) { OutDistance = std::numeric_limits<float>::max(); return; }
            if(W0 >= 0.0f && W1 >= 0.0f && W2 >= 0.0f)
            {
                OutW0 = W0; OutW1 = W1; OutW2 = W2; OutDistance = 0.0f;
                return;
            }

            /* Outside: the nearest point on the three edges. */
            OutDistance = std::numeric_limits<float>::max();
            const float* Corners[3] = { A, B, C };
            for(int Edge = 0; Edge < 3; ++Edge)
            {
                const float* P = Corners[Edge];
                const float* Q = Corners[(Edge + 1) % 3];
                const float DX = Q[0] - P[0], DY = Q[1] - P[1];
                const float Length2 = DX * DX + DY * DY;
                const float T = Length2 > 0.0f ? std::clamp(((PX - P[0]) * DX + (PY - P[1]) * DY) / Length2, 0.0f, 1.0f) : 0.0f;
                const float CX = P[0] + DX * T, CY = P[1] + DY * T;
                const float Distance = std::sqrt((PX - CX) * (PX - CX) + (PY - CY) * (PY - CY));
                if(Distance < OutDistance)
                {
                    OutDistance = Distance;
                    float Weights[3] = { 0.0f, 0.0f, 0.0f };
                    Weights[Edge] = 1.0f - T;
                    Weights[(Edge + 1) % 3] = T;
                    OutW0 = Weights[0]; OutW1 = Weights[1]; OutW2 = Weights[2];
                }
            }
        }

        void Rasterise(const Scene& InScene, const Result& InResult, const float InBias, std::vector<std::vector<Texel>>& OutTexels)
        {
            OutTexels.resize(InResult.Atlases.size());
            for(size_t Index = 0; Index < InResult.Atlases.size(); ++Index)
            {
                const int Size = InResult.Atlases[Index].Size;
                OutTexels[Index].assign(size_t(Size) * size_t(Size), Texel());
            }

            for(const Placement& Place : InResult.Placements)
            {
                const Instance& Inst = InScene.Instances[Place.Instance];
                std::vector<Texel>& Texels = OutTexels[Place.Atlas];
                const int Size = InResult.Atlases[Place.Atlas].Size;
                for(uint32_t TriIndex = Inst.First; TriIndex < Inst.First + Inst.Count; ++TriIndex)
                {
                    const Triangle& T = InScene.Triangles[TriIndex];
                    float Corner[3][2];
                    for(int C = 0; C < 3; ++C)
                    {
                        Corner[C][0] = T.UV1[C][0] * float(Place.Resolution) + float(Place.X);
                        Corner[C][1] = T.UV1[C][1] * float(Place.Resolution) + float(Place.Y);
                    }
                    const int X0 = std::max(Place.X, int(std::floor(std::min({ Corner[0][0], Corner[1][0], Corner[2][0] }))) - 1);
                    const int X1 = std::min(Place.X + Place.Resolution - 1, int(std::ceil(std::max({ Corner[0][0], Corner[1][0], Corner[2][0] }))) + 1);
                    const int Y0 = std::max(Place.Y, int(std::floor(std::min({ Corner[0][1], Corner[1][1], Corner[2][1] }))) - 1);
                    const int Y1 = std::min(Place.Y + Place.Resolution - 1, int(std::ceil(std::max({ Corner[0][1], Corner[1][1], Corner[2][1] }))) + 1);

                    const Vec3 Face = Normalize(Cross(T.P[1] - T.P[0], T.P[2] - T.P[0]));
                    /* World area over texel area: how big a texel is on this triangle. */
                    const Vec3  WorldCross = Cross(T.P[1] - T.P[0], T.P[2] - T.P[0]);
                    const float UvArea = std::fabs((Corner[1][0] - Corner[0][0]) * (Corner[2][1] - Corner[0][1])
                                                   - (Corner[2][0] - Corner[0][0]) * (Corner[1][1] - Corner[0][1]));
                    const float TexelSize = UvArea > 1e-12f ? std::sqrt(std::sqrt(Dot(WorldCross, WorldCross)) / UvArea) : 1.0f;
                    for(int Y = Y0; Y <= Y1; ++Y)
                    {
                        for(int X = X0; X <= X1; ++X)
                        {
                            float W0 = 0, W1 = 0, W2 = 0, Distance = 0;
                            ClosestBarycentric(Corner[0], Corner[1], Corner[2], float(X) + 0.5f, float(Y) + 0.5f, W0, W1, W2, Distance);
                            if(Distance > 0.75f) { continue; }
                            Texel& Out = Texels[size_t(Y) * size_t(Size) + size_t(X)];
                            if(Out.bUsed && Out.Distance <= Distance) { continue; }
                            Out.Normal   = Normalize(T.N[0] * W0 + T.N[1] * W1 + T.N[2] * W2);
                            const Vec3 Off = Dot(Face, Out.Normal) >= 0.0f ? Face : Face * -1.0f;
                            Out.Position = T.P[0] * W0 + T.P[1] * W1 + T.P[2] * W2 + Off * InBias;
                            Out.Distance = Distance;
                            Out.Size     = TexelSize;
                            Out.Instance = Place.Instance;
                            Out.bUsed    = true;
                        }
                    }
                }
            }
        }

        // === Threads ==============================================================

        /* InWork(i) for i in [0, InCount) on every worker; InProgress every
         * 100 ms from this thread. False if cancelled. */
        template<typename TWork>
        bool Parallel(const size_t InCount, const int InThreads, const TWork& InWork, const char* InStage,
                      const ProgressFn& InProgress, const std::atomic<bool>* InCancel)
        {
            std::atomic<size_t> Next{0};
            std::atomic<size_t> Done{0};
            const auto Worker = [&]()
            {
                for(size_t Index = Next++; Index < InCount; Index = Next++)
                {
                    if(InCancel && InCancel->load(std::memory_order_relaxed)) { return; }
                    InWork(Index);
                    ++Done;
                }
            };
            std::vector<std::thread> Workers;
            for(int Index = 0; Index < InThreads; ++Index) { Workers.emplace_back(Worker); }
            while(Done.load() < InCount && !(InCancel && InCancel->load()))
            {
                if(InProgress) { InProgress(InStage, InCount ? float(Done.load()) / float(InCount) : 1.0f); }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            for(std::thread& Each : Workers) { Each.join(); }
            if(InProgress) { InProgress(InStage, 1.0f); }
            return !(InCancel && InCancel->load());
        }

        // === Filtering ============================================================

        /* Which texels in each texel's filter window it sees (Tracer::Sees): bit
         * (DY + FILTER_RADIUS) * FILTER_WIDTH + DX + FILTER_RADIUS. */
        constexpr int FILTER_WIDTH = 2 * FILTER_RADIUS + 1;
        static_assert(FILTER_WIDTH * FILTER_WIDTH <= 64, "a window's visibility fits a mask");
        template<typename TTracer>
        bool FindVisibility(const std::vector<Texel>& InTexels, const int InSize, const TTracer& InTracer, const int InThreads,
                            const ProgressFn& InProgress, const std::atomic<bool>* InCancel, std::vector<uint64_t>& OutVisible)
        {
            OutVisible.assign(InTexels.size(), 0);
            const auto Row = [&](const size_t InRow)
            {
                const int Y = int(InRow);
                for(int X = 0; X < InSize; ++X)
                {
                    const size_t Index = size_t(Y) * size_t(InSize) + size_t(X);
                    const Texel& Centre = InTexels[Index];
                    if(!Centre.bUsed) { continue; }
                    uint64_t Mask = 0;
                    for(int DY = -FILTER_RADIUS; DY <= FILTER_RADIUS; ++DY)
                    {
                        for(int DX = -FILTER_RADIUS; DX <= FILTER_RADIUS; ++DX)
                        {
                            const int NX = X + DX, NY = Y + DY;
                            if(NX < 0 || NY < 0 || NX >= InSize || NY >= InSize) { continue; }
                            const Texel& Near = InTexels[size_t(NY) * size_t(InSize) + size_t(NX)];
                            if(!Near.bUsed || Near.Instance != Centre.Instance) { continue; }
                            if(InTracer.Sees(Centre.Position, Near.Position))
                            {
                                Mask |= uint64_t(1) << uint32_t((DY + FILTER_RADIUS) * FILTER_WIDTH + DX + FILTER_RADIUS);
                            }
                        }
                    }
                    OutVisible[Index] = Mask;
                }
            };
            return Parallel(size_t(InSize), InThreads, Row, "Filtering", InProgress, InCancel);
        }

        void Smooth(const std::vector<Texel>& InTexels, const int InSize, const float InWorldTexel, std::vector<float>& InOutIndirect,
                    const std::vector<uint64_t>& InVisible, const int InChannels = 3)
        {
            const size_t C = size_t(InChannels);
            const std::vector<float> Source = InOutIndirect;
            const float Sigma2 = 2.0f * (2.0f * InWorldTexel) * (2.0f * InWorldTexel);
            for(int Y = 0; Y < InSize; ++Y)
            {
                for(int X = 0; X < InSize; ++X)
                {
                    const size_t Index = size_t(Y) * size_t(InSize) + size_t(X);
                    const Texel& Centre = InTexels[Index];
                    if(!Centre.bUsed) { continue; }
                    float Sum[4] = {};
                    float Weight = 0.0f;
                    for(int DY = -FILTER_RADIUS; DY <= FILTER_RADIUS; ++DY)
                    {
                        for(int DX = -FILTER_RADIUS; DX <= FILTER_RADIUS; ++DX)
                        {
                            const int NX = X + DX, NY = Y + DY;
                            if(NX < 0 || NY < 0 || NX >= InSize || NY >= InSize) { continue; }
                            const size_t Other = size_t(NY) * size_t(InSize) + size_t(NX);
                            const Texel& Near = InTexels[Other];
                            if(!Near.bUsed || Near.Instance != Centre.Instance) { continue; }
                            if(!(InVisible[Index] >> uint32_t((DY + FILTER_RADIUS) * FILTER_WIDTH + DX + FILTER_RADIUS) & 1u)) { continue; }
                            const float Facing = Dot(Centre.Normal, Near.Normal);
                            if(Facing <= 0.0f) { continue; }
                            const Vec3  Delta = Near.Position - Centre.Position;
                            const float W = std::exp(-Dot(Delta, Delta) / Sigma2) * std::pow(Facing, 8.0f);
                            for(size_t Ch = 0; Ch < C; ++Ch) { Sum[Ch] += Source[Other * C + Ch] * W; }
                            Weight += W;
                        }
                    }
                    if(Weight > 0.0f)
                    {
                        for(size_t Ch = 0; Ch < C; ++Ch) { InOutIndirect[Index * C + Ch] = Sum[Ch] / Weight; }
                    }
                }
            }
        }

        /* Empty texels next to lit ones take their average, InPasses times. */
        void Dilate(std::vector<uint8_t>& InOutFilled, const int InSize, std::vector<float>& InOutTexels, const int InChannels = 3)
        {
            const size_t C = size_t(InChannels);
            for(int Pass = 0; Pass < DILATE_PASSES; ++Pass)
            {
                std::vector<uint8_t> Was = InOutFilled;
                const std::vector<float> Source = InOutTexels;
                for(int Y = 0; Y < InSize; ++Y)
                {
                    for(int X = 0; X < InSize; ++X)
                    {
                        const size_t Index = size_t(Y) * size_t(InSize) + size_t(X);
                        if(Was[Index]) { continue; }
                        float Sum[4] = {};
                        int   Count = 0;
                        for(int DY = -1; DY <= 1; ++DY)
                        {
                            for(int DX = -1; DX <= 1; ++DX)
                            {
                                const int NX = X + DX, NY = Y + DY;
                                if(NX < 0 || NY < 0 || NX >= InSize || NY >= InSize) { continue; }
                                const size_t Other = size_t(NY) * size_t(InSize) + size_t(NX);
                                if(!Was[Other]) { continue; }
                                for(size_t Ch = 0; Ch < C; ++Ch) { Sum[Ch] += Source[Other * C + Ch]; }
                                ++Count;
                            }
                        }
                        if(Count == 0) { continue; }
                        for(size_t Ch = 0; Ch < C; ++Ch) { InOutTexels[Index * C + Ch] = Sum[Ch] / float(Count); }
                        InOutFilled[Index] = 1;
                    }
                }
            }
        }

        // === Probes ===============================================================

        void PlaceProbes(const Scene& InScene, const Settings& InSettings, RenderCore::BakedProbeGrid& OutGrid)
        {
            OutGrid = RenderCore::BakedProbeGrid();
            if(InScene.Triangles.empty()) { return; }

            const Vec3 Extent = InScene.BoundsMax - InScene.BoundsMin;
            float Spacing = std::max(InSettings.ProbeSpacing, 1.0f);
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                Spacing = std::max(Spacing, Extent[Axis] / float(MAX_PROBES_AXIS - 1));
            }
            uint32_t Counts[3];
            for(;;)
            {
                uint64_t Total = 1;
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    Counts[Axis] = uint32_t(std::floor(Extent[Axis] / Spacing)) + 2;
                    Total *= Counts[Axis];
                }
                if(Total <= uint64_t(MAX_PROBES)) { break; }
                Spacing *= 1.25f;
            }
            OutGrid.Spacing = Spacing;
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                OutGrid.Counts[Axis] = Counts[Axis];
                const float Span = float(Counts[Axis] - 1) * Spacing;
                OutGrid.Origin[Axis] = InScene.BoundsMin[Axis] - (Span - Extent[Axis]) * 0.5f;
            }
            const size_t Count = size_t(Counts[0]) * Counts[1] * Counts[2];
            OutGrid.Probes.assign(Count, RenderCore::BakedAmbientCube());
            OutGrid.Valid.assign(Count, 1);
        }

        void LightProbe(const Tracer& InTracer, const Settings& InSettings, const size_t InIndex, RenderCore::BakedProbeGrid& InOutGrid)
        {
            const uint32_t CX = InOutGrid.Counts[0], CY = InOutGrid.Counts[1];
            const uint32_t I = uint32_t(InIndex % CX);
            const uint32_t J = uint32_t((InIndex / CX) % CY);
            const uint32_t K = uint32_t(InIndex / (size_t(CX) * CY));
            const Vec3 Point(InOutGrid.Origin[0] + float(I) * InOutGrid.Spacing,
                             InOutGrid.Origin[1] + float(J) * InOutGrid.Spacing,
                             InOutGrid.Origin[2] + float(K) * InOutGrid.Spacing);

            static const Vec3 Axes[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
            Random R(Mix(0x9e3779b97f4a7c15ull ^ uint64_t(InIndex)));
            const int Rays = std::max(64, InSettings.SamplesPerTexel * 2);

            Vec3 Faces[6];
            int  Backfaces = 0;
            for(int Ray = 0; Ray < Rays; ++Ray)
            {
                const Vec3 Dir = SphereSample(R);
                bool bBack = false;
                const Vec3 L = InTracer.Radiance(Point, Dir, InSettings.Bounces, R, &bBack, /*bInPrimary*/ true);
                if(bBack) { ++Backfaces; }
                for(int Face = 0; Face < 6; ++Face)
                {
                    const float Cosine = Dot(Dir, Axes[Face]);
                    if(Cosine > 0.0f) { Faces[Face] += L * Cosine; }
                }
            }

            RenderCore::BakedAmbientCube& Cube = InOutGrid.Probes[InIndex];
            for(int Face = 0; Face < 6; ++Face)
            {
                /* Uniform over the sphere: irradiance over pi is 4/N times the cosine-weighted sum. */
                Vec3 Value = Faces[Face] * (4.0f / float(Rays));
                Value += InTracer.Direct(Point, &Axes[Face], R, InTracer.DirectSamples, /*bInBakedOnly*/ true);
                Cube.Faces[Face][0] = Value.X;
                Cube.Faces[Face][1] = Value.Y;
                Cube.Faces[Face][2] = Value.Z;
            }
            InOutGrid.Valid[InIndex] = float(Backfaces) / float(Rays) < BACKFACE_INSIDE ? 1 : 0;
        }
    }

    EBakeStatus Bake(const Scene& InScene, const Settings& InSettings, Result& OutResult, std::string& OutError,
                     const ProgressFn& InProgress, const std::atomic<bool>* InCancel)
    {
        OutResult = Result();
        const int Threads = InSettings.Threads > 0 ? InSettings.Threads : std::max(1, int(std::thread::hardware_concurrency()));

        if(InProgress) { InProgress("Building the ray tracing structure", 0.0f); }
        BVH Tree;
        Tree.Build(InScene.Triangles);

        if(!LayOut(InScene, InSettings, OutResult, OutError)) { return EBakeStatus::Failed; }

        const float WorldTexel = 1.0f / std::max(InSettings.TexelsPerUnit, 1e-4f);
        Tracer T{ InScene, Tree, InSettings, std::clamp(WorldTexel * 0.05f, 0.05f, 2.0f), 1 };
        bool bSoft = false;
        for(const Light& L : InScene.Lights) { bSoft |= L.SourceSize > 0.0f; }
        T.DirectSamples = bSoft ? std::clamp(InSettings.SamplesPerTexel / 4, 4, 32) : 1;

        std::vector<std::vector<Texel>> Texels;
        Rasterise(InScene, OutResult, T.Bias, Texels);

        // --- Light every used texel: direct and bounced kept apart for filtering.
        for(size_t AtlasIndex = 0; AtlasIndex < OutResult.Atlases.size(); ++AtlasIndex)
        {
            Atlas& A = OutResult.Atlases[AtlasIndex];
            std::vector<Texel>& Used = Texels[AtlasIndex];
            std::vector<float> Indirect(A.Texels.size(), 0.0f);
            /* Per texel, the light's direction times its luminance and the
             * luminance itself: direct (exact) and bounced (smoothed). */
            const size_t TexelCount = size_t(A.Size) * size_t(A.Size);
            std::vector<float> DirectWay(TexelCount * 4, 0.0f), BouncedWay(TexelCount * 4, 0.0f);
            const int Samples = std::max(1, InSettings.SamplesPerTexel);

            const auto LightRow = [&](const size_t InRow)
            {
                for(int X = 0; X < A.Size; ++X)
                {
                    const size_t Index = InRow * size_t(A.Size) + size_t(X);
                    Texel& Each = Used[Index];
                    if(!Each.bUsed) { continue; }
                    /* Its centre inside or under something (a floor texel under a
                     * wall standing through it): lit at the nearest point it comes
                     * out from under, if that is still within the texel and in the
                     * open -- so a texel the wall straddles takes the light of the
                     * side it is on, not its neighbours' from both. Wholly buried,
                     * it is left out and filled from its neighbours. */
                    const float Reach = std::max(Each.Size, T.Bias * 4.0f);
                    Vec3 Exit;
                    if(T.IsBuried(Each.Position, Each.Normal, Reach, &Exit))
                    {
                        Vec3 Out = Exit - Each.Position;
                        Out = Out - Each.Normal * Dot(Out, Each.Normal);
                        const float Moved = std::sqrt(Dot(Out, Out));
                        const Vec3 Outside = Moved > 1e-6f ? Each.Position + Out * ((Moved + T.Bias * 2.0f) / Moved) : Each.Position;
                        if(Moved <= 1e-6f || Moved + T.Bias * 2.0f > Each.Size * 0.75f || T.IsBuried(Outside, Each.Normal, Reach))
                        {
                            Each.bUsed = false;
                            continue;
                        }
                        Each.Position = Outside;
                    }
                    Random R(Mix((uint64_t(AtlasIndex) << 40) ^ uint64_t(Index)));
                    Vec3 DirectToward;
                    const Vec3 Direct = T.Direct(Each.Position, &Each.Normal, R, T.DirectSamples, /*bInBakedOnly*/ true, &DirectToward);
                    Vec3 Bounced, BouncedToward;
                    for(int Sample = 0; Sample < Samples; ++Sample)
                    {
                        const Vec3 Dir = CosineSample(Each.Normal, R);
                        const Vec3 L   = T.Radiance(Each.Position, Dir, InSettings.Bounces, R, nullptr, /*bInPrimary*/ true);
                        Bounced       += L;
                        BouncedToward += Dir * Luminance(L);
                    }
                    const float PerSample = 1.0f / float(Samples);
                    Bounced       = Bounced * PerSample;
                    BouncedToward = BouncedToward * PerSample;
                    A.Texels[Index * 3]     = Direct.X;
                    A.Texels[Index * 3 + 1] = Direct.Y;
                    A.Texels[Index * 3 + 2] = Direct.Z;
                    Indirect[Index * 3]     = Bounced.X;
                    Indirect[Index * 3 + 1] = Bounced.Y;
                    Indirect[Index * 3 + 2] = Bounced.Z;
                    const float DirectWayAndLum[4]  = { DirectToward.X, DirectToward.Y, DirectToward.Z, Luminance(Direct) };
                    const float BouncedWayAndLum[4] = { BouncedToward.X, BouncedToward.Y, BouncedToward.Z, Luminance(Bounced) };
                    std::copy_n(DirectWayAndLum, 4, &DirectWay[Index * 4]);
                    std::copy_n(BouncedWayAndLum, 4, &BouncedWay[Index * 4]);
                }
            };
            const std::string Stage = OutResult.Atlases.size() > 1
                                    ? "Lighting lightmap " + std::to_string(AtlasIndex + 1) + " of " + std::to_string(OutResult.Atlases.size())
                                    : std::string("Lighting the lightmap");
            if(!Parallel(size_t(A.Size), Threads, LightRow, Stage.c_str(), InProgress, InCancel)) { return EBakeStatus::Cancelled; }

            if(InProgress) { InProgress("Filtering", 0.0f); }
            std::vector<uint64_t> Visible;
            if(!FindVisibility(Used, A.Size, T, Threads, InProgress, InCancel, Visible)) { return EBakeStatus::Cancelled; }
            Smooth(Used, A.Size, WorldTexel, Indirect, Visible);
            Smooth(Used, A.Size, WorldTexel, BouncedWay, Visible, 4);
            std::vector<uint8_t> Filled(Used.size(), 0);
            A.Directions.assign(TexelCount * 4, 0.0f);
            for(size_t Index = 0; Index < Used.size(); ++Index)
            {
                Filled[Index] = Used[Index].bUsed ? 1 : 0;
                for(int Channel = 0; Channel < 3; ++Channel) { A.Texels[Index * 3 + size_t(Channel)] += Indirect[Index * 3 + size_t(Channel)]; }

                /* The dominant direction, as long as the light is one-sided
                 * (1: all from there; 0: from all round), and the half-Lambert
                 * at the texel's own normal, so a flat normal reads the bake as is. */
                const float Weight = DirectWay[Index * 4 + 3] + BouncedWay[Index * 4 + 3];
                Vec3 Way(DirectWay[Index * 4] + BouncedWay[Index * 4], DirectWay[Index * 4 + 1] + BouncedWay[Index * 4 + 1],
                         DirectWay[Index * 4 + 2] + BouncedWay[Index * 4 + 2]);
                Way = Weight > 1e-8f ? Way * (1.0f / Weight) : Vec3();
                const float Length = std::sqrt(Dot(Way, Way));
                if(Length > 1.0f) { Way = Way * (1.0f / Length); }
                float* Out = &A.Directions[Index * 4];
                Out[0] = Way.X;
                Out[1] = Way.Y;
                Out[2] = Way.Z;
                Out[3] = Used[Index].bUsed ? Dot(Used[Index].Normal, Way) * 0.5f + 0.5f : 0.5f;
            }
            std::vector<uint8_t> DirectionsFilled = Filled;
            Dilate(Filled, A.Size, A.Texels);
            Dilate(DirectionsFilled, A.Size, A.Directions, 4);
        }

        // --- Probes, for what the lightmaps do not cover.
        PlaceProbes(InScene, InSettings, OutResult.Probes);
        const auto Probe = [&](const size_t InIndex) { LightProbe(T, InSettings, InIndex, OutResult.Probes); };
        if(!Parallel(OutResult.Probes.Probes.size(), Threads, Probe, "Lighting probes", InProgress, InCancel))
        {
            return EBakeStatus::Cancelled;
        }
        return EBakeStatus::Done;
    }
}

#endif // WITH_EDITOR
