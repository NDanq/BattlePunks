#include "Private/RenderCore/InstanceClusters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Public/Core/CoreConfig.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"

/* AUTO INSTANCING (RENDERPIPELINE.md, "Auto instancing"): static entities sharing a mesh and its
 * materials drawn as one instanced draw per AutoInstancingCell-sized cell (world units), once at
 * least AutoInstancingMin of them stand there. Off by default until a machine hang seen during a
 * DemoLab L2 run with it on (2026-11-01) is ruled out. */
NIMBLE_CONFIG_BOOL(GRenderAutoInstancing, "Rendering", "AutoInstancing", false, nullptr);
NIMBLE_CONFIG_FLOAT(GRenderAutoInstancingCell, "Rendering", "AutoInstancingCell", 2000.0f, nullptr);
NIMBLE_CONFIG_INT(GRenderAutoInstancingMin, "Rendering", "AutoInstancingMin", 2, nullptr);

namespace RenderCore
{
    namespace
    {
        /* SplitMix64's finaliser: a proxy and its serial, mixed so the sum over a cell changes
         * with any member joining, leaving or changing. */
        uint64_t Mix(uint64_t InValue)
        {
            InValue += 0x9E3779B97F4A7C15ull;
            InValue = (InValue ^ (InValue >> 30)) * 0xBF58476D1CE4E5B9ull;
            InValue = (InValue ^ (InValue >> 27)) * 0x94D049BB133111EBull;
            return InValue ^ (InValue >> 31);
        }
    }

    void InstanceClusters::Remove(RenderScene& InScene, Group& InGroup, RenderContext& InContext)
    {
        if(!InGroup.Cluster) { return; }
        RenderProxy* Cluster = InGroup.Cluster;
        InGroup.Cluster = nullptr;
        Clusters.erase(Cluster);
        InScene.RemoveProxy(Cluster, InContext);
    }

    void InstanceClusters::Forget(const RenderProxy* InProxy)
    {
        Keys.erase(InProxy);
        if(Clusters.erase(InProxy) == 0) { return; }
        for(auto& Each : Groups)
        {
            if(Each.second.Cluster == InProxy) { Each.second.Cluster = nullptr; Each.second.Count = 0; }
        }
    }

    void InstanceClusters::Update(RenderScene& InScene, const std::vector<RenderProxy*>& InProxies, RenderContext& InContext)
    {
        const bool bOn = GRenderAutoInstancing.GetBool() && InContext.SupportsInstancing();
        if(!bOn && Groups.empty()) { return; }

        const double   Cell     = std::max(double(GRenderAutoInstancingCell.GetFloat()), 1.0);
        const uint32_t MinCount = uint32_t(std::max(GRenderAutoInstancingMin.GetInt(), 2));

        /* Who wants which cell this frame. */
        for(auto& Each : Groups) { Each.second.NewPrint = 0; Each.second.NewCount = 0; }
        std::vector<std::pair<Group*, RenderProxy*>> Wanted;
        Wanted.reserve(InProxies.size());
        for(RenderProxy* Proxy : InProxies)
        {
            if(Clusters.count(Proxy)) { continue; }
            Proxy->bInCluster = false;
            if(!bOn || !Proxy->IsStatic() || !Proxy->bVisible || Proxy->bDetailHidden || Proxy->GetWorldBounds().IsEmpty()) { continue; }
            Cached& Key = Keys[Proxy];
            if(Key.Serial != Proxy->GetChangeSerial())
            {
                Key.Serial    = Proxy->GetChangeSerial();
                Key.bEligible = Proxy->GetInstanceKey(Key.Group);
                if(Key.bEligible)
                {
                    const GameCore::AABB& Box = Proxy->GetWorldBounds();
                    const EngineCore::Vector3 Centre = (Box.Min + Box.Max) * 0.5;
                    char Where[64];
                    std::snprintf(Where, sizeof(Where), "#%lld,%lld,%lld", (long long)std::floor(double(Centre.GetX()) / Cell),
                                  (long long)std::floor(double(Centre.GetY()) / Cell), (long long)std::floor(double(Centre.GetZ()) / Cell));
                    Key.Group += Where;
                }
            }
            if(!Key.bEligible) { continue; }
            Group& Into = Groups[Key.Group];
            Into.NewPrint += Mix(uint64_t(reinterpret_cast<uintptr_t>(Proxy)) ^ (Key.Serial * 0x2545F4914F6CDD1Dull));
            ++Into.NewCount;
            Wanted.emplace_back(&Into, Proxy);
        }

        /* Cells whose members changed are made again; the rest keep their cluster. */
        std::unordered_map<Group*, std::vector<RenderProxy*>> Rebuild;
        for(auto& Each : Groups)
        {
            Group& G = Each.second;
            const bool bWant    = G.NewCount >= MinCount;
            const bool bChanged = G.NewPrint != G.Fingerprint || G.NewCount != G.Count;
            if(bChanged || (bWant && !G.Cluster))
            {
                Remove(InScene, G, InContext);
                G.Fingerprint = G.NewPrint;
                G.Count       = G.NewCount;
                if(bWant) { Rebuild[&G]; }
            }
        }
        for(const auto& Each : Wanted)
        {
            const auto Found = Rebuild.find(Each.first);
            if(Found != Rebuild.end()) { Found->second.push_back(Each.second); }
        }
        bool bChangedAny = false;
        for(auto& Each : Rebuild)
        {
            std::vector<RenderProxy*>& Members = Each.second;
            std::vector<RenderInstance> Instances(Members.size());
            GameCore::AABB Bounds = Members.front()->GetWorldBounds();
            for(size_t Index = 0; Index < Members.size(); ++Index)
            {
                const EngineCore::Matrix4x4& World = Members[Index]->GetLocalToWorld();
                for(int Col = 0; Col < 4; ++Col)
                {
                    for(int Row = 0; Row < 4; ++Row) { Instances[Index].Transform[Col * 4 + Row] = float(World.Data[Row][Col]); }
                }
                Bounds = GameCore::AABB::Union(Bounds, Members[Index]->GetWorldBounds());
            }
            RenderProxy* Cluster = Members.front()->CreateInstanced(InContext, Instances.data(), uint32_t(Instances.size()));
            if(!Cluster) { continue; }
            InScene.AddProxy(Cluster, InContext);
            Cluster->SetStatic(true);
            Cluster->SetTransform(RenderTransform());
            Cluster->SetLocalBounds(Bounds);
            Each.first->Cluster = Cluster;
            Clusters.insert(Cluster);
            bChangedAny = true;
        }

        /* Members of a standing cluster are drawn by it alone. */
        uint32_t Count = 0;
        for(const auto& Each : Wanted)
        {
            if(Each.first->Cluster) { Each.second->bInCluster = true; ++Count; }
        }
        for(auto It = Groups.begin(); It != Groups.end();)
        {
            if(It->second.NewCount == 0 && !It->second.Cluster) { It = Groups.erase(It); }
            else { ++It; }
        }
        if(bChangedAny || Count != Members)
        {
            char Line[160];
            std::snprintf(Line, sizeof(Line), "auto instancing: %u clusters draw %u static entities",
                          unsigned(Clusters.size()), unsigned(Count));
            RENDERCORE_LOG(Line);
        }
        Members = Count;
    }
}
