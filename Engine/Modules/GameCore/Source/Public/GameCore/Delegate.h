#ifndef GAMECORE_DELEGATE_H
#define GAMECORE_DELEGATE_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Core/CoreLog.h"

/*
 * The delegate framework: a binding is {function pointer, void*}, never a
 * std::function.
 *
 * WHY NOT std::function
 *
 * A std::function captured by value allocates in the image that built the
 * lambda and is destroyed by whatever runs the multicast's destructor -- the
 * exact cross-heap free CoreConfig.h's CONFIG HAZARDS documents for the reason
 * ConfigValue is a fixed union rather than a std::string. GameWindowDelegates.h
 * makes the same call for window callbacks and CoreLog.h's LogHandler for the
 * same reason again: a bare function pointer plus a void* has no owner and no
 * allocation, so it has exactly one way to dangle -- its image unloading -- and
 * that case already has a name (see the RELOAD note below).
 *
 * ================================================================
 * THE STRUCTURAL RULE, ONE MORE TIME: TEMPLATES COMPUTE, GameCore.dll OWNS
 * ================================================================
 *
 * DelegateList below is the ERASED, GAMECORE_API, out-of-line implementation --
 * every array, every handle table and every broadcast-in-progress flag lives in
 * GameCore.dll. TMulticastDelegate<TArgs...> and TDelegate<TRet, TArgs...> are
 * header-only and do nothing but cast a typed function pointer to
 * DelegateList::RawFn and back. This is CoreMemory.h's "templates compute,
 * EngineCore allocates" argument, applied to delegates instead of allocations:
 * a template instantiates in every image that calls it, so if the storage lived
 * in the template, NimbleEngine.dll and a hot module would each get their own
 * private list and nobody would see the other's bindings.
 *
 * ================================================================
 * RELOAD / LIFETIME RULE -- read before binding from a module
 * ================================================================
 *
 * A BINDING IS A PROMISE TO UNBIND. A function pointer into a hot module left in
 * a DelegateList after that module unloads is CoreModule.h's DANGLING CALLBACKS
 * hazard, verbatim: the next Broadcast() jumps into unmapped memory, and the
 * crash lands in whatever fired the broadcast, nowhere near the module that
 * forgot to unbind. RemoveAll(UserData) exists so "whatever OnStartup bound,
 * OnShutdown unbinds" is one call; TScopedBinding exists so a C++ owner cannot
 * forget at all. World logs a warning for any of its own built-in delegates
 * still holding entries at destruction, the same "cannot enforce it, but can
 * detect and warn" posture GameWindowSystem::Shutdown takes for GameWindow's own
 * delegate table.
 *
 * ================================================================
 * BROADCAST RE-ENTRANCY
 * ================================================================
 *
 * Add/Remove called FROM INSIDE a callback that Broadcast is currently running
 * are deferred to when that Broadcast finishes -- a callback that unbinds itself
 * must not shift the indices a broadcast in progress is walking. Broadcasting
 * the SAME DelegateList again from inside one of its own callbacks is refused
 * with a CORE_CHECK: this is not a general recursion guard (a different
 * TMulticastDelegate may be broadcast from inside a callback with no issue), it
 * is specifically "this list, again, while its own walk is still on the stack".
 */

namespace GameCore
{
    /* Returned by DelegateList::Add. Index is the slot, Serial disambiguates a
     * slot that has since been reused by a different binding -- the same
     * generation trick EntityID.h uses for the same reason: without it, Remove
     * on a stale handle could unbind whatever binding now happens to occupy that
     * slot instead of silently doing nothing. */
    struct DelegateHandle
    {
        uint32_t Index  = 0xFFFFFFFFu;
        uint32_t Serial = 0;

        bool IsValid() const { return Index != 0xFFFFFFFFu; }

        friend bool operator==(const DelegateHandle& L, const DelegateHandle& R)
        {
            return L.Index == R.Index && L.Serial == R.Serial;
        }
        friend bool operator!=(const DelegateHandle& L, const DelegateHandle& R) { return !(L == R); }
    };

    /*
     * The erased list every TMulticastDelegate<TArgs...> is a typed view over.
     *
     * RawFn is a function-pointer-shaped placeholder with no real signature --
     * every entry is stored as a cast RawFn and cast back to the caller's exact
     * FnType at the point of the call, in TMulticastDelegate::Broadcast, which is
     * the one place that has ever seen the real type. Casting a function pointer
     * through an unrelated function pointer type and back is well-defined in
     * C++ (unlike doing the same through void*, which is only guaranteed for
     * object pointers); calling it through the WRONG type is not, so nothing
     * outside this file may call through a RawFn directly.
     */
    class GAMECORE_API DelegateList
    {
    public:
        using RawFn = void (*)();

        DelegateList();
        ~DelegateList();

