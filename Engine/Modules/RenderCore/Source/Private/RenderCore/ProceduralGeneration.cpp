#include "Public/RenderCore/ProceduralGeneration.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

/* GenerateProcedural: candidates from the source, the cheap filters, the
 * drop onto surfaces, the rest of the filters, then a transform each and a
 * cell to draw it in. */

namespace RenderCore
{
    namespace
    {
        constexpr float Pi = 3.14159265358979f;

        /* Above this many candidate points the rules are refused, not run: Spacing is far too small for the area. */
        constexpr double CANDIDATES_MAX = 20.0e6;

        struct Float3
        {
            float X = 0.0f, Y = 0.0f, Z = 0.0f;
            Float3() = default;
            Float3(const float InX, const float InY, const float InZ) : X(InX), Y(InY), Z(InZ) {}
            explicit Float3(const EngineCore::Vector3& InV) : X(float(InV.GetX())), Y(float(InV.GetY())), Z(float(InV.GetZ())) {}
            Float3 operator+(const Float3& InO) const { return Float3(X + InO.X, Y + InO.Y, Z + InO.Z); }
            Float3 operator-(const Float3& InO) const { return Float3(X - InO.X, Y - InO.Y, Z - InO.Z); }
            Float3 operator*(const float InS) const { return Float3(X * InS, Y * InS, Z * InS); }
            float  Dot(const Float3& InO) const { return X * InO.X + Y * InO.Y + Z * InO.Z; }
            Float3 Cross(const Float3& InO) const { return Float3(Y * InO.Z - Z * InO.Y, Z * InO.X - X * InO.Z, X * InO.Y - Y * InO.X); }
            float  Length() const { return std::sqrt(X * X + Y * Y + Z * Z); }
            Float3 Normalized(const Float3& InFallback = Float3(0.0f, 1.0f, 0.0f)) const
            {
                const float L = Length();
                return L > 1e-8f ? Float3(X / L, Y / L, Z / L) : InFallback;
            }
        };

        /* Row-major, column vectors, as Matrix4x4 -- floats, for the hot loops. */
        struct Affine
        {
            float M[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };

            static Affine From(const EngineCore::Matrix4x4& InMatrix)
            {
                Affine Out;
                for(int Row = 0; Row < 3; ++Row)
                {
                    for(int Col = 0; Col < 4; ++Col) { Out.M[Row][Col] = float(InMatrix.Data[Row][Col]); }
                }
                return Out;
            }
            Float3 Point(const Float3& InP) const
            {
                return Float3(M[0][0] * InP.X + M[0][1] * InP.Y + M[0][2] * InP.Z + M[0][3],
                              M[1][0] * InP.X + M[1][1] * InP.Y + M[1][2] * InP.Z + M[1][3],
                              M[2][0] * InP.X + M[2][1] * InP.Y + M[2][2] * InP.Z + M[2][3]);
            }
            Float3 Vector(const Float3& InV) const
            {
                return Float3(M[0][0] * InV.X + M[0][1] * InV.Y + M[0][2] * InV.Z,
                              M[1][0] * InV.X + M[1][1] * InV.Y + M[1][2] * InV.Z,
                              M[2][0] * InV.X + M[2][1] * InV.Y + M[2][2] * InV.Z);
            }
            /* A normal through the matrix this is the INVERSE of: this matrix's transpose. */
            Float3 NormalFromInverse(const Float3& InN) const
            {
                return Float3(M[0][0] * InN.X + M[1][0] * InN.Y + M[2][0] * InN.Z,
                              M[0][1] * InN.X + M[1][1] * InN.Y + M[2][1] * InN.Z,
                              M[0][2] * InN.X + M[1][2] * InN.Y + M[2][2] * InN.Z).Normalized();
            }
            float ColumnLength(const int InCol) const
            {
                return std::sqrt(M[0][InCol] * M[0][InCol] + M[1][InCol] * M[1][InCol] + M[2][InCol] * M[2][InCol]);
            }
        };

        // === Randomness: hashes of where a point came from, never a running stream ===

        uint32_t Mix(uint32_t InH)
        {
            InH ^= InH >> 16;
            InH *= 0x7FEB352Du;
            InH ^= InH >> 15;
            InH *= 0x846CA68Bu;
            InH ^= InH >> 16;
            return InH;
        }

        uint32_t Hash(const uint32_t InA, const uint32_t InB, const uint32_t InC = 0, const uint32_t InD = 0)
        {
            uint32_t H = Mix(InA * 0x9E3779B1u + 0x632BE5ABu);
            H = Mix(H ^ (InB * 0x85EBCA77u));
            H = Mix(H ^ (InC * 0xC2B2AE3Du));
            return Mix(H ^ (InD * 0x27D4EB2Fu));
        }

        float Unit(const uint32_t InH) { return float(InH & 0xFFFFFFu) / float(0x1000000); }

        /* A stream of fractions for one candidate: salt by salt. */
        float Random(const uint32_t InKey, const uint32_t InSalt) { return Unit(Hash(InKey, InSalt, 0xA511E9B3u)); }

