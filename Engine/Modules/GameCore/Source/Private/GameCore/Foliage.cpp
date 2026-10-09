#include "Public/GameCore/Foliage.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/GameCoreLog.h"

namespace GameCore
{
    namespace
    {
        constexpr uint32_t TAG_FOLIAGE   = EngineCore::MakeArchiveTag('N','F','O','L');
        constexpr uint32_t TAG_HEADER    = EngineCore::MakeArchiveTag('F','H','D','R');
        constexpr uint32_t TAG_TYPES     = EngineCore::MakeArchiveTag('F','T','Y','P');
        constexpr uint32_t TAG_INSTANCES = EngineCore::MakeArchiveTag('F','I','N','S');

        constexpr uint32_t BUCKETS_MAX   = 256;   // a 16385-sample terrain's chunks a side
        constexpr float    Pi            = 3.14159265358979f;

        const std::vector<FoliageInstance>& NoInstances()
        {
            static const std::vector<FoliageInstance> Empty;
            return Empty;
        }

        /* xorshift32: the brushes' randomness, reproducible from a seed. */
        struct Random
        {
            uint32_t State;
            explicit Random(const uint32_t InSeed) : State(InSeed ? InSeed : 0x6D2B79F5u) {}
            uint32_t Next()
            {
                State ^= State << 13;
                State ^= State >> 17;
                State ^= State << 5;
                return State;
            }
            float Fraction() { return float(Next() & 0xFFFFFFu) / float(0x1000000); }
        };
    }

    // === FoliageData ==========================================================

    FoliageData::FoliageData() = default;
    FoliageData::~FoliageData() = default;

    void FoliageData::Reset(const TerrainData& InTerrain)
    {
        Reset(InTerrain.GetChunksPerSide(), float(TERRAIN_CHUNK_QUADS) * InTerrain.GetSpacing());
    }

    void FoliageData::Reset(const uint32_t InBucketsPerSide, const float InBucketSize)
    {
        BucketsPerSide = std::min(InBucketsPerSide, BUCKETS_MAX);
        BucketSize     = InBucketSize > 0.0f ? InBucketSize : 6400.0f;
        Buckets.assign(size_t(BucketsPerSide) * BucketsPerSide, std::vector<FoliageInstance>());
        BucketRevisions.reset(new std::atomic<uint64_t>[std::max<size_t>(Buckets.size(), 1)]);
        const uint64_t Next = Revision.fetch_add(1, std::memory_order_acq_rel) + 1;
        for(size_t Index = 0; Index < std::max<size_t>(Buckets.size(), 1); ++Index)
        {
            BucketRevisions[Index].store(Next, std::memory_order_relaxed);
        }
    }

    const std::vector<FoliageInstance>& FoliageData::GetBucket(const uint32_t InX, const uint32_t InZ) const
    {
        if(InX >= BucketsPerSide || InZ >= BucketsPerSide) { return NoInstances(); }
        return Buckets[size_t(InZ) * BucketsPerSide + InX];
    }

    std::vector<FoliageInstance>* FoliageData::FindBucket(const uint32_t InX, const uint32_t InZ)
    {
        if(InX >= BucketsPerSide || InZ >= BucketsPerSide) { return nullptr; }
        return &Buckets[size_t(InZ) * BucketsPerSide + InX];
    }

    bool FoliageData::BucketOf(const float InX, const float InZ, uint32_t& OutX, uint32_t& OutZ) const
    {
        if(BucketsPerSide == 0 || !(InX >= 0.0f) || !(InZ >= 0.0f)) { return false; }
        const float Side = BucketSize * float(BucketsPerSide);
        if(InX > Side || InZ > Side) { return false; }
        OutX = std::min(uint32_t(InX / BucketSize), BucketsPerSide - 1);
        OutZ = std::min(uint32_t(InZ / BucketSize), BucketsPerSide - 1);
        return true;
    }

    bool FoliageData::Add(const FoliageInstance& InInstance)
    {
        uint32_t BX = 0, BZ = 0;
        if(InInstance.Type >= Types.size() || !BucketOf(InInstance.X, InInstance.Z, BX, BZ)) { return false; }
        Buckets[size_t(BZ) * BucketsPerSide + BX].push_back(InInstance);
        return true;
    }

    size_t FoliageData::GetInstanceCount() const
    {
        size_t Count = 0;
        for(const auto& Bucket : Buckets) { Count += Bucket.size(); }
        return Count;
    }

