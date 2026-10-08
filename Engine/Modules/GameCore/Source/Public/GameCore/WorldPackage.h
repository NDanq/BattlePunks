#ifndef GAMECORE_WORLDPACKAGE_H
#define GAMECORE_WORLDPACKAGE_H

#include <cstdint>
#include <string>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/PrefabInstanceID.h"

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

    /* Loads either format (text needs an uncooked build: not Shipping) into an empty World.
     * A failed load leaves it partly filled; use a discardable World. */
    GAMECORE_API bool LoadWorldFile(const char* InPath, World& InOutWorld,
                                    EWorldFileFormat* OutFormat = nullptr);

    /*
     * CONTENT BY ITS SOURCE PATH. A game names a world or a prefab as the
     * project has it -- "Content/Levels/Arena.nmworld" -- in every build, and
     * the build decides what is read:
     *
     *   Editor     the text source itself: what is being edited.
     *   Debug, RelWithDebInfo Client/Server (WITH_UNCOOKED)
     *              cooked here, on first use: the source is cooked into
     *              Intermediate/Cooked/<source path> beside the executable,
     *              again whenever the source is newer, and that is read -- the
     *              Shipping load path, with no manual cook. [Content]
     *              CookOnLoad=false reads the text source instead.
     *   Shipping   cooked ahead, at its own path: the editor's Build > Cook
     *              Content writes <project>/Cooked/<source path>, and the
     *              build stages that beside the executable.
     *
     * A source that is already binary is read as it is, in every build.
     */
    GAMECORE_API std::string ResolveContentWorld(const char* InSourcePath);
    GAMECORE_API std::string ResolveContentPrefab(const char* InSourcePath);

    /* LoadWorldFile(ResolveContentWorld(InSourcePath)): how a level loads its world. */
    GAMECORE_API bool LoadContentWorld(const char* InSourcePath, World& InOutWorld);

#if WITH_UNCOOKED
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

    /*
     * For an editor, after ReloadPrefab: InInstance spawned again from what
     * its prefab path loads now, keeping all a level file keeps -- its GUID
     * (and so its entities'), name, placement, overrides and parent -- and
     * the loose entities hanging from it. A template the prefab no longer has
     * loses its overrides (warned). Returns the new instance; InInstance
     * itself if it has no prefab path; unset, warned, if that no longer loads
     * (its loose entities are then left unparented). Not while ticking.
     */
    GAMECORE_API PrefabInstanceID RefreshPrefabInstance(World& InWorld, PrefabInstanceID InInstance);
#endif
}

#endif //GAMECORE_WORLDPACKAGE_H
