#ifndef ENGINECORE_COREPOINTER_H
#define ENGINECORE_COREPOINTER_H

#include <cstddef>
#include <type_traits>
#include <utility>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreGC.h"
#include "Public/Core/CoreMemory.h"

/*
 * TUnique, TRef and TWeakRef -- the owning handles over CoreMemory.h.
 *
 *      TUnique<Foo> A = MakeUnique<Foo>(1, 2);        // sole owner, no GC
 *      TRef<Foo>    B = MakeRef<Foo>(1, 2);           // shared, GC-tracked
 *      TRef<Foo>    C = MakeRefIn<Foo>(MyPool, 1, 2); // ... from a chosen pool
 *      TWeakRef<Foo> W(B);                            // observes, does not own
 *
 * ===========================
 * WHICH ONE TO REACH FOR
 * ===========================
 *
 * TUnique unless you have a reason. Most engine objects have exactly one
 * owner, and a TUnique costs nothing at all beyond the allocation -- no
 * refcount, no list, no collector participation. It is a move-only pointer and
 * a destructor.
 *
 * TRef when ownership is genuinely shared and the graph might contain cycles.
 * It is reference counted, so the common case is deterministic: the object
 * dies the instant the last handle drops, on that thread, right there. The
 * collector in CoreGC.h exists only for cycles.
 *
 * ===========================
 * THE COST OF A TRef, STATED PLAINLY
 * ===========================
 *
 * Copying a TRef is an atomic increment PLUS a locked insert into the global
 * handle list, and destroying one is an atomic decrement plus a locked remove.
 * That is roughly an order of magnitude more than copying a std::shared_ptr.
 *
 * It buys something specific: the handle list is how the collector discovers
 * pointer edges without reflection, codegen or a common base class, none of
 * which EngineCore has (see the long note at the top of CoreGC.h). But it does
 * mean:
 *
 *   * pass `const TRef<T>&`, never TRef<T> by value, unless you are storing it;
 *   * use Get() for the raw T* inside a loop, rather than copying a handle
 *     per iteration;
 *   * do not build a std::vector<TRef<T>> of ten thousand elements per frame.
 *
 * A raw T* obtained from Get() is fine to USE. It is not fine to STORE: the
 * collector cannot see it, and will reclaim the object out from under it.
 */

namespace EngineCore
{
    /* Disambiguates "take ownership of a reference that has already been
     * counted" from "share this pointer". Only MakeRef and TWeakRef::Pin
     * produce one. */
    struct AdoptRefTag {};
    constexpr AdoptRefTag AdoptRef{};

    // ========================================================================
    // TUnique
    // ========================================================================

    /*
     * Sole ownership. Move-only, zero overhead beyond the allocation header,
     * invisible to the collector.
     *
     * WHY THE DESTRUCTOR GOES THROUGH THE HEADER AND NOT ~T()
     *
     * Memory::Dealloc<T> calls ~T() directly, which is faster and is right
     * when the static and dynamic types agree. TUnique cannot assume they do:
     * a TUnique<Base> may hold a Derived through the converting move below,
     * and ~Base() on a Derived is undefined unless Base's destructor is
     * virtual -- a requirement this class is in no position to impose.
     *
     * The header records the destructor of the type that was ACTUALLY
     * allocated, so going through it is correct in both cases. The price is
     * one indirect call per destruction, which is not where anyone's frame
     * time goes.
     */
    template<typename T>
    class TUnique
    {
    public:
        TUnique() = default;
        TUnique(std::nullptr_t) {}

        /* Adopts a pointer from Alloc/AllocIn. Explicit because silently
         * taking ownership of somebody else's pointer is how double frees
         * happen. */
        explicit TUnique(T* InPtr) : Ptr(InPtr) {}

        ~TUnique() { Reset(); }

        TUnique(const TUnique&)            = delete;
        TUnique& operator=(const TUnique&) = delete;

        TUnique(TUnique&& InOther) noexcept : Ptr(InOther.Ptr)
        {
            InOther.Ptr = nullptr;
        }

        /* TUnique<Derived> -> TUnique<Base>. */
        template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
        TUnique(TUnique<U>&& InOther) noexcept : Ptr(InOther.Release())
        {
        }