    size_t FoliageData::GetInstanceCount(const uint16_t InType) const
    {
        size_t Count = 0;
        for(const auto& Bucket : Buckets)
        {
            for(const FoliageInstance& Each : Bucket) { Count += Each.Type == InType ? 1 : 0; }
        }
        return Count;
    }

    void FoliageData::RemoveType(const uint16_t InType)
    {
        if(InType >= Types.size()) { return; }
        Types.erase(Types.begin() + InType);
        for(auto& Bucket : Buckets)
        {
            Bucket.erase(std::remove_if(Bucket.begin(), Bucket.end(), [InType](const FoliageInstance& Each) { return Each.Type == InType; }),
                         Bucket.end());
            for(FoliageInstance& Each : Bucket) { if(Each.Type > InType) { --Each.Type; } }
        }
        MarkAllDirty();
    }

    void FoliageData::MarkBucketDirty(const uint32_t InX, const uint32_t InZ)
    {
        const uint64_t Next = Revision.fetch_add(1, std::memory_order_acq_rel) + 1;
        if(InX < BucketsPerSide && InZ < BucketsPerSide)
        {
            BucketRevisions[size_t(InZ) * BucketsPerSide + InX].store(Next, std::memory_order_release);
        }
    }

    void FoliageData::MarkAllDirty()
    {
        const uint64_t Next = Revision.fetch_add(1, std::memory_order_acq_rel) + 1;
        for(size_t Index = 0; Index < Buckets.size(); ++Index) { BucketRevisions[Index].store(Next, std::memory_order_release); }
    }

    uint64_t FoliageData::GetBucketRevision(const uint32_t InX, const uint32_t InZ) const
    {
        if(InX >= BucketsPerSide || InZ >= BucketsPerSide) { return 0; }
        return BucketRevisions[size_t(InZ) * BucketsPerSide + InX].load(std::memory_order_acquire);
    }

