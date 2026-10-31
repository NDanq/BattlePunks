#include "Public/GameCore/WorldSerializer.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"

/* The World walk (format: DESIGN.md). Saving builds a dense entity table
 * first; loading creates all entities before any component, so forward
 * references resolve. */

namespace GameCore
{
namespace
{
    /* Four characters each, so a hex dump of a save reads as text. */
    constexpr uint32_t TAG_WORLD     = EngineCore::MakeArchiveTag('W','R','L','D');
    constexpr uint32_t TAG_COMPONENT = EngineCore::MakeArchiveTag('C','M','P','T');
    constexpr uint32_t TAG_DATA      = EngineCore::MakeArchiveTag('D','A','T','A');
    constexpr uint32_t TAG_SETTINGS  = EngineCore::MakeArchiveTag('W','S','E','T');   // optional, after the components

    /* The entity table (index, GUID, name), a parent index per entity, the
     * prefab instance table, then the component chunks. */
    constexpr uint16_t WORLD_CHUNK_VERSION = 3;   // 2: each entity's mobility; 3: its detail mode; 1 is still read

    /* One prefab instance, as saved: its members by save index. */
    struct SavedInstance
    {
        uint64_t              GuidHi = 0, GuidLo = 0;
        std::string           Name;
        std::string           PrefabPath;
        std::string           ScriptPath;
        uint8_t               TickMode     = 0;
        uint8_t               PhaseEnabled = 0xFF;
        std::vector<uint32_t> Members;    // save indices, the root first
        std::vector<uint32_t> LocalIds;   // parallel
    };

    void SerializeSavedInstance(EngineCore::Archive& Ar, SavedInstance& InOut)
    {
        uint32_t Count = uint32_t(InOut.Members.size());
        Ar << InOut.GuidHi << InOut.GuidLo << InOut.Name << InOut.PrefabPath << InOut.ScriptPath
           << InOut.TickMode << InOut.PhaseEnabled << Count;
        if(Ar.IsLoading())
        {
            if(Ar.HasError() || Count > (Ar.GetSize() - Ar.GetCursor()) / 8)
            {
                Ar.SetError("a prefab instance's member list runs past the end of the save");
                return;
            }
            InOut.Members.resize(Count);
            InOut.LocalIds.resize(Count);
        }
        for(uint32_t Index = 0; Index < Count; ++Index)
        {
            Ar << InOut.Members[Index] << InOut.LocalIds[Index];
        }
    }

    /* Save-local index <-> live EntityID. Saving fills LiveToIndex, loading
     * fills IndexToLive; one class because IEntityRemapper serves both. */
    class EntityTable final : public IEntityRemapper
    {
    public:
        // --- saving ---------------------------------------------------------

        void AddForSaving(const EntityID InEntity)
        {
            LiveToIndex[InEntity.Pack()] = uint32_t(SavedOrder.size());
            SavedOrder.push_back(InEntity);
        }

        const std::vector<EntityID>& GetSavedOrder() const { return SavedOrder; }

        /* UNSET_INDEX if absent -- never for an owner, but possible for a
         * reference to an entity already destroyed. */
        uint32_t IndexOf(const EntityID InEntity) const
        {
            const auto Found = LiveToIndex.find(InEntity.Pack());
            return (Found == LiveToIndex.end()) ? UNSET_INDEX : Found->second;
        }

        // --- loading --------------------------------------------------------

        void AddForLoading(const EntityID InEntity) { IndexToLive.push_back(InEntity); }

        EntityID LiveAt(const uint32_t InIndex) const
        {
            return (InIndex < IndexToLive.size()) ? IndexToLive[InIndex] : InvalidEntityID();
        }

        size_t GetLoadedCount() const { return IndexToLive.size(); }

        // --- IEntityRemapper ------------------------------------------------

