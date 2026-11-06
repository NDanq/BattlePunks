#include "Public/GameCore/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>
#include <unordered_map>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/GameCoreLog.h"

namespace GameCore
{
    namespace
    {
        constexpr uint32_t TAG_TERRAIN = EngineCore::MakeArchiveTag('N','T','R','N');
        constexpr uint32_t TAG_HEADER  = EngineCore::MakeArchiveTag('T','H','D','R');
        constexpr uint32_t TAG_HEIGHTS = EngineCore::MakeArchiveTag('H','G','H','T');
        constexpr uint32_t TAG_WEIGHTS = EngineCore::MakeArchiveTag('S','P','L','T');
        constexpr uint32_t TAG_LAYERS  = EngineCore::MakeArchiveTag('L','A','Y','R');

        uint32_t ChunksFor(const uint32_t InResolution)
        {
            if(InResolution < TERRAIN_RESOLUTION_MIN) { return 0; }
            return (InResolution - 1 + TERRAIN_CHUNK_QUADS - 1) / TERRAIN_CHUNK_QUADS;
        }

        void Normalize(float (&InOut)[3])
        {
            const float Length = std::sqrt(InOut[0] * InOut[0] + InOut[1] * InOut[1] + InOut[2] * InOut[2]);
            if(Length > 1e-12f)
            {
                InOut[0] /= Length;
                InOut[1] /= Length;
                InOut[2] /= Length;
            }
            else
            {
                InOut[0] = 0.0f; InOut[1] = 1.0f; InOut[2] = 0.0f;
            }
        }

        /* Moller-Trumbore, two-sided. */
        bool RayTriangle(const float (&InOrigin)[3], const float (&InDirection)[3],
                         const float (&InA)[3], const float (&InB)[3], const float (&InC)[3], float& OutT)
        {
            const float E1[3] = { InB[0] - InA[0], InB[1] - InA[1], InB[2] - InA[2] };
            const float E2[3] = { InC[0] - InA[0], InC[1] - InA[1], InC[2] - InA[2] };
            const float P[3]  = { InDirection[1] * E2[2] - InDirection[2] * E2[1],
                                  InDirection[2] * E2[0] - InDirection[0] * E2[2],
                                  InDirection[0] * E2[1] - InDirection[1] * E2[0] };
            const float Det = E1[0] * P[0] + E1[1] * P[1] + E1[2] * P[2];
            if(std::fabs(Det) < 1e-12f) { return false; }
            const float Inv = 1.0f / Det;
            const float S[3] = { InOrigin[0] - InA[0], InOrigin[1] - InA[1], InOrigin[2] - InA[2] };
            const float U = (S[0] * P[0] + S[1] * P[1] + S[2] * P[2]) * Inv;
            if(U < -1e-6f || U > 1.0f + 1e-6f) { return false; }
            const float Q[3] = { S[1] * E1[2] - S[2] * E1[1], S[2] * E1[0] - S[0] * E1[2], S[0] * E1[1] - S[1] * E1[0] };
            const float V = (InDirection[0] * Q[0] + InDirection[1] * Q[1] + InDirection[2] * Q[2]) * Inv;
            if(V < -1e-6f || U + V > 1.0f + 1e-6f) { return false; }
            OutT = (E2[0] * Q[0] + E2[1] * Q[1] + E2[2] * Q[2]) * Inv;
            return OutT >= 0.0f;
        }
    }

    // === TerrainData ==========================================================

    TerrainData::TerrainData() = default;
    TerrainData::~TerrainData() = default;

    void TerrainData::Reset(const uint32_t InResolution, const float InSpacing, const float InHeight)
    {
        Resolution = std::clamp<uint32_t>(InResolution, TERRAIN_RESOLUTION_MIN, TERRAIN_RESOLUTION_MAX);
        Spacing    = InSpacing > 0.0f ? InSpacing : 100.0f;
        const size_t Count = size_t(Resolution) * Resolution;
        Heights.assign(Count, InHeight);
        Weights.assign(Count * TERRAIN_LAYERS, 0);
        for(size_t Index = 0; Index < Count; ++Index) { Weights[Index * TERRAIN_LAYERS] = 255; }

        ChunksPerSide  = ChunksFor(Resolution);
        ChunkRevisions.reset(new std::atomic<uint64_t>[size_t(ChunksPerSide) * ChunksPerSide]);
        const uint64_t Next = Revision.fetch_add(1, std::memory_order_acq_rel) + 1;
        for(size_t Index = 0; Index < size_t(ChunksPerSide) * ChunksPerSide; ++Index)
        {
            ChunkRevisions[Index].store(Next, std::memory_order_relaxed);
        }
        MinHeight = MaxHeight = InHeight;
    }

