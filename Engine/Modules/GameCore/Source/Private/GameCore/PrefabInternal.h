#ifndef GAMECORE_PREFABINTERNAL_H
#define GAMECORE_PREFABINTERNAL_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/Prefab.h"

/*
 * What a PrefabAsset holds, for Prefab.cpp and World.cpp only. See
 * DESIGN.md's Prefab section.
 */
namespace GameCore
{
    /* One component: its type by NAME (ids shift between runs) and its
     * serializer's binary form -- an archive holding one DATA chunk, whose
     * version is the serializer's when it was written, with every entity
     * reference as a local id (0: unset). Kept even when this build lacks
     * the type, so a cooked prefab round-trips. */
    struct PrefabComponentData
    {
        std::string          TypeName;
        std::vector<uint8_t> Blob;
    };

    struct PrefabEntityData
    {
        uint32_t                         LocalId  = PREFAB_NO_LOCAL_ID;
        uint32_t                         ParentId = PREFAB_NO_LOCAL_ID;
        std::string                      Name;
        std::vector<PrefabComponentData> Components;   // sorted by type name
    };

    struct PrefabAsset::Data
    {
        int                           RefCount = 1;   // game thread only
        std::string                   Name;
        std::string                   ScriptPath;
        std::string                   NativeBehavior;   // a native prefab's name whose callbacks drive it; "" for none
        EPrefabTickMode               TickMode    = EPrefabTickMode::Tick;
        uint32_t                      NextLocalId = 1;
        std::vector<PrefabEntityData> Entities;       // tree order, root first
    };

    struct PrefabAccess
    {
        static const PrefabAsset::Data& Read(const PrefabAsset& InAsset) { return *InAsset.D; }
        /* Clones first if shared. */
        static PrefabAsset::Data& Write(PrefabAsset& InAsset);
    };

    /* Local id -> what it stands for in one direction of a conversion. */
    using LocalIdMap = std::unordered_map<uint32_t, EntityID>;
    using LiveIdMap  = std::unordered_map<uint64_t, uint32_t>;   // EntityID::Pack -> local id

    /* InComponent (a live component of InType) as a blob, references mapped
     * through InToLocal; anything absent from it is stored unset and counted
     * in OutDropped. False without a serializer. */
    bool WriteComponentBlob(ComponentTypeID InType, void* InComponent, const LiveIdMap& InToLocal,
                            std::vector<uint8_t>& OutBlob, uint32_t& OutDropped);

    /* Loads InBlob into InComponent (already holding InType's defaults),
     * references mapped through InToLive (an id it lacks loads unset). False
     * without a serializer or on a malformed blob. */
    bool ReadComponentBlob(ComponentTypeID InType, const std::vector<uint8_t>& InBlob, const LocalIdMap& InToLive,
                           void* InComponent);

    /* InType's default-constructed bytes. False if it has no serializer. */
    bool ComponentDefaults(ComponentTypeID InType, std::vector<uint8_t>& OutBytes);

    /* Tree order holds: one root, first; every parent before its children;
     * ids unique and non-zero. OutReason says what is wrong otherwise. */
    bool ValidatePrefabTree(const PrefabAsset::Data& InData, std::string& OutReason);
}

#endif //GAMECORE_PREFABINTERNAL_H
