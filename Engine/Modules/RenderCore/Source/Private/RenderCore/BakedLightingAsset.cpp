#include "Public/RenderCore/BakedLightingAsset.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_LIGHTING = EngineCore::MakeArchiveTag('N','B','L','T');
        constexpr uint32_t TAG_HEADER   = EngineCore::MakeArchiveTag('B','L','H','D');
        constexpr uint32_t TAG_ATLASES  = EngineCore::MakeArchiveTag('B','L','A','T');
        constexpr uint32_t TAG_ENTRIES  = EngineCore::MakeArchiveTag('B','L','M','P');
        constexpr uint32_t TAG_PROBES   = EngineCore::MakeArchiveTag('B','L','P','R');

        constexpr uint32_t MAX_PROBES = 1u << 22;
    }

    // === BakedAmbientCube / BakedProbeGrid =====================================

    void BakedAmbientCube::Evaluate(const float (&InNormal)[3], float (&OutRgb)[3]) const
    {
        OutRgb[0] = OutRgb[1] = OutRgb[2] = 0.0f;
        for(int Axis = 0; Axis < 3; ++Axis)
        {
            const float  Weight = InNormal[Axis] * InNormal[Axis];
            const float* Face   = Faces[Axis * 2 + (InNormal[Axis] < 0.0f ? 1 : 0)];
            for(int Channel = 0; Channel < 3; ++Channel) { OutRgb[Channel] += Weight * Face[Channel]; }
        }
    }

    bool BakedProbeGrid::Sample(const float (&InPoint)[3], BakedAmbientCube& OutCube) const
    {
        OutCube = BakedAmbientCube();
        if(Probes.empty() || Spacing <= 0.0f) { return false; }

        int   Base[3];
        float Frac[3];
        for(int Axis = 0; Axis < 3; ++Axis)
        {
            const float Cells = float(Counts[Axis]) - 1.0f;
            const float At    = std::clamp((InPoint[Axis] - Origin[Axis]) / Spacing, 0.0f, std::max(Cells, 0.0f));
            Base[Axis] = std::min(int(At), std::max(int(Counts[Axis]) - 2, 0));
            Frac[Axis] = Counts[Axis] > 1 ? At - float(Base[Axis]) : 0.0f;
        }

        float Total = 0.0f;
        for(int Corner = 0; Corner < 8; ++Corner)
        {
            int   Cell[3];
            float Weight = 1.0f;
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                const int Step = (Corner >> Axis) & 1;
                Cell[Axis] = std::min(Base[Axis] + Step, int(Counts[Axis]) - 1);
                Weight    *= Step ? Frac[Axis] : 1.0f - Frac[Axis];
            }
            const size_t Index = (size_t(Cell[2]) * Counts[1] + size_t(Cell[1])) * Counts[0] + size_t(Cell[0]);
            if(Weight <= 0.0f || Index >= Probes.size() || (Index < Valid.size() && !Valid[Index])) { continue; }
            for(int Face = 0; Face < 6; ++Face)
            {
                for(int Channel = 0; Channel < 3; ++Channel) { OutCube.Faces[Face][Channel] += Weight * Probes[Index].Faces[Face][Channel]; }
            }
            Total += Weight;
        }
        if(Total <= 1e-6f) { return false; }
        for(auto& Face : OutCube.Faces)
        {
            for(float& Channel : Face) { Channel /= Total; }
        }
        return true;
    }

    const BakedLightmapEntry* BakedLightingData::FindEntry(const uint64_t InGuidHi, const uint64_t InGuidLo) const
    {
        for(const BakedLightmapEntry& Each : Entries)
        {
            if(Each.GuidHi == InGuidHi && Each.GuidLo == InGuidLo) { return &Each; }
        }
        return nullptr;
    }

    // === The file =============================================================

    bool SerializeBakedLighting(EngineCore::Archive& Ar, BakedLightingData& InOutData)
    {
        if(!OpenRequiredChunk(Ar, TAG_LIGHTING, 1, "not baked lighting (no NBLT chunk)")) { return false; }

        uint32_t AtlasCount = uint32_t(InOutData.Atlases.size());
        uint32_t EntryCount = uint32_t(InOutData.Entries.size());
        uint32_t ProbeCount = uint32_t(InOutData.Probes.Probes.size());
        uint16_t HeaderVersion = 2;
        if(Ar.OpenChunk(TAG_HEADER, HeaderVersion))
        {
            uint8_t SkyBaked = InOutData.bSkyBaked ? 1 : 0;
            Ar << InOutData.InputHash << SkyBaked << AtlasCount << EntryCount << ProbeCount;
            InOutData.bSkyBaked = SkyBaked != 0;
            uint8_t Directional = InOutData.bDirectional ? 1 : 0;
            if(Ar.GetChunkVersion() >= 2) { Ar << Directional; }
            InOutData.bDirectional = Directional != 0;
            Ar.CloseChunk();
        }
        else
        {
            Ar.SetError("baked lighting: no BLHD chunk");
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_ATLASES, 1, "baked lighting: no BLAT chunk"))
        {
            if(Ar.IsLoading() && CanHoldElements(Ar, AtlasCount, sizeof(uint32_t), "baked lighting: atlas count is past the end"))
            {
                InOutData.Atlases.assign(AtlasCount, std::string());
            }
            for(std::string& Each : InOutData.Atlases) { Ar << Each; }
            Ar.CloseChunk();
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_ENTRIES, 1, "baked lighting: no BLMP chunk"))
        {
            if(Ar.IsLoading() && CanHoldElements(Ar, EntryCount, 2 * sizeof(uint64_t) + sizeof(uint32_t) + 4 * sizeof(float),
                                                 "baked lighting: entry count is past the end"))
            {
                InOutData.Entries.assign(EntryCount, BakedLightmapEntry());
            }
            for(BakedLightmapEntry& Each : InOutData.Entries)
            {
                Ar << Each.GuidHi << Each.GuidLo << Each.Atlas;
                for(float& Value : Each.ScaleOffset) { Ar << Value; }
                if(Ar.IsLoading() && Each.Atlas >= AtlasCount) { Ar.SetError("baked lighting: an entry names an atlas past the last"); }
            }
            Ar.CloseChunk();
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_PROBES, 1, "baked lighting: no BLPR chunk"))
        {
            BakedProbeGrid& Grid = InOutData.Probes;
            for(float& Value : Grid.Origin) { Ar << Value; }
            Ar << Grid.Spacing;
            for(uint32_t& Value : Grid.Counts) { Ar << Value; }
            if(Ar.IsLoading())
            {
                const uint64_t Expected = uint64_t(Grid.Counts[0]) * Grid.Counts[1] * Grid.Counts[2];
                if(Expected != ProbeCount || ProbeCount > MAX_PROBES)
                {
                    Ar.SetError("baked lighting: the probe grid's size does not match its probe count");
                }
                else if(CanHoldElements(Ar, ProbeCount, 18 * sizeof(float) + 1, "baked lighting: probe count is past the end"))
                {
                    Grid.Probes.assign(ProbeCount, BakedAmbientCube());
                    Grid.Valid.assign(ProbeCount, 1);
                }
            }
            for(size_t Index = 0; Index < Grid.Probes.size() && !Ar.HasError(); ++Index)
            {
                for(auto& Face : Grid.Probes[Index].Faces)
                {
                    for(float& Channel : Face) { Ar << Channel; }
                }
                uint8_t Valid = Index < Grid.Valid.size() ? Grid.Valid[Index] : 1;
                Ar << Valid;
                if(Ar.IsLoading()) { Grid.Valid[Index] = Valid; }
            }
            Ar.CloseChunk();
        }

        Ar.CloseChunk();
        return !Ar.HasError();
    }

    bool LoadBakedLightingFile(const std::string& InPath, BakedLightingData& OutData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutData = BakedLightingData();
        if(!SerializeBakedLighting(Ar, OutData))
        {
            RENDERCORE_LOG_WARN("cannot load baked lighting " + InPath)
            OutData = BakedLightingData();
            return false;
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveBakedLightingFile(const std::string& InPath, BakedLightingData& InData)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeBakedLighting(Ar, InData))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    // === BakedLightingCache ===================================================

    namespace
    {
        struct CacheState
        {
            std::mutex                                                                Lock;
            std::unordered_map<std::string, std::shared_ptr<const BakedLightingData>> Bakes;   // null: failed
            std::atomic<uint64_t>                                                     Generation{0};
        };

        CacheState& GetCache()
        {
            static CacheState State;
            return State;
        }
    }

    std::shared_ptr<const BakedLightingData> BakedLightingCache::Find(const std::string& InPath)
    {
        if(InPath.empty()) { return nullptr; }
        CacheState& Cache = GetCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Bakes.find(InPath);
            if(Found != Cache.Bakes.end()) { return Found->second; }
        }

        auto Loaded = std::make_shared<BakedLightingData>();
        std::shared_ptr<const BakedLightingData> Result;
        if(LoadBakedLightingFile(InPath, *Loaded)) { Result = std::move(Loaded); }
        else { RENDERCORE_LOG_WARN("baked lighting '" + InPath + "' cannot be loaded; lighting the world live") }

        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Bakes.emplace(InPath, Result).first->second;
    }

    void BakedLightingCache::Invalidate(const std::string& InPath)
    {
        CacheState& Cache = GetCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Generation.fetch_add(1, std::memory_order_relaxed);
        if(InPath.empty()) { Cache.Bakes.clear(); }
        else { Cache.Bakes.erase(InPath); }
    }

    uint64_t BakedLightingCache::GetGeneration()
    {
        return GetCache().Generation.load(std::memory_order_relaxed);
    }

    std::string BakedLightingFolderFor(const std::string& InWorldPath)
    {
        static const std::string Extension = ".nmworld";
        if(InWorldPath.size() <= Extension.size()
           || InWorldPath.compare(InWorldPath.size() - Extension.size(), Extension.size(), Extension) != 0)
        {
            return std::string();
        }
        return InWorldPath.substr(0, InWorldPath.size() - Extension.size()) + "/Lighting/";
    }
}