        void RemapEntity(EngineCore::Archive& Ar, EntityID& InOut) override
        {
            uint32_t Index = Ar.IsSaving() ? (InOut.IsSet() ? IndexOf(InOut) : UNSET_INDEX)
                                           : UNSET_INDEX;
            Ar << Index;

            if(!Ar.IsLoading() || Ar.HasError()) { return; }

            if(Index == UNSET_INDEX)
            {
                InOut = InvalidEntityID();
                return;
            }

            InOut = LiveAt(Index);
            if(!InOut.IsSet())
            {
                /* Left unset rather than failing: consumers already handle a
                 * dead handle, since entities die at any time. */
                GAMECORE_LOG_WARN("WorldSerializer: an entity reference (save index "
                                  + std::to_string(Index) + ") could not be resolved; "
                                  "leaving it unset")
            }
        }

    private:
        enum : uint32_t { UNSET_INDEX = 0xFFFFFFFFu };

        std::vector<EntityID>                  SavedOrder;    // saving
        std::unordered_map<uint64_t, uint32_t> LiveToIndex;   // saving
        std::vector<EntityID>                  IndexToLive;   // loading
    };

    /* ForEachEntity's void* payload. */
    struct CollectContext
    {
        EntityTable* Table = nullptr;
    };

    void CollectEntity(const EntityID InEntity, void* InUserData)
    {
        static_cast<CollectContext*>(InUserData)->Table->AddForSaving(InEntity);
    }

    // === Saving =============================================================

