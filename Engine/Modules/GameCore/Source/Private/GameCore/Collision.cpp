#include "Public/GameCore/World.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/ScriptLibrary.h"

/*
 * Overlap detection for CollisionComponents (Components/CollisionComponent.h, DESIGN.md).
 *
 * BROAD PHASE is World's spatial index -- every collider is a BoundsComponent
 * box, refit at the end of TP_Physics. NARROW PHASE is the exact world box
 * test. PAIRS are packed entity ids, lower first, sorted; this pass's set is
 * diffed against the last pass's, and only the differences become events.
 * Pairs are stored before anything is dispatched, so a callback that asks
 * AreOverlapping sees this pass's answer, and a callback that destroys or
 * adds entities (deferred inside the tick) disturbs nothing in flight.
 */
namespace GameCore
{
namespace
{
    using Pair = std::pair<uint64_t, uint64_t>;

    CollisionMeshProvider GMeshProvider;
    bool                  bHasMeshProvider = false;
    bool                  bWarnedNoProvider = false;

    /* CS_Mesh colliders: (re)shape any whose mesh path changed since its
     * shape was read -- written into Bounds.Local, adding a Bounds if there
     * is none, and refit at once so this pass already uses it. */
    void ResolveMeshShapes(World& InWorld, ComponentPool& InPool)
    {
        const ComponentTypeID BoundsType = BoundsComponent::StaticType().ID;
        for(size_t Index = 0; Index < InPool.GetCount(); ++Index)
        {
            /* Re-read each time round: adding a Bounds may grow that pool,
             * never this one, but stay safe against a provider that adds. */
            CollisionComponent& C = static_cast<CollisionComponent*>(InPool.GetDenseData())[Index];
            if(!C.bEnabled || C.Source != CS_Mesh) { continue; }

            if(!bHasMeshProvider)
            {
                if(!bWarnedNoProvider)
                {
                    GAMECORE_LOG_WARN("a Collision component's Source is Mesh, but no mesh provider is installed; "
                                      "it uses its Bounds as authored")
                    bWarnedNoProvider = true;
                }
                return;
            }

            const EntityID Self = InPool.GetEntityAt(Index);
            const char* Path = GMeshProvider.GetMeshPath(InWorld, Self, GMeshProvider.UserData);
            if(!Path) { continue; }

            const bool bHasBounds = InWorld.GetEntity(Self).GetComponentByType(BoundsType) != nullptr;
            if(bHasBounds && std::strncmp(Path, C.ResolvedMesh, sizeof(C.ResolvedMesh)) == 0) { continue; }

            AABB Local;
            if(!GMeshProvider.GetMeshBounds(Path, Local, GMeshProvider.UserData)) { continue; }

            std::strncpy(C.ResolvedMesh, Path, sizeof(C.ResolvedMesh) - 1);
            C.ResolvedMesh[sizeof(C.ResolvedMesh) - 1] = '\0';

            if(!bHasBounds)
            {
                BoundsComponent Added;
                Added.Local = Local;
                InWorld.GetEntity(Self).AddComponent(Added);
            }
            else
            {
                static_cast<BoundsComponent*>(InWorld.GetEntity(Self).GetComponentByType(BoundsType))->Local = Local;
            }
            InWorld.RefreshEntityBounds(Self);
        }
    }

    Pair MakePair(const EntityID InA, const EntityID InB)
    {
        const uint64_t A = InA.Pack(), B = InB.Pack();
        return A < B ? Pair(A, B) : Pair(B, A);
    }

    bool LayersMeet(const CollisionComponent& InA, const CollisionComponent& InB)
    {
        return (InA.Layer & InB.Mask) != 0 && (InB.Layer & InA.Mask) != 0;
    }