        TUnique& operator=(TUnique&& InOther) noexcept
        {
            if(this != &InOther)
            {
                Reset();
                Ptr         = InOther.Ptr;
                InOther.Ptr = nullptr;
            }
            return *this;
        }

        TUnique& operator=(std::nullptr_t)
        {
            Reset();
            return *this;
        }

        T*   Get()        const { return Ptr; }
        T*   operator->() const { return Ptr; }
        T&   operator*()  const { return *Ptr; }
        bool IsValid()    const { return Ptr != nullptr; }

        explicit operator bool() const { return Ptr != nullptr; }

        /* Hands the pointer back without freeing it. The caller now owes a
         * Dealloc. */
        T* Release()
        {
            T* Out = Ptr;
            Ptr    = nullptr;
            return Out;
        }

        void Reset(T* InNew = nullptr)
        {
            T* const Old = Ptr;
            Ptr = InNew;

            if(Old)
            {
                Memory::DestroyObject(Old);
                Memory::Free(Old);
            }
        }

    private:
        T* Ptr = nullptr;
    };

    template<typename T, typename... TArgs>
    TUnique<T> MakeUniqueIn(const PoolID InPool, TArgs&&... InArgs)
    {
        return TUnique<T>(Memory::AllocIn<T>(InPool, std::forward<TArgs>(InArgs)...));
    }

    template<typename T, typename... TArgs>
    TUnique<T> MakeUnique(TArgs&&... InArgs)
    {
        return TUnique<T>(Memory::Alloc<T>(std::forward<TArgs>(InArgs)...));
    }

    template<typename T> class TWeakRef;

    // ========================================================================
    // TRef
    // ========================================================================

    /*
     * A counted, collector-visible strong handle.
     *
     * WHY THERE ARE TWO POINTERS IN HERE
     *
     * Node.Target is the ALLOCATION pointer -- what Memory::Allocate returned,
     * and the only thing from which the header can be found. Ptr is the TYPED
     * view.
     *
     * They are usually equal and it is tempting to keep only one. They are not
     * always equal: converting a TRef<Derived> to a TRef<SecondBase> under
     * multiple inheritance ADJUSTS the pointer, and storing the adjusted value
     * as the allocation pointer would make every subsequent header lookup read
     * 48 bytes from the middle of the object. That failure is silent, it only
     * appears with multiple inheritance, and it corrupts rather than crashes.
     * Eight bytes is a cheap way not to have it.
     *
     * NODE MUST STAY THE FIRST MEMBER. The collector classifies a handle by
     * ITS OWN ADDRESS -- inside a tracked payload means edge, anywhere else
     * means root -- and that address is the address of this object. Reordering
     * would still work, since the node would remain within the same payload,
     * but the invariant is worth stating: nothing may be placed before the
     * node that could push it outside the object it belongs to.
     */
    template<typename T>
    class TRef
    {
    public:
        TRef()
        {
            Memory::RegisterHandle(&Node);
        }

        TRef(std::nullptr_t) : TRef() {}

        /* Takes over a reference that has already been counted -- what
         * MakeRef's freshly-allocated object and TWeakRef::Pin's successful
         * TryAddStrongRef both hand over. Does NOT increment. */
        TRef(AdoptRefTag, void* InAllocation, T* InTyped)
        {
            Node.Target = InAllocation;
            Ptr         = InTyped;
            Memory::RegisterHandle(&Node);
        }

        ~TRef()
        {
            // Deregister BEFORE releasing: the destructor that the release may
            // trigger can run arbitrary code, and this node must not still be
            // in the list -- it is about to stop existing.
            Memory::UnregisterHandle(&Node);

            if(Node.Target)
            {
                Memory::ReleaseStrongRef(Node.Target);
            }
        }

        TRef(const TRef& InOther)
        {
            Node.Target = InOther.Node.Target;
            Ptr         = InOther.Ptr;
            Memory::RegisterHandle(&Node);

            if(Node.Target)
            {
                Memory::AddStrongRef(Node.Target);
            }
        }

        TRef(TRef&& InOther) noexcept
        {
            Node.Target       = InOther.Node.Target;
            Ptr               = InOther.Ptr;
            InOther.Node.Target = nullptr;
            InOther.Ptr         = nullptr;

            // No refcount traffic: the reference moved, it was not duplicated.
            Memory::RegisterHandle(&Node);
        }