        /* Smooth value noise, two octaves, 0..1, about InScale units a patch. */
        float Noise(const float InX, const float InZ, const float InScale, const uint32_t InSeed)
        {
            float Sum = 0.0f, Weight = 0.0f, Amplitude = 0.65f, Frequency = 1.0f / InScale;
            for(uint32_t Octave = 0; Octave < 2; ++Octave)
            {
                const float X = InX * Frequency, Z = InZ * Frequency;
                const float FX = std::floor(X), FZ = std::floor(Z);
                const int32_t IX = int32_t(FX), IZ = int32_t(FZ);
                float TX = X - FX, TZ = Z - FZ;
                TX = TX * TX * (3.0f - 2.0f * TX);
                TZ = TZ * TZ * (3.0f - 2.0f * TZ);
                const auto Corner = [&](const int32_t InX2, const int32_t InZ2)
                {
                    return Unit(Hash(uint32_t(InX2), uint32_t(InZ2), InSeed, 0x7015Eu + Octave));
                };
                const float A = Corner(IX, IZ),     B = Corner(IX + 1, IZ);
                const float C = Corner(IX, IZ + 1), D = Corner(IX + 1, IZ + 1);
                const float Value = (A + (B - A) * TX) + ((C + (D - C) * TX) - (A + (B - A) * TX)) * TZ;
                Sum += Value * Amplitude;
                Weight += Amplitude;
                Amplitude *= 0.35f / 0.65f;
                Frequency *= 2.7f;
            }
            return Sum / Weight;
        }

        // === Surfaces =================================================================

        struct Triangle
        {
            Float3 A, B, C;
            Float3 Normal;   // facing up where it can
        };

        /* Static meshes' triangles in world space, bucketed on X/Z, for straight-down rays. */
        class MeshSurfaceGrid
        {
        public:
            void Build(const std::vector<ProceduralMeshInput>& InMeshes, const float (&InMin)[2], const float (&InMax)[2])
            {
                MinX = InMin[0];
                MinZ = InMin[1];
                const float Width = std::max(InMax[0] - InMin[0], 1.0f);
                const float Depth = std::max(InMax[1] - InMin[1], 1.0f);
                Cell    = std::max(std::max(Width, Depth) / 256.0f, 25.0f);
                Columns = std::max(int32_t(std::ceil(Width / Cell)), 1);
                Rows    = std::max(int32_t(std::ceil(Depth / Cell)), 1);

                for(const ProceduralMeshInput& Each : InMeshes)
                {
                    if(!Each.Mesh) { continue; }
                    const Affine ToWorld = Affine::From(Each.ToWorld);
                    const MeshData& Mesh = *Each.Mesh;
                    const size_t Vertices = Mesh.Positions.size() / 3;
                    for(size_t Index = 0; Index + 2 < Mesh.Indices.size(); Index += 3)
                    {
                        const uint32_t I0 = Mesh.Indices[Index], I1 = Mesh.Indices[Index + 1], I2 = Mesh.Indices[Index + 2];
                        if(I0 >= Vertices || I1 >= Vertices || I2 >= Vertices) { continue; }
                        Triangle Tri;
                        Tri.A = ToWorld.Point(Float3(Mesh.Positions[I0 * 3], Mesh.Positions[I0 * 3 + 1], Mesh.Positions[I0 * 3 + 2]));
                        Tri.B = ToWorld.Point(Float3(Mesh.Positions[I1 * 3], Mesh.Positions[I1 * 3 + 1], Mesh.Positions[I1 * 3 + 2]));
                        Tri.C = ToWorld.Point(Float3(Mesh.Positions[I2 * 3], Mesh.Positions[I2 * 3 + 1], Mesh.Positions[I2 * 3 + 2]));
                        const float Lo[2] = { std::min({ Tri.A.X, Tri.B.X, Tri.C.X }), std::min({ Tri.A.Z, Tri.B.Z, Tri.C.Z }) };
                        const float Hi[2] = { std::max({ Tri.A.X, Tri.B.X, Tri.C.X }), std::max({ Tri.A.Z, Tri.B.Z, Tri.C.Z }) };
                        if(Hi[0] < InMin[0] || Lo[0] > InMax[0] || Hi[1] < InMin[1] || Lo[1] > InMax[1]) { continue; }
                        /* Seen from above it must have some area, or no straight-down ray lands on it. */
                        const Float3 Face = (Tri.B - Tri.A).Cross(Tri.C - Tri.A);
                        if(std::fabs(Face.Y) < 1e-6f) { continue; }
                        Tri.Normal = (Face.Y < 0.0f ? Face * -1.0f : Face).Normalized();
                        Triangles.push_back(Tri);
                        Bounds.push_back({ { Lo[0], Lo[1] }, { Hi[0], Hi[1] } });
                    }
                }

                /* Counting sort into the cells each triangle's box covers. */
                Start.assign(size_t(Columns) * Rows + 1, 0u);
                for(int Pass = 0; Pass < 2; ++Pass)
                {
                    std::vector<uint32_t> Fill;
                    if(Pass == 1)
                    {
                        for(size_t Index = 1; Index < Start.size(); ++Index) { Start[Index] += Start[Index - 1]; }
                        Items.assign(Start.back(), 0u);
                        Fill.assign(Start.begin(), Start.end() - 1);
                    }
                    for(size_t Tri = 0; Tri < Triangles.size(); ++Tri)
                    {
                        const int32_t X0 = CellX(Bounds[Tri].Lo[0]), X1 = CellX(Bounds[Tri].Hi[0]);
                        const int32_t Z0 = CellZ(Bounds[Tri].Lo[1]), Z1 = CellZ(Bounds[Tri].Hi[1]);
                        for(int32_t Z = Z0; Z <= Z1; ++Z)
                        {
                            for(int32_t X = X0; X <= X1; ++X)
                            {
                                const size_t Slot = size_t(Z) * Columns + X;
                                if(Pass == 0) { ++Start[Slot + 1]; }
                                else          { Items[Fill[Slot]++] = uint32_t(Tri); }
                            }
                        }
                    }
                }
                Bounds.clear();
            }

