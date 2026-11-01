#include "UI/LabHUD.h"

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/UI/UISystem.h"

#include "Core/LabHealth.h"
#include "Weapons/LabCombat.h"
#include "Weapons/LabWeapon.h"
#include "World/LabEncounter.h"
#include "World/LabInteractable.h"
#include "World/LabLogic.h"

#include <cmath>
#include <cstdio>

using namespace GameCore;
using namespace NimbleEngine;

namespace
{
    constexpr double VIS_SHOWN  = 3.0;   // EUIVisibility::HitTestInvisible
    constexpr double VIS_HIDDEN = 1.0;   // EUIVisibility::Hidden

    void SetText(UI::UISystem& InUI, const char* InKey, const std::string& InText)
    {
        InUI.SetValue(InKey, UI::UIValue::String(InText));
    }

    void SetNumber(UI::UISystem& InUI, const char* InKey, const double InValue)
    {
        InUI.SetValue(InKey, UI::UIValue::Number(InValue));
    }
}

void LabHUD::Open(GameSession& InSession)
{
    if(!InSession.GetUI().IsOpen(DOCUMENT)) { InSession.GetUI().Open(DOCUMENT); }
}

void LabHUD::Close(GameSession& InSession)
{
    InSession.GetUI().Close(std::string(DOCUMENT));
}

void LabHUD::Update(GameSession& InSession, const Story& InStory)
{
    UI::UISystem& UI = InSession.GetUI();
    World& W = InSession.GetWorld();
    const EntityID Avatar = GetLocalAvatar(W, 0);
    const Entity Self = W.GetEntity(Avatar);
    const bool bHave = Avatar.IsSet() && Self.IsAlive();

    SetNumber(UI, "Lab.HudVis", InStory.bCinematic || !bHave ? VIS_HIDDEN : VIS_SHOWN);
    SetText(UI, "Story.Objective", InStory.Objective);
    SetText(UI, "Story.Banner", InStory.Banner);
    SetNumber(UI, "Story.BannerVis", InStory.Banner.empty() ? VIS_HIDDEN : VIS_SHOWN);

    for(int I = 0; I < 4; ++I)
    {
        const int From = int(InStory.Feed.size()) - 4 + I;
        SetText(UI, ("Feed.Line" + std::to_string(I)).c_str(), From >= 0 ? InStory.Feed[size_t(From)] : std::string());
    }
    SetNumber(UI, "Story.IntelVis", InStory.bIntel ? VIS_SHOWN : VIS_HIDDEN);
    SetText(UI, "Story.Timer", LabLogic::TimerLine(W));

    int Alive = 0, Total = 0, Wave = 0, Waves = 0;
    if(LabEncounters::GetActive(W, Alive, Total))
    {
        char Line[64];
        if(LabEncounters::GetWave(W, Wave, Waves) && Waves > 1) { std::snprintf(Line, sizeof(Line), "Wave %d of %d   %d left", Wave, Waves, Alive); }
        else { std::snprintf(Line, sizeof(Line), "Enemies left: %d", Alive); }
        SetText(UI, "Story.Encounter", Line);
    }
    else
    {
        SetText(UI, "Story.Encounter", "");
    }

    if(!bHave) { return; }

    const LabHealthComponent* Health = Self.GetComponent<LabHealthComponent>();
    const float Hp = Health ? Health->Health : 0.f;
    const float Max = Health && Health->MaxHealth > 0.f ? Health->MaxHealth : 100.f;
    SetText(UI, "Lab.Health", std::to_string(int(std::ceil(Hp))));
    SetNumber(UI, "Lab.HealthFill", double(Hp / Max));

    const LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    const LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    const int Slot = Inventory ? Inventory->Equipped : LabWeapons::UNARMED;
    const uint8_t HeldID = Inventory ? Inventory->EquippedWeapon() : uint8_t(0);
    const int32_t Mag = (Inventory && Slot < LabWeapons::SLOTS) ? Inventory->MagAt(Slot) : 0;
    const int32_t Reserve = (Inventory && Slot < LabWeapons::SLOTS) ? Inventory->ReserveAt(Slot) : 0;
    const bool bReloading = Combat && Combat->bReloading;
    const bool bMarker = Combat && Combat->HitMarker > 0.f;

    /* GetData may spawn the definition instance: last, after every pointer above is read. */
    const LabWeaponComponent* Weapon = HeldID ? LabWeapons::GetData(W, HeldID) : nullptr;
    if(Weapon)
    {
        SetText(UI, "Lab.Weapon", Weapon->DisplayName);
        if(Weapon->MagSize > 0)
        {
            char Ammo[32];
            std::snprintf(Ammo, sizeof(Ammo), "%d / %d", int(Mag), int(Reserve));
            SetText(UI, "Lab.Ammo", bReloading ? std::string("reloading") : std::string(Ammo));
        }
        else
        {
            SetText(UI, "Lab.Ammo", "");
        }
    }
    else
    {
        SetText(UI, "Lab.Weapon", "Fists");
        SetText(UI, "Lab.Ammo", "");
    }
    SetNumber(UI, "Lab.WeaponVis", Weapon ? VIS_SHOWN : VIS_HIDDEN);
    SetNumber(UI, "Lab.CrossVis", Weapon && Weapon->MagSize > 0 ? VIS_SHOWN : VIS_HIDDEN);
    SetNumber(UI, "Lab.HitVis", bMarker ? VIS_SHOWN : VIS_HIDDEN);

    const std::string Prompt = LabWorld::PromptFor(W, Avatar);
    SetText(UI, "Lab.Prompt", Prompt.empty() ? Prompt : "[E]  " + Prompt);
    SetNumber(UI, "Lab.PromptVis", Prompt.empty() ? VIS_HIDDEN : VIS_SHOWN);
    const float Hold = LabWorld::HoldFraction(W, Avatar);
    SetNumber(UI, "Lab.HoldVis", Hold >= 0.f && !Prompt.empty() ? VIS_SHOWN : VIS_HIDDEN);
    SetNumber(UI, "Lab.HoldFill", Hold > 0.f ? double(Hold) : 0.0);
}
