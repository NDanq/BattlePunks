#include "Public/NimbleEngine/GameModule.h"

/*
 * DemoLabGame: Demo Lab, NimbleEngine's showcase game (Docs/Design.md), built
 * as a module library loaded by name by the DemoLab launcher (the engine's)
 * and the editor's Play-in-Editor.
 *
 * Everything else is under Source/, ported from BattlePunks (Lab instead of
 * Punk) and grown for Demo Lab's flow:
 *
 *   Core/       the player (native:DemoLab.Player, the SteamDummy), health,
 *               teams, the input map, the move simulation
 *   Weapons/    inventory, hitscan and melee combat, projectiles, the registry
 *   AI/         the Watchman and the Brute, driven by AICore brains
 *   World/      signals, interactables (holds too), gates, triggers,
 *               objectives, encounters (waves), timers, travel points
 *   Flow/       LabTravel (the one travel function) and LabFlow (chapters,
 *               New Game, Continue)
 *   UI/         the HUD and the menus' native actions
 *   GameModes/  LabMenu (MainMenu), LabStory (every level), the AutoPlay test
 *
 * Each mode registers itself (NIMBLE_REGISTER_GAME_MODE); each world names
 * its mode in its <Settings GameMode="..."/>.
 */
class DemoLabGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "DemoLabGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override { return true; }
    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override {}
};

NIMBLE_IMPLEMENT_GAME_MODULE(DemoLabGameModule, "DemoLabGame")
