#include "GameModes/LabMenuTest.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/DetailMode.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Scalability.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidgets.h"

#include "Flow/LabFlow.h"
#include "Flow/LabTravel.h"
#include "GameModes/LabInputDriver.h"
#include "UI/LabUI.h"
#include "Weapons/LabCombat.h"
#include "Weapons/LabWeapon.h"
#include "World/LabSignals.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

using namespace GameCore;
using namespace NimbleEngine;

/* The test hook (GameModes/LabMenuTest.h): 1 the menus, 2 the settings read back. */
NIMBLE_GAME_CONFIG_INT(GLabMenuTest, "DemoLab", "MenuTest", 0, nullptr);

LabMenuTest& GetLabMenuTest()
{
    static LabMenuTest Driver;
    return Driver;
}

namespace
{
    constexpr const char* TEST_USER_FILE = "Saved/DemoLabTest/User.ini";
    constexpr float TIMEOUT_SECONDS = 600.f;

    /* What the settings step sets, and the persistence step expects back. */
    constexpr double SET_MASTER  = 0.35;
    constexpr double SET_LOOK    = 1.7;
    constexpr int    SET_TIER    = 1;      // Medium
    constexpr int    SET_MODE    = 1;      // borderless
    constexpr int    SET_RES     = 1;      // 1920 x 1080
    constexpr int    SET_WIDTH   = 1920;

    // Steps.
    enum : int
    {
        P_SettingsOpen = 0, P_SettingsChange, P_SettingsVerify, P_SettingsSaved,
        P_ChapterOpen, P_ChapterPick, P_ChapterWait,
        P_LevelSettle, P_Overlay, P_OverlayLive, P_OverlayClose,
        P_PauseResume, P_PauseResumed, P_PauseRestart, P_PauseRestored, P_PauseMain,
        P_Leave, P_Done,
        P_PersistCheck = 100, P_PersistPage
    };

    const EngineCore::ConfigVar* Var(const char* InPath) { return EngineCore::ConfigFindVarByPath(InPath); }

    double Number(const char* InPath)
    {
        const EngineCore::ConfigVar* V = Var(InPath);
        if(!V) { return -999.0; }
        switch(V->GetType())
        {
        case EngineCore::EConfigType::CT_Bool:  return V->GetBool() ? 1.0 : 0.0;
        case EngineCore::EConfigType::CT_Int:   return double(V->GetInt());
        case EngineCore::EConfigType::CT_Float: return double(V->GetFloat());
        default: return std::atof(V->GetString());
        }
    }

    bool Near(const double InA, const double InB) { return std::fabs(InA - InB) < 1e-3; }

    std::string ReadFile(const char* InPath)
    {
        std::ifstream In(InPath);
        std::stringstream Text;
        Text << In.rdbuf();
        return Text.str();
    }

    /* "Section.Key" in an .ini's text; "" if it has none. */
    std::string IniValue(const std::string& InText, const std::string& InPath)
    {
        const size_t Dot = InPath.find('.');
        const std::string Section = "[" + InPath.substr(0, Dot) + "]";
        const std::string Key = InPath.substr(Dot + 1);
        std::istringstream Lines(InText);
        std::string Line, In;
        while(std::getline(Lines, Line))
        {
            while(!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) { Line.pop_back(); }
            if(!Line.empty() && Line[0] == '[') { In = Line; continue; }
            if(In != Section) { continue; }
            const size_t Eq = Line.find('=');
            if(Eq == std::string::npos) { continue; }
            std::string Name = Line.substr(0, Eq);
            while(!Name.empty() && Name.back() == ' ') { Name.pop_back(); }
            if(Name != Key) { continue; }
            std::string Value = Line.substr(Eq + 1);
            while(!Value.empty() && Value.front() == ' ') { Value.erase(Value.begin()); }
            return Value;
        }
        return std::string();
    }

    double IniNumber(const std::string& InValue)
    {
        if(InValue == "true") { return 1.0; }
        if(InValue == "false") { return 0.0; }
        return std::atof(InValue.c_str());
    }