            bool IsEmpty() const { return Triangles.empty(); }

            /* The highest triangle under (InX, InZ) between InTop and InBottom. */
            bool Down(const float InX, const float InZ, const float InTop, const float InBottom, float& OutY, Float3& OutNormal) const
            {
                if(Triangles.empty()) { return false; }
                const float FX = (InX - MinX) / Cell, FZ = (InZ - MinZ) / Cell;
                if(FX < 0.0f || FZ < 0.0f || FX >= float(Columns) || FZ >= float(Rows)) { return false; }
                const size_t Slot = size_t(int32_t(FZ)) * Columns + size_t(int32_t(FX));
                bool bHit = false;
                for(uint32_t Item = Start[Slot]; Item < Start[Slot + 1]; ++Item)
                {
                    const Triangle& Tri = Triangles[Items[Item]];
                    /* Barycentric on X/Z. */
                    const float D  = (Tri.B.Z - Tri.C.Z) * (Tri.A.X - Tri.C.X) + (Tri.C.X - Tri.B.X) * (Tri.A.Z - Tri.C.Z);
                    if(std::fabs(D) < 1e-12f) { continue; }
                    const float W0 = ((Tri.B.Z - Tri.C.Z) * (InX - Tri.C.X) + (Tri.C.X - Tri.B.X) * (InZ - Tri.C.Z)) / D;
                    const float W1 = ((Tri.C.Z - Tri.A.Z) * (InX - Tri.C.X) + (Tri.A.X - Tri.C.X) * (InZ - Tri.C.Z)) / D;
                    const float W2 = 1.0f - W0 - W1;
                    if(W0 < -1e-5f || W1 < -1e-5f || W2 < -1e-5f) { continue; }
                    const float Y = Tri.A.Y * W0 + Tri.B.Y * W1 + Tri.C.Y * W2;
                    if(Y > InTop || Y < InBottom || (bHit && Y <= OutY)) { continue; }
                    OutY      = Y;
                    OutNormal = Tri.Normal;
                    bHit      = true;
                }
                return bHit;
            }

        private:
            struct Box2 { float Lo[2]; float Hi[2]; };

            int32_t CellX(const float InX) const { return std::clamp(int32_t((InX - MinX) / Cell), 0, Columns - 1); }
            int32_t CellZ(const float InZ) const { return std::clamp(int32_t((InZ - MinZ) / Cell), 0, Rows - 1); }

            std::vector<Triangle> Triangles;
            std::vector<Box2>     Bounds;
            std::vector<uint32_t> Start;
            std::vector<uint32_t> Items;
            float   MinX = 0.0f, MinZ = 0.0f, Cell = 100.0f;
            int32_t Columns = 1, Rows = 1;
        };

        /* What a straight-down ray landed on. */
        struct SurfaceHit
        {
            float  Y = 0.0f;
            Float3 Normal{ 0.0f, 1.0f, 0.0f };
            int32_t Terrain = -1;                 // which, if a terrain
            float  LayerWeights[GameCore::TERRAIN_LAYERS] = {};
        };

        bool DropOntoTerrains(const std::vector<ProceduralTerrainInput>& InTerrains, const std::vector<Affine>& InToLocal,
                              const float InX, const float InZ,
                              const float InTop, const float InBottom, SurfaceHit& InOutHit, const bool bInHaveHit)
        {
            bool bHit = false;
            const float Length = InTop - InBottom;
            if(!(Length > 0.0f)) { return false; }
            for(size_t Index = 0; Index < InTerrains.size(); ++Index)
            {
                const ProceduralTerrainInput& Each = InTerrains[Index];
                if(!Each.Data) { continue; }
                const Float3 Origin = InToLocal[Index].Point(Float3(InX, InTop, InZ));
                const Float3 Dir    = InToLocal[Index].Vector(Float3(0.0f, -1.0f, 0.0f));
                const float O[3] = { Origin.X, Origin.Y, Origin.Z };
                const float D[3] = { Dir.X, Dir.Y, Dir.Z };
                float Distance = 0.0f, Normal[3] = { 0.0f, 1.0f, 0.0f };
                float Weights[GameCore::TERRAIN_LAYERS] = {};
                {
                    std::lock_guard<std::mutex> Guard(Each.Data->GetLock());
                    if(Each.Data->IsEmpty() || !Each.Data->Raycast(O, D, Length, Distance, Normal)) { continue; }
                    const Float3 Local = Origin + Dir * Distance;
                    Each.Data->SampleWeights(Local.X, Local.Z, Weights);
                }
                const float Y = InTop - Distance;
                if((bInHaveHit || bHit) && Y <= InOutHit.Y) { continue; }
                InOutHit.Y       = Y;
                InOutHit.Normal  = InToLocal[Index].NormalFromInverse(Float3(Normal[0], Normal[1], Normal[2]));
                if(InOutHit.Normal.Y < 0.0f) { InOutHit.Normal = InOutHit.Normal * -1.0f; }
                InOutHit.Terrain = int32_t(Index);
                std::memcpy(InOutHit.LayerWeights, Weights, sizeof(Weights));
                bHit = true;
            }
            return bHit;
        }

