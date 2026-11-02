#ifndef RENDERCORE_INSTANCECLUSTERS_H
#define RENDERCORE_INSTANCECLUSTERS_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RenderCore
{
    class RenderContext;
    class RenderProxy;
    class RenderScene;

    /*
     * AUTO INSTANCING (RENDERPIPELINE.md, "Auto instancing"). Static proxies
     * that draw the same mesh with the same materials and nothing of their
     * own (RenderProxy::GetInstanceKey), and stand in the same cell of a
     * [Rendering] AutoInstancingCell grid, are drawn by one cluster proxy
     * (RenderProxy::CreateInstanced): one instanced draw a stage instead of
     * one each. The cluster is an ordinary static proxy -- culled, occlusion
     * queried, shadow cached and captured like any -- and its members, while
     * it stands, are not renderable (RenderProxy::IsRenderable).
     *
     * Rebuilt cell by cell: a cell's members and their change serials make a
     * fingerprint each frame, and a cluster is made again only when it moves.
     */
    class InstanceClusters
    {
    public:
        /* RENDER THREAD, once a frame before any view: InProxies, the scene's,
         * grouped; clusters added and removed through InScene. */
        void Update(RenderScene& InScene, const std::vector<RenderProxy*>& InProxies, RenderContext& InContext);

        /* RenderScene::RemoveProxy: InProxy leaves (its cell is made again
         * next Update), or, a cluster, its cell forgets it. */
        void Forget(const RenderProxy* InProxy);

        uint32_t GetClusterCount() const { return uint32_t(Clusters.size()); }
        uint32_t GetMemberCount()  const { return Members; }

    private:
        struct Cached
        {
            uint64_t    Serial    = ~uint64_t(0);
            bool        bEligible = false;
            std::string Group;   // the key and the cell
        };
        struct Group
        {
            RenderProxy* Cluster     = nullptr;
            uint64_t     Fingerprint = 0;
            uint32_t     Count       = 0;
            uint64_t     NewPrint    = 0;   // this frame's
            uint32_t     NewCount    = 0;
        };

        void Remove(RenderScene& InScene, Group& InGroup, RenderContext& InContext);

        std::unordered_map<const RenderProxy*, Cached> Keys;
        std::unordered_map<std::string, Group>         Groups;
        std::unordered_set<const RenderProxy*>         Clusters;
        uint32_t                                        Members = 0;
    };
}

#endif //RENDERCORE_INSTANCECLUSTERS_H
