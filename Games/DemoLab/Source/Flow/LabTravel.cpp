#include "Flow/LabTravel.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/UI/UISystem.h"

#include <cstring>
#include <string>

using namespace GameCore;
using namespace NimbleEngine;

namespace
{
    LabTravel::State GState;
    float            GCardSeconds = 0.f;

    /* Fields of a saved state, one per line: World, Entry, Flags. */
    std::string Field(const std::string& InText, const size_t InIndex)
    {
        size_t Start = 0;
        for(size_t I = 0; I < InIndex; ++I)
        {
            Start = InText.find('\n', Start);
            if(Start == std::string::npos) { return std::string(); }
            ++Start;
        }
        const size_t End = InText.find('\n', Start);
        return InText.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
    }
}

bool LabTravel::Travel(GameSession& InSession, const char* InWorld, const char* InEntry, const char* InFlags,
                       const char* InLoadingTitle)
{
    ILevel* Level = InWorld ? FindLevelByName(InWorld) : nullptr;
    if(!Level)
    {
        CORE_LOG_ERROR(std::string("[DemoLab] travel: no level '") + (InWorld ? InWorld : "") + "' ([World_*] in DefaultEngine.ini)")
        return false;
    }
    GState.World        = InWorld;
    GState.Entry        = InEntry ? InEntry : "";
    GState.Flags        = InFlags ? InFlags : "";
    GState.LoadingTitle = InLoadingTitle ? InLoadingTitle : "";
    GState.bCardPending = !GState.LoadingTitle.empty();
    CORE_LOG("[DemoLab] travel to " + GState.World + (GState.Entry.empty() ? std::string() : " at " + GState.Entry)
             + (GState.Flags.empty() ? std::string() : " (" + GState.Flags + ")"))
    InSession.SetPaused(false);
    InSession.RequestLevel(*Level);
    return true;
}

const LabTravel::State& LabTravel::Current() { return GState; }

std::string LabTravel::GetValue(const char* InKey)
{
    if(!InKey) { return std::string(); }
    const std::string Flags = "," + GState.Flags + ",";
    const std::string Key = "," + std::string(InKey) + "=";
    const size_t At = Flags.find(Key);
    if(At == std::string::npos) { return std::string(); }
    const size_t From = At + Key.size();
    return Flags.substr(From, Flags.find(',', From) - From);
}

bool LabTravel::GetFlag(const char* InKey)
{
    const std::string Value = GetValue(InKey);
    return Value == "1" || Value == "true";
}

std::string LabTravel::Save()
{
    return GState.World + "\n" + GState.Entry + "\n" + GState.Flags;
}

void LabTravel::Restore(const std::string& InSaved)
{
    GState.World = Field(InSaved, 0);
    GState.Entry = Field(InSaved, 1);
    GState.Flags = Field(InSaved, 2);
    GState.LoadingTitle.clear();
    GState.bCardPending = false;
}

void LabTravel::ResetFor(const char* InWorld)
{
    GState = State();
    GState.World = InWorld ? InWorld : "";
}

bool LabTravel::ApplyEntry(World& InWorld)
{
    if(GState.Entry.empty()) { return false; }
    const EntityID Entry = InWorld.FindEntityByName(GState.Entry.c_str());
    if(!Entry.IsSet() || !InWorld.GetEntity(Entry).GetComponent<PlayerStartComponent>())
    {
        CORE_LOG_WARN("[DemoLab] travel: no Player Start named '" + GState.Entry + "'; using the level's first")
        return false;
    }
    InWorld.Each<PlayerStartComponent>([&](Entity InEntity, PlayerStartComponent& InStart)
    {
        InStart.bEnabled = InEntity.GetID() == Entry ? 1 : 0;
    });
    return true;
}

void LabTravel::TickLoadingCard(GameSession& InSession, const float InSeconds)
{
    UI::UISystem& UI = InSession.GetUI();
    if(GState.bCardPending)
    {
        GState.bCardPending = false;
        GCardSeconds = 2.0f;
        UI.SetValue("Loading.Title", UI::UIValue::String(GState.LoadingTitle));
        if(!UI.IsOpen(LOADING_DOCUMENT)) { UI.Open(LOADING_DOCUMENT); }
        return;
    }
    if(GCardSeconds <= 0.f) { return; }
    GCardSeconds -= InSeconds;
    UI.SetValue("Loading.Opacity", UI::UIValue::Number(GCardSeconds > 1.f ? 1.0 : double(GCardSeconds > 0.f ? GCardSeconds : 0.f)));
    if(GCardSeconds <= 0.f) { UI.Close(std::string(LOADING_DOCUMENT)); }
}