        /* A candidate point before the drop: where, which way is up, and (along a path) which way it runs. */
        struct Candidate
        {
            Float3   Position;
            Float3   Normal{ 0.0f, 1.0f, 0.0f };
            Float3   Tangent{ 0.0f, 0.0f, 1.0f };
            float    Top    = 0.0f;   // where a drop starts and ends (world Y)
            float    Bottom = 0.0f;
            uint32_t Key    = 0;
            bool     bProject = false;
        };

        /* The world-space X/Z box the source covers, for the mesh grid. */
        struct Footprint
        {
            float Min[2] = {  1e30f,  1e30f };
            float Max[2] = { -1e30f, -1e30f };
            void Add(const float InX, const float InZ)
            {
                Min[0] = std::min(Min[0], InX); Min[1] = std::min(Min[1], InZ);
                Max[0] = std::max(Max[0], InX); Max[1] = std::max(Max[1], InZ);
            }
            void Grow(const float InBy) { Min[0] -= InBy; Min[1] -= InBy; Max[0] += InBy; Max[1] += InBy; }
            bool IsEmpty() const { return Min[0] > Max[0]; }
        };

        /* An instance's world matrix: yawed, tilted toward InUp, scaled, and sunk along InUp. */
        void BuildWorldMatrix(const Float3& InPosition, const Float3& InUp, const float InYawDegrees, const float InScale,
                              const float InSink, float (&OutRows)[3][4])
        {
            const float Yaw = InYawDegrees * Pi / 180.0f;
            const float CY = std::cos(Yaw), SY = std::sin(Yaw);
            Float3 Axes[3] = { Float3(CY, 0.0f, -SY), Float3(0.0f, 1.0f, 0.0f), Float3(SY, 0.0f, CY) };

            /* Rodrigues: the turn that takes +Y to Up, about Y x Up. */
            const Float3 K(InUp.Z, 0.0f, -InUp.X);
            const float SinA = std::sqrt(K.X * K.X + K.Z * K.Z);
            const float CosA = InUp.Y;
            if(SinA > 1e-6f)
            {
                const Float3 N(K.X / SinA, 0.0f, K.Z / SinA);
                for(Float3& V : Axes)
                {
                    const float Dot = N.Dot(V);
                    V = V * CosA + N.Cross(V) * SinA + N * (Dot * (1.0f - CosA));
                }
            }
            const Float3 At = InPosition - InUp * (InSink * InScale);
            for(int Row = 0; Row < 3; ++Row)
            {
                const float A[3] = { Axes[0].X, Axes[0].Y, Axes[0].Z };
                const float B[3] = { Axes[1].X, Axes[1].Y, Axes[1].Z };
                const float C[3] = { Axes[2].X, Axes[2].Y, Axes[2].Z };
                OutRows[Row][0] = A[Row] * InScale;
                OutRows[Row][1] = B[Row] * InScale;
                OutRows[Row][2] = C[Row] * InScale;
            }
            OutRows[0][3] = At.X;
            OutRows[1][3] = At.Y;
            OutRows[2][3] = At.Z;
        }

        int64_t CellKey(const int32_t InX, const int32_t InZ) { return (int64_t(InX) << 32) ^ int64_t(uint32_t(InZ)); }
    }