    void TerrainData::CopyFrom(const TerrainData& InOther)
    {
        if(&InOther == this) { return; }
        std::vector<float>   OtherHeights;
        std::vector<uint8_t> OtherWeights;
        uint32_t             OtherResolution = 0;
        float                OtherSpacing    = 100.0f;
        std::string          OtherNames[TERRAIN_LAYERS];
        {
            std::lock_guard<std::mutex> Guard(InOther.Lock);
            OtherHeights    = InOther.Heights;
            OtherWeights    = InOther.Weights;
            OtherResolution = InOther.Resolution;
            OtherSpacing    = InOther.Spacing;
            for(uint32_t Layer = 0; Layer < TERRAIN_LAYERS; ++Layer) { OtherNames[Layer] = InOther.LayerNames[Layer]; }
        }
        Reset(std::max<uint32_t>(OtherResolution, TERRAIN_RESOLUTION_MIN), OtherSpacing);
        if(OtherHeights.size() == Heights.size()) { Heights = std::move(OtherHeights); }
        if(OtherWeights.size() == Weights.size()) { Weights = std::move(OtherWeights); }
        for(uint32_t Layer = 0; Layer < TERRAIN_LAYERS; ++Layer) { LayerNames[Layer] = OtherNames[Layer]; }
        RefreshHeightRange();
    }

    float TerrainData::GetHeight(const int32_t InX, const int32_t InZ) const
    {
        if(Resolution == 0) { return 0.0f; }
        const int32_t Last = int32_t(Resolution) - 1;
        const int32_t X = std::clamp(InX, 0, Last);
        const int32_t Z = std::clamp(InZ, 0, Last);
        return Heights[size_t(Z) * Resolution + size_t(X)];
    }

    void TerrainData::SetHeight(const int32_t InX, const int32_t InZ, const float InHeight)
    {
        if(InX < 0 || InZ < 0 || InX >= int32_t(Resolution) || InZ >= int32_t(Resolution)) { return; }
        Heights[size_t(InZ) * Resolution + size_t(InX)] = InHeight;
    }

    const uint8_t* TerrainData::GetWeight(const int32_t InX, const int32_t InZ) const
    {
        static const uint8_t None[TERRAIN_LAYERS] = { 255, 0, 0, 0 };
        if(Resolution == 0) { return None; }
        const int32_t Last = int32_t(Resolution) - 1;
        const int32_t X = std::clamp(InX, 0, Last);
        const int32_t Z = std::clamp(InZ, 0, Last);
        return &Weights[(size_t(Z) * Resolution + size_t(X)) * TERRAIN_LAYERS];
    }

    float TerrainData::SampleHeight(const float InX, const float InZ) const
    {
        if(Resolution < TERRAIN_RESOLUTION_MIN) { return 0.0f; }
        const float Cells = float(Resolution - 1);
        const float U = std::clamp(InX / Spacing, 0.0f, Cells);
        const float V = std::clamp(InZ / Spacing, 0.0f, Cells);
        const int32_t X = std::min(int32_t(U), int32_t(Resolution) - 2);
        const int32_t Z = std::min(int32_t(V), int32_t(Resolution) - 2);
        const float FX = U - float(X);
        const float FZ = V - float(Z);

        const float H00 = GetHeight(X, Z);
        const float H11 = GetHeight(X + 1, Z + 1);
        if(FZ >= FX)
        {
            /* (X, Z), (X, Z + 1), (X + 1, Z + 1) */
            const float H01 = GetHeight(X, Z + 1);
            return H00 + FZ * (H01 - H00) + FX * (H11 - H01);
        }
        /* (X, Z), (X + 1, Z + 1), (X + 1, Z) */
        const float H10 = GetHeight(X + 1, Z);
        return H00 + FX * (H10 - H00) + FZ * (H11 - H10);
    }