        /* TRef<Derived> -> TRef<Base>. */
        template<typename U, typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
        TRef(const TRef<U>& InOther)
        {
            Node.Target = InOther.GetAllocation();
            Ptr         = InOther.Get();
            Memory::RegisterHandle(&Node);

            if(Node.Target)
            {
                Memory::AddStrongRef(Node.Target);
            }
        }

        TRef& operator=(const TRef& InOther)
        {
            // ADD BEFORE RELEASE. Self-assignment, and assignment from another
            // handle to the same object, both go through here; releasing first
            // would destroy the object and then "share" a corpse.
            if(Node.Target != InOther.Node.Target)
            {
                void* const Old = Node.Target;

                if(InOther.Node.Target)
                {
                    Memory::AddStrongRef(InOther.Node.Target);
                }

                Node.Target = InOther.Node.Target;
                Ptr         = InOther.Ptr;

                if(Old)
                {
                    Memory::ReleaseStrongRef(Old);
                }
            }
            else
            {
                Ptr = InOther.Ptr;
            }
            return *this;
        }

        TRef& operator=(TRef&& InOther) noexcept
        {
            if(this != &InOther)
            {
                void* const Old = Node.Target;

                Node.Target         = InOther.Node.Target;
                Ptr                 = InOther.Ptr;
                InOther.Node.Target = nullptr;
                InOther.Ptr         = nullptr;

                if(Old)
                {
                    Memory::ReleaseStrongRef(Old);
                }
            }
            return *this;
        }

        TRef& operator=(std::nullptr_t)
        {
            Reset();
            return *this;
        }

        void Reset()
        {
            if(Node.Target)
            {
                void* const Old = Node.Target;
                Node.Target = nullptr;
                Ptr         = nullptr;
                Memory::ReleaseStrongRef(Old);
            }
        }

        T*   Get()        const { return Ptr; }
        T*   operator->() const { return Ptr; }
        T&   operator*()  const { return *Ptr; }
        bool IsValid()    const { return Ptr != nullptr; }

        explicit operator bool() const { return Ptr != nullptr; }

        /* The allocation pointer, for the converting constructors and
         * TWeakRef. Not for general use -- Get() is the pointer you want. */
        void* GetAllocation() const { return Node.Target; }

        uint32_t GetStrongCount() const { return Memory::GetStrongCount(Node.Target); }

        friend bool operator==(const TRef& L, const TRef& R) { return L.Ptr == R.Ptr; }
        friend bool operator!=(const TRef& L, const TRef& R) { return L.Ptr != R.Ptr; }
        friend bool operator==(const TRef& L, std::nullptr_t) { return L.Ptr == nullptr; }
        friend bool operator!=(const TRef& L, std::nullptr_t) { return L.Ptr != nullptr; }

    private:
        Memory::GCHandleNode Node;      // MUST stay first -- see the note above
        T*                   Ptr = nullptr;
    };

    // ========================================================================
    // TWeakRef
    // ========================================================================

    /*
     * Observes without owning, and without keeping the object alive.
     *
     * DELIBERATELY NOT REGISTERED WITH THE COLLECTOR. Being invisible to the
     * mark phase is the entire definition of "weak" -- a TWeakRef that
     * registered a handle would be a root, would keep its target reachable,
     * and would be a TRef with extra steps.
     *
     * WHAT IT DOES KEEP ALIVE: the BLOCK, not the object. The header and the
     * payload are one allocation (see the lifetime note in CoreMemory.h), so a
     * weak reference to a 4MB object holds 4MB of dead bytes until it goes
     * away. The object itself is destroyed on time; only the memory lingers.
     *
     * ALWAYS Pin() BEFORE USE. IsStale() is a hint and nothing more -- between
     * the check and the dereference, another thread's last TRef can drop.
     * Pin() is the atomic version of the same question and is the only safe
     * way to get a usable pointer out of this.
     */
    template<typename T>
    class TWeakRef
    {
    public:
        TWeakRef() = default;
        TWeakRef(std::nullptr_t) {}