    void GenerateProcedural(const ProceduralInputs& InInputs, ProceduralResult& OutResult)
    {
        const auto Started = std::chrono::steady_clock::now();
        OutResult = ProceduralResult();
        const ProceduralSpawnerComponent& S = InInputs.Settings;

        /* The entries that can be placed, and their weights. */
        float TotalWeight = 0.0f;
        for(uint32_t Entry = 0; Entry < PROCEDURAL_MESHES_MAX; ++Entry)
        {
            if(InInputs.Entries[Entry].bValid && S.Meshes[Entry].Weight > 0.0f) { TotalWeight += S.Meshes[Entry].Weight; }
        }
        if(!(TotalWeight > 0.0f))
        {
            OutResult.Problem = "no Meshes entry names a mesh that loads";
            return;
        }

        const Affine ToWorld = Affine::From(InInputs.ToWorld);
        const Affine ToLocal = Affine::From(EngineCore::Matrix4x4::Inverse(InInputs.ToWorld));
        const float  ScaleXZ = std::max(0.5f * (ToWorld.ColumnLength(0) + ToWorld.ColumnLength(2)), 1e-6f);
        const Float3 EntityUp = ToWorld.Vector(Float3(0.0f, 1.0f, 0.0f)).Normalized();
        const float  Spacing = std::max(S.Spacing, 1.0f);
        const float  Jitter  = std::clamp(S.Jitter, 0.0f, 1.0f);
        const bool   bProjectTerrain = (S.Project == PGP_Terrain || S.Project == PGP_All) && !InInputs.Terrains.empty();
        const bool   bProjectMeshes  = (S.Project == PGP_Meshes || S.Project == PGP_All) && !InInputs.Meshes.empty();
        const bool   bProject        = S.Project != PGP_None && S.Source != PGS_MeshSurface;

        // === Candidates ===========================================================
        std::vector<Candidate> Candidates;
        Footprint Area;
        const auto TooMany = [&OutResult](const double InCount)
        {
            if(InCount <= CANDIDATES_MAX) { return false; }
            OutResult.Problem = "Spacing is far too small for the area (over 20 million points)";
            return true;
        };

        if(S.Source == PGS_Box)
        {
            const Float3 Extent(std::fabs(float(S.BoxExtent.GetX())), std::fabs(float(S.BoxExtent.GetY())), std::fabs(float(S.BoxExtent.GetZ())));
            const float Step = Spacing / ScaleXZ;   // entity units
            const int32_t X0 = int32_t(std::floor(-Extent.X / Step)), X1 = int32_t(std::ceil(Extent.X / Step));
            const int32_t Z0 = int32_t(std::floor(-Extent.Z / Step)), Z1 = int32_t(std::ceil(Extent.Z / Step));
            if(TooMany(double(X1 - X0) * double(Z1 - Z0))) { return; }
            float Top = -1e30f, Bottom = 1e30f;
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const Float3 P = ToWorld.Point(Float3((Corner & 1) ? Extent.X : -Extent.X, (Corner & 2) ? Extent.Y : -Extent.Y,
                                                      (Corner & 4) ? Extent.Z : -Extent.Z));
                Top = std::max(Top, P.Y);
                Bottom = std::min(Bottom, P.Y);
                Area.Add(P.X, P.Z);
            }
            Candidates.reserve(size_t(X1 - X0) * size_t(Z1 - Z0));
            for(int32_t Z = Z0; Z < Z1; ++Z)
            {
                for(int32_t X = X0; X < X1; ++X)
                {
                    Candidate C;
                    C.Key = Hash(uint32_t(X), uint32_t(Z), uint32_t(S.Seed), 0xB0Bu);
                    const float LX = (float(X) + 0.5f + (Random(C.Key, 1) - 0.5f) * Jitter) * Step;
                    const float LZ = (float(Z) + 0.5f + (Random(C.Key, 2) - 0.5f) * Jitter) * Step;
                    if(std::fabs(LX) > Extent.X || std::fabs(LZ) > Extent.Z) { continue; }
                    C.Position = ToWorld.Point(Float3(LX, 0.0f, LZ));
                    C.Normal   = EntityUp;
                    C.Top      = Top;
                    C.Bottom   = Bottom;
                    C.bProject = bProject;
                    Candidates.push_back(C);
                }
            }
        }
        else if(S.Source == PGS_SplineArea)
        {
            if(!InInputs.bHasSpline || !InInputs.Spline.IsClosed())
            {
                OutResult.Problem = "Spline Area needs a closed Spline (three points or more) on its entity";
                return;
            }
            EngineCore::Vector3 Lo, Hi;
            InInputs.Spline.GetBounds(Lo, Hi);
            const float Step = Spacing / ScaleXZ;
            const int32_t X0 = int32_t(std::floor(float(Lo.GetX()) / Step)), X1 = int32_t(std::ceil(float(Hi.GetX()) / Step));
            const int32_t Z0 = int32_t(std::floor(float(Lo.GetZ()) / Step)), Z1 = int32_t(std::ceil(float(Hi.GetZ()) / Step));
            if(TooMany(double(X1 - X0) * double(Z1 - Z0))) { return; }

            /* The plane it lies in when nothing is under it: its points' mean height. */
            float MeanY = 0.0f;
            for(const EngineCore::Vector3& Each : InInputs.Spline.GetPoints()) { MeanY += float(Each.GetY()); }
            MeanY /= float(std::max<size_t>(InInputs.Spline.GetPoints().size(), 1));
            float Top = -1e30f, Bottom = 1e30f;
            for(const EngineCore::Vector3& Each : InInputs.Spline.GetPolyline())
            {
                const Float3 P = ToWorld.Point(Float3(Each));
                Top = std::max(Top, P.Y);
                Bottom = std::min(Bottom, P.Y);
                Area.Add(P.X, P.Z);
            }
            const float Range = std::max(S.ProjectRange, 0.0f);
            for(int32_t Z = Z0; Z < Z1; ++Z)
            {
                for(int32_t X = X0; X < X1; ++X)
                {
                    Candidate C;
                    C.Key = Hash(uint32_t(X), uint32_t(Z), uint32_t(S.Seed), 0xA4EAu);
                    const float LX = (float(X) + 0.5f + (Random(C.Key, 1) - 0.5f) * Jitter) * Step;
                    const float LZ = (float(Z) + 0.5f + (Random(C.Key, 2) - 0.5f) * Jitter) * Step;
                    if(!InInputs.Spline.ContainsXZ(LX, LZ)) { continue; }
                    C.Position = ToWorld.Point(Float3(LX, MeanY, LZ));
                    C.Normal   = EntityUp;
                    C.Top      = Top + Range;
                    C.Bottom   = Bottom - Range;
                    C.bProject = bProject;
                    Candidates.push_back(C);
                }
            }
        }
        else if(S.Source == PGS_SplinePath)
        {
            if(!InInputs.bHasSpline || !InInputs.Spline.IsValid())
            {
                OutResult.Problem = "Spline Path needs a Spline (two points or more) on its entity";
                return;
            }
            const GameCore::SplineCurve World = InInputs.Spline.Transformed(InInputs.ToWorld);
            const float Length = World.GetLength();
            const float Width  = std::max(InInputs.SplineWidth, 0.0f);
            const uint32_t Lanes = Width > Spacing ? uint32_t(std::floor(Width / Spacing)) : 1u;
            const uint32_t Steps = World.IsClosed() ? std::max(uint32_t(std::floor(Length / Spacing)), 1u)
                                                    : uint32_t(std::floor(Length / Spacing)) + 1u;
            if(TooMany(double(Steps) * double(Lanes))) { return; }
            const float LaneWidth = Lanes > 1 ? Width / float(Lanes) : Width;
            const float Range     = std::max(S.ProjectRange, 0.0f);
            const uint32_t Sides  = S.bBothSides ? 2u : 1u;
            Candidates.reserve(size_t(Steps) * Lanes * Sides);
            for(uint32_t Step = 0; Step < Steps; ++Step)
            {
                for(uint32_t Lane = 0; Lane < Lanes * Sides; ++Lane)
                {
                    const float Side = Lane >= Lanes ? -1.0f : 1.0f;
                    Candidate C;
                    C.Key = Hash(Step, Lane, uint32_t(S.Seed), 0x9A7Bu);
                    /* Open paths keep their ends exactly on the curve's ends. */
                    const bool bEnd = !World.IsClosed() && (Step == 0 || Step + 1 == Steps);
                    const float Along = float(Step) * Spacing + (bEnd ? 0.0f : (Random(C.Key, 1) - 0.5f) * Jitter * Spacing);
                    const GameCore::SplineSample Sample = World.SampleAtDistance(Along);
                    const Float3 Tangent(Sample.Tangent);
                    const Float3 Right = Float3(Tangent.Z, 0.0f, -Tangent.X).Normalized(Float3(1.0f, 0.0f, 0.0f));
                    float Across = S.PathOffset;
                    if(Width > 0.0f)
                    {
                        Across += -0.5f * Width + (float(Lane % Lanes) + 0.5f) * LaneWidth + (Random(C.Key, 2) - 0.5f) * Jitter * LaneWidth;
                    }
                    Across *= Side;
                    C.Position = Float3(Sample.Position) + Right * Across;
                    C.Tangent  = Tangent;
                    C.Normal   = EntityUp;
                    C.Top      = C.Position.Y + Range;
                    C.Bottom   = C.Position.Y - Range;
                    C.bProject = bProject;
                    Area.Add(C.Position.X, C.Position.Z);
                    Candidates.push_back(C);
                }
            }
        }
        else if(S.Source == PGS_MeshSurface)
        {
            if(!InInputs.Surface.Mesh)
            {
                OutResult.Problem = "Mesh Surface needs a Render mesh on its entity";
                return;
            }
            const MeshData& Mesh = *InInputs.Surface.Mesh;
            const Affine SurfaceToWorld = Affine::From(InInputs.Surface.ToWorld);
            const Affine SurfaceToLocal = Affine::From(EngineCore::Matrix4x4::Inverse(InInputs.Surface.ToWorld));
            const bool   bNormals = Mesh.Normals.size() == Mesh.Positions.size();
            const size_t Vertices = Mesh.Positions.size() / 3;
            const float  PerInstance = Spacing * Spacing;
            double Expected = 0.0;
            for(size_t Index = 0; Index + 2 < Mesh.Indices.size(); Index += 3)
            {
                const uint32_t I[3] = { Mesh.Indices[Index], Mesh.Indices[Index + 1], Mesh.Indices[Index + 2] };
                if(I[0] >= Vertices || I[1] >= Vertices || I[2] >= Vertices) { continue; }
                Float3 P[3];
                for(int Corner = 0; Corner < 3; ++Corner)
                {
                    P[Corner] = SurfaceToWorld.Point(Float3(Mesh.Positions[I[Corner] * 3], Mesh.Positions[I[Corner] * 3 + 1],
                                                            Mesh.Positions[I[Corner] * 3 + 2]));
                }
                const Float3 Face  = (P[1] - P[0]).Cross(P[2] - P[0]);
                const float  Count = 0.5f * Face.Length() / PerInstance;
                Expected += double(Count);
                if(TooMany(Expected)) { return; }
                const uint32_t Triangle = uint32_t(Index / 3);
                const uint32_t Whole = uint32_t(Count);
                const uint32_t Total = Whole + (Random(Hash(Triangle, uint32_t(S.Seed), 0x7E1u), 3) < Count - float(Whole) ? 1u : 0u);
                const Float3 FaceNormal = Face.Normalized();
                for(uint32_t Each = 0; Each < Total; ++Each)
                {
                    Candidate C;
                    C.Key = Hash(Triangle, Each, uint32_t(S.Seed), 0x3E5u);
                    float U = Random(C.Key, 1), V = Random(C.Key, 2);
                    if(U + V > 1.0f) { U = 1.0f - U; V = 1.0f - V; }
                    const float W = 1.0f - U - V;
                    C.Position = P[0] * W + P[1] * U + P[2] * V;
                    if(bNormals)
                    {
                        Float3 N;
                        for(int Corner = 0; Corner < 3; ++Corner)
                        {
                            const float Bary = Corner == 0 ? W : (Corner == 1 ? U : V);
                            N = N + Float3(Mesh.Normals[I[Corner] * 3], Mesh.Normals[I[Corner] * 3 + 1], Mesh.Normals[I[Corner] * 3 + 2]) * Bary;
                        }
                        C.Normal = SurfaceToLocal.NormalFromInverse(N.Normalized(FaceNormal));
                    }
                    else
                    {
                        C.Normal = FaceNormal;
                    }
                    C.bProject = false;
                    Candidates.push_back(C);
                }
            }
        }
        OutResult.Candidates = uint32_t(Candidates.size());