    UI::UIDocumentInstance* Doc(GameSession& InSession, const char* InPath) { return InSession.GetUI().Find(std::string(InPath)); }

    /* A button in an open document, clicked: its OnClick queued as a click queues it. */
    bool Click(GameSession& InSession, const char* InDoc, const char* InWidget)
    {
        UI::UIDocumentInstance* D = Doc(InSession, InDoc);
        UI::UIWidget* W = D ? D->FindWidget(InWidget) : nullptr;
        UI::UIButton* Button = dynamic_cast<UI::UIButton*>(W);
        if(!Button) { return false; }
        InSession.GetUI().QueueActions(Button->OnClick, D, W);
        return true;
    }

    /* A slider moved to InValue (snapped, as a drag snaps it); its OnChanged queued. */
    bool Slide(GameSession& InSession, const char* InDoc, const char* InWidget, const double InValue)
    {
        UI::UIDocumentInstance* D = Doc(InSession, InDoc);
        UI::UIWidget* W = D ? D->FindWidget(InWidget) : nullptr;
        UI::UISlider* Slider = dynamic_cast<UI::UISlider*>(W);
        if(!Slider) { return false; }
        Slider->Value = Slider->Snap(InValue);
        InSession.GetUI().QueueActions(Slider->OnChanged, D, W);
        return true;
    }

    /* A check box set; its OnChanged queued. */
    bool Check(GameSession& InSession, const char* InDoc, const char* InWidget, const bool bInChecked)
    {
        UI::UIDocumentInstance* D = Doc(InSession, InDoc);
        UI::UIWidget* W = D ? D->FindWidget(InWidget) : nullptr;
        UI::UICheckBox* Box = dynamic_cast<UI::UICheckBox*>(W);
        if(!Box) { return false; }
        Box->bChecked = bInChecked;
        InSession.GetUI().QueueActions(Box->OnChanged, D, W);
        return true;
    }

    std::string StoreText(GameSession& InSession, const char* InKey)
    {
        const UI::UIValue* Value = InSession.GetUI().GetValue(InKey);
        return Value ? Value->AsString() : std::string();
    }

    /* The numbers in a line of text, in order. */
    std::vector<double> NumbersIn(const std::string& InText)
    {
        std::vector<double> Out;
        const char* P = InText.c_str();
        while(*P)
        {
            if(std::isdigit(static_cast<unsigned char>(*P)))
            {
                char* End = nullptr;
                Out.push_back(std::strtod(P, &End));
                P = End;
            }
            else { ++P; }
        }
        return Out;
    }

    /* The user file the test writes: beside the executable, not the player's. */
    void UseTestUserFile()
    {
        std::error_code Ignored;
        std::filesystem::create_directories(std::filesystem::path(TEST_USER_FILE).parent_path(), Ignored);
        EngineCore::ConfigSetUserFile(TEST_USER_FILE);
    }

    bool SettingsDefaultShadows = true;
}

bool LabMenuTest::IsActive() const { return GLabMenuTest.GetInt() != 0; }

void LabMenuTest::Log(const std::string& InLine) const
{
    char Line[64];
    std::snprintf(Line, sizeof(Line), "[DemoLab] MenuTest %.1fs ", double(Elapsed));
    CORE_LOG(Line + InLine)
}

void LabMenuTest::Fail(const std::string& InWhy)
{
    if(!Result.empty()) { return; }
    Result = "FAIL: " + InWhy;
    CORE_LOG_ERROR("[DemoLab] MenuTest " + Result)
}

void LabMenuTest::Pass()
{
    if(!Result.empty()) { return; }
    Result = "PASS";
    char Line[96];
    std::snprintf(Line, sizeof(Line), "[DemoLab] MenuTest PASS in %.1fs", double(Elapsed));
    CORE_LOG(Line)
}

void LabMenuTest::TickClock(GameSession& InSession, const float InSeconds)
{
    Elapsed += InSeconds;
    Clock += InSeconds;
    if(Result.empty() && Elapsed > TIMEOUT_SECONDS) { Fail("timed out at step " + std::to_string(Phase)); }
    if(!Result.empty())
    {
        QuitIn -= InSeconds;
        if(QuitIn <= 0.f && QuitIn > -1000.f) { QuitIn = -2000.f; InSession.RequestQuit(); }
    }
}

