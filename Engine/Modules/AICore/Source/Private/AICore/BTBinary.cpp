#include "Public/AICore/BehaviorTreeFile.h"

#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreLog.h"

/*
 * The cooked .nmbt -- "NMBT", a version, then KEYS (the blackboard) and NODE
 * (the nodes, depth-first) chunks -- and the loaders and cache over both
 * forms. BehaviorTreeFile.h has the contract.
 */
namespace AICore
{
    namespace
    {
        constexpr char     MAGIC[4]       = { 'N', 'M', 'B', 'T' };
        constexpr uint32_t BINARY_VERSION = 1;
        constexpr uint32_t COUNT_MAX      = 100000;

        constexpr uint32_t Tag(const char InA, const char InB, const char InC, const char InD)
        {
            return uint32_t(uint8_t(InA)) | (uint32_t(uint8_t(InB)) << 8) | (uint32_t(uint8_t(InC)) << 16) | (uint32_t(uint8_t(InD)) << 24);
        }
        constexpr uint32_t TAG_KEYS  = Tag('K', 'E', 'Y', 'S');
        constexpr uint32_t TAG_NODES = Tag('N', 'O', 'D', 'E');

        bool Count(EngineCore::Archive& Ar, uint32_t& InOutCount)
        {
            Ar << InOutCount;
            if(Ar.IsLoading() && InOutCount > COUNT_MAX) { Ar.SetError("an impossible count"); return false; }
            return !Ar.HasError();
        }

        void SerializeParams(EngineCore::Archive& Ar, std::vector<BTParam>& InOutParams)
        {
            uint32_t N = uint32_t(InOutParams.size());
            if(!Count(Ar, N)) { return; }
            if(Ar.IsLoading()) { InOutParams.resize(N); }
            for(BTParam& Param : InOutParams)
            {
                Ar << Param.Name;
                Ar << Param.Text;
            }
        }

        void SerializeNode(EngineCore::Archive& Ar, BTNodeDef& InOutNode)
        {
            uint8_t Kind = uint8_t(InOutNode.Kind), Composite = uint8_t(InOutNode.Composite);
            Ar << Kind << Composite;
            InOutNode.Kind = Kind == uint8_t(EBTNodeKind::Composite) ? EBTNodeKind::Composite : EBTNodeKind::Task;
            InOutNode.Composite = Composite < uint8_t(EBTComposite::Count) ? EBTComposite(Composite) : EBTComposite::Sequence;
            Ar << InOutNode.Type;
            Ar << InOutNode.Name;
            Ar << InOutNode.Weight;
            SerializeParams(Ar, InOutNode.Params);

            uint32_t N = uint32_t(InOutNode.Children.size());
            if(!Count(Ar, N)) { return; }
            if(Ar.IsLoading()) { InOutNode.Children.resize(N); }
            for(uint32_t& Child : InOutNode.Children) { Ar << Child; }

            N = uint32_t(InOutNode.Decorators.size());
            if(!Count(Ar, N)) { return; }
            if(Ar.IsLoading()) { InOutNode.Decorators.resize(N); }
            for(BTDecoratorDef& D : InOutNode.Decorators)
            {
                uint8_t Abort = uint8_t(D.Abort);
                Ar << D.Type;
                Ar << Abort;
                D.Abort = Abort < 4 ? EBTAbortMode(Abort) : EBTAbortMode::None;
                SerializeParams(Ar, D.Params);
            }

            N = uint32_t(InOutNode.Services.size());
            if(!Count(Ar, N)) { return; }
            if(Ar.IsLoading()) { InOutNode.Services.resize(N); }
            for(BTServiceDef& S : InOutNode.Services)
            {
                Ar << S.Type;
                Ar << S.Interval;
                SerializeParams(Ar, S.Params);
            }

            N = uint32_t(InOutNode.Considerations.size());
            if(!Count(Ar, N)) { return; }
            if(Ar.IsLoading()) { InOutNode.Considerations.resize(N); }
            for(BTConsideration& C : InOutNode.Considerations)
            {
                uint8_t Curve = uint8_t(C.Curve);
                Ar << C.Input;
                Ar << C.Min << C.Max;
                Ar << Curve;
                C.Curve = Curve < uint8_t(EBTCurve::Count) ? EBTCurve(Curve) : EBTCurve::Linear;
                Ar << C.Slope << C.Exponent << C.XShift << C.YShift << C.Weight;
                uint32_t Points = uint32_t(C.Points.size());
                if(!Count(Ar, Points)) { return; }
                if(Ar.IsLoading()) { C.Points.resize(Points); }
                for(float& Value : C.Points) { Ar << Value; }
            }
        }
    }

