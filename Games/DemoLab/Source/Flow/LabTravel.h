#ifndef DEMOLAB_LABTRAVEL_H
#define DEMOLAB_LABTRAVEL_H

#include <string>

namespace NimbleEngine { class GameSession; }
namespace GameCore { class World; }

/*
 * Level travel (Docs/Design.md, Flow). Every move between worlds goes through
 * LabTravel::Travel -- the menu's New Game and Chapter Select, the end of each
 * level, the extraction back to the menu -- so it can switch to the engine's
 * own level entry points, travel state and loading screen later without
 * touching any caller.
 *
 * Today it is built from what the engine has:
 *
 *   - the travel state lives in a game-side static (it survives the level
 *     swap, which rebuilds the game mode) and in the story's SaveGame state,
 *     so a checkpoint restores it too;
 *   - the entry is a Player Start found by entity name: the arriving mode
 *     disables every other start before the avatar spawns (ApplyEntry);
 *   - the loading card is a UI document the arriving mode shows for a moment
 *     (Content/UI/Loading.nmui), since a level swap closes every document.
 *
 * State is "Key=Value" pairs separated by commas: "Alarm=1". A key not given
 * reads as "" (false).
 */
namespace LabTravel {
    struct State
    {
        std::string World;          // the [World_*] name travelled to
        std::string Entry;          // a Player Start's entity name; "" the level's first
        std::string Flags;          // "Alarm=1,..."
        std::string LoadingTitle;   // the loading card's title; "" shows none
        bool        bCardPending = false;
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

    /* The arriving mode, in its OnLoad (before avatars spawn): only the
     * entry's Player Start stays enabled. False if the entry is not found
     * (then every start is left as it was). */
    bool ApplyEntry(GameCore::World& InWorld);

    /* The arriving mode, once started: opens the loading card for the title
     * (if one is pending) and closes it after InSeconds of play. Call every
     * frame with the frame's seconds. */
    void TickLoadingCard(NimbleEngine::GameSession& InSession, float InSeconds);

    constexpr const char* LOADING_DOCUMENT = "Content/UI/Loading.nmui";
}

#endif //DEMOLAB_LABTRAVEL_H