// === The menu ===============================================================

void LabMenuTest::OnMenuStarted(GameSession& InSession)
{
    (void)InSession;
    if(!IsActive() || !Result.empty()) { return; }
    Clock = 0.f;
    if(GLabMenuTest.GetInt() == 2)
    {
        Phase = P_PersistCheck;
        return;
    }
    if(Elapsed <= 0.f)
    {
        /* A fresh test file: what this run saves is all it holds. */
        UseTestUserFile();
        std::error_code Ignored;
        std::filesystem::remove(TEST_USER_FILE, Ignored);
        Phase = P_SettingsOpen;
        Log("settings");
        return;
    }
    if(Phase == P_Leave || Phase == P_PauseMain)
    {
        Log("back at the main menu after chapter " + std::to_string(Chapter + 1));
        ++Chapter;
        if(Chapter >= LabFlow::CHAPTER_COUNT) { Phase = P_Done; Pass(); return; }
        Phase = P_ChapterOpen;
        return;
    }
    Fail("the main menu came back at step " + std::to_string(Phase));
}

void LabMenuTest::TickMenu(GameSession& InSession, const float InSeconds)
{
    if(!IsActive()) { return; }
    TickClock(InSession, InSeconds);
    if(!Result.empty()) { return; }
    if(GLabMenuTest.GetInt() == 2) { TickPersist(InSession); return; }

    switch(Phase)
    {
    case P_SettingsOpen: case P_SettingsChange: case P_SettingsVerify: case P_SettingsSaved:
        TickSettings(InSession);
        break;

    case P_ChapterOpen:
        if(Clock < 1.f) { break; }
        if(!Click(InSession, LabUI::MAIN_MENU, "ChapterSelect")) { Fail("no Chapter Select button on the main menu"); break; }
        Phase = P_ChapterPick;
        Clock = 0.f;
        break;

    case P_ChapterPick:
    {
        if(!InSession.GetUI().IsOpen(LabUI::CHAPTER_SELECT))
        {
            if(Clock > 2.f) { Fail("Chapter Select did not open"); }
            break;
        }
        const std::string Title = StoreText(InSession, ("Chapter.Title" + std::to_string(Chapter)).c_str());
        if(Title != LabFlow::GetChapter(Chapter).Title) { Fail("Chapter Select row " + std::to_string(Chapter + 1) + " reads '" + Title + "'"); break; }
        if(!Click(InSession, LabUI::CHAPTER_SELECT, ("Chapter" + std::to_string(Chapter)).c_str()))
        {
            Fail("no row " + std::to_string(Chapter + 1) + " on Chapter Select");
            break;
        }
        Log("chapter " + std::to_string(Chapter + 1) + ": " + Title);
        Phase = P_ChapterWait;
        Clock = 0.f;
        bLevelStarted = false;
        break;
    }

    case P_ChapterWait:
        if(Clock > 90.f) { Fail("chapter " + std::to_string(Chapter + 1) + " never started"); }
        break;

    default:
        break;
    }
}

