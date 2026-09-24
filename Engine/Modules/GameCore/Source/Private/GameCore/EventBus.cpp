#include "Public/GameCore/EventBus.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * One DelegateList per interned event name, held in a growable array indexed
 * by EventID. Lookup by name is a linear scan -- see DESIGN.md for why that
 * is fine here.
 */

namespace GameCore
{
    namespace
    {
        struct EventEntry
        {
            char         Name[48] = {};
            DelegateList Subscribers;
        };
    }

    struct EventBus::Impl
    {
        std::vector<EventEntry*> Entries;   // pointer per entry: EventEntry holds a non-copyable DelegateList

        ~Impl()
        {
            for(EventEntry* Entry : Entries)
            {
                delete Entry;
            }
        }

        EventID FindByName(const char* InName) const
        {
            for(size_t Index = 0; Index < Entries.size(); ++Index)
            {
                if(std::strcmp(Entries[Index]->Name, InName) == 0)
                {
                    return EventID(Index);
                }
            }
            return EVENT_INVALID;
        }

        EventID Intern(const char* InName)
        {
            const EventID Existing = FindByName(InName);
            if(Existing != EVENT_INVALID)
            {
                return Existing;
            }

            if(Entries.size() >= EVENT_MAX)
            {
                GAMECORE_LOG_ERROR(std::string("EventBus: EVENT_MAX (") + std::to_string(EVENT_MAX)
                                    + ") reached; \"" + InName + "\" was not registered")
                return EVENT_INVALID;
            }

            EventEntry* Entry = new EventEntry();
            std::strncpy(Entry->Name, InName, sizeof(Entry->Name) - 1);
            Entry->Name[sizeof(Entry->Name) - 1] = '\0';

            const EventID NewID = EventID(Entries.size());
            Entries.push_back(Entry);
            return NewID;
        }

        bool IsValid(const EventID InEvent) const
        {
            return InEvent != EVENT_INVALID && size_t(InEvent) < Entries.size();
        }
    };

    EventBus::EventBus() : P(new Impl()) {}
    EventBus::~EventBus() { delete P; }

    EventID EventBus::GetEventID(const char* InName)
    {
        if(!InName) { return EVENT_INVALID; }
        return P->Intern(InName);
    }

    EventID EventBus::FindEventID(const char* InName) const
    {
        if(!InName) { return EVENT_INVALID; }
        return P->FindByName(InName);
    }

    const char* EventBus::GetEventName(const EventID InEvent) const
    {
        if(!P->IsValid(InEvent)) { return ""; }
        return P->Entries[InEvent]->Name;
    }

    size_t EventBus::GetEventCount() const
    {
        return P->Entries.size();
    }

    DelegateHandle EventBus::Subscribe(const EventID InEvent, const EventFn InFn, void* InUserData)
    {
        if(!P->IsValid(InEvent))
        {
            GAMECORE_LOG_WARN("EventBus::Subscribe given an unknown EventID; ignoring")
            return DelegateHandle();
        }
        return P->Entries[InEvent]->Subscribers.Add(reinterpret_cast<DelegateList::RawFn>(InFn), InUserData);
    }

    DelegateHandle EventBus::Subscribe(const char* InName, const EventFn InFn, void* InUserData)
    {
        return Subscribe(GetEventID(InName), InFn, InUserData);
    }

    bool EventBus::Unsubscribe(const EventID InEvent, const DelegateHandle InHandle)
    {
        if(!P->IsValid(InEvent)) { return false; }
        return P->Entries[InEvent]->Subscribers.Remove(InHandle);
    }

    size_t EventBus::UnsubscribeAll(void* InUserData)
    {
        size_t Total = 0;
        for(EventEntry* Entry : P->Entries)
        {
            Total += Entry->Subscribers.RemoveAll(InUserData);
        }
        return Total;
    }

    void EventBus::Emit(const EventID InEvent, const EventArgs& InArgs)
    {
        if(!P->IsValid(InEvent)) { return; }

        DelegateList& Subscribers = P->Entries[InEvent]->Subscribers;
        /* False means a subscriber further up this call stack is already
         * emitting THIS event -- refused, not merely logged. See Delegate.h. */
        if(!Subscribers.BeginBroadcast())
        {
            return;
        }

        for(size_t Index = 0; ; ++Index)
        {
            DelegateList::RawFn Fn;
            void*                UserData;
            if(!Subscribers.GetEntry(Index, Fn, UserData)) { break; }
            if(Fn)
            {
                reinterpret_cast<EventFn>(Fn)(InEvent, InArgs, UserData);
            }
        }
        Subscribers.EndBroadcast();
    }

    void EventBus::Emit(const char* InName, const EventArgs& InArgs)
    {
        Emit(GetEventID(InName), InArgs);
    }
}
