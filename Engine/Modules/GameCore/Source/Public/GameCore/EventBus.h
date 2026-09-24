#ifndef GAMECORE_EVENTBUS_H
#define GAMECORE_EVENTBUS_H

#include <cstdint>
#include <cstring>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"

/*
 * Named events: the layer above TMulticastDelegate for callers that do not
 * share a header (Lua, a hot module, two systems kept apart on purpose).
 * World owns one EventBus and mirrors its own four built-in delegates onto
 * it -- see DESIGN.md's "EventBus.h" section.
 */

namespace GameCore
{
    using EventID = uint16_t;
    enum : EventID
    {
        EVENT_INVALID = 0xFFFF,
        EVENT_MAX     = 512
    };

    /* One typed argument. POD and fixed-size so it crosses images and the
     * Lua boundary -- see DESIGN.md for why. */
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
        /* Truncates silently past STRING_MAX - 1 -- an event argument is a
         * short label, not a payload. */
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

    /* Four is enough for a notification; anything more is data and belongs
     * on a component instead. */
    struct EventArgs
    {
        EventArg Args[4];
        uint8_t  Count = 0;
    };

    using EventFn = void (*)(EventID InEvent, const EventArgs& InArgs, void* InUserData);

    /* Names are interned once (GetEventID); every hot-path call after that
     * uses the id -- the same register-by-name-then-look-up-by-id pattern
     * ComponentType.h uses. */
    class GAMECORE_API EventBus
    {
    public:
        EventBus();
        ~EventBus();

        EventBus(const EventBus&)            = delete;
        EventBus& operator=(const EventBus&) = delete;

        /* Interns InName if unseen and returns its id. Call once, hold the
         * id -- this is the only entry point that scans the name table. */
        EventID GetEventID(const char* InName);

        /* EVENT_INVALID if InName was never interned -- a read-only probe. */
        EventID FindEventID(const char* InName) const;

        /* Never null for a valid id; "" for EVENT_INVALID or out-of-range. */
        const char* GetEventName(EventID InEvent) const;

        /* How many names have been interned so far -- bounds a loop over
         * every known EventID instead of walking all of EVENT_MAX. */
        size_t GetEventCount() const;

        DelegateHandle Subscribe(EventID InEvent, EventFn InFn, void* InUserData = nullptr);
        /* Convenience that interns on first use. */
        DelegateHandle Subscribe(const char* InName, EventFn InFn, void* InUserData = nullptr);

        bool Unsubscribe(EventID InEvent, DelegateHandle InHandle);

        /* Across every event this bus knows about -- RemoveAll(UserData),
         * generalised over the whole name table. */
        size_t UnsubscribeAll(void* InUserData);

        void Emit(EventID InEvent, const EventArgs& InArgs = EventArgs());
        /* Convenience; interns InName on first use, so an Emit can register
         * an id nobody Subscribed to yet -- a no-op, not an error. */
        void Emit(const char* InName, const EventArgs& InArgs = EventArgs());

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_EVENTBUS_H