void LabMenuTest::TickSettings(GameSession& InSession)
{
    UI::UISystem& UI = InSession.GetUI();
    switch(Phase)
    {
    case P_SettingsOpen:
        if(Clock < 1.f) { break; }
        SettingsDefaultShadows = Number("Shadows.Enabled") > 0.5;
        if(!Click(InSession, LabUI::MAIN_MENU, "SettingsButton")) { Fail("no Settings button on the main menu"); break; }
        Phase = P_SettingsChange;
        Clock = 0.f;
        break;

    case P_SettingsChange:
    {
        if(!UI.IsOpen(LabUI::SETTINGS))
        {
            if(Clock > 2.f) { Fail("Settings did not open"); }
            break;
        }
        /* One of each kind of control, on every page. */
        const bool bAll = Check(InSession, LabUI::SETTINGS, "Shadows", !SettingsDefaultShadows)
                       && Slide(InSession, LabUI::SETTINGS, "Tier", double(SET_TIER))
                       && Slide(InSession, LabUI::SETTINGS, "DisplayMode", double(SET_MODE))
                       && Slide(InSession, LabUI::SETTINGS, "Resolution", double(SET_RES))
                       && Slide(InSession, LabUI::SETTINGS, "Master", SET_MASTER)
                       && Slide(InSession, LabUI::SETTINGS, "Look", SET_LOOK)
                       && Check(InSession, LabUI::SETTINGS, "Subtitles", false);
        if(!bAll) { Fail("a Settings control is missing"); break; }
        Phase = P_SettingsVerify;
        Clock = 0.f;
        break;
    }

    case P_SettingsVerify:
    {
        if(Clock < 0.3f) { break; }
        /* Applied: the config vars, the tier, what the game reads. */
        if((Number("Shadows.Enabled") > 0.5) == SettingsDefaultShadows) { Fail("Shadows did not change"); break; }
        if(int(GameCore::Scalability::GetDetailMode()) != SET_TIER) { Fail("the quality tier did not change"); break; }
        if(int(Number("Display.WindowMode")) != SET_MODE) { Fail("the display mode did not change"); break; }
        if(int(Number("Display.ResolutionX")) != SET_WIDTH) { Fail("the resolution did not change"); break; }
        if(!Near(Number("Audio.MasterVolume"), SET_MASTER)) { Fail("the master volume did not change"); break; }
        if(!Near(double(LabUI::LookScale()), SET_LOOK)) { Fail("the look sensitivity did not reach the game"); break; }
        if(Number("DemoLab.Subtitles") > 0.5) { Fail("subtitles did not turn off"); break; }
        if(StoreText(InSession, "Set.DisplayModeName") != "Borderless") { Fail("the page shows display mode '" + StoreText(InSession, "Set.DisplayModeName") + "'"); break; }
        if(StoreText(InSession, "Set.TierName") != "Medium") { Fail("the page shows tier '" + StoreText(InSession, "Set.TierName") + "'"); break; }
        Log("settings apply");
        if(!Click(InSession, LabUI::SETTINGS, "Back")) { Fail("no Back on Settings"); break; }
        Phase = P_SettingsSaved;
        Clock = 0.f;
        break;
    }

    case P_SettingsSaved:
    {
        if(Clock < 0.3f) { break; }
        if(UI.IsOpen(LabUI::SETTINGS)) { Fail("Back did not close Settings"); break; }
        /* Closing saved them: the user file has every one. */
        const std::string Text = ReadFile(TEST_USER_FILE);
        if(Text.empty()) { Fail(std::string("closing Settings wrote no ") + TEST_USER_FILE); break; }
        if(IniNumber(IniValue(Text, "Shadows.Enabled")) != (SettingsDefaultShadows ? 0.0 : 1.0)
           || IniValue(Text, "Shadows.Enabled").empty())             { Fail("Shadows not saved"); break; }
        if(int(IniNumber(IniValue(Text, "Display.WindowMode"))) != SET_MODE) { Fail("the display mode not saved"); break; }
        if(int(IniNumber(IniValue(Text, "Display.ResolutionX"))) != SET_WIDTH) { Fail("the resolution not saved"); break; }
        if(!Near(IniNumber(IniValue(Text, "Audio.MasterVolume")), SET_MASTER)) { Fail("the master volume not saved"); break; }
        if(!Near(IniNumber(IniValue(Text, "DemoLab.LookSensitivity")), SET_LOOK)) { Fail("the look sensitivity not saved"); break; }
        if(IniValue(Text, "DemoLab.Subtitles") != "false")           { Fail("subtitles not saved"); break; }
        Log("settings saved to " + std::string(TEST_USER_FILE));
        Phase = P_ChapterOpen;
        Clock = 0.f;
        Chapter = 0;
        break;
    }

    default:
        break;
    }
}