        DelegateList(const DelegateList&)            = delete;
        DelegateList& operator=(const DelegateList&) = delete;

        /* Never fails -- the list grows. */
        DelegateHandle Add(RawFn InFn, void* InUserData);

        /* False if the handle is stale (already removed, or from a different
         * list generation) -- never a hard error, since a module racing its own
         * unload against a broadcast is exactly the case this exists to make
         * survivable. Deferred to EndBroadcast() if called while
         * IsBroadcasting(). */
        bool Remove(DelegateHandle InHandle);

        /* Removes every binding whose user pointer equals InUserData. The
         * "OnShutdown undoes whatever OnStartup bound" primitive -- a module
         * that bound several callbacks with itself (or one owned object) as the
         * user pointer unbinds all of them in one call rather than tracking N
         * handles. Returns the number removed. */
        size_t RemoveAll(void* InUserData);

        void Clear();

        size_t GetCount() const;
        bool   IsBroadcasting() const;

        /*
         * The broadcast protocol TMulticastDelegate::Broadcast drives:
         *
         *   if(BeginBroadcast()) {                   // false means REFUSED -- see below -- skip the walk AND EndBroadcast
         *       for(size_t I = 0; ; ++I) {
         *           RawFn Fn; void* User;
         *           if(!GetEntry(I, Fn, User)) break;   // I reached the count captured at Begin
         *           if(Fn) CallThroughRealType(Fn, User, args...);  // Fn is null for an index removed mid-walk
         *       }
         *       EndBroadcast();                      // applies deferred adds/removes, compacts tombstones
         *   }
         *
         * GetEntry never returns a moved or resized entry mid-walk: the backing
         * array does not reallocate while IsBroadcasting() is true, which is
         * exactly what BeginBroadcast latches by capturing the current count and
         * deferring structural changes.
         *
         * BeginBroadcast RETURNS false, IT DOES NOT JUST LOG, for the
         * re-entrancy case (see the BROADCAST RE-ENTRANCY note above). A
         * CORE_CHECK failure only logs and returns when a handler is
         * installed -- it does not stop the code after it from running, which
         * is CoreLog.h's own documented Fatal semantics ("LogDispatch does
         * exactly what the handler does and then RETURNS, even for Fatal").
         * An earlier version of this function called CORE_CHECK and then fell
         * straight through into starting the broadcast anyway, which meant
         * the "refusal" never refused anything: a callback that broadcasts its
         * own delegate recursed until the stack overflowed, logging one
         * warning per frame on the way down. The lesson generalises: CORE_CHECK
         * is a DIAGNOSTIC, never a GATE -- anywhere code must not proceed past
         * a failed condition needs an explicit if/return, with CORE_CHECK (or
         * a plain log call) only supplying the message.
         */
        bool BeginBroadcast();
        bool GetEntry(size_t InIndex, RawFn& OutFn, void*& OutUserData) const;
        void EndBroadcast();

    private:
        struct Impl;
        Impl* P;
    };

    /*
     * Single-bound, may return a value. Header-only and two words -- a raw
     * function pointer and a void* -- so it allocates nothing and does not need
     * DelegateList's machinery: there is only ever one binding to displace, never
     * a broadcast to protect.
     */
    template<typename TRet, typename... TArgs>
    class TDelegate
    {
    public:
        using FnType = TRet (*)(TArgs..., void*);

        void Bind(FnType InFn, void* InUserData = nullptr) { Fn = InFn; UserData = InUserData; }
        void Unbind() { Fn = nullptr; UserData = nullptr; }
        bool IsBound() const { return Fn != nullptr; }

        /* CORE_CHECKs IsBound() first -- calling an unbound TDelegate is a
         * programming error, not a runtime condition, the same distinction
         * CoreConfig.h draws between GetInt() on a bool var (a CORE_CHECK) and a
         * missing .ini file (a warning). Use ExecuteIfBound for the case where
         * "nobody is listening" is expected. */
        TRet Execute(TArgs... InArgs) const
        {
            CORE_CHECK(Fn != nullptr, "TDelegate::Execute called while unbound")
            return Fn(InArgs..., UserData);
        }

        bool ExecuteIfBound(TArgs... InArgs, TRet* OutRet = nullptr) const
        {
            if(!Fn) { return false; }
            const TRet Result = Fn(InArgs..., UserData);
            if(OutRet) { *OutRet = Result; }
            return true;
        }

    private:
        FnType Fn       = nullptr;
        void*  UserData = nullptr;
    };

    /* void-returning specialization -- ExecuteIfBound has nothing to write back
     * and Execute has nothing to return, so both are simpler than the primary
     * template rather than instantiating dead code around an unused TRet. */
    template<typename... TArgs>
    class TDelegate<void, TArgs...>
    {
    public:
        using FnType = void (*)(TArgs..., void*);

