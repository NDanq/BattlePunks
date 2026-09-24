#ifndef NIMBLEENGINE_SAVEGAME_H
#define NIMBLEENGINE_SAVEGAME_H

#include <cstdint>
#include <string>

#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace NimbleEngine
{
    class Application;
    class ILevel;

    /*
     * Saving and restoring a game: the level's name, its World, and its
     * GameState. The IGameMode is not saved -- it is the rules, rebuilt by
     * loading the level, and anything a mode needs kept belongs in the
     * GameState (GameState.h).
     *
     * A load is DEFERRED. LoadGameFromFile resolves the level and calls
     * RequestLevel, and the restore runs inside the next LoadLevel -- see
     * Application::LoadLevel for exactly where, and why there. Loading from
     * inside OnTick is therefore safe.
     *
     * The level's own OnLoad runs before the restore, so a level that spawns
     * entities must clear them in ILevel::OnPrepareForLoadedGame or the scene
     * is doubled.
     *
     * Game thread only.
     */

    /* The save's leading fields, for a save-slot list (PeekSaveGameHeader). */
    struct SaveGameHeader
    {
        /* ILevel::GetName(), truncated to 63 characters. */
        char     LevelName[64] = {};

        /* Wall-clock seconds since the epoch. For display only. */
        uint64_t SavedAtUnixSeconds = 0;

        /* The engine save format's version, not any chunk's. */
        uint16_t FormatVersion = 0;
    };

    /*
     * Writes the active level's name, World and GameState, after calling
     * IGameMode::OnPreSaveGame. False, with nothing written, if there is no
     * level, the World is ticking, or the write fails.
     */
    NE_API bool SaveGameToFile(Application& InApp, const std::string& InPath);

    /*
     * Reads InPath and requests its level; the scene is restored next frame.
     * True means "accepted", not "restored" -- act on the result in
     * IGameMode::OnPostLoadSave. On any failure (missing or malformed file,
     * unregistered level) the running game is untouched.
     */
    NE_API bool LoadGameFromFile(Application& InApp, const std::string& InPath);

    /* The header alone. False if the file is missing or not a save. */
    NE_API bool PeekSaveGameHeader(const std::string& InPath, SaveGameHeader& OutHeader);

    // === The level registry =================================================
    /*
     * Turns a save's level name back into an ILevel*. Register each loadable
     * level once, before any load, and unregister it before it is destroyed --
     * the registry holds bare pointers and owns nothing, as Exec does. A
     * duplicate name keeps the first and logs an error, since it would make
     * saves ambiguous. Not a factory: creating levels on demand is the game's
     * business.
     */
    NE_API void    RegisterLevel(ILevel& InLevel);
    NE_API void    UnregisterLevel(ILevel& InLevel);
    NE_API ILevel* FindLevelByName(const char* InName);
    NE_API size_t  GetRegisteredLevelCount();
}

#endif //NIMBLEENGINE_SAVEGAME_H