void LabMenuTest::TickPersist(GameSession& InSession)
{
    switch(Phase)
    {
    case P_PersistCheck:
    {
        if(Clock < 1.f) { break; }
        /* As the launcher loads the player's file at start: over the project's values. */
        const std::string Text = ReadFile(TEST_USER_FILE);
        if(Text.empty()) { Fail(std::string("no ") + TEST_USER_FILE + " (run DemoLab.MenuTest first)"); break; }
        UseTestUserFile();
        if(!EngineCore::LoadConfigFile(TEST_USER_FILE, nullptr, EngineCore::EConfigLayer::User)) { Fail("the user file would not load"); break; }
        if(int(Number("Display.WindowMode")) != SET_MODE)     { Fail("the display mode did not persist"); break; }
        if(int(Number("Display.ResolutionX")) != SET_WIDTH)   { Fail("the resolution did not persist"); break; }
        if(!Near(Number("Audio.MasterVolume"), SET_MASTER))   { Fail("the master volume did not persist"); break; }
        if(!Near(double(LabUI::LookScale()), SET_LOOK))       { Fail("the look sensitivity did not persist"); break; }
        if(Number("DemoLab.Subtitles") > 0.5)                 { Fail("subtitles did not persist"); break; }
        if(Near(Number("Shadows.Enabled"), IniNumber(IniValue(Text, "Shadows.Enabled"))) == false) { Fail("Shadows did not persist"); break; }
        if(!Click(InSession, LabUI::MAIN_MENU, "SettingsButton")) { Fail("no Settings button on the main menu"); break; }
        Phase = P_PersistPage;
        Clock = 0.f;
        break;
    }

    case P_PersistPage:
    {
        if(Clock < 0.5f) { break; }
        if(!InSession.GetUI().IsOpen(LabUI::SETTINGS)) { Fail("Settings did not open"); break; }
        if(StoreText(InSession, "Set.DisplayModeName") != "Borderless") { Fail("the page shows display mode '" + StoreText(InSession, "Set.DisplayModeName") + "'"); break; }
        const UI::UIValue* Master = InSession.GetUI().GetValue("Set.Audio.MasterVolume");
        if(!Master || !Near(Master->AsNumber(), SET_MASTER)) { Fail("the page shows another master volume"); break; }
        Log("the settings came back in a new process");
        /* Nowhere to save from here on (the page saves as it closes, at exit), and the file gone. */
        EngineCore::ConfigSetUserFile(std::string());
        std::error_code Ignored;
        std::filesystem::remove(TEST_USER_FILE, Ignored);
        Phase = P_Done;
        Pass();
        break;
    }

    default:
        break;
    }
}

// === The levels =============================================================

bool LabMenuTest::CheckChapter(GameSession& InSession, const std::string& InLevel, const bool bInAlarm)
{
    const LabFlow::Chapter& C = LabFlow::GetChapter(Chapter);
    const std::string Flags = C.Flags;
    const bool bWantAlarm = Flags.find("Alarm=1") != std::string::npos;
    const bool bWantArmed = Flags.find("Armed=1") != std::string::npos;
    const std::string Row = "chapter " + std::to_string(Chapter + 1) + ": ";
    if(InLevel != C.World) { Fail(Row + "started " + InLevel + ", not " + C.World); return false; }
    if(bInAlarm != bWantAlarm) { Fail(Row + (bInAlarm ? "on alarm" : "calm") + ", not as the row says"); return false; }
    if(LabTravel::Current().Entry != C.Entry) { Fail(Row + "arrived at '" + LabTravel::Current().Entry + "'"); return false; }

    World& W = InSession.GetWorld();
    if(bWantAlarm && !LabSignals::Get(W, "State.Alarm")) { Fail(Row + "the Alarm state's signal is off"); return false; }
    const EntityID Avatar = GetLocalAvatar(W, 0);
    if(!Avatar.IsSet()) { Fail(Row + "no player character"); return false; }
    const EntityID Entry = W.FindEntityByName(C.Entry);
    if(!Entry.IsSet()) { Fail(Row + "no entry " + C.Entry); return false; }
    const EngineCore::Vector3 At = W.GetEntity(Avatar).GetWorldPose().Position;
    const EngineCore::Vector3 Start = W.GetEntity(Entry).GetWorldPose().Position;
    const double Off = EngineCore::Vector3(At.GetX() - Start.GetX(), 0, At.GetZ() - Start.GetZ()).GetLength();
    if(Off > 400.0) { Fail(Row + "the player stands " + std::to_string(int(Off)) + " cm from " + C.Entry); return false; }

    const LabInventoryComponent* Inventory = W.GetEntity(Avatar).GetComponent<LabInventoryComponent>();
    bool bPistol = false;
    for(int Slot = 0; Inventory && Slot < LabWeapons::SLOTS; ++Slot) { bPistol |= Inventory->WeaponAt(Slot) == LAB_WEAPON_PIPE_PISTOL; }
    if(bPistol != bWantArmed) { Fail(Row + (bPistol ? "armed, but should not be" : "unarmed, but the alarm chapter needs the sidearm")); return false; }
    Log(Row + InLevel + (bInAlarm ? " (Alarm)" : "") + " at " + C.Entry + (bPistol ? ", armed" : ""));
    return true;
}

