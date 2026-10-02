#ifndef GAMECORE_PREFAB_H
#define GAMECORE_PREFAB_H

#include <cstddef>
#include <cstdint>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/PrefabInstanceID.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/World.h"

/*
 * Prefabs: a tree of entity templates -- names, parents, components -- plus
 * the script and tick settings that will drive every instance (PREFABS.md).
 * A PrefabAsset is the definition; World::SpawnPrefab makes a
 * PrefabInstance of it, whose entities are ordinary entities that remember
 * which instance and which template they came from.
 *
 * Files are .nmprefab, text (the editor's XML) or binary (cooked), told
 * apart like worlds: '<' first is text. See DESIGN.md's Prefab section.
 *
 * Game thread only.
 */
namespace GameCore
{
    /* How an instance is driven (PREFABS.md section 3; stored, not yet acted
     * on): Tick runs its phase functions, EventsOnly only its events, and
     * Off is static geometry with no script at all. */
    enum class EPrefabTickMode : uint8_t
    {
        Tick = 0,
        EventsOnly,
        Off,
    };

    /* "Tick", "EventsOnly", "Off"; "Unknown" outside the enum. */
    GAMECORE_API const char* PrefabTickModeName(EPrefabTickMode InMode);
    /* False, OutMode untouched, for anything else. */
    GAMECORE_API bool ParsePrefabTickMode(const char* InName, EPrefabTickMode& OutMode);

    /* A template's id inside its prefab: stable across edits, never reused,
     * never 0. What instances, overrides and saves key on. */
    constexpr uint32_t PREFAB_NO_LOCAL_ID = 0;

    /*
     * The definition. Entities are in tree order -- the root first, every
     * parent before its children -- and each component is held as its
     * serializer's binary form, entity references as local ids, so spawning,
     * the cooked file and the text file all go through the same serializers.
     *
     * A value type that SHARES its data: copying is a reference count, and
     * the first change after a copy clones. That is how an instance keeps
     * the prefab it was spawned from at no cost, even after a reload.
     */
    class GAMECORE_API PrefabAsset
    {
    public:
        PrefabAsset();
        PrefabAsset(const PrefabAsset& InOther);
        PrefabAsset& operator=(const PrefabAsset& InOther);
        ~PrefabAsset();

        /* No entities. */
        bool IsEmpty() const;

        /* Defaults to the file's stem when loaded. */
        const char* GetName() const;
        void        SetName(const char* InName);

        /* The .lua or .script.nmgraph that drives each instance; "" if none. */
        const char* GetScriptPath() const;
        void        SetScriptPath(const char* InPath);

        EPrefabTickMode GetTickMode() const;
        void            SetTickMode(EPrefabTickMode InMode);

        // === Entities, in tree order =======================================

        size_t      GetEntityCount() const;
        uint32_t    GetEntityLocalId(size_t InIndex) const;
        /* PREFAB_NO_LOCAL_ID for the root. */
        uint32_t    GetEntityParentId(size_t InIndex) const;
        const char* GetEntityName(size_t InIndex) const;
        size_t      GetEntityComponentCount(size_t InIndex) const;
        /* The component's registered type name. */
        const char* GetEntityComponentType(size_t InIndex, size_t InComponent) const;

        /* PREFAB_NO_LOCAL_ID if empty. */
        uint32_t GetRootLocalId() const;
        /* The first entity named InName, PREFAB_NO_LOCAL_ID if none. */
        uint32_t FindLocalId(const char* InName) const;
        /* SIZE_MAX if absent. */
        size_t   FindEntityIndex(uint32_t InLocalId) const;

        /* The id the next new entity gets; ids are never reused. */
        uint32_t GetNextLocalId() const;

        /* Defined only inside GameCore (PrefabInternal.h); incomplete here. */
        struct Data;

    private:
        friend struct PrefabAccess;   // Prefab.cpp and World.cpp

        Data* D = nullptr;
    };

    /*
     * Authoring: replaces InOutAsset's entities with InRoot's subtree as it is
     * in InWorld (name, script and tick mode are kept). An entity spawned from
     * an instance of this prefab keeps its local id; any other gets a new one.
     * The root's Transform is stored as identity -- where an instance stands
     * is its placement, not part of the prefab. Components without a
     * serializer are left out, warned. A reference to an entity outside the
     * subtree is stored unset, warned. False if InRoot is not alive.
     */
    GAMECORE_API bool CapturePrefab(World& InWorld, EntityID InRoot, PrefabAsset& InOutAsset);

    // === Files ===============================================================

    /* The cooked form, both directions. */
    GAMECORE_API bool SerializePrefab(EngineCore::Archive& Ar, PrefabAsset& InOutAsset);

    /* Either format (text needs an editor build). False, with the reason
     * logged, if it cannot be read or holds no valid tree. */
    GAMECORE_API bool LoadPrefabFile(const char* InPath, PrefabAsset& OutAsset);

#if WITH_EDITOR
    /* The text form, deterministic like a text world: entities in tree
     * order, components sorted by type name. Written to "<path>.tmp" first
     * and renamed over the target. */
    GAMECORE_API bool SavePrefabFile(const char* InPath, const PrefabAsset& InAsset);

