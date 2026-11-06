#ifndef NIMBLEENGINE_SAVEGAMEFORMAT_H
#define NIMBLEENGINE_SAVEGAMEFORMAT_H

#include <cstdint>

#include "Public/Core/CoreArchive.h"

/*
 * The save file's tags and version, shared by SaveGame.cpp (writes, reads the
 * header) and NimbleEngine.cpp (reads the rest in LoadLevel). Private: nothing
 * a game links needs them.
 *
 *   NMSG                           the whole save
 *     uint16  format version       (the chunk's own version field)
 *     string  level name           ILevel::GetName()
 *     uint64  saved-at, Unix seconds
 *     WRLD                         GameCore::SerializeWorld's output
 *     GMST                         optional -- absent when the level has no
 *                                  GameState, so the reader probes for it
 *     GPHS                         optional, with GMST -- the GameState's
 *                                  phase: string name, double seconds in it,
 *                                  int32 round. Absent from older saves.
 */

namespace NimbleEngine
{
    constexpr uint32_t TAG_SAVEGAME  = EngineCore::MakeArchiveTag('N','M','S','G');
    constexpr uint32_t TAG_GAMESTATE = EngineCore::MakeArchiveTag('G','M','S','T');
    constexpr uint32_t TAG_GAMEPHASE = EngineCore::MakeArchiveTag('G','P','H','S');

    /* GPHS's own version. */
    constexpr uint16_t GAMEPHASE_CHUNK_VERSION = 1;

    /* The NMSG layout's version, not the World's or GameState's. A newer one is
     * refused: everything else is found through this header. */
    constexpr uint16_t SAVEGAME_FORMAT_VERSION = 1;

    /* SaveGameHeader::LevelName's capacity, for writer and reader to agree on. */
    enum : size_t { SAVEGAME_LEVEL_NAME_MAX = 64 };
}

#endif //NIMBLEENGINE_SAVEGAMEFORMAT_H
