#include "Public/NimbleEngine/SaveGame.h"

#include <cstring>
#include <ctime>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldSerializer.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NimbleEngine.h"

#include "Private/NimbleEngine/SaveGameFormat.h"

/*
 * Writes saves, arms loads, and resolves level names. The restore itself is in
 * GameSession::LoadLevel, which owns the ordering it has to fit into.
 */

namespace NimbleEngine
{
namespace
{
    /* A plain array, constant-initialised, so registering from a file-scope
     * object is safe -- CoreConfig.h's GConfigHead argument. Past 64 levels a
     * game wants its own catalogue rather than a linear scan. */
    enum : size_t { MAX_REGISTERED_LEVELS = 64 };

    ILevel* GRegisteredLevels[MAX_REGISTERED_LEVELS] = {};
    size_t  GRegisteredLevelCount = 0;

    /* Wall time, because a human reads it; never used for ordering. */
    uint64_t NowUnixSeconds()
    {
        return uint64_t(std::time(nullptr));
    }
}

// === The level registry =====================================================

void RegisterLevel(ILevel& InLevel)
{
    const char* Name = InLevel.GetName();
    if(Name == nullptr || Name[0] == '\0')
    {
        CORE_LOG_ERROR("RegisterLevel: a level with no name cannot be named by a save; ignoring it")
        return;
    }

    if(std::strlen(Name) >= SAVEGAME_LEVEL_NAME_MAX)
    {
        /* Still works -- saved and compared truncated -- but collides with any
         * level sharing its first 63 characters. */
        CORE_LOG_WARN(std::string("RegisterLevel: level name '") + Name + "' is longer than "
                      + std::to_string(SAVEGAME_LEVEL_NAME_MAX - 1)
                      + " characters and will be truncated in a save file")
    }

    /* First wins, and it is an error: one name for two levels makes every
     * save naming it ambiguous. */
    if(ILevel* Existing = FindLevelByName(Name))
    {
        if(Existing != &InLevel)
        {
            CORE_LOG_ERROR(std::string("RegisterLevel: a different level is already registered as '")
                           + Name + "'; keeping the first")
        }
        return;
    }

    if(GRegisteredLevelCount >= MAX_REGISTERED_LEVELS)
    {
        CORE_LOG_ERROR("RegisterLevel: the level registry is full ("
                       + std::to_string(MAX_REGISTERED_LEVELS)
                       + "); '" + Name + "' will not be loadable from a save")
        return;
    }

    GRegisteredLevels[GRegisteredLevelCount++] = &InLevel;
}

void UnregisterLevel(ILevel& InLevel)
{
    for(size_t Index = 0; Index < GRegisteredLevelCount; ++Index)
    {
        if(GRegisteredLevels[Index] != &InLevel) { continue; }

        /* Swap-and-pop; order is not meaningful. */
        GRegisteredLevels[Index] = GRegisteredLevels[GRegisteredLevelCount - 1];
        GRegisteredLevels[--GRegisteredLevelCount] = nullptr;
        return;
    }
}

ILevel* FindLevelByName(const char* InName)
{
    if(InName == nullptr) { return nullptr; }

    for(size_t Index = 0; Index < GRegisteredLevelCount; ++Index)
    {
        ILevel* Level = GRegisteredLevels[Index];
        if(Level == nullptr) { continue; }

        const char* Name = Level->GetName();
        /* Only as much as a save carries, so a truncated name still matches. */
        if(Name && std::strncmp(Name, InName, SAVEGAME_LEVEL_NAME_MAX - 1) == 0)
        {
            return Level;
        }
    }
    return nullptr;
}

size_t GetRegisteredLevelCount() { return GRegisteredLevelCount; }

// === Saving =================================================================

bool SaveGameToFile(GameSession& InSession, const std::string& InPath)
{
    ILevel* Level = InSession.GetLevel();
    if(Level == nullptr)
    {
        CORE_LOG_ERROR("SaveGameToFile: there is no level loaded to save")
        return false;
    }

    /* Refused here, before anything is built, rather than left to
     * SerializeWorld's CORE_CHECK -- with a log handler installed that returns
     * instead of aborting. Mid-tick, deferred destroys have not run yet. */
    GameCore::World& World = InSession.GetWorld();
    if(World.IsTicking())
    {
        CORE_LOG_ERROR("SaveGameToFile: refusing to save while the World is ticking -- "
                       "call this from OnTick's own frame, not from inside a World system")
        return false;
    }

    /* The mode is not saved; this is its chance to flush into the GameState. */
    if(IGameMode* Mode = Level->GetGameMode())
    {
        Mode->OnPreSaveGame();
    }

    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();

    uint16_t FormatVersion = SAVEGAME_FORMAT_VERSION;
    Ar.OpenChunk(TAG_SAVEGAME, FormatVersion);

    std::string LevelName(Level->GetName() ? Level->GetName() : "");
    if(LevelName.size() >= SAVEGAME_LEVEL_NAME_MAX)
    {
        LevelName.resize(SAVEGAME_LEVEL_NAME_MAX - 1);
    }
    Ar << LevelName;

    uint64_t SavedAt = NowUnixSeconds();
    Ar << SavedAt;

    /* The same call LoadLevel makes, so the two paths cannot drift. */
    if(!GameCore::SerializeWorld(Ar, World))
    {
        CORE_LOG_ERROR("SaveGameToFile: failed to serialize the World")
        return false;
    }

    /* Only when there is one -- which is why the reader probes for it. */
    if(GameState* State = Level->GetGameState())
    {
        uint16_t StateVersion = State->GetSerialVersion();
        Ar.OpenChunk(TAG_GAMESTATE, StateVersion);
        State->Serialize(Ar);
        Ar.CloseChunk();
    }

    Ar.CloseChunk();

    if(Ar.HasError())
    {
        CORE_LOG_ERROR("SaveGameToFile: the archive failed while being built; nothing was written")
        return false;
    }

    if(!EngineCore::SaveArchiveToFile(InPath, Ar))
    {
        return false;
    }

    CORE_LOG("[Save] wrote '" + InPath + "' (" + std::to_string(Ar.GetBytes().size())
             + " bytes, level '" + LevelName + "')")
    return true;
}

// === Loading ================================================================

namespace
{
    /* Reads the header and leaves the cursor at the World chunk -- with the NMSG
     * chunk still OPEN on success. Shared by Peek and Load so they agree on what
     * a valid save is. */
    bool ReadSaveHeader(EngineCore::Archive& Ar, SaveGameHeader& OutHeader)
    {
        uint16_t FormatVersion = 0;
        if(!Ar.OpenChunk(TAG_SAVEGAME, FormatVersion))
        {
            CORE_LOG_ERROR("Save file does not begin with a save-game chunk; it is not a save")
            return false;
        }

        if(FormatVersion > SAVEGAME_FORMAT_VERSION)
        {
            /* Refused, not skipped: everything else is found through this header. */
            CORE_LOG_ERROR("Save file is format version " + std::to_string(FormatVersion)
                           + "; this build understands up to "
                           + std::to_string(SAVEGAME_FORMAT_VERSION))
            Ar.CloseChunk();
            return false;
        }

        std::string LevelName;
        Ar << LevelName;

        uint64_t SavedAt = 0;
        Ar << SavedAt;

        if(Ar.HasError())
        {
            CORE_LOG_ERROR("Save file's header is truncated or corrupt")
            Ar.CloseChunk();
            return false;
        }

        OutHeader = SaveGameHeader{};
        OutHeader.FormatVersion      = FormatVersion;
        OutHeader.SavedAtUnixSeconds = SavedAt;

        const size_t Copy = (LevelName.size() < SAVEGAME_LEVEL_NAME_MAX - 1)
                          ? LevelName.size() : SAVEGAME_LEVEL_NAME_MAX - 1;
        std::memcpy(OutHeader.LevelName, LevelName.data(), Copy);
        OutHeader.LevelName[Copy] = '\0';

        return true;
    }
}

bool PeekSaveGameHeader(const std::string& InPath, SaveGameHeader& OutHeader)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
    if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
    {
        return false;
    }

