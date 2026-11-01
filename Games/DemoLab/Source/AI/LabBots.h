#ifndef DEMOLAB_LABBOTS_H
#define DEMOLAB_LABBOTS_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/TransformMath.h"

namespace GameCore { class World; }
namespace NimbleEngine { struct InputSnapshot; }

/*
 * Bots (TDD §4.2, §8): a character driven through the mode's avatar input filter,
 * like a player -- so movement prediction, combat, interaction and every rule
 * apply to it unchanged. Its AI Brain (AICore, through the engine's facade)
 * picks where to go and what to look at; the driver turns that into input:
 * steering (SteerInput), aiming with the archetype's skill and reaction, and
 * the fire, aim and reload buttons. Authority only: brains run on the server.
 *
 * Bot players take local-player indices from 16 up (BOT_PLAYER_BASE): past the
 * four real pads, so a bot never reads a device, never gets a split-screen
 * view, and never joins or leaves with a pad. Only scoreboard bots (arena
 * fill-ins) get a PlayerState; story enemies do not.
 */

namespace LabBots {
    constexpr uint8_t BOT_PLAYER_BASE = 16;

    /* Demo Lab's enemies (Docs/Design.md, Cast): the Watchman is ranged (pipe
     * pistol, red eyes), the Brute melee (wrench, scaled 1.25, slow). */
    enum EArchetype : uint8_t { BA_Brute = 0, BA_Watchman = 1 };

    /* "Watchman" / "Brute" (a wave table's names); Watchman for anything else. */
    uint8_t     ArchetypeByName(const char* InName);
    const char* TreeFor(uint8_t InArchetype);

    /* Authority: a player entity for a bot; unset if every index is taken. */
    GameCore::EntityID CreateBotPlayer(GameCore::World& InWorld, bool bInScoreboard, const char* InName = nullptr);
    /* Authority: unpossesses and destroys it. */
    void ReleaseBotPlayer(GameCore::World& InWorld, GameCore::EntityID InPlayer);

    struct Spawn
    {
        const char*             Prefab = "native:DemoLab.Player";
        const char*             Tree = "Content/AI/Watchman.nmbt";
        uint8_t                 Archetype = BA_Watchman;
        uint8_t                 Team = 2;
        bool                    bScoreboard = false;
        const char*             Name = nullptr;
        GameCore::TransformPose At;
        uint8_t                 Weapons[3] = { 0, 0, 0 };   // registry ids; all 0: the archetype's loadout
        float                   MaxHealth = 0.f;            // 0: the character's
    };
    /* Authority: a character at InSpawn.At, possessed by a new bot player, with a
     * brain, a loadout and its team. Returns the avatar; unset on failure. */
    GameCore::EntityID SpawnBot(GameCore::World& InWorld, const Spawn& InSpawn);

    /* The filter's part: overwrites InOutInput for a bot's avatar. */
    void Drive(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput, float InSeconds);

    /* Authority, each tick: every character's AI stimulus follows whether it is
     * up, so the dead and the downed are not seen. */
    void TickStimuli(GameCore::World& InWorld);

    /* Authority: a bot that was hurt turns on whoever hurt it. */
    void OnDamaged(GameCore::World& InWorld, GameCore::EntityID InVictim, GameCore::EntityID InInstigator);
}

/* On a bot's avatar (server-side; not saved with a level). */
struct LabBotComponent {
    GAMECORE_COMPONENT_BODY()
    uint8_t Archetype = LabBots::BA_Watchman;
    float   AimSkill = 0.6f;            // 0..1: how tight the aim is
    float   ReactionSeconds = 0.35f;    // before shooting at a target it just saw
    float   PreferredRange = 700.f;
    GameCore::EntityID BotPlayer;

    // Driver scratch.
    GameCore::EntityID LastTarget;
    float   Reaction = 0.f;
    float   ErrorYaw = 0.f, ErrorPitch = 0.f, ErrorClock = 0.f;
    float   FireClock = 0.f;
    float   KeyClock = 0.f;
    float   ThrowClock = 0.f;
    float   ThrowHold = 0.f;
    uint32_t Seed = 1;
};

#endif //DEMOLAB_LABBOTS_H
