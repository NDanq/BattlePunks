#include "World/LabSignals.h"

#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"

#include <map>
#include <string>

using namespace GameCore;

namespace
{
    /* Per World: a level's latches, keyed by channel. */
    std::map<const World*, std::map<std::string, bool>> GLatches;

    struct Queued { std::string Channel; bool bValue; EntityID Source; };
    std::map<const World*, int> GHandling;
    std::map<const World*, std::vector<Queued>> GQueue;
}

void LabSignals::BeginHandling(World& InWorld) { ++GHandling[&InWorld]; }

void LabSignals::EndHandling(World& InWorld)
{
    int& Depth = GHandling[&InWorld];
    if(Depth > 0) { --Depth; }
}

void LabSignals::Flush(World& InWorld)
{
    if(GHandling[&InWorld] > 0) { return; }
    /* Each emit may queue more (a chain of listeners): until none is left. */
    for(int Guard = 0; Guard < 32; ++Guard)
    {
        std::vector<Queued> Pending;
        Pending.swap(GQueue[&InWorld]);
        if(Pending.empty()) { return; }
        for(const Queued& Q : Pending) { Emit(InWorld, Q.Channel.c_str(), Q.bValue, Q.Source); }
    }
}

void LabSignals::Emit(World& InWorld, const char* InChannel, const bool bInValue, const EntityID InSource)
{
    if(!InChannel || !InChannel[0]) { return; }
    if(GHandling[&InWorld] > 0)
    {
        GQueue[&InWorld].push_back({ InChannel, bInValue, InSource });
        return;
    }
    EventArgs Args;
    Args.Args[Args.Count++] = EventArg::Make(InChannel);
    Args.Args[Args.Count++] = EventArg::Make(bInValue);
    Args.Args[Args.Count++] = EventArg::Make(InSource);
    InWorld.GetEvents().Emit(EVENT, Args);
}

bool LabSignals::Read(const EventArgs& InArgs, const char*& OutChannel, bool& bOutValue)
{
    if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_String || !InArgs.Args[0].String[0]) { return false; }
    OutChannel = InArgs.Args[0].String;
    bOutValue = true;
    if(InArgs.Count >= 2)
    {
        const EventArg& Value = InArgs.Args[1];
        if(Value.Type == EventArg::ET_Bool) { bOutValue = Value.Bool; }
        else if(Value.Type == EventArg::ET_Number) { bOutValue = Value.Number != 0.0; }
        else if(Value.Type == EventArg::ET_String) { bOutValue = std::string(Value.String) != "false" && std::string(Value.String) != "0"; }
    }
    return true;
}

bool LabSignals::Get(World& InWorld, const char* InChannel)
{
    const auto Level = GLatches.find(&InWorld);
    if(Level == GLatches.end() || !InChannel) { return false; }
    const auto Found = Level->second.find(InChannel);
    return Found != Level->second.end() && Found->second;
}

bool LabSignals::WasEmitted(World& InWorld, const char* InChannel)
{
    const auto Level = GLatches.find(&InWorld);
    return Level != GLatches.end() && InChannel && Level->second.count(InChannel) != 0;
}

void LabSignals::Latch(World& InWorld, const char* InChannel, const bool bInValue)
{
    if(InChannel && InChannel[0]) { GLatches[&InWorld][InChannel] = bInValue; }
}

void LabSignals::Reset(World& InWorld)
{
    GLatches.erase(&InWorld);
    GQueue.erase(&InWorld);
    GHandling.erase(&InWorld);
}

std::vector<std::pair<std::string, bool>> LabSignals::Snapshot(World& InWorld)
{
    std::vector<std::pair<std::string, bool>> Out;
    const auto Level = GLatches.find(&InWorld);
    if(Level != GLatches.end()) { for(const auto& Pair : Level->second) { Out.emplace_back(Pair.first, Pair.second); } }
    return Out;
}

void LabSignals::Restore(World& InWorld, const std::vector<std::pair<std::string, bool>>& InLatches)
{
    std::map<std::string, bool>& Level = GLatches[&InWorld];
    Level.clear();
    for(const auto& Pair : InLatches) { Level[Pair.first] = Pair.second; }
}