void LabMenuTest::OnLevelStarted(GameSession& InSession, const std::string& InLevel, const bool bInAlarm, const bool bInRestored)
{
    if(!IsActive() || !Result.empty()) { return; }
    if(bInRestored)
    {
        if(Phase == P_PauseRestored) { bRestoredSeen = true; Clock = 0.f; return; }
        Fail("a checkpoint loaded at step " + std::to_string(Phase));
        return;
    }
    if(Phase != P_ChapterWait) { Fail(InLevel + " started at step " + std::to_string(Phase)); return; }
    if(!CheckChapter(InSession, InLevel, bInAlarm)) { return; }
    bLevelStarted = true;
    Phase = P_LevelSettle;
    Clock = 0.f;
}

void LabMenuTest::Drive(InputSnapshot& InOut)
{
    InOut = InputSnapshot();
    if(bPressOverlay) { bPressOverlay = false; LabDrive::Press(InOut, "TechOverlay"); }
    if(bPressPause)   { bPressPause = false;   LabDrive::Press(InOut, "Pause"); }
}

void LabMenuTest::TickStory(GameSession& InSession, const std::string& InLevel, const bool bInCutscene, const float InSeconds)
{
    if(!IsActive()) { return; }
    TickClock(InSession, InSeconds);
    if(!Result.empty()) { return; }
    TickLevel(InSession, InLevel, bInCutscene, InSeconds);
}

