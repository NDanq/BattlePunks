#ifndef DEMOLAB_LABTRAVEL_H
#define DEMOLAB_LABTRAVEL_H

#include <string>

namespace NimbleEngine { class GameSession; }
namespace GameCore { class World; }

/*
 * Level travel (Docs/Design.md, Flow). Every move between worlds goes through
 * LabTravel::Travel -- the menu's New Game and Chapter Select, the end of each
 * level, the extraction back to the menu.
 *
 * It is the engine's level travel (NimbleEngine/LevelTravel.h,
 * GameSession::OpenLevel): the entry is a Player Start's name (the engine
 * places the avatar there), the state rides as the travel's values, and the
 * transition's loading card shows the title ([LevelTransition] in
 * Config/DefaultGame.ini, Content/UI/Loading.nmui).
 *
 * The engine's travel state is not saved, so the game keeps its own copy:
 * the story's SaveGame state holds Save()'s string, and a checkpoint load
 * puts it back (Restore). State is "Key=Value" pairs: "Alarm=1,Armed=1".
 */
namespace LabTravel {
    struct State
    {
        std::string World;          // the [World_*] name travelled to
        std::string Entry;          // a Player Start's entity name; "" the level's first
        std::string Flags;          // "Alarm=1,..."
        std::string LoadingTitle;   // the loading card's title; "" a plain cut
    };

    /* THE travel function. InWorld is a [World_*] name ("L2_Laboratory");
     * InEntry a Player Start's name in it ("Entry_FreightLift", or nullptr);
     * InFlags the travel state ("Alarm=1", or nullptr); InLoadingTitle the
     * card's title. False (logged) if the world is not a registered level. */
    bool Travel(NimbleEngine::GameSession& InSession, const char* InWorld, const char* InEntry,
                const char* InFlags, const char* InLoadingTitle);

    /* What the last Travel asked for (or what a checkpoint restored). */
    const State& Current();
    /* A flag of the current state: "1"/"true" is true. */
    bool        GetFlag(const char* InKey);
    std::string GetValue(const char* InKey);

    /* For a checkpoint: the state as one string, and back. */
    std::string Save();
    void        Restore(const std::string& InSaved);
    /* A run that did not come from Travel (the -Game.StartLevel a test or the
     * editor's Play starts in): the state for that world, no entry. */
    void        ResetFor(const char* InWorld);

    /* The arriving mode, in its OnLoad: takes the state the engine's travel
     * carried in (Lua's Game.OpenLevel too); a level loaded without travel
     * (a test's -Game.StartLevel, the editor's Play) gets a fresh one. */
    void Arrive(NimbleEngine::GameSession& InSession, const char* InWorld);

    /* The loading card's tip for a world. */
    const char* TipFor(const std::string& InWorld);
}

#endif //DEMOLAB_LABTRAVEL_H
