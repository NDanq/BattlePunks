#ifndef DEMOLAB_LABHUD_H
#define DEMOLAB_LABHUD_H

#include <string>
#include <vector>

namespace NimbleEngine { class GameSession; }

/*
 * The HUD (Docs/Design.md, UI): Content/UI/LabHUD.nmui, fed through the UI
 * data store each frame for the local player's character. Keys:
 *
 *   Lab.Health, Lab.HealthFill     "75", 0..1
 *   Lab.Weapon, Lab.Ammo           "Pipe Pistol", "6 / 24"
 *   Lab.WeaponVis                  the weapon panel: only once armed (after C5)
 *   Lab.Prompt, Lab.PromptVis      "[E]  Turn the crank (hold)"
 *   Lab.HoldFill, Lab.HoldVis      a hold's progress bar under the prompt
 *   Lab.CrossVis, Lab.HitVis       crosshair while armed, hit marker
 *   Lab.HudVis                     the whole HUD; hidden during cutscenes
 *   Story.Objective                the current objective
 *   Story.Banner, Story.BannerVis  a centred message
 *   Story.Encounter                "Wave 2 of 3   4 left" while a fight is on
 *   Story.Timer                    "Beacon charging  0:42" (LabTimer)
 *   Story.IntelVis                 the intel marker, once the intel is held
 *   Feed.Line0..3                  the kill feed, newest at the bottom
 *
 * Visibility binds take EUIVisibility's index: 3 HitTestInvisible, 1 Hidden.
 */
namespace LabHUD {
    constexpr const char* DOCUMENT = "Content/UI/LabHUD.nmui";

    struct Story
    {
        std::string Objective;
        std::string Banner;
        bool        bCinematic = false;
        std::vector<std::string> Feed;    // newest last; the HUD shows four
        bool        bIntel = false;       // the intel marker
    };

    void Open(NimbleEngine::GameSession& InSession);
    void Close(NimbleEngine::GameSession& InSession);
    void Update(NimbleEngine::GameSession& InSession, const Story& InStory);
}

#endif //DEMOLAB_LABHUD_H
