#ifndef DEMOLAB_LABFLOW_H
#define DEMOLAB_LABFLOW_H

namespace NimbleEngine { class GameSession; }

/*
 * The game's flow (Docs/Design.md, Flow): the four chapters Chapter Select
 * starts, New Game, Continue and the way back to the menu. Every one ends in
 * LabTravel::Travel, or in a checkpoint load.
 */
namespace LabFlow {
    /* Where the story's checkpoint goes (beside the executable). */
    constexpr const char* CHECKPOINT_PATH = "Saved/DemoLab/Checkpoint.nmsave";
    constexpr const char* MENU_WORLD      = "MainMenu";

    struct Chapter
    {
        const char* Title;       // Chapter Select's row and the loading card
        const char* World;
        const char* Entry;       // a Player Start's name
        const char* Flags;       // the travel state
    };
    constexpr int CHAPTER_COUNT = 4;
    const Chapter& GetChapter(int InIndex);   // 0..3, clamped

    void NewGame(NimbleEngine::GameSession& InSession);
    void StartChapter(NimbleEngine::GameSession& InSession, int InIndex);
    /* Loads the checkpoint; false if there is none. */
    bool Continue(NimbleEngine::GameSession& InSession);
    bool HasCheckpoint();
    void ToMainMenu(NimbleEngine::GameSession& InSession);
}

#endif //DEMOLAB_LABFLOW_H