    void TerrainData::SampleNormal(const float InX, const float InZ, float (&OutNormal)[3]) const
    {
        OutNormal[0] = 0.0f; OutNormal[1] = 1.0f; OutNormal[2] = 0.0f;
        if(Resolution < TERRAIN_RESOLUTION_MIN) { return; }
        const float Cells = float(Resolution - 1);
        const float U = std::clamp(InX / Spacing, 0.0f, Cells);
        const float V = std::clamp(InZ / Spacing, 0.0f, Cells);
        const int32_t X = std::min(int32_t(U), int32_t(Resolution) - 2);
        const int32_t Z = std::min(int32_t(V), int32_t(Resolution) - 2);
        const float FX = U - float(X);
        const float FZ = V - float(Z);

        const float H00 = GetHeight(X, Z);
        const float H11 = GetHeight(X + 1, Z + 1);
        /* The plane's gradient: dh/dx and dh/dz on this triangle. */
        float DX = 0.0f;
        float DZ = 0.0f;
        if(FZ >= FX)
        {
            const float H01 = GetHeight(X, Z + 1);
            DX = (H11 - H01) / Spacing;
            DZ = (H01 - H00) / Spacing;
        }
        else
        {
            const float H10 = GetHeight(X + 1, Z);
            DX = (H10 - H00) / Spacing;
            DZ = (H11 - H10) / Spacing;
        }
        OutNormal[0] = -DX; OutNormal[1] = 1.0f; OutNormal[2] = -DZ;
        Normalize(OutNormal);
    }

    void TerrainData::GetSmoothNormal(const int32_t InX, const int32_t InZ, float (&OutNormal)[3]) const
    {
        const float Left  = GetHeight(InX - 1, InZ);
        const float Right = GetHeight(InX + 1, InZ);
        const float Down  = GetHeight(InX, InZ - 1);
        const float Up    = GetHeight(InX, InZ + 1);
        /* One-sided at the edges: clamped samples halve the span there. */
        const float SpanX = float((InX + 1 < int32_t(Resolution) ? 1 : 0) + (InX > 0 ? 1 : 0)) * Spacing;
        const float SpanZ = float((InZ + 1 < int32_t(Resolution) ? 1 : 0) + (InZ > 0 ? 1 : 0)) * Spacing;
        OutNormal[0] = SpanX > 0.0f ? -(Right - Left) / SpanX : 0.0f;
        OutNormal[1] = 1.0f;
        OutNormal[2] = SpanZ > 0.0f ? -(Up - Down) / SpanZ : 0.0f;
        Normalize(OutNormal);
    }

    void TerrainData::SampleWeights(const float InX, const float InZ, float (&OutWeights)[TERRAIN_LAYERS]) const
    {
        for(float& Each : OutWeights) { Each = 0.0f; }
        if(Resolution < TERRAIN_RESOLUTION_MIN) { OutWeights[0] = 1.0f; return; }
        const float Cells = float(Resolution - 1);
        const float U = std::clamp(InX / Spacing, 0.0f, Cells);
        const float V = std::clamp(InZ / Spacing, 0.0f, Cells);
        const int32_t X = std::min(int32_t(U), int32_t(Resolution) - 2);
        const int32_t Z = std::min(int32_t(V), int32_t(Resolution) - 2);
        const float FX = U - float(X);
        const float FZ = V - float(Z);
        const float Corner[4] = { (1.0f - FX) * (1.0f - FZ), FX * (1.0f - FZ), (1.0f - FX) * FZ, FX * FZ };
        const uint8_t* Samples[4] = { GetWeight(X, Z), GetWeight(X + 1, Z), GetWeight(X, Z + 1), GetWeight(X + 1, Z + 1) };
        for(int Each = 0; Each < 4; ++Each)
        {
            for(uint32_t Layer = 0; Layer < TERRAIN_LAYERS; ++Layer)
            {
                OutWeights[Layer] += Corner[Each] * float(Samples[Each][Layer]) / 255.0f;
            }
        }
    }