    bool SaveWorld(EngineCore::Archive& Ar, World& InWorld, EntityTable& OutTable)
    {
        uint16_t Version = WORLD_CHUNK_VERSION;
        Ar.OpenChunk(TAG_WORLD, Version);

        CollectContext Context{ &OutTable };
        InWorld.ForEachEntity(&CollectEntity, &Context);

        uint32_t EntityCount = uint32_t(OutTable.GetSavedOrder().size());
        Ar << EntityCount;

        /* The index is redundant today (0..count-1) but lets later versions
         * skip entities. Then the entity's members. */
        for(const EntityID EntityId : OutTable.GetSavedOrder())
        {
            uint32_t Index = OutTable.IndexOf(EntityId);
            Ar << Index;

            const Entity Handle = InWorld.GetEntity(EntityId);
            const EntityGuid Guid = Handle.GetGuid();
            uint64_t GuidHi = Guid.Hi;
            uint64_t GuidLo = Guid.Lo;
            Ar << GuidHi << GuidLo;

            std::string Name(Handle.GetName());
            uint8_t     Mobility = uint8_t(Handle.GetAuthoredMobility());
            uint8_t     Detail   = uint8_t(Handle.GetDetailMode());
            Ar << Name << Mobility << Detail;
        }

        /* Each entity's parent as a save index, in table order -- after the
         * whole table, so a parent later in it still resolves. */
        for(const EntityID EntityId : OutTable.GetSavedOrder())
        {
            uint32_t ParentIndex = OutTable.IndexOf(InWorld.GetEntity(EntityId).GetParent());
            Ar << ParentIndex;
        }

        /* Prefab instances, whole: every entity above is already saved in
         * full, so this only says which of them form which instance. A
         * load does not need the prefab file to rebuild them. */
        std::vector<SavedInstance> Instances;
        {
            std::vector<PrefabInstanceID> Live;
            InWorld.ForEachInstance([](const PrefabInstanceID InInstance, void* InUserData)
            {
                static_cast<std::vector<PrefabInstanceID>*>(InUserData)->push_back(InInstance);
            }, &Live);
            for(const PrefabInstanceID Id : Live)
            {
                const PrefabInstance Instance(&InWorld, Id);
                SavedInstance Saved;
                const EntityGuid Guid = Instance.GetGuid();
                Saved.GuidHi     = Guid.Hi;
                Saved.GuidLo     = Guid.Lo;
                Saved.Name       = Instance.GetName();
                Saved.PrefabPath = Instance.GetPrefabPath();
                Saved.ScriptPath = InWorld.GetInstanceScriptPath(Id);
                Saved.TickMode   = uint8_t(Instance.GetTickMode());
                Saved.PhaseEnabled = 0;
                for(int Phase = 0; Phase < TP_MAX; ++Phase)
                {
                    if(Instance.IsPhaseEnabled(ETickPhase(Phase))) { Saved.PhaseEnabled |= uint8_t(1u << Phase); }
                }
                const EntityID Root = Instance.GetRoot().GetID();
                Saved.Members.push_back(OutTable.IndexOf(Root));
                Saved.LocalIds.push_back(InWorld.GetEntity(Root).GetPrefabLocalId());
                for(const EntityID EntityId : OutTable.GetSavedOrder())
                {
                    const Entity Handle = InWorld.GetEntity(EntityId);
                    if(EntityId != Root && Handle.GetInstance() == Id)
                    {
                        Saved.Members.push_back(OutTable.IndexOf(EntityId));
                        Saved.LocalIds.push_back(Handle.GetPrefabLocalId());
                    }
                }
                Instances.push_back(std::move(Saved));
            }
        }
        uint32_t InstanceCount = uint32_t(Instances.size());
        Ar << InstanceCount;
        for(SavedInstance& Saved : Instances) { SerializeSavedInstance(Ar, Saved); }

        /* Type id order, so saving the same World twice gives identical bytes. */
        for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
        {
            ComponentPool* Pool = InWorld.FindPool(Type);
            if(!Pool || Pool->GetCount() == 0) { continue; }

            const ComponentSerializerInfo* Serializer = FindComponentSerializer(Type);
            if(!Serializer)
            {
                /* Skipped, not fatal. One warning per type per save. */
                const ComponentTypeInfo* Info = GetComponentType(Type);
                GAMECORE_LOG_WARN(std::string("WorldSerializer: no serializer registered for '")
                                  + (Info ? Info->Name : "<unknown>") + "'; "
                                  + std::to_string(Pool->GetCount()) + " component(s) will not be saved")
                continue;
            }

            const ComponentTypeInfo* Info = GetComponentType(Type);
            if(!Info) { continue; }

            uint16_t ChunkVersion = Serializer->Version;
            Ar.OpenChunk(TAG_COMPONENT, ChunkVersion);

            std::string TypeName(Info->Name);
            Ar << TypeName;

            uint32_t InstanceCount = uint32_t(Pool->GetCount());
            Ar << InstanceCount;

            for(size_t Dense = 0; Dense < Pool->GetCount(); ++Dense)
            {
                const EntityID Owner = Pool->GetEntityAt(Dense);

                uint32_t OwnerIndex = OutTable.IndexOf(Owner);
                Ar << OwnerIndex;

                /* Per instance, so a version mismatch costs one component
                 * rather than desynchronising the whole pool. */
                uint16_t DataVersion = Serializer->Version;
                Ar.OpenChunk(TAG_DATA, DataVersion);

                uint8_t* Bytes = static_cast<uint8_t*>(Pool->GetDenseData()) + Dense * Info->SizeBytes;
                BinaryComponentArchive ComponentAr(Ar, OutTable);
                Serializer->Fn(ComponentAr, Bytes);

                Ar.CloseChunk();
            }

            Ar.CloseChunk();
        }

        /* Last, so a build from before it stops at it as at any non-CMPT tag. */
        uint16_t SettingsVersion = WORLD_SETTINGS_VERSION;
        Ar.OpenChunk(TAG_SETTINGS, SettingsVersion);
        SerializeWorldSettings(Ar, InWorld.GetSettings(), SettingsVersion);
        Ar.CloseChunk();

        Ar.CloseChunk();
        return !Ar.HasError();
    }

    // === Loading ============================================================

