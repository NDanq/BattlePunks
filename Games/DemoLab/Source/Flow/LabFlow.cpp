#include "Flow/LabFlow.h"

#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/SaveGame.h"

#include "Flow/LabTravel.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

using namespace NimbleEngine;

namespace
{
    /* The design's flow table, one row per chapter. */
    const LabFlow::Chapter GChapters[LabFlow::CHAPTER_COUNT] = {
        { "1  Foothills Approach",       "L1_Foothills",  "Entry_Dock",        "" },
        { "2  The Laboratory",           "L2_Laboratory", "Entry_Airlock",     "" },
        { "3  The Core",                 "L3_Core",       "Entry_Elevator",    "" },
        { "4  The Laboratory: Alarm",    "L2_Laboratory", "Entry_FreightLift", "Alarm=1" },
    };
}

const LabFlow::Chapter& LabFlow::GetChapter(const int InIndex)
{
    return GChapters[std::clamp(InIndex, 0, CHAPTER_COUNT - 1)];
}

void LabFlow::StartChapter(GameSession& InSession, const int InIndex)
{
    const Chapter& C = GetChapter(InIndex);
    /* A chapter is a new run from there: an older run's checkpoint is not its own. */
    std::error_code Ignored;
    std::filesystem::remove(CHECKPOINT_PATH, Ignored);
    LabTravel::Travel(InSession, C.World, C.Entry, C.Flags, C.Title + 3);   // the title without its number
}

void LabFlow::NewGame(GameSession& InSession)
{
    StartChapter(InSession, 0);
}

bool LabFlow::HasCheckpoint()
{
    std::error_code Ignored;
    return std::filesystem::exists(CHECKPOINT_PATH, Ignored);
}

bool LabFlow::Continue(GameSession& InSession)
{
    if(!HasCheckpoint()) { return false; }
    InSession.SetPaused(false);
    if(!LoadGameFromFile(InSession, CHECKPOINT_PATH))
    {
        CORE_LOG_WARN("[DemoLab] the checkpoint would not load")
        return false;
    }
    return true;
}

void LabFlow::ToMainMenu(GameSession& InSession)
{
    LabTravel::Travel(InSession, MENU_WORLD, nullptr, nullptr, nullptr);
}