    bool TerrainData::Raycast(const float (&InOrigin)[3], const float (&InDirection)[3], const float InMaxDistance,
                              float& OutDistance, float (&OutNormal)[3]) const
    {
        if(Resolution < TERRAIN_RESOLUTION_MIN || !(InMaxDistance > 0.0f)) { return false; }

        /* Clip to the terrain's box (a little taller, so a ray along the top still enters). */
        const float Size   = GetSize();
        const float Pad    = std::max(1e-3f, (MaxHeight - MinHeight) * 1e-4f);
        const float Lo[3]  = { 0.0f, MinHeight - Pad, 0.0f };
        const float Hi[3]  = { Size, MaxHeight + Pad, Size };
        float Enter = 0.0f;
        float Exit  = InMaxDistance;
        for(int Axis = 0; Axis < 3; ++Axis)
        {
            if(std::fabs(InDirection[Axis]) < 1e-12f)
            {
                if(InOrigin[Axis] < Lo[Axis] || InOrigin[Axis] > Hi[Axis]) { return false; }
                continue;
            }
            float T0 = (Lo[Axis] - InOrigin[Axis]) / InDirection[Axis];
            float T1 = (Hi[Axis] - InOrigin[Axis]) / InDirection[Axis];
            if(T0 > T1) { std::swap(T0, T1); }
            Enter = std::max(Enter, T0);
            Exit  = std::min(Exit, T1);
            if(Enter > Exit) { return false; }
        }

        /* Walk the cells along XZ from Enter to Exit (Amanatides-Woo), in cell units. */
        const float StartX = (InOrigin[0] + InDirection[0] * Enter) / Spacing;
        const float StartZ = (InOrigin[2] + InDirection[2] * Enter) / Spacing;
        const int32_t LastCell = int32_t(Resolution) - 2;
        int32_t CellX = std::clamp(int32_t(std::floor(StartX)), 0, LastCell);
        int32_t CellZ = std::clamp(int32_t(std::floor(StartZ)), 0, LastCell);

        const float DirX = InDirection[0] / Spacing;
        const float DirZ = InDirection[2] / Spacing;
        const int32_t StepX = DirX > 0.0f ? 1 : (DirX < 0.0f ? -1 : 0);
        const int32_t StepZ = DirZ > 0.0f ? 1 : (DirZ < 0.0f ? -1 : 0);
        const float Inf = std::numeric_limits<float>::infinity();
        const float DeltaX = StepX != 0 ? std::fabs(1.0f / DirX) : Inf;
        const float DeltaZ = StepZ != 0 ? std::fabs(1.0f / DirZ) : Inf;
        float NextX = StepX > 0 ? Enter + (float(CellX + 1) - StartX) / DirX
                    : StepX < 0 ? Enter + (float(CellX) - StartX) / DirX : Inf;
        float NextZ = StepZ > 0 ? Enter + (float(CellZ + 1) - StartZ) / DirZ
                    : StepZ < 0 ? Enter + (float(CellZ) - StartZ) / DirZ : Inf;

        /* A cell more than the ray could need: the walk always ends. */
        const int32_t MaxSteps = int32_t(Resolution) * 2 + 4;
        for(int32_t Step = 0; Step < MaxSteps; ++Step)
        {
            const float X0 = float(CellX) * Spacing;
            const float Z0 = float(CellZ) * Spacing;
            const float X1 = X0 + Spacing;
            const float Z1 = Z0 + Spacing;
            const float P00[3] = { X0, GetHeight(CellX,     CellZ),     Z0 };
            const float P01[3] = { X0, GetHeight(CellX,     CellZ + 1), Z1 };
            const float P11[3] = { X1, GetHeight(CellX + 1, CellZ + 1), Z1 };
            const float P10[3] = { X1, GetHeight(CellX + 1, CellZ),     Z0 };

            float Best = Inf;
            float T    = 0.0f;
            if(RayTriangle(InOrigin, InDirection, P00, P01, P11, T) && T <= InMaxDistance && T < Best) { Best = T; }
            if(RayTriangle(InOrigin, InDirection, P00, P11, P10, T) && T <= InMaxDistance && T < Best) { Best = T; }
            if(Best < Inf)
            {
                OutDistance = Best;
                SampleNormal(InOrigin[0] + InDirection[0] * Best, InOrigin[2] + InDirection[2] * Best, OutNormal);
                return true;
            }

            if(NextX < NextZ)
            {
                if(NextX > Exit) { break; }
                CellX += StepX;
                NextX += DeltaX;
            }
            else
            {
                if(NextZ > Exit) { break; }
                CellZ += StepZ;
                NextZ += DeltaZ;
            }
            if(CellX < 0 || CellZ < 0 || CellX > LastCell || CellZ > LastCell) { break; }
        }
        return false;
    }