    bool LoadWorld(EngineCore::Archive& Ar, World& InWorld, EntityTable& OutTable)
    {
        uint16_t Version = 0;
        if(!Ar.OpenChunk(TAG_WORLD, Version))
        {
            Ar.SetError("expected a WRLD chunk");
            return false;
        }

        if(Version > WORLD_CHUNK_VERSION || Version < 1)
        {
            /* Refused, unlike an unknown component type: every index refers to
             * the entity table, so a different table invalidates everything. */
            GAMECORE_LOG_WARN("WorldSerializer: save's world chunk is version "
                              + std::to_string(Version) + "; this build reads "
                              + std::to_string(WORLD_CHUNK_VERSION))
            Ar.SetError("world chunk is a version this build cannot read");
            Ar.CloseChunk();
            return false;
        }

        uint32_t EntityCount = 0;
        Ar << EntityCount;
        if(Ar.HasError()) { Ar.CloseChunk(); return false; }

        /* Every entity before any component -- see the file header. */
        for(uint32_t Ordinal = 0; Ordinal < EntityCount; ++Ordinal)
        {
            uint32_t SavedIndex = 0;
            Ar << SavedIndex;
            if(Ar.HasError()) { Ar.CloseChunk(); return false; }

            if(SavedIndex != Ordinal)
            {
                Ar.SetError("entity indices are not contiguous, which this version does not support");
                Ar.CloseChunk();
                return false;
            }

            EntityGuid  Guid;
            std::string Name;
            uint8_t     Mobility = 0;
            uint8_t     Detail   = 0;
            Ar << Guid.Hi << Guid.Lo << Name;
            if(Version >= 2) { Ar << Mobility; }
            if(Version >= 3) { Ar << Detail; }
            if(Ar.HasError()) { Ar.CloseChunk(); return false; }

            /* Name and GUID at creation, so OnEntityCreated sees both. */
            const Entity Loaded = InWorld.CreateEntity(Name.c_str(), Guid);
            if(Mobility == uint8_t(EMobility::Static) || Mobility == uint8_t(EMobility::Stationary)) { Loaded.SetMobility(EMobility(Mobility)); }
            if(Detail > 0 && Detail < DETAIL_MODE_COUNT) { Loaded.SetDetailMode(EDetailMode(Detail)); }
            OutTable.AddForLoading(Loaded);
        }

        /* Read now, applied after the components: the saved Transforms are
         * already local to these parents, so nothing should move. */
        std::vector<uint32_t> ParentIndices(EntityCount);
        for(uint32_t& ParentIndex : ParentIndices) { Ar << ParentIndex; }
        if(Ar.HasError()) { Ar.CloseChunk(); return false; }

        /* Applied last, once the tree is in place. */
        std::vector<SavedInstance> Instances;
        uint32_t InstanceCount = 0;
        Ar << InstanceCount;
        for(uint32_t Index = 0; Index < InstanceCount && !Ar.HasError(); ++Index)
        {
            SavedInstance Saved;
            SerializeSavedInstance(Ar, Saved);
            Instances.push_back(std::move(Saved));
        }
        if(Ar.HasError()) { Ar.CloseChunk(); return false; }

        /* A non-CMPT tag ends the loop; CloseChunk below skips the rest. */
        uint16_t ChunkVersion = 0;
        while(!Ar.IsAtEnd() && Ar.OpenChunk(TAG_COMPONENT, ChunkVersion))
        {
            std::string TypeName;
            Ar << TypeName;

            uint32_t InstanceCount = 0;
            Ar << InstanceCount;

            if(Ar.HasError()) { Ar.CloseChunk(); break; }

            const ComponentTypeInfo*       Info       = FindComponentType(TypeName.c_str());
            const ComponentSerializerInfo* Serializer = Info ? FindComponentSerializer(Info->ID) : nullptr;

            if(!Info || !Serializer)
            {
                /* Skipped by chunk size: a type this build lacks (RenderCore
                 * absent, a removed game component) costs only itself. */
                GAMECORE_LOG_WARN("WorldSerializer: skipping " + std::to_string(InstanceCount)
                                  + " component(s) of type '" + TypeName + "' -- "
                                  + (Info ? "no serializer registered" : "type not registered in this build"))
                Ar.CloseChunk();
                continue;
            }

            /* Default-constructed, NOT zeroed -- see DESIGN.md. Through
             * AddComponentRaw so the Owner header is written in its one place. */
            std::vector<uint8_t> Defaults(Info->SizeBytes, 0);
            if(Serializer->DefaultsFn)
            {
                Serializer->DefaultsFn(Defaults.data());
            }

            for(uint32_t Instance = 0; Instance < InstanceCount; ++Instance)
            {
                uint32_t OwnerIndex = 0;
                Ar << OwnerIndex;
                if(Ar.HasError()) { break; }

                const EntityID Owner = OutTable.LiveAt(OwnerIndex);

                uint16_t DataVersion = 0;
                if(!Ar.OpenChunk(TAG_DATA, DataVersion))
                {
                    Ar.SetError("expected a DATA chunk for a component instance");
                    break;
                }

                if(!Owner.IsSet())
                {
                    /* Skip this instance, not the whole type. */
                    GAMECORE_LOG_WARN("WorldSerializer: component of type '" + TypeName
                                      + "' names save index " + std::to_string(OwnerIndex)
                                      + ", which no entity claims; skipping it")
                    Ar.CloseChunk();
                    continue;
                }

                void* Component = Entity(&InWorld, Owner).AddComponentByType(Info->ID, Defaults.data());
                if(!Component)
                {
                    /* AddComponentByType already warned (likely a duplicate). */
                    Ar.CloseChunk();
                    continue;
                }

                BinaryComponentArchive ComponentAr(Ar, OutTable);
                Serializer->Fn(ComponentAr, Component);
                Ar.CloseChunk();
            }

            Ar.CloseChunk();
            if(Ar.HasError()) { break; }
        }

        /* A world saved before WSET keeps the defaults. */
        uint16_t SettingsVersion = 0;
        if(!Ar.HasError() && Ar.PeekChunkTag() == TAG_SETTINGS && Ar.OpenChunk(TAG_SETTINGS, SettingsVersion))
        {
            SerializeWorldSettings(Ar, InWorld.GetSettings(), SettingsVersion);
            Ar.CloseChunk();
        }

        Ar.CloseChunk();

        for(uint32_t Ordinal = 0; Ordinal < uint32_t(ParentIndices.size()); ++Ordinal)
        {
            const EntityID Parent = OutTable.LiveAt(ParentIndices[Ordinal]);
            if(Parent.IsSet())
            {
                InWorld.GetEntity(OutTable.LiveAt(Ordinal)).SetParent(Parent, false);
            }
        }

        for(const SavedInstance& Saved : Instances)
        {
            std::vector<EntityID> Members;
            for(const uint32_t Index : Saved.Members) { Members.push_back(OutTable.LiveAt(Index)); }

            /* The prefab only for its template names; the save holds the rest. */
            const PrefabAsset* Prefab = Saved.PrefabPath.empty() ? nullptr : FindOrLoadPrefab(Saved.PrefabPath.c_str());
            PrefabInstanceRecord Record;
            Record.Prefab       = Prefab;
            Record.PrefabPath   = Saved.PrefabPath.c_str();
            Record.Guid.Hi      = Saved.GuidHi;
            Record.Guid.Lo      = Saved.GuidLo;
            Record.Name         = Saved.Name.c_str();
            Record.ScriptPath   = Saved.ScriptPath.c_str();
            Record.TickMode     = Saved.TickMode <= uint8_t(EPrefabTickMode::Off) ? EPrefabTickMode(Saved.TickMode) : EPrefabTickMode::Tick;
            Record.PhaseEnabled = Saved.PhaseEnabled;
            Record.Entities     = Members.data();
            Record.LocalIds     = Saved.LocalIds.data();
            Record.Count        = Members.size();
            InWorld.RestorePrefabInstance(Record);
        }

        /* Gives every restored Bounds its World box and a leaf (ProxyID is not
         * saved). Covers bStatic too, which still needs its first insert. */
        if(!Ar.HasError())
        {
            InWorld.RefreshSpatialIndex();
        }

        return !Ar.HasError();
    }
}

bool SerializeWorld(EngineCore::Archive& Ar, World& InOutWorld)
{
    /* Mid-tick would load into pools a system is iterating, or save entities
     * whose deferred destroy has not run yet. */
    CORE_CHECK(!InOutWorld.IsTicking(),
               "SerializeWorld called while the World is ticking -- see WorldSerializer.h")
    if(InOutWorld.IsTicking()) { return false; }

    EntityTable Table;
    return Ar.IsLoading() ? LoadWorld(Ar, InOutWorld, Table)
                          : SaveWorld(Ar, InOutWorld, Table);
}

}