    EBTFileFormat DetectBehaviorTreeFormat(const std::vector<uint8_t>& InBytes)
    {
        if(InBytes.size() >= 4 && std::memcmp(InBytes.data(), MAGIC, 4) == 0) { return EBTFileFormat::Binary; }
        size_t At = 0;
        if(InBytes.size() >= 3 && InBytes[0] == 0xEF && InBytes[1] == 0xBB && InBytes[2] == 0xBF) { At = 3; }
        while(At < InBytes.size() && std::isspace(InBytes[At])) { ++At; }
        return At < InBytes.size() && InBytes[At] == '<' ? EBTFileFormat::Text : EBTFileFormat::Unknown;
    }

    std::vector<uint8_t> WriteBehaviorTreeBinary(const BehaviorTreeAsset& InAsset)
    {
        BehaviorTreeAsset Asset = InAsset;
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        char Magic[4];
        std::memcpy(Magic, MAGIC, 4);
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = BINARY_VERSION;
        Ar << Version;
        Ar << Asset.Name;

        uint16_t ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_KEYS, ChunkVersion))
        {
            uint32_t N = uint32_t(Asset.Keys.size());
            Count(Ar, N);
            for(BlackboardKey& Key : Asset.Keys)
            {
                uint8_t Type = uint8_t(Key.Type);
                Ar << Key.Name << Type << Key.Default;
            }
            Ar.CloseChunk();
        }
        ChunkVersion = 1;
        if(Ar.OpenChunk(TAG_NODES, ChunkVersion))
        {
            uint32_t N = uint32_t(Asset.Nodes.size());
            Count(Ar, N);
            for(BTNodeDef& Node : Asset.Nodes) { SerializeNode(Ar, Node); }
            Ar.CloseChunk();
        }
        return Ar.GetBytes();
    }

    bool ReadBehaviorTreeBinary(const std::vector<uint8_t>& InBytes, BehaviorTreeAsset& OutAsset, std::string* OutError)
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) { *OutError = InWhy; } return false; };
        if(DetectBehaviorTreeFormat(InBytes) != EBTFileFormat::Binary) { return Fail("is not a cooked .nmbt"); }

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        char Magic[4];
        Ar.SerializeBytes(Magic, 4);
        uint32_t Version = 0;
        Ar << Version;
        if(Version == 0 || Version > BINARY_VERSION) { return Fail("was cooked by a newer build (version " + std::to_string(Version) + ")"); }

        BehaviorTreeAsset Asset;
        Ar << Asset.Name;
        while(!Ar.HasError() && !Ar.IsAtEnd())
        {
            uint16_t ChunkVersion = 0;
            if(Ar.OpenChunk(TAG_KEYS, ChunkVersion))
            {
                uint32_t N = 0;
                if(Count(Ar, N))
                {
                    Asset.Keys.resize(N);
                    for(BlackboardKey& Key : Asset.Keys)
                    {
                        uint8_t Type = 0;
                        Ar << Key.Name << Type << Key.Default;
                        Key.Type = Type < uint8_t(EBlackboardType::Count) ? EBlackboardType(Type) : EBlackboardType::Bool;
                    }
                }
                Ar.CloseChunk();
            }
            else if(Ar.OpenChunk(TAG_NODES, ChunkVersion))
            {
                uint32_t N = 0;
                if(Count(Ar, N))
                {
                    Asset.Nodes.resize(N);
                    for(BTNodeDef& Node : Asset.Nodes)
                    {
                        SerializeNode(Ar, Node);
                        if(Ar.HasError()) { break; }
                    }
                }
                Ar.CloseChunk();
            }
            else
            {
                uint16_t Skip = 0;
                if(Ar.OpenChunk(Ar.PeekChunkTag(), Skip)) { Ar.CloseChunk(); }
                else { break; }
            }
        }
        if(Ar.HasError()) { return Fail("is damaged"); }
        std::string Error;
        if(!Asset.Resolve(&Error)) { return Fail(Error); }
        OutAsset = std::move(Asset);
        return true;
    }

    bool ReadBehaviorTree(const std::vector<uint8_t>& InBytes, BehaviorTreeAsset& OutAsset, std::string* OutError)
    {
        switch(DetectBehaviorTreeFormat(InBytes))
        {
            case EBTFileFormat::Binary:
                return ReadBehaviorTreeBinary(InBytes, OutAsset, OutError);
            case EBTFileFormat::Text:
#if WITH_UNCOOKED
                return ParseBehaviorTreeXml(std::string(InBytes.begin(), InBytes.end()), OutAsset, OutError);
#else
                if(OutError) { *OutError = "is XML, which a Shipping build cannot read: cook it"; }
                return false;
#endif
            default:
                if(OutError) { *OutError = "is not a behavior tree"; }
                return false;
        }
    }

    bool LoadBehaviorTree(const std::string& InPath, BehaviorTreeAsset& OutAsset, std::string* OutError)
    {
        std::vector<uint8_t> Bytes;
        /* Through the file provider (paks, loose files), else straight off disk. */
        EngineCore::IFileProvider* Files = EngineCore::GetFileProvider();
        if(!(Files && Files->ReadAll(InPath, Bytes)) && !EngineCore::appFileReadAll(InPath, Bytes))
        {
            if(OutError) { *OutError = "cannot be read"; }
            return false;
        }
        return ReadBehaviorTree(Bytes, OutAsset, OutError);
    }

    // === The cache ============================================================

    namespace
    {
        struct Cache
        {
            std::mutex Lock;
            std::unordered_map<std::string, std::shared_ptr<const BehaviorTreeAsset>> Trees;
            std::unordered_set<std::string> Failed;
            uint64_t Generation = 1;
        };

        Cache& GetCache()
        {
            static Cache Instance;
            return Instance;
        }
    }

    std::shared_ptr<const BehaviorTreeAsset> BehaviorTreeAssetCache::Find(const std::string& InPath)
    {
        Cache& C = GetCache();
        {
            std::lock_guard<std::mutex> Lock(C.Lock);
            const auto Found = C.Trees.find(InPath);
            if(Found != C.Trees.end()) { return Found->second; }
            if(C.Failed.count(InPath)) { return nullptr; }
        }
        auto Asset = std::make_shared<BehaviorTreeAsset>();
        std::string Error;
        const bool bLoaded = LoadBehaviorTree(InPath, *Asset, &Error);
        std::lock_guard<std::mutex> Lock(C.Lock);
        if(!bLoaded)
        {
            CORE_LOG_WARN("[AI] the behavior tree " + InPath + " " + Error)
            C.Failed.insert(InPath);
            return nullptr;
        }
        C.Trees[InPath] = Asset;
        return Asset;
    }

    void BehaviorTreeAssetCache::Invalidate(const std::string& InPath)
    {
        Cache& C = GetCache();
        std::lock_guard<std::mutex> Lock(C.Lock);
        if(InPath.empty())
        {
            C.Trees.clear();
            C.Failed.clear();
        }
        else
        {
            C.Trees.erase(InPath);
            C.Failed.erase(InPath);
        }
        ++C.Generation;
    }

    uint64_t BehaviorTreeAssetCache::GetGeneration()
    {
        Cache& C = GetCache();
        std::lock_guard<std::mutex> Lock(C.Lock);
        return C.Generation;
    }
}