    void TerrainData::MarkDirty(const TerrainRect& InRect)
    {
        if(InRect.IsEmpty() || Resolution < TERRAIN_RESOLUTION_MIN) { return; }
        const uint64_t Next = Revision.fetch_add(1, std::memory_order_acq_rel) + 1;

        /* A chunk's mesh reads its samples and their neighbours (normals). */
        const int32_t Last = int32_t(Resolution) - 1;
        const int32_t X0 = std::clamp(InRect.X0 - 1, 0, Last);
        const int32_t Z0 = std::clamp(InRect.Z0 - 1, 0, Last);
        const int32_t X1 = std::clamp(InRect.X1 + 1, 0, Last);
        const int32_t Z1 = std::clamp(InRect.Z1 + 1, 0, Last);
        const int32_t Quads = int32_t(TERRAIN_CHUNK_QUADS);
        const int32_t MaxChunk = int32_t(ChunksPerSide) - 1;
        /* Sample S is in chunk S / Quads, and on a shared edge in the one before too. */
        const int32_t CX0 = std::clamp((X0 - 1) / Quads, 0, MaxChunk);
        const int32_t CZ0 = std::clamp((Z0 - 1) / Quads, 0, MaxChunk);
        const int32_t CX1 = std::clamp(X1 / Quads, 0, MaxChunk);
        const int32_t CZ1 = std::clamp(Z1 / Quads, 0, MaxChunk);
        for(int32_t CZ = CZ0; CZ <= CZ1; ++CZ)
        {
            for(int32_t CX = CX0; CX <= CX1; ++CX)
            {
                ChunkRevisions[size_t(CZ) * ChunksPerSide + size_t(CX)].store(Next, std::memory_order_release);
            }
        }
        RefreshHeightRange();
    }

    uint64_t TerrainData::GetChunkRevision(const uint32_t InX, const uint32_t InZ) const
    {
        if(InX >= ChunksPerSide || InZ >= ChunksPerSide) { return 0; }
        return ChunkRevisions[size_t(InZ) * ChunksPerSide + size_t(InX)].load(std::memory_order_acquire);
    }

    TerrainRect TerrainData::GetChunkSamples(const uint32_t InX, const uint32_t InZ) const
    {
        TerrainRect Rect;
        if(InX >= ChunksPerSide || InZ >= ChunksPerSide) { return Rect; }
        const int32_t Last = int32_t(Resolution) - 1;
        Rect.X0 = int32_t(InX * TERRAIN_CHUNK_QUADS);
        Rect.Z0 = int32_t(InZ * TERRAIN_CHUNK_QUADS);
        Rect.X1 = std::min(Rect.X0 + int32_t(TERRAIN_CHUNK_QUADS), Last);
        Rect.Z1 = std::min(Rect.Z0 + int32_t(TERRAIN_CHUNK_QUADS), Last);
        return Rect;
    }

    TerrainRect TerrainData::GetSamplesInRadius(const float InX, const float InZ, const float InRadius) const
    {
        TerrainRect Rect;
        if(Resolution < TERRAIN_RESOLUTION_MIN || !(InRadius >= 0.0f)) { return Rect; }
        const int32_t Last = int32_t(Resolution) - 1;
        const int32_t X0 = int32_t(std::ceil((InX - InRadius) / Spacing));
        const int32_t X1 = int32_t(std::floor((InX + InRadius) / Spacing));
        const int32_t Z0 = int32_t(std::ceil((InZ - InRadius) / Spacing));
        const int32_t Z1 = int32_t(std::floor((InZ + InRadius) / Spacing));
        if(X1 < 0 || Z1 < 0 || X0 > Last || Z0 > Last) { return Rect; }
        Rect.X0 = std::max(X0, 0);
        Rect.Z0 = std::max(Z0, 0);
        Rect.X1 = std::min(X1, Last);
        Rect.Z1 = std::min(Z1, Last);
        return Rect;
    }

