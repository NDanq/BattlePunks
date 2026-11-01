#include "Flow/LabTravel.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/LevelTravel.h"
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
    if(!InWorld || !FindLevelByName(InWorld))
    {
        CORE_LOG_ERROR(std::string("[DemoLab] travel: no level '") + (InWorld ? InWorld : "") + "' ([World_*] in DefaultEngine.ini)")
        return false;
    }
    GState.World        = InWorld;
    GState.Entry        = InEntry ? InEntry : "";
    GState.Flags        = InFlags ? InFlags : "";
    GState.LoadingTitle = InLoadingTitle ? InLoadingTitle : "";
    CORE_LOG("[DemoLab] travel to " + GState.World + (GState.Entry.empty() ? std::string() : " at " + GState.Entry)
             + (GState.Flags.empty() ? std::string() : " (" + GState.Flags + ")"))

    /* The engine's level travel: the entry places the avatar, the values are
     * the new level's GetTravel(), and the transition shows the loading card
     * ([LevelTransition] in DefaultGame.ini, Content/UI/Loading.nmui). */
    LevelTravel Travel;
    Travel.Entry = GState.Entry;
    Travel.SetFromText(GState.Flags.c_str());
    Travel.Title = GState.LoadingTitle;
    Travel.Tip   = TipFor(GState.World);
    Travel.Transition = GState.LoadingTitle.empty() ? TT_Default : TT_Fade;
    InSession.SetPaused(false);
    return InSession.OpenLevel(InWorld, Travel);
}

void LabTravel::Arrive(GameSession& InSession, const char* InWorld)
{
    const LevelTravel& Came = InSession.GetTravel();
    if(!Came.Previous.empty() || !Came.Entry.empty() || !Came.Values.empty())
    {
        /* Came by travel (ours, or Lua's Game.OpenLevel): what it carried. */
        GState.World = InWorld ? InWorld : "";
        GState.Entry = Came.Entry;
        GState.Flags = Came.ToText();
        return;
    }
    if(GState.World != (InWorld ? InWorld : "")) { ResetFor(InWorld); }
}

const char* LabTravel::TipFor(const std::string& InWorld)
{
    if(InWorld == "L1_Foothills")  { return "Hold E on a crank to turn it. F1 shows the engine features on screen."; }
    if(InWorld == "L2_Laboratory") { return "The breaker is in the foyer. Watch the light come back room by room."; }
    if(InWorld == "L3_Core")       { return "The vault is at the bottom of the ring. Checkpoints save as you go."; }
    return "";
}

const LabTravel::State& LabTravel::Current() { return GState; }

std::string LabTravel::GetValue(const char* InKey)
{
    if(!InKey) { return std::string(); }
    std::string Flags = "," + GState.Flags + ",";
    for(char& C : Flags) { if(C == ';') { C = ','; } }   // the engine's ToText separates with ';'
    for(size_t At = Flags.find(" "); At != std::string::npos; At = Flags.find(" ")) { Flags.erase(At, 1); }
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
}

void LabTravel::ResetFor(const char* InWorld)
{
    GState = State();
    GState.World = InWorld ? InWorld : "";
}

