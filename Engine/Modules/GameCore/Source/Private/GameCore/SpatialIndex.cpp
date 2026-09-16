#include "Public/GameCore/SpatialIndex.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * The dynamic AABB tree. This is Box2D's b2DynamicTree in structure -- a node
 * array with an intrusive free list, insertion by the surface-area heuristic,
 * rebalancing by the same two-rotation scheme -- adapted to hold an EntityID
 * per leaf instead of a fixture pointer, and grown through
 * EngineCore::SystemAlignedAlloc instead of a bespoke allocator, so
 * that it participates in the same allocation accounting the rest of GameCore
 * does.
 *
 * NODE LAYOUT. One array, referenced by int32_t index -- never a pointer --
 * with NullNode (-1) meaning "no node". A proxy id IS a node index; there is no
 * separate handle table, unlike DelegateHandle or ComponentPool's sparse array,
 * because a tree node's identity does not need to survive being "moved" the way
 * a delegate slot or a dense component slot does: Move() mutates a leaf node
 * in place when it fits its fat box, and only reinserts (which frees the old
 * node and allocates a new one, same index-stability contract as any other
 * node) when it escapes. The proxy id a caller holds is therefore stable across
 * every call that does not explicitly document otherwise -- and none do; even a
 * reinsertion inside Move() keeps the SAME node index, because Move() removes
 * and reinserts the leaf into the SAME slot rather than freeing it (see
 * Move()'s implementation).
 *
 * WHY POINTER-FREE. The array can grow by reallocation (EnsureCapacity below)
 * without invalidating anything a caller holds, because callers hold int32_t
 * indices, never Node*. This is the same reason ComponentPool's sparse array is
 * indices rather than pointers into Dense.
 */

namespace GameCore
{
    namespace
    {
        constexpr int32_t NULL_NODE = -1;

        struct Node
        {
            AABB Box;   // FAT for a leaf, the union of children for an internal node

            int32_t Parent = NULL_NODE;
            int32_t Left   = NULL_NODE;
            int32_t Right  = NULL_NODE;
            int32_t Next   = NULL_NODE;   // free-list link when this node is not in use

            int32_t Height = -1;   // -1 marks a free node; 0 for a leaf; max(child heights)+1 otherwise

            EntityID Entity;        // meaningful only when IsLeaf()

            bool IsLeaf() const { return Left == NULL_NODE; }
        };
    }

    struct SpatialIndex::Impl
    {
        std::vector<Node> Nodes;
        int32_t            Root         = NULL_NODE;
        int32_t            FreeList     = NULL_NODE;
        size_t             NodeCount    = 0;   // nodes in use (leaves + internal), not Nodes.size()
        size_t             LeafCount    = 0;
        float              FatMargin    = 0.5f;

        // --- node pool -------------------------------------------------------

        int32_t AllocateNode()
        {
            if(FreeList == NULL_NODE)
            {
                const size_t OldCapacity = Nodes.size();
                const size_t NewCapacity = OldCapacity == 0 ? 32 : OldCapacity * 2;
                Nodes.resize(NewCapacity);
                for(size_t Index = OldCapacity; Index < NewCapacity; ++Index)
                {
                    Nodes[Index].Next   = int32_t(Index + 1 == NewCapacity ? NULL_NODE : Index + 1);
                    Nodes[Index].Height = -1;
                }
                FreeList = int32_t(OldCapacity);
            }

            const int32_t NodeIndex = FreeList;
            FreeList = Nodes[NodeIndex].Next;

            Node& N = Nodes[NodeIndex];
            N.Parent = NULL_NODE;
            N.Left   = NULL_NODE;
            N.Right  = NULL_NODE;
            N.Height = 0;
            N.Box    = AABB();

            ++NodeCount;
            return NodeIndex;
        }

        void FreeNode(const int32_t NodeIndex)
        {
            Node& N = Nodes[NodeIndex];
            N.Height = -1;
            N.Next   = FreeList;
            FreeList = NodeIndex;
            --NodeCount;
        }

        // --- balancing ---------------------------------------------------------

        int32_t Height(const int32_t NodeIndex) const
        {
            return NodeIndex == NULL_NODE ? -1 : Nodes[NodeIndex].Height;
        }

        /*
         * The single-rotation rebalance Box2D's b2DynamicTree::Balance runs
         * bottom-up from every leaf's parent after an insert or remove. A is the
         * node whose two children's heights differ by more than one; this picks
         * the heavier child, promotes it, and re-parents the light grandchild
         * onto the OTHER child -- whichever of that child's two children keeps
         * the tree's total surface area (via each side's own height, as a
         * proxy) smaller. Returns the new subtree root (never A when a rotation
         * happened).
         */
        int32_t Balance(const int32_t IndexA)
        {
            Node& A = Nodes[IndexA];
            if(A.IsLeaf() || A.Height < 2)
            {
                return IndexA;
            }

            const int32_t IndexB = A.Left;
            const int32_t IndexC = A.Right;
            const int32_t BalanceFactor = Height(IndexC) - Height(IndexB);

            if(std::abs(BalanceFactor) <= 1)
            {
                return IndexA;
            }

            /* Heavier side becomes the new subtree root; A is demoted to be its
             * sibling. F/G naming matches b2DynamicTree for anyone cross-
             * referencing the reference implementation. */
            const int32_t IndexHeavy = BalanceFactor > 0 ? IndexC : IndexB;
            const int32_t IndexLight = BalanceFactor > 0 ? IndexB : IndexC;
            Node&         Heavy      = Nodes[IndexHeavy];

            const int32_t IndexF = Heavy.Left;
            const int32_t IndexG = Heavy.Right;

            Heavy.Left  = IndexA;
            Heavy.Parent = A.Parent;
            A.Parent     = IndexHeavy;

            if(Heavy.Parent != NULL_NODE)
            {
                Node& GrandParent = Nodes[Heavy.Parent];
                if(GrandParent.Left == IndexA) { GrandParent.Left = IndexHeavy; }
                else                            { GrandParent.Right = IndexHeavy; }
            }
            else
            {
                Root = IndexHeavy;
            }

            /* Re-parent whichever of Heavy's old children (F, G) keeps F on the
             * side with the smaller height, minimising the resulting SurfaceArea
             * the same way the sibling-selection heuristic in Insert() does. */
            if(Height(IndexF) > Height(IndexG))
            {
                Heavy.Right = IndexF;
                A.Right      = IndexG;
                if(IndexG != NULL_NODE) { Nodes[IndexG].Parent = IndexA; }
                A.Box    = AABB::Union(Nodes[IndexLight].Box, Nodes[IndexG].Box);
                Heavy.Box = AABB::Union(A.Box, Nodes[IndexF].Box);
                A.Height    = std::max(Height(IndexLight), Height(IndexG)) + 1;
                Heavy.Height = std::max(Height(IndexA), Height(IndexF)) + 1;
            }
            else
            {
                Heavy.Right = IndexG;
                A.Right      = IndexF;
                if(IndexF != NULL_NODE) { Nodes[IndexF].Parent = IndexA; }
                A.Box    = AABB::Union(Nodes[IndexLight].Box, Nodes[IndexF].Box);
                Heavy.Box = AABB::Union(A.Box, Nodes[IndexG].Box);
                A.Height    = std::max(Height(IndexLight), Height(IndexF)) + 1;
                Heavy.Height = std::max(Height(IndexA), Height(IndexG)) + 1;
            }

            /* A's OTHER child (the one that was not chosen as heavy/light) keeps
             * its slot: A.Left is whichever of B/C was not promoted. */
            A.Left = IndexLight;

            return IndexHeavy;
        }

        void RefitAndBalanceUpward(int32_t NodeIndex)
        {
            while(NodeIndex != NULL_NODE)
            {
                NodeIndex = Balance(NodeIndex);

                Node& N = Nodes[NodeIndex];
                N.Height = std::max(Height(N.Left), Height(N.Right)) + 1;
                N.Box    = AABB::Union(Nodes[N.Left].Box, Nodes[N.Right].Box);

                NodeIndex = N.Parent;
            }
        }

        // --- insert / remove ---------------------------------------------------

        /*
         * Sibling selection: the standard "minimise the SurfaceArea increase"
         * descent (Catto, "Dynamic Bounding Volume Hierarchies") -- at each
         * internal node, compare the cost of making the new leaf a sibling of
         * THIS node against descending into whichever child costs less once its
         * own enlargement is accounted for. AABB::SurfaceArea() is exactly the
         * cost function this needs it for.
         */
        int32_t PickSibling(const AABB& InLeafBox) const
        {
            int32_t Current = Root;
            while(!Nodes[Current].IsLeaf())
            {
                const Node& N = Nodes[Current];
                const float  CombinedArea = AABB::Union(N.Box, InLeafBox).SurfaceArea();
                const float  DirectCost   = 2.0f * CombinedArea;
                const float  InheritCost  = 2.0f * (CombinedArea - N.Box.SurfaceArea());

                const auto ChildCost = [&](const int32_t ChildIndex) -> float
                {
                    const Node& Child = Nodes[ChildIndex];
                    const float EnlargedArea = AABB::Union(Child.Box, InLeafBox).SurfaceArea();
                    return Child.IsLeaf()
                         ? EnlargedArea + InheritCost
                         : EnlargedArea - Child.Box.SurfaceArea() + InheritCost;
                };

                const float LeftCost  = ChildCost(N.Left);
                const float RightCost = ChildCost(N.Right);

                if(DirectCost < LeftCost && DirectCost < RightCost)
                {
                    break;
                }
                Current = (LeftCost < RightCost) ? N.Left : N.Right;
            }
            return Current;
        }

        int32_t InsertLeaf(const int32_t LeafIndex)
        {
            if(Root == NULL_NODE)
            {
                Root = LeafIndex;
                Nodes[LeafIndex].Parent = NULL_NODE;
                return LeafIndex;
            }

            const int32_t Sibling = PickSibling(Nodes[LeafIndex].Box);
            const int32_t OldParent = Nodes[Sibling].Parent;

            const int32_t NewParent = AllocateNode();
            Node& NP = Nodes[NewParent];
            NP.Parent = OldParent;
            NP.Box    = AABB::Union(Nodes[LeafIndex].Box, Nodes[Sibling].Box);
            NP.Height = Height(Sibling) + 1;

            if(OldParent != NULL_NODE)
            {
                Node& OP = Nodes[OldParent];
                if(OP.Left == Sibling) { OP.Left = NewParent; }
                else                    { OP.Right = NewParent; }
            }
            else
            {
                Root = NewParent;
            }

            NP.Left  = Sibling;
            NP.Right = LeafIndex;
            Nodes[Sibling].Parent   = NewParent;
            Nodes[LeafIndex].Parent = NewParent;

            RefitAndBalanceUpward(Nodes[LeafIndex].Parent);
            return LeafIndex;
        }

        void RemoveLeaf(const int32_t LeafIndex)
        {
            if(LeafIndex == Root)
            {
                Root = NULL_NODE;
                return;
            }

            const int32_t ParentIndex      = Nodes[LeafIndex].Parent;
            const int32_t GrandParentIndex = Nodes[ParentIndex].Parent;
            const int32_t SiblingIndex     = (Nodes[ParentIndex].Left == LeafIndex)
                                            ? Nodes[ParentIndex].Right : Nodes[ParentIndex].Left;

            if(GrandParentIndex != NULL_NODE)
            {
                Node& GP = Nodes[GrandParentIndex];
                if(GP.Left == ParentIndex) { GP.Left = SiblingIndex; }
                else                        { GP.Right = SiblingIndex; }
                Nodes[SiblingIndex].Parent = GrandParentIndex;
                FreeNode(ParentIndex);
                RefitAndBalanceUpward(GrandParentIndex);
            }
            else
            {
                Root = SiblingIndex;
                Nodes[SiblingIndex].Parent = NULL_NODE;
                FreeNode(ParentIndex);
            }
        }

        // --- queries -------------------------------------------------------

        template<typename TVisitLeaf>
        void WalkOverlapping(const AABB& InBox, TVisitLeaf&& InVisit) const
        {
            if(Root == NULL_NODE) { return; }

            /* Explicit stack, no recursion, no allocation once Stack has grown
             * to the tree's depth once -- SpatialIndex.h promises exactly this
             * in its class comment. */
            std::vector<int32_t> Stack;
            Stack.push_back(Root);

            while(!Stack.empty())
            {
                const int32_t NodeIndex = Stack.back();
                Stack.pop_back();

                const Node& N = Nodes[NodeIndex];
                if(!N.Box.Overlaps(InBox)) { continue; }

                if(N.IsLeaf())
                {
                    if(!InVisit(N.Entity)) { return; }
                }
                else
                {
                    Stack.push_back(N.Left);
                    Stack.push_back(N.Right);
                }
            }
        }

        /* Slab method (Kay-Kajiya / "An Efficient and Robust Ray-Box
         * Intersection Algorithm"): intersect the ray against each axis' pair
         * of planes, track the running [TMin, TMax] intersection of all three
         * intervals. A zero ray component divides by zero, which produces
         * +-inf and is handled correctly by IEEE 754 comparisons without a
         * special case, exactly as that paper depends on. */
        static bool RayIntersectsBox(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InInvDir,
                                      const AABB& InBox, const float InMaxT, float& OutT)
        {
            float TMin = 0.0f;
            float TMax = InMaxT;

            const double OriginAxis[3]  = { InOrigin.GetX(), InOrigin.GetY(), InOrigin.GetZ() };
            const double InvDirAxis[3]  = { InInvDir.GetX(), InInvDir.GetY(), InInvDir.GetZ() };
            const double MinAxis[3]     = { InBox.Min.GetX(), InBox.Min.GetY(), InBox.Min.GetZ() };
            const double MaxAxis[3]     = { InBox.Max.GetX(), InBox.Max.GetY(), InBox.Max.GetZ() };

            for(int Axis = 0; Axis < 3; ++Axis)
            {
                float T0 = float((MinAxis[Axis] - OriginAxis[Axis]) * InvDirAxis[Axis]);
                float T1 = float((MaxAxis[Axis] - OriginAxis[Axis]) * InvDirAxis[Axis]);
                if(T0 > T1) { std::swap(T0, T1); }

                TMin = std::max(TMin, T0);
                TMax = std::min(TMax, T1);
                if(TMin > TMax) { return false; }
            }

            OutT = TMin;
            return true;
        }

        // --- rebuild ---------------------------------------------------------

        /* Top-down median split on the longest axis -- the standard bottom-up-
         * quality-for-top-down-cost tree build used when insertion order is not
         * trusted (a fresh level's leaves, inserted in file order, produce a
         * badly unbalanced incremental tree; this fixes it in one pass). Returns
         * the node index of the subtree root for InLeaves[InBegin, InEnd). */
        int32_t BuildRecursive(std::vector<int32_t>& InLeaves, const size_t InBegin, const size_t InEnd)
        {
            const size_t Count = InEnd - InBegin;
            if(Count == 1)
            {
                Nodes[InLeaves[InBegin]].Parent = NULL_NODE;
                return InLeaves[InBegin];
            }

            AABB Bounds;
            for(size_t Index = InBegin; Index < InEnd; ++Index)
            {
                Bounds = AABB::Union(Bounds, Nodes[InLeaves[Index]].Box);
            }
            const EngineCore::Vector3 Extent = Bounds.Extent();
            const int SplitAxis = (Extent.GetX() >= Extent.GetY() && Extent.GetX() >= Extent.GetZ()) ? 0
                                 : (Extent.GetY() >= Extent.GetZ() ? 1 : 2);

            const auto AxisValue = [SplitAxis](const AABB& InBox) -> double
            {
                const EngineCore::Vector3 C = InBox.Center();
                return SplitAxis == 0 ? C.GetX() : (SplitAxis == 1 ? C.GetY() : C.GetZ());
            };

            const size_t Mid = InBegin + Count / 2;
            std::nth_element(InLeaves.begin() + long(InBegin), InLeaves.begin() + long(Mid), InLeaves.begin() + long(InEnd),
                              [&](const int32_t A, const int32_t B) { return AxisValue(Nodes[A].Box) < AxisValue(Nodes[B].Box); });

            const int32_t LeftRoot  = BuildRecursive(InLeaves, InBegin, Mid);
            const int32_t RightRoot = BuildRecursive(InLeaves, Mid, InEnd);

            const int32_t ParentIndex = AllocateNode();
            Node& P = Nodes[ParentIndex];
            P.Left   = LeftRoot;
            P.Right  = RightRoot;
            P.Box    = AABB::Union(Nodes[LeftRoot].Box, Nodes[RightRoot].Box);
            P.Height = std::max(Height(LeftRoot), Height(RightRoot)) + 1;
            P.Parent = NULL_NODE;
            Nodes[LeftRoot].Parent  = ParentIndex;
            Nodes[RightRoot].Parent = ParentIndex;
            return ParentIndex;
        }

        // --- validation ------------------------------------------------------

        bool ValidateRecursive(const int32_t NodeIndex, size_t& OutLeavesSeen) const
        {
            if(NodeIndex == NULL_NODE) { return true; }
            const Node& N = Nodes[NodeIndex];

            if(N.IsLeaf())
            {
                ++OutLeavesSeen;
                return N.Height == 0;
            }

            if(N.Left == NULL_NODE || N.Right == NULL_NODE) { return false; }
            if(Nodes[N.Left].Parent != NodeIndex || Nodes[N.Right].Parent != NodeIndex) { return false; }
            if(N.Height != std::max(Height(N.Left), Height(N.Right)) + 1) { return false; }
            if(!N.Box.Contains(Nodes[N.Left].Box) || !N.Box.Contains(Nodes[N.Right].Box)) { return false; }

            return ValidateRecursive(N.Left, OutLeavesSeen) && ValidateRecursive(N.Right, OutLeavesSeen);
        }
    };

    SpatialIndex::SpatialIndex() : P(new Impl()) {}
    SpatialIndex::~SpatialIndex() { delete P; }

    int32_t SpatialIndex::Insert(const EntityID InEntity, const AABB& InWorldBox)
    {
        const int32_t LeafIndex = P->AllocateNode();
        Node& N   = P->Nodes[LeafIndex];
        N.Box     = InWorldBox.Fattened(P->FatMargin);
        N.Entity  = InEntity;
        N.Height  = 0;

        P->InsertLeaf(LeafIndex);
        ++P->LeafCount;
        return LeafIndex;
    }

    void SpatialIndex::Remove(const int32_t InProxy)
    {
        if(InProxy == NULL_NODE || size_t(InProxy) >= P->Nodes.size() || P->Nodes[InProxy].Height == -1)
        {
            GAMECORE_LOG_WARN("SpatialIndex::Remove given an invalid or already-freed proxy id; ignoring")
            return;
        }
        P->RemoveLeaf(InProxy);
        P->FreeNode(InProxy);
        --P->LeafCount;
    }

    bool SpatialIndex::Move(const int32_t InProxy, const AABB& InWorldBox)
    {
        Node& N = P->Nodes[InProxy];
        if(N.Box.Contains(InWorldBox))
        {
            /* The common case: still inside the fat box, nothing to do. */
            return false;
        }

        /* Escaped -- remove from the tree (freeing nothing; the node stays
         * allocated at the SAME index) and reinsert with a freshly fattened
         * box. The proxy id the caller holds is unchanged. */
        P->RemoveLeaf(InProxy);
        N.Box = InWorldBox.Fattened(P->FatMargin);
        P->InsertLeaf(InProxy);
        return true;
    }

    void SpatialIndex::QueryBounds(const AABB& InBox, const SpatialQueryFn InFn, void* InUserData) const
    {
        if(!InFn) { return; }
        P->WalkOverlapping(InBox, [&](const EntityID InEntity) { return InFn(InEntity, InUserData); });
    }

    bool SpatialIndex::Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir,
                                const float InMaxT, RaycastHit& OutHit) const
    {
        if(P->Root == NULL_NODE) { return false; }

        const EngineCore::Vector3 InvDir(
            InDir.GetX() != 0.0 ? 1.0 / InDir.GetX() : DBL_MAX,
            InDir.GetY() != 0.0 ? 1.0 / InDir.GetY() : DBL_MAX,
            InDir.GetZ() != 0.0 ? 1.0 / InDir.GetZ() : DBL_MAX);

        bool  bFoundAny = false;
        float BestT     = InMaxT;
        EntityID BestEntity;

        std::vector<int32_t> Stack;
        Stack.push_back(P->Root);

        while(!Stack.empty())
        {
            const int32_t NodeIndex = Stack.back();
            Stack.pop_back();

            const Node& N = P->Nodes[NodeIndex];
            float HitT;
            if(!Impl::RayIntersectsBox(InOrigin, InvDir, N.Box, BestT, HitT)) { continue; }

            if(N.IsLeaf())
            {
                if(!bFoundAny || HitT < BestT)
                {
                    bFoundAny  = true;
                    BestT      = HitT;
                    BestEntity = N.Entity;
                }
            }
            else
            {
                Stack.push_back(N.Left);
                Stack.push_back(N.Right);
            }
        }

        if(bFoundAny)
        {
            OutHit.Entity = BestEntity;
            OutHit.T      = BestT;
        }
        return bFoundAny;
    }

    void SpatialIndex::Rebuild()
    {
        if(P->LeafCount == 0) { return; }

        std::vector<int32_t> Leaves;
        Leaves.reserve(P->LeafCount);
        for(size_t Index = 0; Index < P->Nodes.size(); ++Index)
        {
            if(P->Nodes[Index].Height == 0)   // a leaf: height 0, still allocated
            {
                Leaves.push_back(int32_t(Index));
            }
        }

        /* Free every INTERNAL node (leaves are reused in place, exactly as
         * Move()'s reinsertion does, so every proxy id a caller holds stays
         * valid). Internal nodes are identified by Height > 0 among in-use
         * nodes -- Height == -1 is already free and untouched. */
        for(size_t Index = 0; Index < P->Nodes.size(); ++Index)
        {
            if(P->Nodes[Index].Height > 0)
            {
                P->FreeNode(int32_t(Index));
            }
        }

        P->Root = P->BuildRecursive(Leaves, 0, Leaves.size());
    }

    void SpatialIndex::SetFatMargin(const float InMargin) { P->FatMargin = InMargin; }
    float SpatialIndex::GetFatMargin() const { return P->FatMargin; }

    size_t SpatialIndex::GetProxyCount() const { return P->LeafCount; }
    int32_t SpatialIndex::GetHeight() const { return P->Root == NULL_NODE ? 0 : P->Nodes[P->Root].Height; }

    bool SpatialIndex::Validate() const
    {
        if(P->Root == NULL_NODE) { return P->LeafCount == 0; }
        size_t LeavesSeen = 0;
        const bool bOK = P->ValidateRecursive(P->Root, LeavesSeen);
        return bOK && LeavesSeen == P->LeafCount;
    }

    const AABB& SpatialIndex::GetFatBox(const int32_t InProxy) const { return P->Nodes[InProxy].Box; }
    EntityID    SpatialIndex::GetEntity(const int32_t InProxy) const { return P->Nodes[InProxy].Entity; }
}
