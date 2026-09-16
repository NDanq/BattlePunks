#ifndef GAMECORE_EVENTBUS_H
#define GAMECORE_EVENTBUS_H

#include <cstdint>
#include <cstring>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"

/*
 * Named events: the layer above TMulticastDelegate for callers that do not
 * share a header.
 *
 * A TMulticastDelegate<TArgs...> is the right primitive when both sides of a
 * binding can #include the same declaration -- no lookup, no packing, a direct
 * call through a typed function pointer. EventBus exists for the callers that
 * cannot do that: a Lua script (which has no C++ type to name), a hot module
 * (which should not need to link against the specific header that declares
 * "PlayerScored"), or two systems that are deliberately kept from knowing about
 * each other. The cost of that decoupling is a name lookup on Subscribe and an
 * argument pack on Emit -- worth paying at those boundaries, not on a hot path
 * that already has both ends in the same translation unit.
 *
 * World owns exactly one EventBus (World::GetEvents()) and mirrors its own four
 * built-in typed delegates onto it under fixed names -- see World.h's
 * "OnEntityCreated" etc. and ScriptEngine::BindWorld -- so Lua observes engine
 * events through this one mechanism rather than a second, script-specific path.
 */

namespace GameCore
{
    using EventID = uint16_t;
    enum : EventID
    {
        EVENT_INVALID = 0xFFFF,
        EVENT_MAX     = 512
    };

    /*
     * One typed argument. POD and fixed-size so it crosses images and the Lua
     * boundary the way ConfigValue does in CoreConfig.h, and for the same
     * reason: a std::string member here would allocate in whichever image
     * called Emit and be read (or freed) by a subscriber in a different one.
     */
    struct EventArg
    {
        enum EType : uint8_t { ET_None = 0, ET_Number, ET_Bool, ET_String, ET_Entity };

        static constexpr size_t STRING_MAX = 64;

        EType Type = ET_None;
        union
        {
            double   Number;
            bool     Bool;
            char     String[STRING_MAX];
            uint64_t Entity;   // a packed EntityID -- see EntityID::Pack()
        };

        EventArg() : Number(0.0) {}

        static EventArg Make(const double InValue)
        {
            EventArg A; A.Type = ET_Number; A.Number = InValue; return A;
        }
        static EventArg Make(const bool InValue)
        {
            EventArg A; A.Type = ET_Bool; A.Bool = InValue; return A;
        }
        /* Truncates silently past STRING_MAX - 1, the same call CoreConfig.h's
         * MakeConfigString makes: an event argument is a short label, not a
         * payload, and anything long enough to truncate belongs on a component
         * instead. */
        static EventArg Make(const char* InValue)
        {
            EventArg A;
            A.Type = ET_String;
            if(InValue)
            {
                std::strncpy(A.String, InValue, STRING_MAX - 1);
                A.String[STRING_MAX - 1] = '\0';
            }
            else
            {
                A.String[0] = '\0';
            }
            return A;
        }
        static EventArg Make(const EntityID InValue)
        {
            EventArg A; A.Type = ET_Entity; A.Entity = InValue.Pack(); return A;
        }
    };

    /* Four is enough for a notification -- "PlayerScored", 10, "Player" -- which
     * is the shape this is for. Anything that needs more is not an event, it is
     * data, and belongs on a component the subscriber can query by EntityID. */
    struct EventArgs
    {
        EventArg Args[4];
        uint8_t  Count = 0;
    };

    using EventFn = void (*)(EventID InEvent, const EventArgs& InArgs, void* InUserData);

    /*
     * Names are interned once (GetEventID) and every hot-path call after that
     * uses the id -- exactly the ComponentType.h pattern of "register once by
     * name, look up by id afterwards" applied to events instead of components.
     */
    class GAMECORE_API EventBus
    {
    public:
        EventBus();
        ~EventBus();

        EventBus(const EventBus&)            = delete;
        EventBus& operator=(const EventBus&) = delete;

        /* Interns InName if it has not been seen before and returns its id.
         * Call once, hold the id -- this is the ONLY entry point that scans the
         * name table on every call by design; everything else takes the id it
         * returns. */
        EventID GetEventID(const char* InName);

        /* EVENT_INVALID if InName was never interned -- a read-only probe,
         * unlike GetEventID which registers on a miss. */
        EventID FindEventID(const char* InName) const;

        /* Never null for a valid id; "" for EVENT_INVALID or an out-of-range
         * id. The pointer is into this bus's fixed name table and is valid for
         * the bus's lifetime -- unlike ConfigVar::GetString, nothing here ever
         * overwrites an already-interned name. */
        const char* GetEventName(EventID InEvent) const;

        /* How many names have been interned so far -- the ComponentType.h
         * GetComponentTypeCount() pattern, applied to events. What bounds a
         * loop over every known EventID (Events.Off's ScriptBindings.cpp
         * implementation is the one caller today) instead of walking all of
         * EVENT_MAX on every call. */
        size_t GetEventCount() const;

        DelegateHandle Subscribe(EventID InEvent, EventFn InFn, void* InUserData = nullptr);
        /* Convenience that interns on first use -- the hot path (repeated
         * Emit/Subscribe against a name already known) wants the EventID
         * overload instead. */
        DelegateHandle Subscribe(const char* InName, EventFn InFn, void* InUserData = nullptr);

        bool Unsubscribe(EventID InEvent, DelegateHandle InHandle);

        /* Across EVERY event this bus knows about -- the RemoveAll(UserData)
         * primitive from Delegate.h, generalised over the whole name table. What
         * ScriptEngine::UnbindWorld calls once rather than tracking a handle per
         * Events.On() a script ever made. */
        size_t UnsubscribeAll(void* InUserData);

        void Emit(EventID InEvent, const EventArgs& InArgs = EventArgs());
        /* Convenience; interns InName if this is the first time anything ever
         * named it, which means an Emit can register an id nobody Subscribed
         * to yet -- a no-op broadcast to zero listeners, not an error. */
        void Emit(const char* InName, const EventArgs& InArgs = EventArgs());

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_EVENTBUS_H
