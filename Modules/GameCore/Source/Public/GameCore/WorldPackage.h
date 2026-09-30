#ifndef GAMECORE_WORLDPACKAGE_H
#define GAMECORE_WORLDPACKAGE_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    class World;

    /*
     * World FILES. A .nmworld is one of two things, told apart by its first
     * bytes:
     *
     *   Text     what the editor saves: an XML manifest, with one XML .nmentity
     *            per entity in a sibling folder named after it --
     *
     *                Levels/Sandbox.nmworld
     *                Levels/Sandbox/<guid>.nmentity
     *
     *            so an edit to one entity is a diff in one small file. Entities
     *            are named by EntityGuid, never by their (non-unique) names.
     *   Binary   the cooked form a game ships: SerializeWorld's archive, one
     *            file. CookWorldFile turns the first into the second.
     *
     * Save games are neither -- they embed SerializeWorld in a SaveGame archive.
     * Game thread only; refused while the World is ticking. See DESIGN.md for
     * the file formats and the save's commit order.
     */
    enum class EWorldFileFormat : uint8_t
    {
        Unknown = 0,   // unreadable or empty
        Text,          // XML: '<' after any BOM/whitespace
        Binary,        // anything else: a cooked SerializeWorld archive
    };

    GAMECORE_API EWorldFileFormat DetectWorldFileFormat(const char* InPath);

    /*
     * Loads either format into InOutWorld, which should be empty -- an entity
     * GUID already present gets a fresh one (warned). Text needs an editor
     * build; a game build refuses it and says to cook. OutFormat, if given,
     * receives what was found. A failed load leaves the World partly filled;
     * load into one you can discard.
     */
    GAMECORE_API bool LoadWorldFile(const char* InPath, World& InOutWorld,
                                    EWorldFileFormat* OutFormat = nullptr);

#if WITH_EDITOR
    /*
     * Saves InWorld as a text world at InPath (a .nmworld), its entities in the
     * sibling folder. As close to atomic as separate files allow:
     *
     *   1. every file is built in memory; one whose bytes are unchanged is
     *      left alone
     *   2. each changed file is written to "<file>.tmp" -- any failure deletes
     *      the temporaries and leaves the old world exactly as it was
     *   3. the temporaries are renamed over their targets, entities first and
     *      the manifest LAST: the manifest is what a load trusts, so it is the
     *      commit
     *   4. only then, .nmentity files the manifest no longer lists (deleted
     *      entities) and stale .tmp files are removed from the sibling folder
     *      -- nothing else in it is touched
     */
    GAMECORE_API bool SaveWorldPackage(const char* InPath, World& InWorld);

    /*
     * Loads InSourcePath (either format) into a scratch World and writes it to
     * InCookedPath as a binary world -- via a temporary and a rename, creating
     * missing directories.
     */
    GAMECORE_API bool CookWorldFile(const char* InSourcePath, const char* InCookedPath);
#endif
}

#endif //GAMECORE_WORLDPACKAGE_H
