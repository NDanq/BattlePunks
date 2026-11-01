#include "Public/Core/CoreLog.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/UI/UISystem.h"

#include "Flow/LabFlow.h"
#include "Flow/LabTravel.h"
#include "GameModes/LabAutoPlay.h"
#include "GameModes/LabMenuTest.h"
#include "UI/LabUI.h"

#include <string>

using namespace NimbleEngine;

bool LabIsAutoPlay();   // LabStoryMode.cpp

/*
 * The main menu (Content/Levels/MainMenu.nmworld): the lab's facade at dusk
 * behind Content/UI/MainMenu.nmui, the camera on a looping Sequencer dolly
 * (a placed Sequence Player with AutoPlay and Loop). No avatar: the menu
 * owns the mouse. Its buttons are the native actions in UI/LabUI.cpp.
 */
class LabMenuMode : public IGameMode
{
public:
    bool OnLoad(GameSession& InSession) override
    {
        LabTravel::ResetFor(LabFlow::MENU_WORLD);
        (void)InSession;
        return true;
    }

    void OnStart() override
    {
        CORE_LOG("[DemoLab] main menu")
        if(IsDedicatedServer()) { return; }
        GameSession& Session = GetSession();
        Session.GetUI().SetValue("Menu.CanContinue", UI::UIValue::Bool(LabFlow::HasCheckpoint()));
        Session.GetUI().SetValue("Menu.ContinueVis", UI::UIValue::Number(LabFlow::HasCheckpoint() ? 0.0 : 2.0));   // Visible / Collapsed
        for(int I = 0; I < LabFlow::CHAPTER_COUNT; ++I)
        {
            Session.GetUI().SetValue("Chapter.Title" + std::to_string(I), UI::UIValue::String(LabFlow::GetChapter(I).Title));
        }
        Session.GetUI().Open(LabUI::MAIN_MENU);
        Session.SetMouseCaptured(false);
        GetLabMenuTest().OnMenuStarted(Session);
    }

    void OnTick(const FrameContext& InFrame) override
    {
        if(IsDedicatedServer()) { return; }
        const float Dt = float(InFrame.DeltaSeconds);
        LabUI::UpdateTechOverlay(GetSession(), LabFlow::MENU_WORLD, false);
        GetLabMenuTest().TickMenu(GetSession(), Dt);
        if(LabIsAutoPlay())
        {
            LabAutoPlay& Auto = GetLabAutoPlay();
            Auto.TickClock(Dt);
            Auto.TickMenu(GetSession(), Dt);
            if(!Auto.GetResult().empty())
            {
                QuitIn -= Dt;
                if(QuitIn <= 0.f && QuitIn > -1000.f) { QuitIn = -2000.f; GetSession().RequestQuit(); }
            }
        }
    }

    void OnUnload() override
    {
        if(!HasSession() || IsDedicatedServer()) { return; }
        UI::UISystem& UI = GetSession().GetUI();
        for(const char* Doc : { LabUI::MAIN_MENU, LabUI::CHAPTER_SELECT, LabUI::SETTINGS, LabUI::CREDITS })
        {
            UI.Close(std::string(Doc));
        }
    }

private:
    float QuitIn = 2.f;
};

NIMBLE_REGISTER_GAME_MODE(LabMenuMode, "LabMenu")
