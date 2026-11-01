#include "UI/LabUI.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/DetailMode.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Scalability.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/UI/UIAction.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"

#include "Flow/LabFlow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace NimbleEngine;

/* Controls: look sensitivity (multiplies the look axes) and inverted look. */
NIMBLE_GAME_CONFIG_FLOAT(GLabLookSensitivity, "DemoLab", "LookSensitivity", 1.0f, nullptr);
NIMBLE_GAME_CONFIG_BOOL(GLabInvertY, "DemoLab", "InvertY", false, nullptr);
/* Accessibility: the cinematics' subtitle lines. */
NIMBLE_GAME_CONFIG_BOOL(GLabSubtitles, "DemoLab", "Subtitles", true, nullptr);
/* Graphics: foliage density for L1's spawners (read by the art pass's
 * Procedural Spawners; the grey-box levels have no foliage yet). */
NIMBLE_GAME_CONFIG_FLOAT(GLabFoliageDensity, "DemoLab", "FoliageDensity", 1.0f, nullptr);

namespace
{
    constexpr double VIS_SHOWN  = 3.0;   // EUIVisibility::HitTestInvisible
    constexpr double VIS_HIDDEN = 1.0;   // EUIVisibility::Hidden

    /* Every var the Settings page shows; a '|' group is set together and read
     * from its first. */
    const char* const SETTING_VARS[] = {
        "Rendering.DynamicGI|Rendering.ScreenSpaceGI",
        "Shadows.Enabled",
        "Rendering.AllowScreenSpaceReflections|Rendering.StochasticReflections",
        "Rendering.AllowVolumetricFog",
        "DemoLab.FoliageDensity",
        "Display.VSync",
        "Display.FrameRateLimit",
        "Audio.MasterVolume",
        "Audio.MusicVolume",
        "Audio.SFXVolume",
        "Audio.VoiceVolume",
        "Audio.AmbientVolume",
        "Audio.UIVolume",
        "DemoLab.LookSensitivity",
        "DemoLab.InvertY",
        "DemoLab.Subtitles",
    };

    struct Resolution { int X, Y; const char* Name; };
    const Resolution RESOLUTIONS[] = {
        { 1280, 720, "1280 x 720" }, { 1920, 1080, "1920 x 1080" }, { 2560, 1440, "2560 x 1440" }, { 3840, 2160, "3840 x 2160" },
    };
    const char* const TIERS[] = { "Low", "Medium", "High", "Ultra" };

    std::vector<std::string> SplitPaths(const std::string& InPaths)
    {
        std::vector<std::string> Out(1);
        for(const char C : InPaths) { if(C == '|') { Out.emplace_back(); } else { Out.back() += C; } }
        return Out;
    }

    std::string FirstPath(const std::string& InPaths) { return SplitPaths(InPaths).front(); }

    void SetVar(EngineCore::ConfigVar& InVar, const double InValue)
    {
        switch(InVar.GetType())
        {
        case EngineCore::EConfigType::CT_Bool:  InVar.SetBool(InValue != 0.0); break;
        case EngineCore::EConfigType::CT_Int:   InVar.SetInt(int32_t(InValue + (InValue < 0.0 ? -0.5 : 0.5))); break;
        case EngineCore::EConfigType::CT_Float: InVar.SetFloat(float(InValue)); break;
        default: InVar.SetFromString(std::to_string(InValue)); break;
        }
    }

    double NumberArg(const UI::UIActionContext& InContext, const int InIndex, const double InDefault)
    {
        const GameCore::EventArgs& Args = *InContext.Args;
        if(InIndex >= Args.Count) { return InDefault; }
        const GameCore::EventArg& Arg = Args.Args[InIndex];
        if(Arg.Type == GameCore::EventArg::ET_Number) { return Arg.Number; }
        if(Arg.Type == GameCore::EventArg::ET_Bool) { return Arg.Bool ? 1.0 : 0.0; }
        if(Arg.Type == GameCore::EventArg::ET_String) { return std::atof(Arg.String); }
        return InDefault;
    }