    void TerrainData::RefreshHeightRange()
    {
        if(Heights.empty()) { MinHeight = MaxHeight = 0.0f; return; }
        const auto Range = std::minmax_element(Heights.begin(), Heights.end());
        MinHeight = *Range.first;
        MaxHeight = *Range.second;
    }

    bool TerrainData::Serialize(EngineCore::Archive& Ar)
    {
        uint16_t FileVersion = 1;
        if(!Ar.OpenChunk(TAG_TERRAIN, FileVersion))
        {
            Ar.SetError("not a terrain (no NTRN chunk)");
            return false;
        }

        uint32_t FileResolution = Resolution;
        float    FileSpacing    = Spacing;
        float    Low            = MinHeight;
        float    High           = MaxHeight;
        if(Ar.IsSaving()) { RefreshHeightRange(); Low = MinHeight; High = MaxHeight; }

        uint16_t Version = 1;
        if(!Ar.OpenChunk(TAG_HEADER, Version))
        {
            Ar.SetError("terrain: no header (THDR)");
            return false;
        }
        Ar << FileResolution << FileSpacing << Low << High;
        Ar.CloseChunk();
        if(Ar.HasError()) { return false; }

        if(Ar.IsLoading())
        {
            if(FileResolution < TERRAIN_RESOLUTION_MIN || FileResolution > TERRAIN_RESOLUTION_MAX || !(FileSpacing > 0.0f)
               || !(High >= Low))
            {
                Ar.SetError("terrain: the header's resolution, spacing or height range is invalid");
                return false;
            }
            Reset(FileResolution, FileSpacing, Low);
        }

        const size_t Count = size_t(Resolution) * Resolution;
        Version = 1;
        if(!Ar.OpenChunk(TAG_HEIGHTS, Version))
        {
            Ar.SetError("terrain: no heights (HGHT)");
            return false;
        }
        if(Ar.IsLoading() && uint64_t(Ar.GetSize() - Ar.GetCursor()) < uint64_t(Count) * 2)
        {
            Ar.SetError("terrain: the heights are cut short");
            return false;
        }
        const float Range = High - Low;
        const float ToSample = Range > 0.0f ? 65535.0f / Range : 0.0f;
        const float ToHeight = Range / 65535.0f;
        for(size_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            uint16_t Sample = 0;
            if(Ar.IsSaving())
            {
                Sample = uint16_t(std::clamp(std::lround((Heights[Index] - Low) * ToSample), 0L, 65535L));
            }
            Ar << Sample;
            if(Ar.IsLoading()) { Heights[Index] = Low + float(Sample) * ToHeight; }
        }
        Ar.CloseChunk();

        Version = 1;
        if(Ar.IsSaving() || Ar.PeekChunkTag() == TAG_WEIGHTS)
        {
            if(Ar.OpenChunk(TAG_WEIGHTS, Version))
            {
                if(Ar.IsLoading() && uint64_t(Ar.GetSize() - Ar.GetCursor()) < uint64_t(Count) * TERRAIN_LAYERS)
                {
                    Ar.SetError("terrain: the paint weights are cut short");
                    return false;
                }
                for(size_t Index = 0; Index < Count * TERRAIN_LAYERS && !Ar.HasError(); ++Index)
                {
                    Ar << Weights[Index];
                }
                Ar.CloseChunk();
            }
        }

        Version = 1;
        if(Ar.IsSaving() || Ar.PeekChunkTag() == TAG_LAYERS)
        {
            if(Ar.OpenChunk(TAG_LAYERS, Version))
            {
                for(std::string& Name : LayerNames) { Ar << Name; }
                Ar.CloseChunk();
            }
        }

        Ar.CloseChunk();
        if(Ar.IsLoading() && !Ar.HasError())
        {
            RefreshHeightRange();
            Revision.fetch_add(1, std::memory_order_acq_rel);
        }
        return !Ar.HasError();
    }

