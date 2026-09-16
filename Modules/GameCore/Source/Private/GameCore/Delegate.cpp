#include "Public/GameCore/Delegate.h"

#include <vector>

#include "Public/Core/CoreLog.h"

/*
 * DelegateList's storage: a flat vector of slots, each carrying the serial that
 * makes a DelegateHandle safe to hold past the slot's reuse.
 *
 * BROADCAST PROTOCOL, IN FULL
 *
 * BeginBroadcast() increments BroadcastDepth and records the CURRENT slot count
 * as BroadcastCount. GetEntry() only ever looks at indices below that count, so
 * an Add() during the walk (which appends past BroadcastCount) is invisible to
 * THIS broadcast and picked up by the next one -- exactly Broadcast()'s own
 * comment: "the addition is not called this broadcast". A Remove() during the
 * walk does not erase the slot (that would shift every later index mid-walk);
 * it clears the slot's Fn to null and queues the index in PendingRemoves.
 * GetEntry() returns true with Fn == nullptr for such a slot, which
 * Broadcast()'s `if(Fn)` skips. EndBroadcast() decrements BroadcastDepth and,
 * once it reaches zero, actually erases every queued slot -- reusing entries in
 * PendingRemoves also means it does not matter that erasing shifts every index
 * after it, because nothing is walking the array anymore.
 *
 * RE-ENTRANCY GUARD
 *
 * BroadcastDepth > 0 on entry to BeginBroadcast() means THIS SAME LIST is
 * already mid-walk somewhere up the call stack -- a callback bound here just
 * broadcast this exact delegate again. That is refused with a CORE_CHECK rather
 * than silently nesting, because nesting would mean GetEntry's "indices below
 * BroadcastCount" contract now has two different BroadcastCounts in flight
 * against one PendingRemoves queue, and the inner EndBroadcast erasing while the
 * outer walk is still reading indices is exactly the corruption the whole
 * design exists to avoid.
 */

namespace GameCore
{
    struct DelegateList::Impl
    {
        struct Slot
        {
            RawFn    Fn       = nullptr;
            void*    UserData = nullptr;
            uint32_t Serial   = 1;   // starts at 1 so a default-constructed DelegateHandle{0xFFFFFFFF, 0} never validates
            bool     bInUse   = false;
        };

        std::vector<Slot>        Slots;
        std::vector<uint32_t>    FreeIndices;
        std::vector<uint32_t>    PendingRemoves;   // slot indices to erase once BroadcastDepth reaches 0
        size_t                   BroadcastCount = 0;
        int                      BroadcastDepth = 0;

        size_t LiveCount() const
        {
            size_t Count = 0;
            for(const Slot& S : Slots)
            {
                if(S.bInUse) { ++Count; }
            }
            return Count;
        }

        void ApplyPendingRemoves()
        {
            /* Slot state was already cleared to "not in use" at Remove() time;
             * this only returns the index to the free list so Add() can reuse
             * it. Order does not matter -- FreeIndices is not a queue with a
             * meaning, just a pool. */
            for(uint32_t Index : PendingRemoves)
            {
                FreeIndices.push_back(Index);
            }
            PendingRemoves.clear();
        }
    };

    DelegateList::DelegateList() : P(new Impl()) {}

    DelegateList::~DelegateList()
    {
        CORE_CHECK(P->BroadcastDepth == 0, "DelegateList destroyed while a Broadcast was still on the stack")
        delete P;
    }

    DelegateHandle DelegateList::Add(const RawFn InFn, void* InUserData)
    {
        uint32_t Index;
        if(!P->FreeIndices.empty())
        {
            Index = P->FreeIndices.back();
            P->FreeIndices.pop_back();
        }
        else
        {
            Index = uint32_t(P->Slots.size());
            P->Slots.emplace_back();
        }

        Impl::Slot& S = P->Slots[Index];
        S.Fn       = InFn;
        S.UserData = InUserData;
        S.bInUse   = true;
        /* Bumped on every reuse of this slot, never on first use, so a handle
         * from the slot's FIRST occupant and a handle from its second both work
         * out to distinct Serials without special-casing "never used before". */
        ++S.Serial;

        DelegateHandle Handle;
        Handle.Index  = Index;
        Handle.Serial = S.Serial;
        return Handle;
    }

