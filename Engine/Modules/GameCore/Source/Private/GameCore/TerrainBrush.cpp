#include "Public/GameCore/TerrainBrush.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace GameCore
{
    namespace
    {
        const char* const ToolNames[TT_Count]    = { "Raise", "Smooth", "Flatten", "Noise", "Set Height", "Paint", "Smooth Paint" };
        const char* const FalloffNames[TF_Count] = { "Smooth", "Linear", "Sphere", "Tip" };

        /* Lattice hash to 0..1. */
        float Hash(const int32_t InX, const int32_t InZ, const uint32_t InSeed)
        {
            uint32_t H = uint32_t(InX) * 0x8DA6B343u ^ uint32_t(InZ) * 0xD8163841u ^ InSeed * 0xCB1AB31Fu;
            H ^= H >> 13;
            H *= 0x5BD1E995u;
            H ^= H >> 15;
            return float(H & 0xFFFFFFu) / float(0xFFFFFF);
        }

        /* Two octaves of smooth value noise, -1..1. */
        float ValueNoise(const float InX, const float InZ, const uint32_t InSeed)
        {
            float Total = 0.0f;
            float Amplitude = 1.0f;
            float Frequency = 1.0f;
            float Norm = 0.0f;
            for(int Octave = 0; Octave < 2; ++Octave)
            {
                const float X = InX * Frequency;
                const float Z = InZ * Frequency;
                const int32_t X0 = int32_t(std::floor(X));
                const int32_t Z0 = int32_t(std::floor(Z));
                float FX = X - float(X0);
                float FZ = Z - float(Z0);
                FX = FX * FX * (3.0f - 2.0f * FX);
                FZ = FZ * FZ * (3.0f - 2.0f * FZ);
                const uint32_t Seed = InSeed + uint32_t(Octave) * 101u;
                const float A = Hash(X0, Z0, Seed),     B = Hash(X0 + 1, Z0, Seed);
                const float C = Hash(X0, Z0 + 1, Seed), D = Hash(X0 + 1, Z0 + 1, Seed);
                const float Value = (A + (B - A) * FX) + ((C + (D - C) * FX) - (A + (B - A) * FX)) * FZ;
                Total += (Value * 2.0f - 1.0f) * Amplitude;
                Norm  += Amplitude;
                Amplitude *= 0.5f;
                Frequency *= 2.0f;
            }
            return Total / Norm;
        }

        /* How much of a full step a dab takes this frame, for the tools that ease toward a target. */
        float EaseAmount(const float InStrength, const float InWeight, const float InDeltaSeconds)
        {
            /* At full strength a sample gets ~95% of the way in a quarter second. */
            const float Rate = 12.0f * std::clamp(InStrength, 0.0f, 1.0f);
            return std::clamp(InWeight * (1.0f - std::exp(-Rate * std::max(InDeltaSeconds, 0.0f))), 0.0f, 1.0f);
        }
    }

    float GetTerrainBrushWeight(const TerrainBrush& InBrush, const float InDistance)
    {
        if(!(InBrush.Radius > 0.0f) || InDistance >= InBrush.Radius) { return 0.0f; }
        const float T = std::clamp(InDistance / InBrush.Radius, 0.0f, 1.0f);   // 0 centre, 1 edge
        switch(InBrush.Falloff)
        {
        case TF_Linear: return 1.0f - T;
        case TF_Sphere: return std::sqrt(std::max(0.0f, 1.0f - T * T));
        case TF_Tip:    { const float U = 1.0f - T; return U * U * U; }
        case TF_Smooth:
        default:        { const float U = 1.0f - T; return U * U * (3.0f - 2.0f * U); }
        }
    }

    TerrainRect ApplyTerrainBrush(TerrainData& InData, const TerrainBrush& InBrush, const TerrainBrushOp& InOp)
    {
        std::lock_guard<std::mutex> Guard(InData.GetLock());
        const TerrainRect Rect = InData.GetSamplesInRadius(InBrush.X, InBrush.Z, InBrush.Radius);
        if(Rect.IsEmpty() || InData.IsEmpty() || InOp.Tool >= TT_Count) { return TerrainRect(); }

        const uint32_t Resolution = InData.GetResolution();
        const float    Spacing    = InData.GetSpacing();
        const float    Strength   = std::clamp(InBrush.Strength, 0.0f, 1.0f);
        const float    Delta      = std::max(InOp.DeltaSeconds, 0.0f);
        float*   Heights = InData.GetHeights();
        uint8_t* Weights = InData.GetWeights();

        /* The smoothing tools read the samples as they were before this dab: a copy of the rectangle and its border. */
        const int32_t Last = int32_t(Resolution) - 1;
        const int32_t BX0 = std::max(Rect.X0 - 1, 0), BZ0 = std::max(Rect.Z0 - 1, 0);
        const int32_t BX1 = std::min(Rect.X1 + 1, Last), BZ1 = std::min(Rect.Z1 + 1, Last);
        const int32_t BW = BX1 - BX0 + 1;
        std::vector<float>   HeightCopy;
        std::vector<uint8_t> WeightCopy;
        if(InOp.Tool == TT_Smooth)
        {
            HeightCopy.resize(size_t(BW) * size_t(BZ1 - BZ0 + 1));
            for(int32_t Z = BZ0; Z <= BZ1; ++Z)
            {
                std::memcpy(&HeightCopy[size_t(Z - BZ0) * BW], &Heights[size_t(Z) * Resolution + BX0], sizeof(float) * BW);
            }
        }
        else if(InOp.Tool == TT_SmoothWeights)
        {
            WeightCopy.resize(size_t(BW) * size_t(BZ1 - BZ0 + 1) * TERRAIN_LAYERS);
            for(int32_t Z = BZ0; Z <= BZ1; ++Z)
            {
                std::memcpy(&WeightCopy[size_t(Z - BZ0) * BW * TERRAIN_LAYERS],
                            &Weights[(size_t(Z) * Resolution + BX0) * TERRAIN_LAYERS], size_t(BW) * TERRAIN_LAYERS);
            }
        }
        const auto CopiedHeight = [&](const int32_t InX, const int32_t InZ)
        {
            const int32_t X = std::clamp(InX, BX0, BX1), Z = std::clamp(InZ, BZ0, BZ1);
            return HeightCopy[size_t(Z - BZ0) * BW + size_t(X - BX0)];
        };
        const auto CopiedWeight = [&](const int32_t InX, const int32_t InZ, const uint32_t InLayer)
        {
            const int32_t X = std::clamp(InX, BX0, BX1), Z = std::clamp(InZ, BZ0, BZ1);
            return float(WeightCopy[(size_t(Z - BZ0) * BW + size_t(X - BX0)) * TERRAIN_LAYERS + InLayer]);
        };

        TerrainRect Changed;
        for(int32_t Z = Rect.Z0; Z <= Rect.Z1; ++Z)
        {
            for(int32_t X = Rect.X0; X <= Rect.X1; ++X)
            {
                const float DX = float(X) * Spacing - InBrush.X;
                const float DZ = float(Z) * Spacing - InBrush.Z;
                const float Weight = GetTerrainBrushWeight(InBrush, std::sqrt(DX * DX + DZ * DZ));
                if(Weight <= 0.0f) { continue; }
                const size_t Index = size_t(Z) * Resolution + size_t(X);
                float& Height = Heights[Index];
                uint8_t* Paint = &Weights[Index * TERRAIN_LAYERS];

                switch(InOp.Tool)
                {
                case TT_Raise:
                    Height += InOp.Rate * Strength * Weight * Delta;
                    break;
                case TT_Smooth:
                {
                    float Sum = 0.0f;
                    for(int32_t OZ = -1; OZ <= 1; ++OZ)
                    {
                        for(int32_t OX = -1; OX <= 1; ++OX) { Sum += CopiedHeight(X + OX, Z + OZ); }
                    }
                    Height += (Sum / 9.0f - Height) * EaseAmount(Strength, Weight, Delta);
                    break;
                }
                case TT_Flatten:
                    Height += (InOp.TargetHeight - Height) * EaseAmount(Strength, Weight, Delta);
                    break;
                case TT_SetHeight:
                    Height += (InOp.TargetHeight - Height) * std::clamp(Weight * 4.0f, 0.0f, 1.0f);
                    break;
                case TT_Noise:
                {
                    const float Scale = std::max(InOp.NoiseScale, Spacing);
                    Height += ValueNoise(float(X) * Spacing / Scale, float(Z) * Spacing / Scale, InOp.NoiseSeed)
                              * InOp.Rate * Strength * Weight * Delta;
                    break;
                }
                case TT_Paint:
                case TT_SmoothWeights:
                {
                    float Next[TERRAIN_LAYERS];
                    if(InOp.Tool == TT_Paint)
                    {
                        const uint32_t Layer = std::min<uint32_t>(InOp.Layer, TERRAIN_LAYERS - 1);
                        const float Amount = EaseAmount(Strength, Weight, Delta);
                        for(uint32_t Each = 0; Each < TERRAIN_LAYERS; ++Each)
                        {
                            const float Target = Each == Layer ? 255.0f : 0.0f;
                            Next[Each] = float(Paint[Each]) + (Target - float(Paint[Each])) * Amount;
                        }
                    }
                    else
                    {
                        const float Amount = EaseAmount(Strength, Weight, Delta);
                        for(uint32_t Each = 0; Each < TERRAIN_LAYERS; ++Each)
                        {
                            float Sum = 0.0f;
                            for(int32_t OZ = -1; OZ <= 1; ++OZ)
                            {
                                for(int32_t OX = -1; OX <= 1; ++OX) { Sum += CopiedWeight(X + OX, Z + OZ, Each); }
                            }
                            Next[Each] = float(Paint[Each]) + (Sum / 9.0f - float(Paint[Each])) * Amount;
                        }
                    }
                    /* Renormalise to 255 total, rounding so the sum is exact. */
                    float Total = 0.0f;
                    for(const float Each : Next) { Total += std::max(Each, 0.0f); }
                    if(Total <= 0.0f) { Next[0] = 255.0f; Total = 255.0f; }
                    int32_t Assigned = 0;
                    uint32_t Largest = 0;
                    for(uint32_t Each = 0; Each < TERRAIN_LAYERS; ++Each)
                    {
                        const int32_t Value = int32_t(std::lround(std::max(Next[Each], 0.0f) * 255.0f / Total));
                        Paint[Each] = uint8_t(std::clamp(Value, 0, 255));
                        Assigned += Paint[Each];
                        if(Paint[Each] > Paint[Largest]) { Largest = Each; }
                    }
                    Paint[Largest] = uint8_t(std::clamp(int32_t(Paint[Largest]) + (255 - Assigned), 0, 255));
                    break;
                }
                default:
                    break;
                }
                Changed.Add(X, Z);
            }
        }
        InData.MarkDirty(Changed);
        return Changed;
    }

    const char* GetTerrainToolName(const uint8_t InTool)
    {
        return InTool < TT_Count ? ToolNames[InTool] : nullptr;
    }

    uint8_t FindTerrainTool(const char* InName)
    {
        if(!InName) { return TT_Count; }
        for(uint8_t Tool = 0; Tool < TT_Count; ++Tool)
        {
            if(std::strcmp(ToolNames[Tool], InName) == 0) { return Tool; }
        }
        return TT_Count;
    }

    const char* GetTerrainFalloffName(const uint8_t InFalloff)
    {
        return InFalloff < TF_Count ? FalloffNames[InFalloff] : nullptr;
    }

    uint8_t FindTerrainFalloff(const char* InName)
    {
        if(!InName) { return TF_Count; }
        for(uint8_t Falloff = 0; Falloff < TF_Count; ++Falloff)
        {
            if(std::strcmp(FalloffNames[Falloff], InName) == 0) { return Falloff; }
        }
        return TF_Count;
    }
}