    bool FoliageData::Serialize(EngineCore::Archive& Ar)
    {
        uint16_t Version = 1;
        if(!Ar.OpenChunk(TAG_FOLIAGE, Version))
        {
            Ar.SetError("not foliage (no NFOL chunk)");
            return false;
        }

        uint32_t Side = BucketsPerSide;
        float    Size = BucketSize;
        Version = 1;
        if(!Ar.OpenChunk(TAG_HEADER, Version)) { Ar.SetError("foliage: no header (FHDR)"); return false; }
        Ar << Side << Size;
        Ar.CloseChunk();
        if(Ar.HasError()) { return false; }
        if(Ar.IsLoading())
        {
            if(Side > BUCKETS_MAX || !(Size > 0.0f)) { Ar.SetError("foliage: the header's bucket grid is invalid"); return false; }
            Types.clear();
            Reset(Side, Size);
        }

        Version = 1;
        if(!Ar.OpenChunk(TAG_TYPES, Version)) { Ar.SetError("foliage: no types (FTYP)"); return false; }
        uint32_t TypeCount = uint32_t(Types.size());
        Ar << TypeCount;
        if(Ar.IsLoading())
        {
            if(TypeCount > FOLIAGE_TYPES_MAX) { Ar.SetError("foliage: too many types"); return false; }
            Types.assign(TypeCount, FoliageType());
        }
        for(FoliageType& Type : Types)
        {
            Ar << Type.Name;
            for(std::string& Mesh : Type.Mesh) { Ar << Mesh; }
            Ar << Type.Material << Type.Spacing << Type.ScaleMin << Type.ScaleMax << Type.AlignToNormal << Type.MaxSlope
               << Type.Sink << Type.CullDistance;
            for(float& Density : Type.LODDensity) { Ar << Density; }
            Ar << Type.ColorVariation << Type.bCastShadows << Type.bRandomYaw;
            if(Ar.HasError()) { return false; }
        }
        Ar.CloseChunk();

        Version = 1;
        if(!Ar.OpenChunk(TAG_INSTANCES, Version)) { Ar.SetError("foliage: no instances (FINS)"); return false; }
        uint32_t Count = uint32_t(GetInstanceCount());
        Ar << Count;
        if(Ar.IsLoading())
        {
            if(uint64_t(Ar.GetSize() - Ar.GetCursor()) < uint64_t(Count) * 22)
            {
                Ar.SetError("foliage: the instances are cut short");
                return false;
            }
            for(uint32_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
            {
                FoliageInstance Each;
                Ar << Each.Type << Each.X << Each.Z << Each.Yaw << Each.Scale << Each.Seed;
                Add(Each);   // one outside the grid, or of no type, is dropped
            }
        }
        else
        {
            for(auto& Bucket : Buckets)
            {
                for(FoliageInstance& Each : Bucket) { Ar << Each.Type << Each.X << Each.Z << Each.Yaw << Each.Scale << Each.Seed; }
            }
        }
        Ar.CloseChunk();

        Ar.CloseChunk();
        if(Ar.IsLoading() && !Ar.HasError()) { MarkAllDirty(); }
        return !Ar.HasError();
    }

    // === Files ================================================================

    bool LoadFoliageFile(const std::string& InPath, FoliageData& OutData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar)) { return false; }
        std::lock_guard<std::mutex> Guard(OutData.GetLock());
        if(!OutData.Serialize(Ar))
        {
            GAMECORE_LOG_WARN("cannot load foliage " + InPath)
            OutData.GetTypes().clear();
            OutData.Reset(0, 6400.0f);
            return false;
        }
        return true;
    }

    bool SaveFoliageFile(const std::string& InPath, const FoliageData& InData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        {
            std::lock_guard<std::mutex> Guard(InData.GetLock());
            /* Serialize only reads when saving. */
            if(!const_cast<FoliageData&>(InData).Serialize(Ar)) { return false; }
        }
        const std::string Temp = InPath + ".tmp";
        if(!EngineCore::SaveArchiveToFile(Temp, Ar))
        {
            GAMECORE_LOG_WARN("cannot write foliage " + Temp)
            return false;
        }
        std::error_code Error;
        std::filesystem::rename(std::filesystem::u8path(Temp), std::filesystem::u8path(InPath), Error);
        if(Error)
        {
            GAMECORE_LOG_WARN("cannot replace foliage " + InPath + ": " + Error.message())
            EngineCore::appFileDelete(Temp);
            return false;
        }
        return true;
    }

    // === FoliageAssetCache ====================================================

    namespace
    {
        struct FoliageCacheState
        {
            std::mutex                                                     Lock;
            std::unordered_map<std::string, std::shared_ptr<FoliageData>> Entries;   // null: failed
            std::atomic<uint64_t>                                          Generation{0};
        };

        FoliageCacheState& GetFoliageCache()
        {
            static FoliageCacheState State;
            return State;
        }
    }

    std::shared_ptr<FoliageData> FoliageAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty()) { return nullptr; }
        FoliageCacheState& Cache = GetFoliageCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Entries.find(InPath);
            if(Found != Cache.Entries.end()) { return Found->second; }
        }
        auto Loaded = std::make_shared<FoliageData>();
        std::shared_ptr<FoliageData> Result;
        if(LoadFoliageFile(InPath, *Loaded)) { Result = std::move(Loaded); }
        else { GAMECORE_LOG_WARN("foliage '" + InPath + "' cannot be loaded: none draws") }
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Entries.emplace(InPath, Result).first->second;
    }

    std::shared_ptr<FoliageData> FoliageAssetCache::FindLoaded(const std::string& InPath)
    {
        FoliageCacheState& Cache = GetFoliageCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        const auto Found = Cache.Entries.find(InPath);
        return Found != Cache.Entries.end() ? Found->second : nullptr;
    }

    void FoliageAssetCache::Store(const std::string& InPath, std::shared_ptr<FoliageData> InData)
    {
        if(InPath.empty()) { return; }
        FoliageCacheState& Cache = GetFoliageCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Generation.fetch_add(1, std::memory_order_relaxed);
        Cache.Entries[InPath] = std::move(InData);
    }

    void FoliageAssetCache::Invalidate(const std::string& InPath)
    {
        FoliageCacheState& Cache = GetFoliageCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Generation.fetch_add(1, std::memory_order_relaxed);
        if(InPath.empty()) { Cache.Entries.clear(); }
        else { Cache.Entries.erase(InPath); }
    }

    uint64_t FoliageAssetCache::GetGeneration()
    {
        return GetFoliageCache().Generation.load(std::memory_order_relaxed);
    }

    // === Brushes ==============================================================

    FoliageBrushResult ApplyFoliageBrush(FoliageData& InFoliage, const TerrainData& InTerrain, const TerrainBrush& InBrush,
                                         FoliageBrushOp& InOutOp)
    {
        FoliageBrushResult Result;
        std::lock_guard<std::mutex> Guard(InFoliage.GetLock());
        std::lock_guard<std::mutex> TerrainGuard(InTerrain.GetLock());
        const std::vector<FoliageType>& Types = InFoliage.GetTypes();
        if(Types.empty() || !(InBrush.Radius > 0.0f) || InFoliage.GetBucketsPerSide() == 0) { return Result; }

        std::vector<uint16_t> Chosen = InOutOp.Types;
        if(Chosen.empty())
        {
            for(uint16_t Each = 0; Each < Types.size(); ++Each) { Chosen.push_back(Each); }
        }
        Chosen.erase(std::remove_if(Chosen.begin(), Chosen.end(), [&](const uint16_t InType) { return InType >= Types.size(); }), Chosen.end());

        Random Rng(InOutOp.Seed);
        InOutOp.Seed = Rng.Next() ^ 0xA5A5A5A5u;
        const float Strength = std::clamp(InBrush.Strength, 0.0f, 1.0f);
        const float Delta    = std::max(InOutOp.DeltaSeconds, 0.0f);

        const auto Touch = [&](const uint32_t InX, const uint32_t InZ)
        {
            const auto Key = std::make_pair(InX, InZ);
            if(std::find(Result.Buckets.begin(), Result.Buckets.end(), Key) == Result.Buckets.end())
            {
                Result.Buckets.push_back(Key);
                InFoliage.MarkBucketDirty(InX, InZ);
            }
        };

        /* The buckets a circle overlaps. */
        const float BucketSize = InFoliage.GetBucketSize();
        const uint32_t Side = InFoliage.GetBucketsPerSide();
        const auto BucketRange = [&](const float InX, const float InZ, const float InRadius, uint32_t& X0, uint32_t& Z0, uint32_t& X1, uint32_t& Z1)
        {
            const auto Clamp = [&](const float InValue) { return uint32_t(std::clamp(int32_t(std::floor(InValue / BucketSize)), 0, int32_t(Side) - 1)); };
            X0 = Clamp(InX - InRadius); X1 = Clamp(InX + InRadius);
            Z0 = Clamp(InZ - InRadius); Z1 = Clamp(InZ + InRadius);
        };

        if(InOutOp.Tool == FT_Erase)
        {
            uint32_t X0, Z0, X1, Z1;
            BucketRange(InBrush.X, InBrush.Z, InBrush.Radius, X0, Z0, X1, Z1);
            for(uint32_t BZ = Z0; BZ <= Z1; ++BZ)
            {
                for(uint32_t BX = X0; BX <= X1; ++BX)
                {
                    std::vector<FoliageInstance>& Bucket = *InFoliage.FindBucket(BX, BZ);
                    const size_t Before = Bucket.size();
                    Bucket.erase(std::remove_if(Bucket.begin(), Bucket.end(), [&](const FoliageInstance& Each)
                    {
                        if(std::find(Chosen.begin(), Chosen.end(), Each.Type) == Chosen.end()) { return false; }
                        const float DX = Each.X - InBrush.X, DZ = Each.Z - InBrush.Z;
                        const float Weight = GetTerrainBrushWeight(InBrush, std::sqrt(DX * DX + DZ * DZ));
                        return Weight > 0.0f && Rng.Fraction() < std::min(1.0f, Weight * Strength * Delta * 10.0f);
                    }), Bucket.end());
                    if(Bucket.size() != Before)
                    {
                        Result.Removed += uint32_t(Before - Bucket.size());
                        Touch(BX, BZ);
                    }
                }
            }
            return Result;
        }

        /* Paint: per type, as many candidates as fill the brush in a third of a second at full strength. */
        const float Area = Pi * InBrush.Radius * InBrush.Radius;
        const float TerrainSize = InTerrain.GetSize();
        for(const uint16_t TypeIndex : Chosen)
        {
            const FoliageType& Type = Types[TypeIndex];
            const float Spacing = std::max(Type.Spacing, 1.0f);
            const float Expected = Area / (Spacing * Spacing) * Strength * Delta * 3.0f;
            uint32_t Tries = uint32_t(Expected);
            if(Rng.Fraction() < Expected - float(Tries)) { ++Tries; }
            Tries = std::min<uint32_t>(Tries, 4096);
            const float MinUp = std::cos(std::clamp(Type.MaxSlope, 0.0f, 90.0f) * Pi / 180.0f);

            for(uint32_t Try = 0; Try < Tries; ++Try)
            {
                const float R     = InBrush.Radius * std::sqrt(Rng.Fraction());
                const float Angle = Rng.Fraction() * 2.0f * Pi;
                const float X = InBrush.X + R * std::cos(Angle);
                const float Z = InBrush.Z + R * std::sin(Angle);
                if(Rng.Fraction() > GetTerrainBrushWeight(InBrush, R)) { continue; }   // fewer toward the rim
                if(X < 0.0f || Z < 0.0f || X > TerrainSize || Z > TerrainSize) { continue; }

                float Normal[3];
                InTerrain.SampleNormal(X, Z, Normal);
                if(Normal[1] < MinUp) { continue; }

                /* Room for it: none of its type nearer than Spacing. */
                bool bCrowded = false;
                uint32_t X0, Z0, X1, Z1;
                BucketRange(X, Z, Spacing, X0, Z0, X1, Z1);
                for(uint32_t BZ = Z0; BZ <= Z1 && !bCrowded; ++BZ)
                {
                    for(uint32_t BX = X0; BX <= X1 && !bCrowded; ++BX)
                    {
                        for(const FoliageInstance& Other : InFoliage.GetBucket(BX, BZ))
                        {
                            if(Other.Type != TypeIndex) { continue; }
                            const float DX = Other.X - X, DZ = Other.Z - Z;
                            if(DX * DX + DZ * DZ < Spacing * Spacing) { bCrowded = true; break; }
                        }
                    }
                }
                if(bCrowded) { continue; }

                FoliageInstance Made;
                Made.Type  = TypeIndex;
                Made.X     = X;
                Made.Z     = Z;
                Made.Yaw   = Type.bRandomYaw ? Rng.Fraction() * 360.0f : 0.0f;
                Made.Scale = Type.ScaleMin + (Type.ScaleMax - Type.ScaleMin) * Rng.Fraction();
                Made.Seed  = Rng.Next();
                uint32_t BX = 0, BZ = 0;
                if(InFoliage.BucketOf(X, Z, BX, BZ) && InFoliage.Add(Made))
                {
                    ++Result.Added;
                    Touch(BX, BZ);
                }
            }
        }
        return Result;
    }

    void GetFoliageTransform(const TerrainData& InTerrain, const FoliageType& InType, const FoliageInstance& InInstance,
                             float (&OutMatrix)[16])
    {
        const float Height = InTerrain.SampleHeight(InInstance.X, InInstance.Z);
        float Normal[3] = { 0.0f, 1.0f, 0.0f };
        if(InType.AlignToNormal > 0.0f) { InTerrain.SampleNormal(InInstance.X, InInstance.Z, Normal); }

        /* Up, leaned toward the ground's normal. */
        const float Align = std::clamp(InType.AlignToNormal, 0.0f, 1.0f);
        float Up[3] = { Normal[0] * Align, 1.0f - Align + Normal[1] * Align, Normal[2] * Align };
        const float UpLength = std::sqrt(Up[0] * Up[0] + Up[1] * Up[1] + Up[2] * Up[2]);
        for(float& Each : Up) { Each /= UpLength > 1e-6f ? UpLength : 1.0f; }

        /* Yawed axes, then the tilt that takes +Y to Up (Rodrigues about Y x Up). */
        const float Yaw = InInstance.Yaw * Pi / 180.0f;
        const float CY = std::cos(Yaw), SY = std::sin(Yaw);
        float Axes[3][3] = { { CY, 0.0f, -SY }, { 0.0f, 1.0f, 0.0f }, { SY, 0.0f, CY } };   // X, Y, Z columns

        const float K[3] = { Up[2], 0.0f, -Up[0] };    // (0, 1, 0) x Up
        const float SinA = std::sqrt(K[0] * K[0] + K[2] * K[2]);
        const float CosA = Up[1];
        if(SinA > 1e-6f)
        {
            const float N[3] = { K[0] / SinA, 0.0f, K[2] / SinA };
            for(auto& V : Axes)
            {
                const float Dot = N[0] * V[0] + N[1] * V[1] + N[2] * V[2];
                const float Cross[3] = { N[1] * V[2] - N[2] * V[1], N[2] * V[0] - N[0] * V[2], N[0] * V[1] - N[1] * V[0] };
                for(int Row = 0; Row < 3; ++Row)
                {
                    V[Row] = V[Row] * CosA + Cross[Row] * SinA + N[Row] * Dot * (1.0f - CosA);
                }
            }
        }

        const float Scale = InInstance.Scale;
        for(int Column = 0; Column < 3; ++Column)
        {
            for(int Row = 0; Row < 3; ++Row) { OutMatrix[Column * 4 + Row] = Axes[Column][Row] * Scale; }
            OutMatrix[Column * 4 + 3] = 0.0f;
        }
        const float Sink = InType.Sink * Scale;
        OutMatrix[12] = InInstance.X - Up[0] * Sink;
        OutMatrix[13] = Height - Up[1] * Sink;
        OutMatrix[14] = InInstance.Z - Up[2] * Sink;
        OutMatrix[15] = 1.0f;
    }
}