    GameSession* SessionOf(const UI::UIActionContext& InContext) { return static_cast<GameSession*>(InContext.HostData); }

    /* A menu's click (the UI bus): Confirm for what starts play, Back for what closes. */
    void Click(GameSession& InSession, const char* InSound)
    {
        if(InSession.HasWorld()) { PlaySound2D(InSession.GetWorld(), InSound); }
    }
    constexpr const char* SND_CONFIRM = "Content/Sounds/UI/UI_Confirm.nmsound";
    constexpr const char* SND_BACK    = "Content/Sounds/UI/UI_Back.nmsound";

    int CurrentResolution()
    {
        const EngineCore::ConfigVar* X = EngineCore::ConfigFindVarByPath("Display.ResolutionX");
        const int Width = X ? X->GetInt() : 1280;
        int Best = 0;
        for(int I = 0; I < 4; ++I) { if(RESOLUTIONS[I].X <= Width) { Best = I; } }
        return Best;
    }

    /* The engine features each level shows (the F1 overlay). */
    const char* FeaturesFor(const std::string& InLevel, const bool bInAlarm)
    {
        if(InLevel == "MainMenu")
        {
            return "Time Of Day plugin: physical sky at dusk\nSequencer: looping camera dolly\nUI module: menus, settings, data binding";
        }
        if(InLevel == "L1_Foothills")
        {
            return "Time Of Day plugin: golden hour sun and sky\nCharacter Mover: walk, sprint, crouch, slopes\n"
                   "Sequencer: C1 arrival, C2 blast door\nSignals and interactables: the gatehouse crank\n"
                   "AICore: navmesh built at runtime\nProcedural Spawner: road, forest, meadow (art pass)";
        }
        if(InLevel == "L2_Laboratory" && bInAlarm)
        {
            return "Calm/Alarm state groups: one world, two states\nEmergency lighting, shutters rerouting\n"
                   "AICore: navmesh, behavior trees, perception (W3, W4)\nJolt ragdolls, hit boxes\n"
                   "Sequencer: C6 extraction\nLabTimer: the atrium's 60 s hold";
        }
        if(InLevel == "L2_Laboratory")
        {
            return "Dynamic GI (DDGI) and SSGI: C3 powers the lab room by room\n"
                   "Materials: Anisotropic, Clear Coat, Cloth, Subsurface, translucency, Volume (art pass)\n"
                   "Sequencer: C3 breaker, C4 elevator\nSignals: breaker, keycard door, valve sequence";
        }
        if(InLevel == "L3_Core")
        {
            return "Volumetric fog and light shafts around the reactor (art pass)\n"
                   "AICore: navmesh, behavior trees, perception (W1, W2)\nCombat: hitscan, spread, reload, ragdolls\n"
                   "Sequencer: C5 theft and alarm\nSaveGame checkpoints";
        }
        return "";
    }
}

// === Pause ==================================================================

void LabUI::OpenPauseMenu(GameSession& InSession)
{
    if(InSession.GetUI().IsOpen(PAUSE_MENU)) { return; }
    InSession.GetUI().SetValue("Pause.CanRestart", UI::UIValue::Bool(LabFlow::HasCheckpoint()));
    InSession.GetUI().Open(PAUSE_MENU);
    InSession.SetPaused(true);
    InSession.SetMouseCaptured(false);
}

void LabUI::ClosePauseMenu(GameSession& InSession)
{
    InSession.GetUI().Close(std::string(SETTINGS));
    InSession.GetUI().Close(std::string(PAUSE_MENU));
    InSession.SetPaused(false);
    InSession.SetMouseCaptured(true);
}

bool LabUI::IsPauseMenuOpen(GameSession& InSession) { return InSession.GetUI().IsOpen(PAUSE_MENU); }

// === Overlay ================================================================

