#include "Public/GameCore/World.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"

/*
 * Overlap detection for CollisionComponents (Components.h, DESIGN.md).
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

// === CollisionComponent ==========================================================

bool CollisionComponent::IsOverlapping(const EntityID InOther) const
{
    return Owner.OwningWorld && Owner.OwningWorld->AreOverlapping(Owner.Entity, InOther);
}

const char* CollisionComponent::GetSourceName() const
{
    return Source == CS_Mesh ? "Mesh" : "Bounds";
}

void CollisionComponent::SetSourceName(const char* InName)
{
    if(!InName) { return; }
    if(std::strcmp(InName, "Mesh") == 0)   { Source = CS_Mesh; }
    if(std::strcmp(InName, "Bounds") == 0) { Source = CS_Bounds; }
}

// === World =======================================================================

void World::UpdateOverlaps()
{
    std::vector<Pair> Now;

    ComponentPool* Pool = FindPool(CollisionComponent::StaticType().ID);
    const ComponentTypeID CollisionType = CollisionComponent::StaticType().ID;
    if(Pool)
    {
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
                if(ABox->bStatic && BBox->bStatic) { continue; }
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
