#include "Public/GameCore/Delegate.h"

#include <vector>

#include "Public/Core/CoreLog.h"

/*
 * DelegateList's storage: a flat vector of slots, each carrying the serial
 * that makes a DelegateHandle safe to hold past the slot's reuse. See
 * DESIGN.md's "Delegate.h" section for the full broadcast protocol and the
 * re-entrancy guard's reasoning.
 */

namespace GameCore
{
    struct DelegateList::Impl
    {
        struct Slot
        {
            RawFn    Fn       = nullptr;
            void*    UserData = nullptr;
            uint32_t Serial   = 1;   // never 0, so a default DelegateHandle never validates
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
        ++S.Serial;   // bumped on reuse only -- see DESIGN.md

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
            /* Deferred: inert now, but not returned to the free list until
             * every broadcast walking the array has finished. */
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
            /* Explicit refusal, not merely a logged one -- see DESIGN.md for
             * why a bare CORE_CHECK previously let the recursion through. */
            CORE_CHECK(false,
                       "DelegateList::Broadcast called re-entrantly on the SAME delegate -- "
                       "a bound callback broadcast this exact delegate again.")
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
        /* bInUse deliberately not checked here -- see DESIGN.md. */
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