void LabUI::ToggleTechOverlay(GameSession& InSession)
{
    UI::UISystem& UI = InSession.GetUI();
    if(UI.IsOpen(TECH_OVERLAY)) { UI.Close(std::string(TECH_OVERLAY)); }
    else { UI.Open(TECH_OVERLAY); }
}

void LabUI::UpdateTechOverlay(GameSession& InSession, const std::string& InLevel, const bool bInAlarm)
{
    UI::UISystem& UI = InSession.GetUI();
    if(!UI.IsOpen(TECH_OVERLAY)) { return; }
    RenderCore::RenderStats Stats;
    InSession.GetRenderer().GetStats(Stats);
    char Line[256];
    const double Fps = Stats.FrameMs > 0.f ? 1000.0 / double(Stats.FrameMs) : 0.0;
    std::snprintf(Line, sizeof(Line), "Frame %.2f ms (%.0f fps)\nGame %.2f ms   Render %.2f ms\nWait %.2f ms   Present %.2f ms",
                  double(Stats.FrameMs), Fps, double(Stats.GameThreadMs), double(Stats.RenderThreadMs),
                  double(Stats.WaitMs), double(Stats.PresentMs));
    UI.SetValue("Tech.Stats", UI::UIValue::String(Line));
    UI.SetValue("Tech.Level", UI::UIValue::String(InLevel + (bInAlarm ? "  (Alarm)" : "")));
    UI.SetValue("Tech.Features", UI::UIValue::String(FeaturesFor(InLevel, bInAlarm)));
}

void LabUI::ApplySubtitleSetting(GameSession& InSession)
{
    if(GLabSubtitles.GetBool()) { return; }
    UI::UIDocumentInstance* Doc = InSession.GetUI().Find(std::string("Engine/Sequencer/Cinematic.nmui"));
    if(!Doc) { return; }
    for(const char* Name : { "Subtitle", "Speaker" })
    {
        if(UI::UIWidget* Widget = Doc->FindWidget(Name)) { Doc->SetPropertyText(*Widget, "Visibility", "Hidden"); }
    }
}

float LabUI::LookScale() { return std::max(0.05f, GLabLookSensitivity.GetFloat()); }
bool  LabUI::InvertLook() { return GLabInvertY.GetBool(); }

// === Settings ===============================================================

void LabUI::RefreshSettings(GameSession& InSession)
{
    UI::UISystem& UI = InSession.GetUI();
    for(const char* Paths : SETTING_VARS)
    {
        const std::string Path = FirstPath(Paths);
        const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVarByPath(Path);
        if(!Var) { continue; }
        const std::string Key = "Set." + Path;
        switch(Var->GetType())
        {
        case EngineCore::EConfigType::CT_Bool:  UI.SetValue(Key, UI::UIValue::Bool(Var->GetBool())); break;
        case EngineCore::EConfigType::CT_Int:   UI.SetValue(Key, UI::UIValue::Number(double(Var->GetInt()))); break;
        case EngineCore::EConfigType::CT_Float: UI.SetValue(Key, UI::UIValue::Number(double(Var->GetFloat()))); break;
        default: UI.SetValue(Key, UI::UIValue::String(Var->GetString())); break;
        }
    }
    const int Tier = int(GameCore::Scalability::GetDetailMode());
    UI.SetValue("Set.Tier", UI::UIValue::Number(double(Tier)));
    UI.SetValue("Set.TierName", UI::UIValue::String(TIERS[std::clamp(Tier, 0, 3)]));
    const int Res = CurrentResolution();
    UI.SetValue("Set.Resolution", UI::UIValue::Number(double(Res)));
    UI.SetValue("Set.ResolutionName", UI::UIValue::String(std::string(RESOLUTIONS[Res].Name) + "  (next launch)"));
}

// === Actions ================================================================