        // === Surfaces =============================================================
        MeshSurfaceGrid Grid;
        std::vector<Affine> TerrainToLocal;
        if(bProject)
        {
            if(bProjectMeshes && !Area.IsEmpty())
            {
                Footprint Padded = Area;
                Padded.Grow(Spacing);
                Grid.Build(InInputs.Meshes, Padded.Min, Padded.Max);
            }
            if(bProjectTerrain)
            {
                for(const ProceduralTerrainInput& Each : InInputs.Terrains)
                {
                    TerrainToLocal.push_back(Affine::From(Each.ToLocal));
                }
            }
        }

        // === Filters, then instances ===============================================
        const float CosMaxSlope = std::cos(std::clamp(S.MaxSlope, 0.0f, 180.0f) * Pi / 180.0f);
        const float CellLocal   = std::max(S.CellSize, 100.0f) / ScaleXZ;
        const float Align       = std::clamp(S.AlignToNormal, 0.0f, 1.0f);
        const float Density     = std::clamp(S.Density, 0.0f, 1.0f);
        const uint32_t Layer    = S.TerrainLayer;

        struct Placed
        {
            RenderInstance Instance;
            float          Order = 0.0f;
        };
        struct CellBuild
        {
            float Min[3] = {  1e30f,  1e30f,  1e30f };
            float Max[3] = { -1e30f, -1e30f, -1e30f };
            std::vector<Placed> Entries[PROCEDURAL_MESHES_MAX];
        };
        std::unordered_map<int64_t, size_t> CellIndex;
        std::vector<CellBuild> Cells;

