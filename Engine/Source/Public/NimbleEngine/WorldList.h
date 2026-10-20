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
     *     StartupScript=Scripts/X.lua ; run once its mode has loaded; absent: none
     *     Cook=true                 ; cooked and packaged by Build Project
     *
     * Every one becomes a level named by its section ("BrickBreaker": what
     * [Game] StartLevel, a save, `open` and Game.OpenLevel name), loading its
     * world file and then the mode the world names (GameCore's
     * WorldSettings::GameMode, the editor's World Settings; empty: no rules).
     * It is the only way a world becomes a level: a game registers modes, not
     * levels. A [World_*] GameMode key is no longer read (warned).
     */
    constexpr const char* WORLD_SECTION_PREFIX = "World_";

    struct WorldEntry
    {
        std::string Name;            // the section, less WORLD_SECTION_PREFIX
        std::string File;            // runtime-relative .nmworld; it names its game mode
        std::string StartupScript;   // empty: none
        bool        bCook             = true;
    };

    /* The loaded config's list, in file order. Entries with no File are
     * logged and left out. */
    NE_API std::vector<WorldEntry> ReadWorldList();

    // === Game modes =========================================================
    /*
     * A game's rules, by name: a world's GameMode names one. A mode is
     * created per level load and destroyed when the level unloads.
     *
     * NIMBLE_REGISTER_GAME_MODE (below) is the usual way: it registers as the
     * game's image loads and unregisters as it goes. By hand, register in the
     * module's OnStartup and unregister in OnShutdown -- the functions are in
     * its image. InName must outlive the registration (a literal).
     */
    using GameModeCreateFn  = IGameMode* (*)();
    using GameModeDestroyFn = void (*)(IGameMode*);

    /* False (logged) for a duplicate name, a full registry or a missing function. */
    NE_API bool        RegisterGameMode(const char* InName, GameModeCreateFn InCreate, GameModeDestroyFn InDestroy);
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
    bool RegisterGameMode(const char* InName)
    {
        return RegisterGameMode(InName,
                         []() -> IGameMode* { return new ModeType(); },
                         [](IGameMode* InMode) { delete static_cast<ModeType*>(InMode); });
    }

    /* What NIMBLE_REGISTER_GAME_MODE makes: registers ModeType as the game's
     * image loads and unregisters it as the image unloads. Header-only, so new
     * and delete stay in the game's image. A duplicate name is refused and
     * not unregistered, so it never removes the first mode's entry. */
    template <class ModeType>
    struct GameModeRegistrar
    {
        explicit GameModeRegistrar(const char* InName)
            : Name(InName), bRegistered(RegisterGameMode<ModeType>(InName)) {}
        ~GameModeRegistrar() { if(bRegistered) { UnregisterGameMode(Name); } }

        GameModeRegistrar(const GameModeRegistrar&)            = delete;
        GameModeRegistrar& operator=(const GameModeRegistrar&) = delete;

        const char* Name;
        bool        bRegistered;
    };

    // === The levels =========================================================
    /* One level per listed world, registered (SaveGame.h's registry), after
     * dropping the previous set. GameModuleHost calls these around the game
     * module's life: modes are only known once it has started. */
    NE_API void RegisterWorldLevels();
    NE_API void UnregisterWorldLevels();
}

/*
 * One per mode, at file scope in the game module's .cpp, after the class:
 *
 *     NIMBLE_REGISTER_GAME_MODE(BrickBreakerMode, "BrickBreaker")
 *
 * ModeClass is unqualified (it names the registrar); NameLiteral is what a
 * world's GameMode (World Settings) names. The module's OnStartup and OnShutdown need not
 * mention it.
 */
#define NIMBLE_REGISTER_GAME_MODE(ModeClass, NameLiteral)                                                \
    static ::NimbleEngine::GameModeRegistrar<ModeClass> GGameModeRegistrar_##ModeClass(NameLiteral);

#endif //NIMBLEENGINE_WORLDLIST_H