UI_DEFINE_ACTIONS(Lab)
{
    Actions.Add("Continue", [](const UI::UIActionContext& InContext)
    {
        if(GameSession* Session = SessionOf(InContext)) { Click(*Session, SND_CONFIRM); LabFlow::Continue(*Session); }
    }, "Loads the last checkpoint");

    Actions.Add("NewGame", [](const UI::UIActionContext& InContext)
    {
        if(GameSession* Session = SessionOf(InContext)) { Click(*Session, SND_CONFIRM); LabFlow::NewGame(*Session); }
    }, "Starts the story at Level 1");

    Actions.Add("Chapter", [](const UI::UIActionContext& InContext)
    {
        if(GameSession* Session = SessionOf(InContext))
        {
            Click(*Session, SND_CONFIRM);
            LabFlow::StartChapter(*Session, int(NumberArg(InContext, 0, 0.0)));
        }
    }, "Starts a chapter (0..3) of the flow table");

    Actions.Add("Resume", [](const UI::UIActionContext& InContext)
    {
        if(GameSession* Session = SessionOf(InContext)) { Click(*Session, SND_BACK); LabUI::ClosePauseMenu(*Session); }
    }, "Closes the pause menu and resumes");

    Actions.Add("RestartCheckpoint", [](const UI::UIActionContext& InContext)
    {
        GameSession* Session = SessionOf(InContext);
        if(!Session) { return; }
        LabUI::ClosePauseMenu(*Session);
        LabFlow::Continue(*Session);
    }, "Reloads the last checkpoint");

    Actions.Add("MainMenu", [](const UI::UIActionContext& InContext)
    {
        if(GameSession* Session = SessionOf(InContext)) { LabFlow::ToMainMenu(*Session); }
    }, "Back to the main menu");

    Actions.Add("SetVar", [](const UI::UIActionContext& InContext)
    {
        const GameCore::EventArgs& Args = *InContext.Args;
        if(Args.Count < 2 || Args.Args[0].Type != GameCore::EventArg::ET_String) { return; }
        const double Value = NumberArg(InContext, 1, 0.0);
        for(const std::string& Path : SplitPaths(Args.Args[0].String))
        {
            if(EngineCore::ConfigVar* Var = EngineCore::ConfigFindVarByPath(Path)) { SetVar(*Var, Value); }
            else { CORE_LOG_WARN("[DemoLab] settings: no config var '" + Path + "'") }
        }
    }, "Sets config vars ('A.B|C.D') to a control's value");

    Actions.Add("SetTier", [](const UI::UIActionContext& InContext)
    {
        const int Tier = std::clamp(int(NumberArg(InContext, 0, 2.0) + 0.5), 0, 3);
        GameCore::Scalability::SetDetailMode(GameCore::EDetailMode(Tier));
        if(GameSession* Session = SessionOf(InContext)) { LabUI::RefreshSettings(*Session); }
    }, "Sets the quality tier (0 Low .. 3 Ultra)");

    Actions.Add("SetResolution", [](const UI::UIActionContext& InContext)
    {
        const int Index = std::clamp(int(NumberArg(InContext, 0, 0.0) + 0.5), 0, 3);
        if(EngineCore::ConfigVar* X = EngineCore::ConfigFindVarByPath("Display.ResolutionX")) { X->SetInt(RESOLUTIONS[Index].X); }
        if(EngineCore::ConfigVar* Y = EngineCore::ConfigFindVarByPath("Display.ResolutionY")) { Y->SetInt(RESOLUTIONS[Index].Y); }
        if(GameSession* Session = SessionOf(InContext)) { LabUI::RefreshSettings(*Session); }
    }, "Sets the window resolution, applied at the next launch");

    Actions.Add("RefreshSettings", [](const UI::UIActionContext& InContext)
    {
        if(GameSession* Session = SessionOf(InContext)) { LabUI::RefreshSettings(*Session); }
    }, "Copies the settings into the controls");

    Actions.Add("SaveSettings", [](const UI::UIActionContext&)
    {
        std::string Error;
        if(!EngineCore::ConfigSaveUserFile(&Error) && !Error.empty()) { CORE_LOG_WARN("[DemoLab] settings not saved: " + Error) }
    }, "Writes the player's settings to User.ini");
}
