#ifndef NIMBLEENGINE_WORLDLIST_H
#define NIMBLEENGINE_WORLDLIST_H

#include <cstddef>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace NimbleEngine
{
    class IGameMode;

    /*
     * THE PROJECT'S WORLDS are listed in Config/DefaultEngine.ini, one section
     * each, in order (the editor's Project Settings > Worlds edits them):
     *
     *     [World_BrickBreaker]
     *     File=Content/Levels/BrickBreaker.nmworld
     *     GameMode=BrickBreaker     ; a RegisterGameMode name; empty: no rules
     *     StartupScript=Scripts/X.lua ; run once its mode has loaded; absent: none
     *     Cook=true                 ; cooked and packaged by Build Project
     *
     * Every one becomes a level named by its section ("BrickBreaker": what
     * [Game] StartLevel, a save, `open` and Game.OpenLevel name), loading its
     * world file and then its mode. It is the only way a world becomes a
     * level: a game registers modes, not levels.
     */
    constexpr const char* WORLD_SECTION_PREFIX = "World_";

    struct WorldEntry
    {
        std::string Name;            // the section, less WORLD_SECTION_PREFIX
        std::string File;            // runtime-relative .nmworld
        std::string GameMode;
        std::string StartupScript;   // empty: none
        bool        bCook             = true;
    };

    /* The loaded config's list, in file order. Entries with no File are
     * logged and left out. */
    NE_API std::vector<WorldEntry> ReadWorldList();

    // === Game modes =========================================================
    /*
     * A game's rules, by name: a [World_*] GameMode names one. Register in the
     * game module's OnStartup and unregister in OnShutdown -- the functions
     * are in its image. A mode is created per level load and destroyed when
     * the level unloads. InName must outlive the registration (a literal).
     */
    using GameModeCreateFn  = IGameMode* (*)();
    using GameModeDestroyFn = void (*)(IGameMode*);

    NE_API void        RegisterGameMode(const char* InName, GameModeCreateFn InCreate, GameModeDestroyFn InDestroy);
    NE_API void        UnregisterGameMode(const char* InName);
    NE_API bool        HasGameMode(const char* InName);
    NE_API size_t      GetGameModeCount();
    NE_API const char* GetGameModeName(size_t InIndex);

    /* Whether InName's mode is a NetGameMode (PlayerStart.h): one that gives
     * each connected client an avatar. Made and destroyed to find out, as a
     * level load would -- no OnLoad, no World. False for a name not
     * registered. Collaboration's content report: Play Together warns of a
     * level where players won't see each other. */
    NE_API bool IsNetworkedGameMode(const char* InName);

    /* A NetGameMode's own avatar prefab (PeekAvatarPrefab): "" if InName isn't
     * one, or leaves it to the Player Start and [Avatar] DefaultPrefab. */
    NE_API std::string GetGameModeAvatarPrefab(const char* InName);

    /* RegisterGameMode<BrickBreakerMode>("BrickBreaker"): new and delete in the
     * caller's image. */
    template <class ModeType>
    void RegisterGameMode(const char* InName)
    {
        RegisterGameMode(InName,
                         []() -> IGameMode* { return new ModeType(); },
                         [](IGameMode* InMode) { delete static_cast<ModeType*>(InMode); });
    }

    // === The levels =========================================================
    /* One level per listed world, registered (SaveGame.h's registry), after
     * dropping the previous set. GameModuleHost calls these around the game
     * module's life: modes are only known once it has started. */
    NE_API void RegisterWorldLevels();
    NE_API void UnregisterWorldLevels();
}

#endif //NIMBLEENGINE_WORLDLIST_H
