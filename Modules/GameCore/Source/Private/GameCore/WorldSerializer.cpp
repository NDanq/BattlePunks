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
#include "Public/GameCore/World.h"

/*
 * The World walk; the format is in DESIGN.md. Saving builds the dense entity
 * table before writing any component, since slot order has holes. Loading
 * creates every entity -- with its members: name, script path -- before any
 * component, so a reference to an entity whose components come later in the
 * file still resolves.
 */

namespace GameCore
{
namespace
{
    /* Four characters each, so a hex dump of a save reads as text. */
    constexpr uint32_t TAG_WORLD     = EngineCore::MakeArchiveTag('W','R','L','D');
    constexpr uint32_t TAG_COMPONENT = EngineCore::MakeArchiveTag('C','M','P','T');
    constexpr uint32_t TAG_DATA      = EngineCore::MakeArchiveTag('D','A','T','A');

    constexpr uint16_t WORLD_CHUNK_VERSION = 1;

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

        /* The index is redundant today (always 0..count-1), written so a
         * later version can omit entities without changing what an index
         * means. Then the entity's members, which are not components. */
        for(const EntityID EntityId : OutTable.GetSavedOrder())
        {
            uint32_t Index = OutTable.IndexOf(EntityId);
            Ar << Index;

            const Entity Handle = InWorld.GetEntity(EntityId);
            std::string Name(Handle.GetName());
            std::string ScriptPath(Handle.GetScriptPath());
            const EntityScriptState* Script = Handle.GetScriptState();
            uint8_t bCull = Script ? Script->bCullOutsideActiveRegion : 0;
            Ar << Name << ScriptPath << bCull;
        }

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
                Serializer->Fn(Ar, Bytes, OutTable);

                Ar.CloseChunk();
            }

            Ar.CloseChunk();
        }

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

        if(Version > WORLD_CHUNK_VERSION)
        {
            /* Refused, unlike an unknown component type: every index refers to
             * the entity table, so a changed table invalidates everything. */
            GAMECORE_LOG_WARN("WorldSerializer: save's world chunk is version "
                              + std::to_string(Version) + "; this build understands "
                              + std::to_string(WORLD_CHUNK_VERSION))
            Ar.SetError("world chunk is newer than this build can read");
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

            std::string Name;
            std::string ScriptPath;
            uint8_t     bCull = 0;
            Ar << Name << ScriptPath << bCull;
            if(Ar.HasError()) { Ar.CloseChunk(); return false; }

            /* Named at creation, so OnEntityCreated sees the name. The script
             * is only a path here; a bound ScriptEngine instantiates it on
             * the next tick, by which time every component is in place. */
            const Entity Loaded = InWorld.CreateEntity(Name.c_str());
            if(!ScriptPath.empty())
            {
                Loaded.SetScript(ScriptPath.c_str());
                if(EntityScriptState* Script = Loaded.GetScriptState())
                {
                    Script->bCullOutsideActiveRegion = bCull ? 1 : 0;
                }
            }
            OutTable.AddForLoading(Loaded);
        }

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

                Serializer->Fn(Ar, Component, OutTable);
                Ar.CloseChunk();
            }

            Ar.CloseChunk();
            if(Ar.HasError()) { break; }
        }

        Ar.CloseChunk();

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