    /* InSourcePath (either format) to InCookedPath as binary. */
    GAMECORE_API bool CookPrefabFile(const char* InSourcePath, const char* InCookedPath);
#endif

    // === The registry ========================================================
    // Each path is loaded once and shared by everything that spawns it.

    /* nullptr if it cannot be loaded (logged once per call). The pointer
     * stays valid until ClearPrefabCache; ReloadPrefab changes what it
     * points at, never where. */
    GAMECORE_API const PrefabAsset* FindOrLoadPrefab(const char* InPath);

    /* Reads InPath again (after an edit). Live instances keep the version
     * they were spawned from. False, leaving the cache as it was, if the new
     * file cannot be loaded. */
    GAMECORE_API bool ReloadPrefab(const char* InPath);

    GAMECORE_API void ClearPrefabCache();

    /* An instance's entity's GUID: a hash of the instance's GUID and the
     * template's local id, so the same instance spawns the same GUIDs every
     * time. Never unset. */
    GAMECORE_API EntityGuid DerivePrefabEntityGuid(const EntityGuid& InInstanceGuid, uint32_t InLocalId);

    // === The instance handle =================================================

    /*
     * A World* and a PrefabInstanceID, like Entity. Header-only; every call
     * forwards to World.
     */
    class PrefabInstance
    {
    public:
        PrefabInstance() = default;
        PrefabInstance(World* InWorld, PrefabInstanceID InID) : OwningWorld(InWorld), ID(InID) {}

        bool IsValid() const { return OwningWorld != nullptr && ID.IsSet(); }
        bool IsAlive() const { return OwningWorld != nullptr && OwningWorld->IsInstanceAlive(ID); }

        PrefabInstanceID GetID()    const { return ID; }
        World*           GetWorld() const { return OwningWorld; }
        operator PrefabInstanceID() const { return ID; }

        /* The root entity; its Transform is the instance's placement. */
        Entity GetRoot() const { return OwningWorld ? Entity(OwningWorld, OwningWorld->GetInstanceRoot(ID)) : Entity(); }

        /* The instance's entity from template InLocalId, or from the first
         * template named InName (the template's name, whatever the entity
         * has been renamed to since). Unset if gone. */
        Entity FindEntity(uint32_t InLocalId) const { return OwningWorld ? Entity(OwningWorld, OwningWorld->FindInstanceEntity(ID, InLocalId)) : Entity(); }
        Entity FindEntity(const char* InTemplateName) const
        {
            const PrefabAsset* Prefab = GetPrefab();
            return Prefab ? FindEntity(Prefab->FindLocalId(InTemplateName)) : Entity();
        }

        /* What it was spawned from; nullptr if not alive. */
        const PrefabAsset* GetPrefab() const { return OwningWorld ? OwningWorld->GetInstancePrefab(ID) : nullptr; }
        /* The path it was spawned from; "" for an in-memory asset. */
        const char* GetPrefabPath() const { return OwningWorld ? OwningWorld->GetInstancePrefabPath(ID) : ""; }

        EntityGuid  GetGuid() const { return OwningWorld ? OwningWorld->GetInstanceGuid(ID) : EntityGuid(); }
        const char* GetName() const { return OwningWorld ? OwningWorld->GetInstanceName(ID) : ""; }
        bool        SetName(const char* InName) const { return OwningWorld && OwningWorld->SetInstanceName(ID, InName); }

        /* Off releases the script (OnDestroy); leaving Off starts it afresh.
         * EventsOnly keeps it but calls no phase function. DEFERRED in a tick. */
        EPrefabTickMode GetTickMode() const { return OwningWorld ? OwningWorld->GetInstanceTickMode(ID) : EPrefabTickMode::Tick; }
        bool            SetTickMode(EPrefabTickMode InMode) const { return OwningWorld && OwningWorld->SetInstanceTickMode(ID, InMode); }

        /* Whether InPhase's function runs (when the tick mode is Tick and the
         * script defines one). All enabled at spawn. DEFERRED in a tick. */
        bool IsPhaseEnabled(ETickPhase InPhase) const { return OwningWorld && OwningWorld->IsInstancePhaseEnabled(ID, InPhase); }
        bool SetPhaseEnabled(ETickPhase InPhase, bool bInEnabled) const { return OwningWorld && OwningWorld->SetInstancePhaseEnabled(ID, InPhase, bInEnabled); }

        /* Destroys the root, and with it every entity. DEFERRED in a tick. */
        void Destroy() const { if(OwningWorld) { OwningWorld->DestroyPrefabInstance(ID); } }

        friend bool operator==(const PrefabInstance& L, const PrefabInstance& R) { return L.OwningWorld == R.OwningWorld && L.ID == R.ID; }
        friend bool operator!=(const PrefabInstance& L, const PrefabInstance& R) { return !(L == R); }

    private:
        World*           OwningWorld = nullptr;
        PrefabInstanceID ID;
    };
}

#endif //GAMECORE_PREFAB_H