        /* Each entry's box centre and half diagonal, at scale 1. */
        Float3 EntryCentre[PROCEDURAL_MESHES_MAX];
        float  EntryRadius[PROCEDURAL_MESHES_MAX] = {};
        for(uint32_t Entry = 0; Entry < PROCEDURAL_MESHES_MAX; ++Entry)
        {
            const ProceduralEntryInput& In = InInputs.Entries[Entry];
            EntryCentre[Entry] = Float3(0.5f * (In.BoundsMin[0] + In.BoundsMax[0]), 0.5f * (In.BoundsMin[1] + In.BoundsMax[1]),
                                        0.5f * (In.BoundsMin[2] + In.BoundsMax[2]));
            EntryRadius[Entry] = 0.5f * (Float3(In.BoundsMax[0], In.BoundsMax[1], In.BoundsMax[2])
                                         - Float3(In.BoundsMin[0], In.BoundsMin[1], In.BoundsMin[2])).Length();
        }

        const uint32_t Limit = uint32_t(std::max<int32_t>(S.MaxInstances, 1));
        for(const Candidate& Each : Candidates)
        {
            if(OutResult.InstanceCount >= Limit)
            {
                OutResult.bCapped = true;
                break;
            }
            /* Cheap ones first: nothing to drop yet. */
            if(Density < 1.0f && Random(Each.Key, 3) >= Density) { continue; }
            if(S.NoiseScale > 0.0f && Noise(Each.Position.X, Each.Position.Z, S.NoiseScale, uint32_t(S.Seed)) < std::clamp(S.NoiseThreshold, 0.0f, 1.0f))
            {
                continue;
            }

            Float3 Position = Each.Position;
            Float3 Normal   = Each.Normal;
            if(Each.bProject)
            {
                SurfaceHit Hit;
                bool bHit = false;
                if(bProjectTerrain)
                {
                    bHit = DropOntoTerrains(InInputs.Terrains, TerrainToLocal, Position.X, Position.Z,
                                            Each.Top, Each.Bottom, Hit, false);
                }
                if(bProjectMeshes)
                {
                    float Y = 0.0f;
                    Float3 MeshNormal;
                    if(Grid.Down(Position.X, Position.Z, Each.Top, Each.Bottom, Y, MeshNormal) && (!bHit || Y > Hit.Y))
                    {
                        Hit.Y       = Y;
                        Hit.Normal  = MeshNormal;
                        Hit.Terrain = -1;
                        bHit = true;
                    }
                }
                if(!bHit) { continue; }
                Position.Y = Hit.Y;
                Normal     = Hit.Normal;
                if(Layer != PROCEDURAL_ANY_LAYER)
                {
                    if(Hit.Terrain < 0 || Layer > GameCore::TERRAIN_LAYERS) { continue; }
                    if(Hit.LayerWeights[Layer - 1] < std::clamp(S.LayerThreshold, 0.0f, 1.0f)) { continue; }
                }
            }
            else if(Layer != PROCEDURAL_ANY_LAYER)
            {
                continue;   // only a terrain says what is painted
            }

            if(Normal.Y < CosMaxSlope - 1e-6f) { continue; }
            if(Position.Y < S.MinHeight || Position.Y > S.MaxHeight) { continue; }

            bool bExcluded = false;
            for(const ProceduralExclusionInput& Out : InInputs.Exclusions)
            {
                const EngineCore::Vector3 Here(Position.X, Position.Y, Position.Z);
                if(Out.Curve.IsClosed() && Out.Curve.ContainsXZ(Position.X, Position.Z)) { bExcluded = true; break; }
                GameCore::SplineSample Near;
                float Distance = 0.0f;
                if(Out.HalfWidth > 0.0f && Out.Curve.NearestToXZ(Here, Near, Distance) && Distance < Out.HalfWidth) { bExcluded = true; break; }
            }
            if(bExcluded) { continue; }

            /* Which entry, by weight. */
            float Pick = Random(Each.Key, 4) * TotalWeight;
            uint32_t Entry = 0;
            for(uint32_t Index = 0; Index < PROCEDURAL_MESHES_MAX; ++Index)
            {
                if(!InInputs.Entries[Index].bValid || !(S.Meshes[Index].Weight > 0.0f)) { continue; }
                Entry = Index;
                Pick -= S.Meshes[Index].Weight;
                if(Pick < 0.0f) { break; }
            }

            const float Scale = (S.ScaleMin + (S.ScaleMax - S.ScaleMin) * Random(Each.Key, 5)) * S.Meshes[Entry].Scale;
            if(!(Scale > 0.0f)) { continue; }
            float Yaw = 0.0f;
            if(S.bAlignToPath && S.Source == PGS_SplinePath)
            {
                Yaw = std::atan2(Each.Tangent.X, Each.Tangent.Z) * 180.0f / Pi;
            }
            else if(S.bRandomYaw)
            {
                Yaw = Random(Each.Key, 6) * 360.0f;
            }
            const Float3 Up = (Float3(0.0f, 1.0f, 0.0f) * (1.0f - Align) + Normal * Align).Normalized();

            float WorldRows[3][4];
            BuildWorldMatrix(Position, Up, Yaw, Scale, S.Sink, WorldRows);

            /* Into the entity's space: ToLocal * World. */
            float Rows[3][4];
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 4; ++Col)
                {
                    Rows[Row][Col] = ToLocal.M[Row][0] * WorldRows[0][Col] + ToLocal.M[Row][1] * WorldRows[1][Col]
                                   + ToLocal.M[Row][2] * WorldRows[2][Col] + (Col == 3 ? ToLocal.M[Row][3] : 0.0f);
                }
            }

            Placed Out;
            for(int Col = 0; Col < 4; ++Col)
            {
                for(int Row = 0; Row < 3; ++Row) { Out.Instance.Transform[Col * 4 + Row] = Rows[Row][Col]; }
                Out.Instance.Transform[Col * 4 + 3] = Col == 3 ? 1.0f : 0.0f;
            }
            const float Shade = 1.0f - std::clamp(S.ColorVariation, 0.0f, 1.0f) * Random(Each.Key, 7);
            Out.Instance.Color[0] = Out.Instance.Color[1] = Out.Instance.Color[2] = Shade;
            Out.Instance.Color[3] = 1.0f;
            Out.Order = Random(Each.Key, 8);

            /* Its cell, by where it stands in the entity's space. */
            const Float3 Local(Rows[0][3], Rows[1][3], Rows[2][3]);
            const int64_t Key = CellKey(int32_t(std::floor(Local.X / CellLocal)), int32_t(std::floor(Local.Z / CellLocal)));
            auto Found = CellIndex.find(Key);
            if(Found == CellIndex.end())
            {
                Found = CellIndex.emplace(Key, Cells.size()).first;
                Cells.emplace_back();
            }
            CellBuild& Cell = Cells[Found->second];

            /* Its box: the mesh's, as a sphere through its largest scale. */
            float Stretch = 0.0f;
            for(int Col = 0; Col < 3; ++Col)
            {
                Stretch = std::max(Stretch, std::sqrt(Rows[0][Col] * Rows[0][Col] + Rows[1][Col] * Rows[1][Col] + Rows[2][Col] * Rows[2][Col]));
            }
            const Float3& EC = EntryCentre[Entry];
            const Float3 Centre(Rows[0][0] * EC.X + Rows[0][1] * EC.Y + Rows[0][2] * EC.Z + Rows[0][3],
                                Rows[1][0] * EC.X + Rows[1][1] * EC.Y + Rows[1][2] * EC.Z + Rows[1][3],
                                Rows[2][0] * EC.X + Rows[2][1] * EC.Y + Rows[2][2] * EC.Z + Rows[2][3]);
            const float Radius = EntryRadius[Entry] * Stretch;
            const float C[3] = { Centre.X, Centre.Y, Centre.Z };
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                Cell.Min[Axis] = std::min(Cell.Min[Axis], C[Axis] - Radius);
                Cell.Max[Axis] = std::max(Cell.Max[Axis], C[Axis] + Radius);
            }
            Cell.Entries[Entry].push_back(Out);
            ++OutResult.InstanceCount;
        }

        // === Cells, each entry in thinning order =================================
        OutResult.Cells.reserve(Cells.size());
        for(CellBuild& Each : Cells)
        {
            ProceduralCell Cell;
            std::memcpy(Cell.Min, Each.Min, sizeof(Cell.Min));
            std::memcpy(Cell.Max, Each.Max, sizeof(Cell.Max));
            for(uint32_t Entry = 0; Entry < PROCEDURAL_MESHES_MAX; ++Entry)
            {
                std::vector<Placed>& List = Each.Entries[Entry];
                if(List.empty()) { continue; }
                std::sort(List.begin(), List.end(), [](const Placed& InA, const Placed& InB) { return InA.Order < InB.Order; });
                ProceduralBatch Batch;
                Batch.Entry = uint8_t(Entry);
                Batch.Instances.reserve(List.size());
                for(const Placed& One : List) { Batch.Instances.push_back(One.Instance); }
                Cell.Batches.push_back(std::move(Batch));
            }
            OutResult.Cells.push_back(std::move(Cell));
        }

        OutResult.Milliseconds = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count());
    }
}
