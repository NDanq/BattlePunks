#include "Private/RenderCore/TextureStreaming.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/GDI/GDI.h"

/* TEXTURE_STREAMING.md, "Settings". PoolMB: the levels the draws may ask for
 * before every wanted level is raised alike; MipBias: added to each wanted
 * level (positive: blurrier, less memory); MinResident: the largest side of
 * the levels a texture always keeps, and a texture no larger is not
 * streamed; UploadMB: the textures committed in one frame (0: no limit);
 * DropDelay: seconds a texture keeps more than it wants while the pool has
 * room; MaxLoads: loads on the streaming thread at once. */
NIMBLE_CONFIG_BOOL(GTextureStreaming,            "Rendering", "TextureStreaming",            true,  nullptr);
NIMBLE_CONFIG_INT(GTextureStreamingPoolMB,       "Rendering", "TextureStreamingPoolMB",      512,   nullptr);
NIMBLE_CONFIG_FLOAT(GTextureStreamingMipBias,    "Rendering", "TextureStreamingMipBias",     0.0f,  nullptr);
NIMBLE_CONFIG_INT(GTextureStreamingMinResident,  "Rendering", "TextureStreamingMinResident", 64,    nullptr);
NIMBLE_CONFIG_INT(GTextureStreamingUploadMB,     "Rendering", "TextureStreamingUploadMB",    8,     nullptr);
NIMBLE_CONFIG_FLOAT(GTextureStreamingDropDelay,  "Rendering", "TextureStreamingDropDelay",   5.0f,  nullptr);
/* The "Textures" pool's free blocks kept for reuse (TextureAsset.h). */
NIMBLE_CONFIG_INT(GTextureMemoryCacheMB,         "Rendering", "TextureMemoryCacheMB",        32,    nullptr);
NIMBLE_CONFIG_INT(GTextureStreamingMaxLoads,     "Rendering", "TextureStreamingMaxLoads",    8,     nullptr);

namespace RenderCore
{
    namespace
    {
        constexpr double RETRY_SECONDS   = 5.0;   // after a load fails
        constexpr int    MAX_BUDGET_BIAS = 15;

