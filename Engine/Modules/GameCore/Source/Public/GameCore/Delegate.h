#ifndef GAMECORE_DELEGATE_H
#define GAMECORE_DELEGATE_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Core/CoreLog.h"

/*
 * The delegate framework: a binding is {function pointer, void*}, never a
 * std::function -- see DESIGN.md's "Delegate.h" section for why, for the
 * reload hazard (a binding is a promise to unbind), and for the re-entrancy
 * bug that shaped BeginBroadcast's contract below.
 */

namespace GameCore
{
    /* Returned by DelegateList::Add. Index is the slot; Serial disambiguates
     * a reused slot the same way EntityID's generation does, so Remove on a
     * stale handle is a no-op rather than unbinding whatever occupies that
     * slot now. */
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
     * The erased list every TMulticastDelegate<TArgs...> is a typed view
     * over -- storage and policy live in GameCore.dll, never in the template
     * (see DESIGN.md). RawFn is only ever cast back to its real signature
     * inside TMulticastDelegate::Broadcast; do not call through it elsewhere.
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

        /* False for a stale handle -- never a hard error. Deferred to
         * EndBroadcast() if called while IsBroadcasting(). */
        bool Remove(DelegateHandle InHandle);

        /* Removes every binding whose user pointer equals InUserData --
         * "whatever OnStartup bound, OnShutdown unbinds" in one call.
         * Returns the number removed. */
        size_t RemoveAll(void* InUserData);

        void Clear();

        size_t GetCount() const;
        bool   IsBroadcasting() const;

        /*
         * The broadcast protocol TMulticastDelegate::Broadcast drives -- see
         * DESIGN.md for the loop shape. BeginBroadcast RETURNS false (does
         * not just log) to refuse a re-entrant broadcast of THIS SAME list;
         * the caller must branch on it and skip both the walk and
         * EndBroadcast when refused.
         */
        bool BeginBroadcast();
        bool GetEntry(size_t InIndex, RawFn& OutFn, void*& OutUserData) const;
        void EndBroadcast();

    private:
        struct Impl;
        Impl* P;
    };

    /* Single-bound, may return a value. Header-only and two words, so it
     * allocates nothing and needs none of DelegateList's machinery -- only
     * one binding to displace, never a broadcast to protect. */
    template<typename TRet, typename... TArgs>
    class TDelegate
    {
    public:
        using FnType = TRet (*)(TArgs..., void*);

        void Bind(FnType InFn, void* InUserData = nullptr) { Fn = InFn; UserData = InUserData; }
        void Unbind() { Fn = nullptr; UserData = nullptr; }
        bool IsBound() const { return Fn != nullptr; }

        /* CORE_CHECKs IsBound() first -- an unbound call is a programming
         * error. Use ExecuteIfBound where "nobody is listening" is expected. */
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

    /* void-returning specialization -- simpler than the primary template
     * rather than instantiating dead code around an unused TRet. */
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

    /* Multicast, void return -- the typed view over a DelegateList. Every
     * member does nothing but cast and forward into List. */
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
            /* BeginBroadcast() false means refused -- see its own comment.
             * Skip the walk AND EndBroadcast() entirely in that case. */
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
                    /* The one place RawFn is cast back to its real signature. */
                    reinterpret_cast<FnType>(Fn)(InArgs..., UserData);
                }
            }
            List.EndBroadcast();
        }

        /* For the Lua bridge (ScriptBindings.cpp) and anything else that
         * needs the erased list directly. */
        DelegateList&       GetList()       { return List; }
        const DelegateList& GetList() const { return List; }

    private:
        /* Mutable so Broadcast() can stay const -- see DESIGN.md. */
        mutable DelegateList List;
    };

    /* RAII unbind for a C++ owner: Remove()s on destruction. Move-only -- a
     * copy would unbind the same handle twice. */
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
