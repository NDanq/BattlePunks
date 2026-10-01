#ifndef GAMECORE_WORLDPACKAGE_H
#define GAMECORE_WORLDPACKAGE_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    class World;

    /*
     * A .nmworld is one of two formats, told apart by its first bytes:
     *
     *   Text     the editor's: an XML manifest plus one .nmentity per entity,
     *            named by EntityGuid, in a sibling folder:
     *
     *                Levels/Sandbox.nmworld
     *                Levels/Sandbox/<guid>.nmentity
     *
     *   Binary   the cooked, shipped form (SerializeWorld). See CookWorldFile.
     *
     * Save games embed SerializeWorld instead. Game thread only; refused while
     * ticking. See DESIGN.md.
     */
    enum class EWorldFileFormat : uint8_t
    {
        Unknown = 0,   // unreadable or empty
        Text,          // XML: '<' after any BOM/whitespace
        Binary,        // anything else: a cooked SerializeWorld archive
    };

    GAMECORE_API EWorldFileFormat DetectWorldFileFormat(const char* InPath);

    /* Loads either format (text needs an editor build) into an empty World.
     * A failed load leaves it partly filled; use a discardable World. */
    GAMECORE_API bool LoadWorldFile(const char* InPath, World& InOutWorld,
                                    EWorldFileFormat* OutFormat = nullptr);

#if WITH_EDITOR
    /*
     * Saves InWorld as a text world. As atomic as separate files allow:
     *
     *   1. build every file in memory; skip unchanged ones
     *   2. write changed files to "<file>.tmp"; on failure delete them all
     *   3. rename over the targets, the manifest LAST (it is the commit)
     *   4. remove unlisted .nmentity files and stale .tmp files
     */
    GAMECORE_API bool SaveWorldPackage(const char* InPath, World& InWorld);

    /* Writes InSourcePath (either format) to InCookedPath as a binary world. */
    GAMECORE_API bool CookWorldFile(const char* InSourcePath, const char* InCookedPath);
#endif
}

#endif //GAMECORE_WORLDPACKAGE_H