        void Bind(FnType InFn, void* InUserData = nullptr) { Fn = InFn; UserData = InUserData; }
        void Unbind() { Fn = nullptr; UserData = nullptr; }
        bool IsBound() const { return Fn != nullptr; }

        void Execute(TArgs... InArgs) const
        {
            CORE_CHECK(Fn != nullptr, "TDelegate::Execute called while unbound")
            if(Fn) { Fn(InArgs..., UserData); }
        }

        bool ExecuteIfBound(TArgs... InArgs) const
        {
            if(!Fn) { return false; }
            Fn(InArgs..., UserData);
            return true;
        }

    private:
        FnType Fn       = nullptr;
        void*  UserData = nullptr;
    };

    /*
     * Multicast, void return -- the typed view over a DelegateList. Every member
     * here does nothing but cast and forward into List; the list itself is what
     * is exported (see the structural-rule note at the top of this file).
     */
    template<typename... TArgs>
    class TMulticastDelegate
    {
    public:
        using FnType = void (*)(TArgs..., void*);

        DelegateHandle Add(FnType InFn, void* InUserData = nullptr)
        {
            return List.Add(reinterpret_cast<DelegateList::RawFn>(InFn), InUserData);
        }

        bool   Remove(DelegateHandle InHandle)      { return List.Remove(InHandle); }
        size_t RemoveAll(void* InUserData)          { return List.RemoveAll(InUserData); }
        void   Clear()                              { List.Clear(); }
        size_t GetCount() const                     { return List.GetCount(); }

        void Broadcast(TArgs... InArgs) const
        {
            /* BeginBroadcast() returning false means REFUSED -- a callback
             * further up this exact call stack broadcast this exact delegate
             * already. Skip the walk AND EndBroadcast() entirely in that case;
             * see BeginBroadcast's own comment for why this check must be an
             * explicit branch and not rely on CORE_CHECK to stop anything. */
            if(!List.BeginBroadcast())
            {
                return;
            }

            for(size_t Index = 0; ; ++Index)
            {
                DelegateList::RawFn Fn;
                void*               UserData;
                if(!List.GetEntry(Index, Fn, UserData))
                {
                    break;
                }
                if(Fn)
                {
                    /* The one place the erased RawFn is cast back to its real
                     * signature -- see the note on DelegateList::RawFn above. */
                    reinterpret_cast<FnType>(Fn)(InArgs..., UserData);
                }
            }
            List.EndBroadcast();
        }

        /* For the Lua bridge (ScriptBindings.cpp) and anything else that needs
         * the erased list directly rather than a typed Add/Broadcast pair. */
        DelegateList&       GetList()       { return List; }
        const DelegateList& GetList() const { return List; }

    private:
        /* Mutable so Broadcast() can stay a const method -- broadcasting does
         * not conceptually mutate what the delegate MEANS, only its bookkeeping,
         * matching TRef::GetStrongCount()'s constness in CorePointer.h. */
        mutable DelegateList List;
    };

    /*
     * RAII unbind for a C++ owner: Remove()s on destruction so a game object
     * that binds in its constructor cannot forget to unbind in its destructor.
     * Move-only -- a copy would unbind the same handle twice.
     */
    template<typename... TArgs>
    class TScopedBinding
    {
    public:
        TScopedBinding() = default;

        TScopedBinding(TMulticastDelegate<TArgs...>& InOwner, typename TMulticastDelegate<TArgs...>::FnType InFn, void* InUserData = nullptr)
            : Owner(&InOwner), Handle(InOwner.Add(InFn, InUserData))
        {
        }

        ~TScopedBinding() { Reset(); }

        TScopedBinding(const TScopedBinding&)            = delete;
        TScopedBinding& operator=(const TScopedBinding&) = delete;

        TScopedBinding(TScopedBinding&& InOther) noexcept : Owner(InOther.Owner), Handle(InOther.Handle)
        {
            InOther.Owner  = nullptr;
            InOther.Handle = DelegateHandle();
        }

        TScopedBinding& operator=(TScopedBinding&& InOther) noexcept
        {
            if(this != &InOther)
            {
                Reset();
                Owner  = InOther.Owner;
                Handle = InOther.Handle;
                InOther.Owner  = nullptr;
                InOther.Handle = DelegateHandle();
            }
            return *this;
        }

        void Reset()
        {
            if(Owner && Handle.IsValid())
            {
                Owner->Remove(Handle);
            }
            Owner  = nullptr;
            Handle = DelegateHandle();
        }

        bool IsBound() const { return Owner != nullptr && Handle.IsValid(); }

    private:
        TMulticastDelegate<TArgs...>* Owner = nullptr;
        DelegateHandle                Handle;
    };
}

#endif //GAMECORE_DELEGATE_H