    // === Files ================================================================

    bool LoadTerrainFile(const std::string& InPath, TerrainData& OutData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        std::lock_guard<std::mutex> Guard(OutData.GetLock());
        if(!OutData.Serialize(Ar))
        {
            GAMECORE_LOG_WARN("cannot load terrain " + InPath)
            OutData.Reset(TERRAIN_RESOLUTION_MIN, 100.0f);
            return false;
        }
        return true;
    }

    bool SaveTerrainFile(const std::string& InPath, const TerrainData& InData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        {
            std::lock_guard<std::mutex> Guard(InData.GetLock());
            /* Serialize only reads when saving (and refreshes the range it would have anyway). */
            if(!const_cast<TerrainData&>(InData).Serialize(Ar)) { return false; }
        }
        const std::string Temp = InPath + ".tmp";
        if(!EngineCore::SaveArchiveToFile(Temp, Ar))
        {
            GAMECORE_LOG_WARN("cannot write terrain " + Temp)
            return false;
        }
        std::error_code Error;
        std::filesystem::rename(std::filesystem::u8path(Temp), std::filesystem::u8path(InPath), Error);
        if(Error)
        {
            GAMECORE_LOG_WARN("cannot replace terrain " + InPath + ": " + Error.message())
            EngineCore::appFileDelete(Temp);
            return false;
        }
        return true;
    }

    // === TerrainAssetCache ====================================================

    namespace
    {
        struct TerrainCacheState
        {
            std::mutex                                                     Lock;
            std::unordered_map<std::string, std::shared_ptr<TerrainData>> Terrains;   // null: failed
            std::atomic<uint64_t>                                          Generation{0};
        };

        TerrainCacheState& GetTerrainCache()
        {
            static TerrainCacheState State;
            return State;
        }

        TerrainProvider GTerrainProvider;
        bool            bHasTerrainProvider = false;
    }

    std::shared_ptr<TerrainData> TerrainAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty()) { return nullptr; }
        TerrainCacheState& Cache = GetTerrainCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Terrains.find(InPath);
            if(Found != Cache.Terrains.end()) { return Found->second; }
        }

        auto Loaded = std::make_shared<TerrainData>();
        std::shared_ptr<TerrainData> Result;
        if(LoadTerrainFile(InPath, *Loaded)) { Result = std::move(Loaded); }
        else { GAMECORE_LOG_WARN("terrain '" + InPath + "' cannot be loaded: it has no ground") }

        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Terrains.emplace(InPath, Result).first->second;
    }

    std::shared_ptr<TerrainData> TerrainAssetCache::FindLoaded(const std::string& InPath)
    {
        TerrainCacheState& Cache = GetTerrainCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        const auto Found = Cache.Terrains.find(InPath);
        return Found != Cache.Terrains.end() ? Found->second : nullptr;
    }

    void TerrainAssetCache::Store(const std::string& InPath, std::shared_ptr<TerrainData> InData)
    {
        if(InPath.empty()) { return; }
        TerrainCacheState& Cache = GetTerrainCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Generation.fetch_add(1, std::memory_order_relaxed);
        Cache.Terrains[InPath] = std::move(InData);
    }

    void TerrainAssetCache::Invalidate(const std::string& InPath)
    {
        TerrainCacheState& Cache = GetTerrainCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Generation.fetch_add(1, std::memory_order_relaxed);
        if(InPath.empty()) { Cache.Terrains.clear(); }
        else { Cache.Terrains.erase(InPath); }
    }

    uint64_t TerrainAssetCache::GetGeneration()
    {
        return GetTerrainCache().Generation.load(std::memory_order_relaxed);
    }

    void SetTerrainProvider(const TerrainProvider* InProvider)
    {
        bHasTerrainProvider = InProvider != nullptr;
        GTerrainProvider    = InProvider ? *InProvider : TerrainProvider();
    }

    const TerrainProvider* GetTerrainProvider()
    {
        return bHasTerrainProvider ? &GTerrainProvider : nullptr;
    }
}