    if(!ReadSaveHeader(Ar, OutHeader))
    {
        return false;
    }

    /* The NMSG chunk ReadSaveHeader left open. */
    Ar.CloseChunk();
    return true;
}

bool LoadGameFromFile(GameSession& InSession, const std::string& InPath)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
    if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
    {
        /* LoadArchiveFromFile already warned. */
        return false;
    }

    SaveGameHeader Header;
    if(!ReadSaveHeader(Ar, Header))
    {
        return false;
    }

    ILevel* Level = FindLevelByName(Header.LevelName);
    if(Level == nullptr)
    {
        /* Nothing is armed yet, so the running game is unchanged. */
        CORE_LOG_ERROR(std::string("LoadGameFromFile: no level is registered as '")
                       + Header.LevelName + "'; call RegisterLevel for it before loading. "
                       "The running game is unchanged.")
        return false;
    }

    /* Arm the restore and request the level; the deferred swap does the rest.
     * Everything from the cursor on -- the WRLD chunk, then the optional GMST --
     * so LoadLevel starts at WRLD without re-reading the header. */
    const std::vector<uint8_t>& Bytes = Ar.GetBytes();
    InSession.PendingSaveBytes.assign(Bytes.begin() + std::ptrdiff_t(Ar.GetCursor()), Bytes.end());

    if(InSession.PendingSaveBytes.empty())
    {
        CORE_LOG_ERROR("LoadGameFromFile: the save's header is valid but it contains no scene")
        return false;
    }

    InSession.RequestLevel(*Level);

    CORE_LOG("[Save] loading '" + InPath + "' into level '" + std::string(Header.LevelName)
             + "'; the swap happens at the top of the next frame")
    return true;
}
}