    /* Each side, as Self: C++ first, then its script. A destroyed Self is
     * skipped -- its script and components are gone. */
    void Notify(World& InWorld, const Pair& InPair, const bool bInBegin)
    {
        const EntityID A = EntityID::Unpack(InPair.first);
        const EntityID B = EntityID::Unpack(InPair.second);
        const char* Method = bInBegin ? "OnOverlapBegin" : "OnOverlapEnd";
        TMulticastDelegate<EntityID, EntityID>& Delegate = bInBegin ? InWorld.OnOverlapBegin() : InWorld.OnOverlapEnd();

        for(int Side = 0; Side < 2; ++Side)
        {
            const EntityID Self  = Side == 0 ? A : B;
            const EntityID Other = Side == 0 ? B : A;
            if(!InWorld.IsAlive(Self)) { continue; }
            Delegate.Broadcast(Self, Other);
            InWorld.CallScriptMethod(Self, Method, Other);
        }
    }
}

void SetCollisionMeshProvider(const CollisionMeshProvider* InProvider)
{
    bHasMeshProvider = InProvider && InProvider->GetMeshPath && InProvider->GetMeshBounds;
    GMeshProvider    = bHasMeshProvider ? *InProvider : CollisionMeshProvider();
}

// === World =======================================================================

void World::UpdateOverlaps()
{
    std::vector<Pair> Now;

    ComponentPool* Pool = FindPool(CollisionComponent::StaticType().ID);
    const ComponentTypeID CollisionType = CollisionComponent::StaticType().ID;
    if(Pool)
    {
        ResolveMeshShapes(*this, *Pool);

        std::vector<EntityID> Candidates;
        CollisionComponent* Colliders = static_cast<CollisionComponent*>(Pool->GetDenseData());
        for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
        {
            const CollisionComponent& A = Colliders[Index];
            if(!A.bEnabled) { continue; }
            const EntityID AId = Pool->GetEntityAt(Index);
            const BoundsComponent* ABox = static_cast<const BoundsComponent*>(GetComponentRaw(AId, BoundsComponent::StaticType().ID));
            if(!ABox || ABox->ProxyID < 0) { continue; }   // not in the index until the next refit

            Candidates.clear();
            QueryBounds(ABox->World, [](const EntityID InHit, void* InOut) -> bool
            {
                static_cast<std::vector<EntityID>*>(InOut)->push_back(InHit);
                return true;
            }, &Candidates);

            for(const EntityID BId : Candidates)
            {
                if(!(AId.Pack() < BId.Pack())) { continue; }   // each pair once, from its lower side
                const auto* B = static_cast<const CollisionComponent*>(GetComponentRaw(BId, CollisionType));
                const auto* BBox = static_cast<const BoundsComponent*>(GetComponentRaw(BId, BoundsComponent::StaticType().ID));
                if(!B || !B->bEnabled || !BBox || !LayersMeet(A, *B)) { continue; }
                const bool bAStatic = ABox->bStatic || GetEntityMobility(AId) == EMobility::Static;
                const bool bBStatic = BBox->bStatic || GetEntityMobility(BId) == EMobility::Static;
                if(bAStatic && bBStatic) { continue; }
                if(ABox->World.Overlaps(BBox->World)) { Now.push_back(MakePair(AId, BId)); }
            }
        }

        std::sort(Now.begin(), Now.end());
        Now.erase(std::unique(Now.begin(), Now.end()), Now.end());

        for(size_t Index = 0; Index < Pool->GetCount(); ++Index) { Colliders[Index].OverlapCount = 0; }
        for(const Pair& Each : Now)
        {
            for(const uint64_t Packed : { Each.first, Each.second })
            {
                if(auto* C = static_cast<CollisionComponent*>(GetComponentRaw(EntityID::Unpack(Packed), CollisionType)))
                {
                    ++C->OverlapCount;
                }
            }
        }
    }

    /* What changed: ended pairs, then begun ones. Stored first (see above). */
    std::vector<Pair>& Previous = GetOverlapPairs();
    std::vector<Pair> Ended, Began;
    std::set_difference(Previous.begin(), Previous.end(), Now.begin(), Now.end(), std::back_inserter(Ended));
    std::set_difference(Now.begin(), Now.end(), Previous.begin(), Previous.end(), std::back_inserter(Began));
    Previous = std::move(Now);

    for(const Pair& Each : Ended) { Notify(*this, Each, false); }
    for(const Pair& Each : Began) { Notify(*this, Each, true); }
}

bool World::AreOverlapping(const EntityID InA, const EntityID InB) const
{
    const std::vector<Pair>& Pairs = GetOverlapPairs();
    return std::binary_search(Pairs.begin(), Pairs.end(), MakePair(InA, InB));
}

size_t World::GetOverlaps(const EntityID InEntity, EntityID* OutOthers, const size_t InMax) const
{
    const uint64_t Self = InEntity.Pack();
    size_t Count = 0;
    for(const Pair& Each : GetOverlapPairs())
    {
        if(Each.first != Self && Each.second != Self) { continue; }
        if(OutOthers && Count < InMax) { OutOthers[Count] = EntityID::Unpack(Each.first == Self ? Each.second : Each.first); }
        ++Count;
    }
    return Count;
}

size_t World::OverlapBox(const AABB& InBox, const int32_t InMask, EntityID* OutHits, const size_t InMax) const
{
    struct Query
    {
        const World* Self;
        AABB         Box;
        int32_t      Mask;
        EntityID*    Out;
        size_t       Max;
        size_t       Count;
    } Q{ this, InBox, InMask, OutHits, InMax, 0 };

    QueryBounds(InBox, [](const EntityID InHit, void* InUserData) -> bool
    {
        Query& Q = *static_cast<Query*>(InUserData);
        World& W = const_cast<World&>(*Q.Self);
        const auto* C = static_cast<const CollisionComponent*>(W.GetComponentRaw(InHit, CollisionComponent::StaticType().ID));
        const auto* B = static_cast<const BoundsComponent*>(W.GetComponentRaw(InHit, BoundsComponent::StaticType().ID));
        if(C && C->bEnabled && B && (C->Layer & Q.Mask) != 0 && B->World.Overlaps(Q.Box))
        {
            if(Q.Out && Q.Count < Q.Max) { Q.Out[Q.Count] = InHit; }
            ++Q.Count;
        }
        return true;
    }, &Q);
    return Q.Count;
}
}