        TWeakRef(const TRef<T>& InRef)
            : Allocation(InRef.GetAllocation())
            , Ptr(InRef.Get())
        {
            if(Allocation)
            {
                Memory::AddWeakRef(Allocation);
            }
        }

        TWeakRef(const TWeakRef& InOther)
            : Allocation(InOther.Allocation)
            , Ptr(InOther.Ptr)
        {
            if(Allocation)
            {
                Memory::AddWeakRef(Allocation);
            }
        }

        TWeakRef(TWeakRef&& InOther) noexcept
            : Allocation(InOther.Allocation)
            , Ptr(InOther.Ptr)
        {
            InOther.Allocation = nullptr;
            InOther.Ptr        = nullptr;
        }

        ~TWeakRef() { Reset(); }

        TWeakRef& operator=(const TWeakRef& InOther)
        {
            if(Allocation != InOther.Allocation)
            {
                if(InOther.Allocation)
                {
                    Memory::AddWeakRef(InOther.Allocation);
                }
                void* const Old = Allocation;

                Allocation = InOther.Allocation;
                Ptr        = InOther.Ptr;

                if(Old)
                {
                    Memory::ReleaseWeakRef(Old);
                }
            }
            return *this;
        }

        TWeakRef& operator=(TWeakRef&& InOther) noexcept
        {
            if(this != &InOther)
            {
                Reset();
                Allocation         = InOther.Allocation;
                Ptr                = InOther.Ptr;
                InOther.Allocation = nullptr;
                InOther.Ptr        = nullptr;
            }
            return *this;
        }

        TWeakRef& operator=(const TRef<T>& InRef)
        {
            TWeakRef Temp(InRef);
            *this = std::move(Temp);
            return *this;
        }

        void Reset()
        {
            if(Allocation)
            {
                void* const Old = Allocation;
                Allocation = nullptr;
                Ptr        = nullptr;
                Memory::ReleaseWeakRef(Old);
            }
        }

        /*
         * The only safe way to use what this points at. Returns a null TRef if
         * the object is already gone; otherwise the returned handle owns a
         * reference and the object cannot die while it is held.
         */
        TRef<T> Pin() const
        {
            if(Allocation && Memory::TryAddStrongRef(Allocation))
            {
                return TRef<T>(AdoptRef, Allocation, Ptr);
            }
            return TRef<T>();
        }

        /* A HINT. True if the object is definitely gone; false does not
         * promise it is still there by the time you look. Use Pin(). */
        bool IsStale() const
        {
            if(!Allocation)
            {
                return true;
            }
            return Memory::GetStrongCount(Allocation) == 0;
        }

        bool IsSet() const { return Allocation != nullptr; }

        explicit operator bool() const { return !IsStale(); }

    private:
        void* Allocation = nullptr;
        T*    Ptr        = nullptr;
    };

    // ========================================================================
    // Factories
    // ========================================================================

    /*
     * AF_GCTracked is set HERE and nowhere else. A plain Alloc or a TUnique is
     * not garbage-collected and has no business paying for a list insert; an
     * object that might participate in a cycle is, and does.
     */
    template<typename T, typename... TArgs>
    TRef<T> MakeRefIn(const PoolID InPool, TArgs&&... InArgs)
    {
        static_assert(alignof(T) <= ALLOC_MAX_ALIGN, "alignof(T) exceeds ALLOC_MAX_ALIGN");

        // GImageID read HERE, in the caller's image -- see the note on
        // Memory::Allocate. A module's tracked objects must carry the module's
        // id or the unload barrier cannot find them.
        void* Block = Memory::Allocate(sizeof(T), alignof(T), InPool,
                                       Memory::DestructorFor<T>(), AF_GCTracked, GImageID);
        if(!Block)
        {
            return TRef<T>();
        }

        // StrongCount was born at 1 -- see Memory::Allocate -- so the handle
        // below adopts that reference rather than adding a second one.
        T* Object = ::new (Block) T(std::forward<TArgs>(InArgs)...);
        return TRef<T>(AdoptRef, Block, Object);
    }

    template<typename T, typename... TArgs>
    TRef<T> MakeRef(TArgs&&... InArgs)
    {
        return MakeRefIn<T>(POOL_Default, std::forward<TArgs>(InArgs)...);
    }
}

#endif //ENGINECORE_COREPOINTER_H