void LabMenuTest::TickLevel(GameSession& InSession, const std::string& InLevel, const bool bInCutscene, const float InSeconds)
{
    (void)InSeconds;
    UI::UISystem& UI = InSession.GetUI();
    switch(Phase)
    {
    case P_LevelSettle:
    {
        /* Each row writes its checkpoint as it starts (Chapter 1's after C1). */
        if(Chapter == 0)
        {
            if(Clock > 1.f) { Phase = P_Leave; Clock = 0.f; LabFlow::ToMainMenu(InSession); }
            break;
        }
        if(bInCutscene || !LabFlow::HasCheckpoint())
        {
            if(Clock > 10.f) { Fail("chapter " + std::to_string(Chapter + 1) + " wrote no checkpoint"); }
            break;
        }
        if(Clock < 1.f) { break; }
        if(Chapter == 1) { Phase = P_Overlay; bPressOverlay = true; Clock = 0.f; Log("F1"); }
        else if(Chapter == 2) { Phase = P_PauseResume; bPressPause = true; Clock = 0.f; Log("pause"); }
        else { Phase = P_Leave; Clock = 0.f; LabFlow::ToMainMenu(InSession); }
        break;
    }

    case P_Overlay:
    {
        if(!UI.IsOpen(LabUI::TECH_OVERLAY))
        {
            if(Clock > 1.f) { Fail("F1 did not open the tech overlay"); }
            break;
        }
        if(Clock < 0.3f) { break; }
        if(StoreText(InSession, "Tech.Level").find(InLevel) == std::string::npos) { Fail("the overlay names '" + StoreText(InSession, "Tech.Level") + "'"); break; }
        if(StoreText(InSession, "Tech.Features").empty()) { Fail("the overlay lists no features"); break; }
        Stats = StoreText(InSession, "Tech.Stats");
        if(Stats.find("Frame") == std::string::npos || NumbersIn(Stats).size() < 6) { Fail("the overlay's stat unit reads '" + Stats + "'"); break; }
        Phase = P_OverlayLive;
        Clock = 0.f;
        break;
    }

    case P_OverlayLive:
    {
        if(Clock < 1.0f) { break; }
        const std::string Now = StoreText(InSession, "Tech.Stats");
        if(Now == Stats) { Fail("the overlay's numbers are not live: '" + Now + "'"); break; }
        Log("F1 shows: " + Now.substr(0, Now.find('\n')));
        bPressOverlay = true;
        Phase = P_OverlayClose;
        Clock = 0.f;
        break;
    }

    case P_OverlayClose:
        if(Clock < 0.3f) { break; }
        if(UI.IsOpen(LabUI::TECH_OVERLAY)) { Fail("F1 did not close the tech overlay"); break; }
        Log("F1 toggles");
        Phase = P_Leave;
        Clock = 0.f;
        LabFlow::ToMainMenu(InSession);
        break;

    case P_PauseResume:
        /* Escape was pressed: this tick (after the mode's own) finds the menu up. */
        if(!UI.IsOpen(LabUI::PAUSE_MENU))
        {
            if(Clock > 1.f) { Fail("Escape did not open the pause menu"); }
            break;
        }
        if(!InSession.IsPaused()) { Fail("the pause menu did not pause the game"); break; }
        if(!Click(InSession, LabUI::PAUSE_MENU, "Resume")) { Fail("no Resume on the pause menu"); break; }
        Phase = P_PauseResumed;
        Clock = 0.f;
        break;

    case P_PauseResumed:
        /* Ticking again: the game is running. */
        if(UI.IsOpen(LabUI::PAUSE_MENU)) { Fail("Resume left the pause menu open"); break; }
        Log("pause: Resume");
        Phase = P_PauseRestart;
        Clock = 0.f;
        bPressPause = true;
        break;

    case P_PauseRestart:
    {
        if(!UI.IsOpen(LabUI::PAUSE_MENU))
        {
            if(Clock > 1.f) { Fail("Escape did not open the pause menu again"); }
            break;
        }
        const UI::UIValue* Can = UI.GetValue("Pause.CanRestart");
        if(!Can || !Can->AsBool()) { Fail("Restart checkpoint is disabled with a checkpoint saved"); break; }
        if(!Click(InSession, LabUI::PAUSE_MENU, "Restart")) { Fail("no Restart checkpoint on the pause menu"); break; }
        Phase = P_PauseRestored;
        bRestoredSeen = false;
        Clock = 0.f;
        break;
    }

    case P_PauseRestored:
        if(!bRestoredSeen)
        {
            if(Clock > 30.f) { Fail("Restart checkpoint did not reload the checkpoint"); }
            break;
        }
        if(InSession.IsPaused() || UI.IsOpen(LabUI::PAUSE_MENU)) { Fail("the checkpoint came back paused"); break; }
        if(Clock < 1.f) { break; }
        Log("pause: Restart checkpoint");
        Phase = P_PauseMain;
        Clock = 0.f;
        bPressPause = true;
        break;

    case P_PauseMain:
        if(!UI.IsOpen(LabUI::PAUSE_MENU))
        {
            if(Clock > 1.f) { Fail("Escape did not open the pause menu a third time"); }
            break;
        }
        if(Clock > 0.5f) { break; }   // clicked; the menu loads
        if(!Click(InSession, LabUI::PAUSE_MENU, "MainMenu")) { Fail("no Main menu on the pause menu"); break; }
        Log("pause: Main menu");
        Clock = 1.f;   // clicked once
        break;

    case P_Leave:
        if(Clock > 90.f) { Fail("the main menu never came back"); }
        break;

    default:
        break;
    }
}