// === The "Collision" script library ===============================================
// Intersection without events, for Lua and script graphs. Overlap events are
// the script's OnOverlapBegin / OnOverlapEnd methods, not functions here.

namespace
{
    bool ScriptAreOverlapping(GameCore::World& InWorld, const GameCore::EntityID InA, const GameCore::EntityID InB)
    {
        return InWorld.AreOverlapping(InA, InB);
    }

    int32_t ScriptCountInBox(GameCore::World& InWorld, const EngineCore::Vector3& InMin, const EngineCore::Vector3& InMax,
                             const int32_t InMask)
    {
        return int32_t(InWorld.OverlapBox(GameCore::AABB(InMin, InMax), InMask, nullptr, 0));
    }

    /* Collision.OverlapBox(minX, minY, minZ, maxX, maxY, maxZ[, mask]) -> every
     * entity hit (up to 64). A graph sees the first. */
    void ScriptOverlapBox(GameCore::ScriptCall& InCall)
    {
        const EngineCore::Vector3 Min = InCall.GetVector3(1);
        const EngineCore::Vector3 Max = InCall.GetVector3(4);
        const int32_t Mask = InCall.HasArg(7) ? InCall.GetInt(7) : -1;
        if(InCall.HasError()) { return; }

        GameCore::EntityID Hits[64];
        const size_t Count = InCall.GetWorld().OverlapBox(GameCore::AABB(Min, Max), Mask, Hits, 64);
        for(size_t Index = 0; Index < Count && Index < 64; ++Index) { InCall.PushEntity(Hits[Index]); }
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Collision)
{
    Lib.NativePure<&ScriptAreOverlapping>("AreOverlapping", "A, B -> Overlapping")
       .Tooltip("Whether two colliders overlapped at the last overlap pass");
    Lib.NativePure<&ScriptCountInBox>("CountInBox", "Min, Max, Mask -> Count")
       .Tooltip("Enabled colliders in a box now, whose Layer is in Mask (-1: every layer)");
    Lib.Pure("OverlapBox", &ScriptOverlapBox, "Min:Vector3, Max:Vector3, Mask:integer -> First:Entity")
       .Tooltip("Colliders in a box now; Lua gets every one, a graph the first");
}