    bool DelegateList::Remove(const DelegateHandle InHandle)
    {
        if(!InHandle.IsValid() || InHandle.Index >= P->Slots.size())
        {
            return false;
        }

        Impl::Slot& S = P->Slots[InHandle.Index];
        if(!S.bInUse || S.Serial != InHandle.Serial)
        {
            return false;
        }

        S.Fn       = nullptr;
        S.UserData = nullptr;
        S.bInUse   = false;

        if(P->BroadcastDepth > 0)
        {
            /* Deferred: the slot is already inert (Fn is null, GetEntry will
             * report it as a tombstone), but it is not returned to the free list
             * until every broadcast walking the array has finished. */
            P->PendingRemoves.push_back(InHandle.Index);
        }
        else
        {
            P->FreeIndices.push_back(InHandle.Index);
        }
        return true;
    }

    size_t DelegateList::RemoveAll(void* InUserData)
    {
        size_t RemovedCount = 0;
        for(uint32_t Index = 0; Index < P->Slots.size(); ++Index)
        {
            Impl::Slot& S = P->Slots[Index];
            if(S.bInUse && S.UserData == InUserData)
            {
                DelegateHandle Handle;
                Handle.Index  = Index;
                Handle.Serial = S.Serial;
                if(Remove(Handle))
                {
                    ++RemovedCount;
                }
            }
        }
        return RemovedCount;
    }

    void DelegateList::Clear()
    {
        CORE_CHECK(P->BroadcastDepth == 0, "DelegateList::Clear() called while a Broadcast was on the stack")
        P->Slots.clear();
        P->FreeIndices.clear();
        P->PendingRemoves.clear();
    }

    size_t DelegateList::GetCount() const
    {
        return P->LiveCount();
    }

    bool DelegateList::IsBroadcasting() const
    {
        return P->BroadcastDepth > 0;
    }

    bool DelegateList::BeginBroadcast()
    {
        if(P->BroadcastDepth > 0)
        {
            /* An EXPLICIT refusal, not merely a logged one -- see the long
             * comment on this method's declaration in Delegate.h for why a
             * bare CORE_CHECK here previously let the recursion through: with
             * a handler installed, Fatal logs and returns, it does not stop
             * anything. CORE_CHECK below is still the right way to SURFACE
             * this to the log (matching how every other diagnostic in this
             * module is reported), it is just no longer the thing doing the
             * refusing. */
            CORE_CHECK(false,
                       "DelegateList::Broadcast called re-entrantly on the SAME delegate -- "
                       "a bound callback broadcast this exact delegate again. See the "
                       "BROADCAST RE-ENTRANCY note in Delegate.h.")
            return false;
        }

        P->BroadcastCount = P->Slots.size();
        ++P->BroadcastDepth;
        return true;
    }

    bool DelegateList::GetEntry(const size_t InIndex, RawFn& OutFn, void*& OutUserData) const
    {
        if(InIndex >= P->BroadcastCount)
        {
            return false;
        }
        const Impl::Slot& S = P->Slots[InIndex];
        /* bInUse is deliberately NOT checked here: a slot removed mid-broadcast
         * still returns true with Fn == nullptr (its natural state after
         * Remove()), which is what lets Broadcast()'s `if(Fn)` skip it without
         * GetEntry itself knowing about the caller's loop shape. A slot that was
         * never in use within [0, BroadcastCount) cannot occur -- indices below
         * BroadcastCount were all allocated before this broadcast began. */
        OutFn       = S.Fn;
        OutUserData = S.UserData;
        return true;
    }

    void DelegateList::EndBroadcast()
    {
        CORE_CHECK(P->BroadcastDepth > 0, "DelegateList::EndBroadcast without a matching BeginBroadcast")
        if(--P->BroadcastDepth == 0)
        {
            P->ApplyPendingRemoves();
        }
    }
}