        double SecondsSince(const std::chrono::steady_clock::time_point InStart)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - InStart).count();
        }

        /* Levels [InFrom, ...) of InSource, InFrom counted from its own level 0. */
        std::shared_ptr<const TextureData> SliceLevels(const TextureData& InSource, const int InFrom)
        {
            auto Out = std::make_shared<TextureData>();
            Out->Format      = InSource.Format;
            Out->bSRGB       = InSource.bSRGB;
            Out->Filter      = InSource.Filter;
            Out->Wrap        = InSource.Wrap;
            Out->MipGen      = InSource.MipGen;
            Out->Compression = InSource.Compression;
            if(!InSource.Blocks.empty())
            {
                Out->Blocks.assign(InSource.Blocks.begin() + InFrom, InSource.Blocks.end());
                return Out;
            }
            Out->Pixels = *InSource.GetMip(InFrom);
            for(int Level = InFrom + 1; Level < InSource.GetMipCount(); ++Level) { Out->Mips.push_back(*InSource.GetMip(Level)); }
            return Out;
        }

        /* InTop's levels (loaded), then InBelow's (resident): one chain. */
        std::shared_ptr<const TextureData> JoinLevels(TextureData&& InTop, const TextureData& InBelow)
        {
            auto Out = std::make_shared<TextureData>(std::move(InTop));
            if(!Out->Blocks.empty())
            {
                Out->Blocks.insert(Out->Blocks.end(), InBelow.Blocks.begin(), InBelow.Blocks.end());
                return Out;
            }
            Out->Mips.push_back(InBelow.Pixels);
            Out->Mips.insert(Out->Mips.end(), InBelow.Mips.begin(), InBelow.Mips.end());
            return Out;
        }
    }

    struct TextureStreamer::Entry
    {
        std::string                        Path;
        TextureFileLayout                  Layout;
        std::vector<uint64_t>              Tail;            // bytes of levels [Level, last], by level
        std::shared_ptr<const TextureData> Resident;
        int      ResidentTop  = 0;
        int      MinTop       = 0;     // the levels always kept: [MinTop, last]
        int      MaxSide      = 1;     // level 0's larger side
        bool     bStreamed    = false; // false: the caller loads it whole

        /* This frame's draws: the most pixels a UV unit spans, or all of it. */
        float    Demand       = 0.0f;
        bool     bDemanded    = false;
        bool     bDemandFull  = false;

        int      DemandWanted = 0;     // from the draws, before the pool's bias
        int      Wanted       = 0;     // ... and after it
        double   LastDemandAt = -1.0e9;
        double   LessDetailSince = -1.0;
        double   FailedAt     = -1.0e9;
        int      PendingTop   = -1;    // a load of [PendingTop, ResidentTop) under way
        uint64_t Generation   = 0;     // bumped by a reimport: older loads are dropped
    };

    struct TextureStreamer::Impl
    {
        std::unordered_map<std::string, std::unique_ptr<Entry>> Entries;
        std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
        uint64_t InvalidationSerial = 0;
        int      BudgetBias         = 0;

        /* The streaming thread's queues. */
        struct Request
        {
            Entry*            Target     = nullptr;
            uint64_t          Generation = 0;
            std::string       Path;
            TextureFileLayout Layout;
            int               First = 0;
            int               End   = 0;
        };
        struct Result
        {
            Entry*      Target     = nullptr;
            uint64_t    Generation = 0;
            int         First = 0;
            int         End   = 0;
            bool        bOk   = false;
            TextureData Levels;
            double      Ms    = 0.0;
            uint64_t    Bytes = 0;
        };
        std::thread               Thread;
        mutable std::mutex        Lock;
        std::condition_variable   Wake;      // a request, or stop
        std::condition_variable   Idle;      // a result
        std::deque<Request>       Requests;
        std::vector<Result>       Results;
        uint32_t                  Working = 0;
        bool                      bStop   = false;

        std::vector<Result>       Ready;     // loaded, waiting for the upload budget

        /* Lifetime counters, and the stats as the last Update left them. */
        uint64_t Loads = 0, BytesRead = 0, Drops = 0;
        float    CreateMs = 0.0f;                 // this frame's so far
        float    CreateHistory[120] = {};         // the frames before, a ring
        uint32_t CreateAt = 0;
        int      AppliedCacheMB = 32;   // the pool's own default (TextureAsset.cpp)
        double   LastLoadMs = 0.0;
        mutable std::mutex    StatsLock;
        TextureStreamingStats Stats;

        ~Impl() { StopThread(); }

        double Now() const { return SecondsSince(Start); }

        void StartThread()
        {
            if(Thread.joinable()) { return; }
            bStop  = false;
            Thread = std::thread([this]() { Run(); });
        }

        void StopThread()
        {
            {
                std::lock_guard<std::mutex> Guard(Lock);
                bStop = true;
                Requests.clear();
            }
            Wake.notify_all();
            if(Thread.joinable()) { Thread.join(); }
            Results.clear();
            Working = 0;
        }

        /* The streaming thread: one load at a time, off copies of the layout. */
        void Run()
        {
            std::unique_lock<std::mutex> Guard(Lock);
            for(;;)
            {
                Wake.wait(Guard, [this]() { return bStop || !Requests.empty(); });
                if(bStop) { return; }
                Request Job = std::move(Requests.front());
                Requests.pop_front();
                ++Working;
                Guard.unlock();

                Result Done;
                Done.Target     = Job.Target;
                Done.Generation = Job.Generation;
                Done.First      = Job.First;
                Done.End        = Job.End;
                const auto Began = std::chrono::steady_clock::now();
                Done.bOk = LoadTextureLevels(Job.Path, Job.Layout, Job.First, Job.End, Done.Levels);
                Done.Ms  = SecondsSince(Began) * 1000.0;
                for(int Level = Job.First; Level < Job.End; ++Level) { Done.Bytes += Job.Layout.LevelBytes[size_t(Level)]; }

                Guard.lock();
                --Working;
                Results.push_back(std::move(Done));
                Idle.notify_all();
            }
        }

        uint32_t InFlight() const
        {
            std::lock_guard<std::mutex> Guard(Lock);
            return uint32_t(Requests.size()) + Working + uint32_t(Results.size());
        }

        /* (Re)reads InEntry's layout and loads its tail at once. */
        void Prepare(Entry& InEntry)
        {
            ++InEntry.Generation;
            InEntry.PendingTop = -1;
            InEntry.bStreamed  = false;
            InEntry.Resident.reset();
            InEntry.LessDetailSince = -1.0;

            TextureFileLayout Layout;
            if(!ReadTextureLayout(InEntry.Path, Layout) || Layout.GetMipCount() < 2
               || Layout.Header.Format != ETexturePixelFormat::RGBA8)
            {
                return;
            }
            const int MinSide = std::max(GTextureStreamingMinResident.GetInt(), 1);
            int MinTop = 0;
            while(MinTop + 1 < Layout.GetMipCount()
                  && std::max(Layout.LevelWidth(MinTop), Layout.LevelHeight(MinTop)) > MinSide)
            {
                ++MinTop;
            }
            if(MinTop == 0) { return; }   // small already: whole

            TextureData Tail;
            if(!LoadTextureLevels(InEntry.Path, Layout, MinTop, -1, Tail)) { return; }

            InEntry.Layout = std::move(Layout);
            InEntry.Tail.assign(size_t(InEntry.Layout.GetMipCount()) + 1, 0);
            for(int Level = InEntry.Layout.GetMipCount() - 1; Level >= 0; --Level)
            {
                InEntry.Tail[size_t(Level)] = InEntry.Tail[size_t(Level) + 1] + InEntry.Layout.LevelBytes[size_t(Level)];
            }
            InEntry.MaxSide      = std::max(InEntry.Layout.Header.Width, InEntry.Layout.Header.Height);
            InEntry.MinTop       = MinTop;
            InEntry.ResidentTop  = MinTop;
            InEntry.DemandWanted = MinTop;
            InEntry.Wanted       = MinTop;
            InEntry.Resident     = std::make_shared<const TextureData>(std::move(Tail));
            InEntry.bStreamed    = true;
        }

        /* A reimport (TextureAssetCache::Invalidate): the file again. */
        void TakeInvalidations()
        {
            std::vector<std::string> Paths;
            if(!TextureAssetCache::ConsumeInvalidations(InvalidationSerial, Paths)) { return; }
            for(const std::string& Path : Paths)
            {
                if(Path.empty())
                {
                    for(auto& Each : Entries) { Prepare(*Each.second); }
                    return;
                }
                const auto Found = Entries.find(Path);
                if(Found != Entries.end()) { Prepare(*Found->second); }
            }
        }

        /* The streaming thread's results, to Ready (or dropped: stale, failed). */
        void TakeResults()
        {
            std::vector<Result> Taken;
            {
                std::lock_guard<std::mutex> Guard(Lock);
                Taken.swap(Results);
            }
            const double Time = Now();
            for(Result& Each : Taken)
            {
                Entry& Target = *Each.Target;
                if(Each.Generation != Target.Generation) { continue; }
                if(!Each.bOk)
                {
                    Target.PendingTop = -1;
                    Target.FailedAt   = Time;
                    continue;
                }
                ++Loads;
                BytesRead  += Each.Bytes;
                LastLoadMs  = Each.Ms;
                Ready.push_back(std::move(Each));
            }
        }

        /* Ready's loads joined to what is resident, until InBudget bytes of
         * textures are made this frame (at least one; 0: no limit). */
        void Commit(const uint64_t InBudget, uint32_t& OutCommits, uint64_t& OutUploaded)
        {
            OutCommits  = 0;
            OutUploaded = 0;
            size_t Index = 0;
            for(; Index < Ready.size(); ++Index)
            {
                Result& Each   = Ready[Index];
                Entry&  Target = *Each.Target;
                if(Each.Generation != Target.Generation || Each.End != Target.ResidentTop || !Target.Resident)
                {
                    Target.PendingTop = Each.Generation == Target.Generation ? -1 : Target.PendingTop;
                    continue;   // reimported, or dropped below what it was loaded onto
                }
                const uint64_t Bytes = Target.Tail[size_t(Each.First)];
                if(InBudget > 0 && OutCommits > 0 && OutUploaded + Bytes > InBudget) { break; }
                Target.Resident    = JoinLevels(std::move(Each.Levels), *Target.Resident);
                Target.ResidentTop = Each.First;
                Target.PendingTop  = -1;
                ++OutCommits;
                OutUploaded += Bytes;
            }
            Ready.erase(Ready.begin(), Ready.begin() + std::ptrdiff_t(Index));
        }

        /* This frame's demand to each texture's wanted level, then the pool's bias. */
        void ComputeWanted(const double InTime)
        {
            const float  MipBias = GTextureStreamingMipBias.GetFloat();
            const double Delay   = std::max(GTextureStreamingDropDelay.GetFloat(), 0.0f);
            for(auto& Pair : Entries)
            {
                Entry& Each = *Pair.second;
                if(!Each.bStreamed) { continue; }
                if(Each.bDemanded)
                {
                    int Level = 0;
                    if(!Each.bDemandFull && Each.Demand > 0.0f)
                    {
                        /* Texels a pixel covers at level 0, as a level: 1:1 at the level chosen. */
                        const float Texels = float(Each.MaxSide) / Each.Demand;
                        Level = int(std::floor(std::log2(std::max(Texels, 1.0e-6f)) + MipBias));
                    }
                    Each.DemandWanted = std::clamp(Level, 0, Each.MinTop);
                    Each.LastDemandAt = InTime;
                }
                else if(InTime - Each.LastDemandAt > Delay)
                {
                    Each.DemandWanted = Each.MinTop;   // not drawn for a while
                }
                Each.bDemanded = Each.bDemandFull = false;
                Each.Demand    = 0.0f;
            }

            /* The smallest bias, for every texture alike, that fits the pool. */
            const uint64_t Pool = uint64_t(std::max(GTextureStreamingPoolMB.GetInt(), 1)) * 1024 * 1024;
            for(BudgetBias = 0; BudgetBias < MAX_BUDGET_BIAS; ++BudgetBias)
            {
                if(WantedBytes(BudgetBias) <= Pool) { break; }
            }
            for(auto& Pair : Entries)
            {
                Entry& Each = *Pair.second;
                if(Each.bStreamed) { Each.Wanted = std::min(Each.DemandWanted + BudgetBias, Each.MinTop); }
            }
        }

        uint64_t WantedBytes(const int InBias) const
        {
            uint64_t Bytes = 0;
            for(const auto& Pair : Entries)
            {
                const Entry& Each = *Pair.second;
                if(Each.bStreamed) { Bytes += Each.Tail[size_t(std::min(Each.DemandWanted + InBias, Each.MinTop))]; }
            }
            return Bytes;
        }

        uint64_t ResidentBytes() const
        {
            uint64_t Bytes = 0;
            for(const auto& Pair : Entries)
            {
                if(Pair.second->bStreamed) { Bytes += Pair.second->Tail[size_t(Pair.second->ResidentTop)]; }
            }
            return Bytes;
        }

        /* More than wanted: dropped after DropDelay, or now with the pool over. */
        void Drop(const double InTime)
        {
            const uint64_t Pool  = uint64_t(std::max(GTextureStreamingPoolMB.GetInt(), 1)) * 1024 * 1024;
            const double   Delay = std::max(GTextureStreamingDropDelay.GetFloat(), 0.0f);
            uint64_t Resident = ResidentBytes();
            for(auto& Pair : Entries)
            {
                Entry& Each = *Pair.second;
                if(!Each.bStreamed || Each.ResidentTop >= Each.Wanted)
                {
                    Each.LessDetailSince = -1.0;
                    continue;
                }
                if(Each.LessDetailSince < 0.0) { Each.LessDetailSince = InTime; }
                if(Resident <= Pool && InTime - Each.LessDetailSince < Delay) { continue; }
                Resident -= Each.Tail[size_t(Each.ResidentTop)] - Each.Tail[size_t(Each.Wanted)];
                Each.Resident        = SliceLevels(*Each.Resident, Each.Wanted - Each.ResidentTop);
                Each.ResidentTop     = Each.Wanted;
                Each.LessDetailSince = -1.0;
                ++Drops;
            }
        }

        /* Loads for what wants more than it has: the largest shortfall
         * first, then the nearest, until InMaxLoads are in flight. */
        uint32_t RequestLoads(const uint32_t InMaxLoads, const double InTime)
        {
            std::vector<Entry*> Wanting;
            for(auto& Pair : Entries)
            {
                Entry& Each = *Pair.second;
                if(Each.bStreamed && Each.PendingTop < 0 && Each.Wanted < Each.ResidentTop && InTime - Each.FailedAt > RETRY_SECONDS)
                {
                    Wanting.push_back(&Each);
                }
            }
            std::sort(Wanting.begin(), Wanting.end(), [](const Entry* A, const Entry* B)
            {
                const int GapA = A->ResidentTop - A->Wanted, GapB = B->ResidentTop - B->Wanted;
                if(GapA != GapB) { return GapA > GapB; }
                return A->Wanted < B->Wanted;
            });

            uint32_t Flying = InFlight() + uint32_t(Ready.size());
            size_t   Next   = 0;
            for(; Next < Wanting.size() && (InMaxLoads == 0 || Flying < InMaxLoads); ++Next, ++Flying)
            {
                Entry& Each = *Wanting[Next];
                Impl::Request Job;
                Job.Target     = &Each;
                Job.Generation = Each.Generation;
                Job.Path       = Each.Path;
                Job.Layout     = Each.Layout;
                Job.First      = Each.Wanted;
                Job.End        = Each.ResidentTop;
                Each.PendingTop = Each.Wanted;
                StartThread();
                {
                    std::lock_guard<std::mutex> Guard(Lock);
                    Requests.push_back(std::move(Job));
                }
                Wake.notify_one();
            }
            return uint32_t(Wanting.size() - Next);
        }

        void Publish(const uint32_t InWaiting, const uint32_t InCommits, const uint64_t InUploaded, const double InUpdateMs)
        {
            TextureStreamingStats Out;
            Out.bEnabled = GTextureStreaming.GetBool();
            for(const auto& Pair : Entries)
            {
                const Entry& Each = *Pair.second;
                if(!Each.bStreamed) { continue; }
                ++Out.Textures;
                Out.ResidentBytes += Each.Tail[size_t(Each.ResidentTop)];
                Out.WantedBytes   += Each.Tail[size_t(Each.Wanted)];
                Out.FullBytes     += Each.Tail[0];
            }
            Out.PoolBytes            = uint64_t(std::max(GTextureStreamingPoolMB.GetInt(), 1)) * 1024 * 1024;
            Out.BudgetBias           = BudgetBias;
            Out.PendingLoads         = InFlight() + uint32_t(Ready.size());
            Out.WaitingLoads         = InWaiting;
            Out.CommitsLastFrame     = InCommits;
            Out.UploadBytesLastFrame = InUploaded;
            Out.Loads     = Loads;
            Out.BytesRead = BytesRead;
            Out.Drops     = Drops;
            Out.LoadMs    = float(LastLoadMs);
            Out.UpdateMs  = float(InUpdateMs);
            if(EngineCore::BucketMemoryPool* Memory = GetTextureMemory())
            {
                EngineCore::MemoryPoolStats Pool;
                Memory->GetStats(Pool);
                Out.CpuBytes       = Pool.BytesInUse;
                Out.CpuCachedBytes = Pool.BytesCommitted - Pool.BytesInUse;
            }
            GDITextureMemoryStats Gpu;
            if(GDIGetTextureMemoryStats(Gpu))
            {
                Out.GpuBlockBytes = Gpu.BlockBytes;
                Out.GpuBlockUsed  = Gpu.BlockUsedBytes;
                Out.GpuDedicated  = Gpu.DedicatedBytes;
                Out.GpuStaging    = Gpu.StagingBytes;
            }
            Out.CreateMs  = CreateHistory[(CreateAt + 119) % 120];
            Out.CreateMsPeak = *std::max_element(std::begin(CreateHistory), std::end(CreateHistory));
            std::lock_guard<std::mutex> Guard(StatsLock);
            Stats = Out;
        }
    };

    TextureStreamer::TextureStreamer() : P(new Impl())
    {
        /* Reimports from before it are nothing to it. */
        std::vector<std::string> Before;
        TextureAssetCache::ConsumeInvalidations(P->InvalidationSerial, Before);
    }
    TextureStreamer::~TextureStreamer() = default;

    bool TextureStreamer::IsEnabled()
    {
        return GTextureStreaming.GetBool();
    }

    TextureStreamer::Entry* TextureStreamer::FindEntry(const std::string& InPath)
    {
        if(InPath.empty() || !GTextureStreaming.GetBool()) { return nullptr; }
        std::unique_ptr<Entry>& Slot = P->Entries[InPath];
        if(!Slot)
        {
            Slot = std::make_unique<Entry>();
            Slot->Path = InPath;
            P->Prepare(*Slot);
        }
        return Slot->bStreamed ? Slot.get() : nullptr;
    }

    const std::shared_ptr<const TextureData>& TextureStreamer::GetResident(const Entry& InEntry)
    {
        return InEntry.Resident;
    }

    int TextureStreamer::GetResidentTop(const Entry& InEntry)
    {
        return InEntry.ResidentTop;
    }

    int TextureStreamer::GetWanted(const Entry& InEntry)
    {
        return InEntry.Wanted;
    }

    void TextureStreamer::NoteDemand(Entry& InEntry, const float InPixelsPerUV)
    {
        InEntry.bDemanded = true;
        if(InPixelsPerUV <= 0.0f || !std::isfinite(InPixelsPerUV)) { InEntry.bDemandFull = true; }
        else                                                       { InEntry.Demand = std::max(InEntry.Demand, InPixelsPerUV); }
    }

    void TextureStreamer::NoteCreateMs(const float InMs)
    {
        P->CreateMs += InMs;
    }

    void TextureStreamer::Update()
    {
        const auto Began = std::chrono::steady_clock::now();
        P->CreateHistory[P->CreateAt] = P->CreateMs;   // the frame that just ended
        const int CacheMB = std::max(GTextureMemoryCacheMB.GetInt(), 0);
        if(CacheMB != P->AppliedCacheMB)
        {
            if(EngineCore::BucketMemoryPool* Memory = GetTextureMemory()) { Memory->SetMaxCachedBytes(size_t(CacheMB) * 1024 * 1024); }
            P->AppliedCacheMB = CacheMB;
        }
        P->CreateAt = (P->CreateAt + 1) % 120;
        P->CreateMs = 0.0f;
        const double Time = P->Now();
        P->TakeInvalidations();
        P->TakeResults();
        uint32_t Commits  = 0;
        uint64_t Uploaded = 0;
        P->Commit(uint64_t(std::max(GTextureStreamingUploadMB.GetInt(), 0)) * 1024 * 1024, Commits, Uploaded);
        P->ComputeWanted(Time);
        P->Drop(Time);
        const uint32_t Waiting = P->RequestLoads(uint32_t(std::max(GTextureStreamingMaxLoads.GetInt(), 1)), Time);
        P->Publish(Waiting, Commits, Uploaded, SecondsSince(Began) * 1000.0);
    }

    void TextureStreamer::Flush()
    {
        /* The levels the last Update wanted: no demand has been gathered since. */
        const double Time = P->Now();
        P->TakeInvalidations();
        uint32_t Commits = 0, AllCommits = 0;
        uint64_t Uploaded = 0, AllUploaded = 0;
        for(int Round = 0; Round < 64; ++Round)
        {
            P->TakeResults();
            P->Commit(0, Commits, Uploaded);
            AllCommits  += Commits;
            AllUploaded += Uploaded;
            const uint32_t Waiting = P->RequestLoads(0, Time);
            if(Waiting == 0 && P->InFlight() == 0 && P->Ready.empty()) { break; }
            std::unique_lock<std::mutex> Guard(P->Lock);
            P->Idle.wait(Guard, [this]() { return P->Requests.empty() && P->Working == 0; });
        }
        P->Publish(0, AllCommits, AllUploaded, 0.0);
    }

    void TextureStreamer::ReleaseAll()
    {
        P->StopThread();
        P->Ready.clear();
        P->Entries.clear();
        P->BudgetBias = 0;
        P->Publish(0, 0, 0, 0.0);
    }

    TextureStreamingStats TextureStreamer::GetStats() const
    {
        std::lock_guard<std::mutex> Guard(P->StatsLock);
        return P->Stats;
    }
}
