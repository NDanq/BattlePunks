#if WITH_EDITOR

#include "Private/Editor/EditorHistory.h"

#include <chrono>
#include <cstring>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Editor/EditorContext.h"

using GameCore::ComponentTypeID;
using GameCore::EntityGuid;
using GameCore::EntityID;
using GameCore::World;

namespace
{
    constexpr int64_t MergeWindowMs = 1000;

    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /* In-process ComponentArchive over raw field bytes in serializer order.
     * Vectors via GetX/SetX (no SIMD padding); EntityIDs as GUIDs, so
     * references survive delete/restore. */
    class SnapshotArchive final : public GameCore::ComponentArchive
    {
    public:
        /* Saving appends to InOutBytes; loading reads it from the start. */
        SnapshotArchive(World& InWorld, std::vector<uint8_t>& InOutBytes, const bool bInLoading, const uint16_t InVersion)
            : W(InWorld), Bytes(InOutBytes), bLoading(bInLoading), Version(InVersion) {}

        bool     IsLoading() const override  { return bLoading; }
        uint16_t GetVersion() const override { return Version; }

        using ComponentArchive::Field;
        void Field(const char*, float& InOut) override    { Raw(&InOut, sizeof(InOut)); }
        void Field(const char*, double& InOut) override   { Raw(&InOut, sizeof(InOut)); }
        void Field(const char*, int32_t& InOut) override  { Raw(&InOut, sizeof(InOut)); }
        void Field(const char*, uint32_t& InOut) override { Raw(&InOut, sizeof(InOut)); }
        void Field(const char*, uint8_t& InOut) override  { Raw(&InOut, sizeof(InOut)); }
        void Field(const char*, bool& InOut) override     { Raw(&InOut, sizeof(InOut)); }
        void Field(const char*, EngineCore::Color& InOut) override { Raw(&InOut, sizeof(InOut)); }
        void Flag(const char*, uint8_t& InOut) override   { Raw(&InOut, sizeof(InOut)); }

        void Field(const char*, EngineCore::Vector3& InOut) override
        {
            double X = InOut.GetX(), Y = InOut.GetY(), Z = InOut.GetZ();
            Raw(&X, sizeof(X)); Raw(&Y, sizeof(Y)); Raw(&Z, sizeof(Z));
            if(bLoading) { InOut = EngineCore::Vector3(X, Y, Z); }
        }

        void Field(const char* InName, GameCore::AABB& InOut) override
        {
            Field(InName, InOut.Min);
            Field(InName, InOut.Max);
        }

        void FixedString(const char*, char* InOutBuffer, const size_t InCapacity) override
        {
            if(InCapacity == 0) { return; }
            /* The whole buffer: same build, same capacity. */
            Raw(InOutBuffer, InCapacity);
            InOutBuffer[InCapacity - 1] = '\0';
        }

        void Entity(const char*, EntityID& InOut) override
        {
            EntityGuid Guid = bLoading ? EntityGuid() : W.GetEntity(InOut).GetGuid();
            Raw(&Guid.Hi, sizeof(Guid.Hi));
            Raw(&Guid.Lo, sizeof(Guid.Lo));
            if(bLoading)
            {
                InOut = Guid.IsSet() ? W.FindEntityByGuid(Guid) : GameCore::InvalidEntityID();
            }
        }

    private:
        void Raw(void* InOutData, const size_t InSize)
        {
            if(!bLoading)
            {
                const auto* Data = static_cast<const uint8_t*>(InOutData);
                Bytes.insert(Bytes.end(), Data, Data + InSize);
                return;
            }
            if(Cursor + InSize > Bytes.size())
            {
                /* Only a serializer that reads differently than it wrote. */
                CORE_LOG_ERROR("[Editor] undo snapshot is shorter than its serializer reads")
                Cursor = Bytes.size();
                return;
            }
            std::memcpy(InOutData, Bytes.data() + Cursor, InSize);
            Cursor += InSize;
        }

        World&                W;
        std::vector<uint8_t>& Bytes;
        size_t                Cursor = 0;
        bool                  bLoading;
        uint16_t              Version;
    };

    /* A default-constructed instance of InType -- never zeroes. */
    bool DefaultsFor(const ComponentTypeID InType, std::vector<uint8_t>& OutBytes)
    {
        const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(InType);
        if(!Info)
        {
            return false;
        }
        OutBytes.assign(Info->SizeBytes, 0);
        const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(InType);
        if(Serializer && Serializer->DefaultsFn)
        {
            Serializer->DefaultsFn(OutBytes.data());
            return true;
        }
        return GameCore::GetComponentPropertyDefaults(InType, OutBytes.data());
    }

    QString TypeName(const ComponentTypeID InType)
    {
        const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(InType);
        return Info ? QString::fromLatin1(Info->Name) : QStringLiteral("component");
    }

    /* The live entity for InGuid in the edited World, or unset with a warning
     * -- only reachable if something outside the history changed the World. */
    EntityID Resolve(World& InWorld, const EntityGuid& InGuid, const QString& InWhat)
    {
        const EntityID Found = InWorld.FindEntityByGuid(InGuid);
        if(!Found.IsSet())
        {
            CORE_LOG_WARN("[Editor] cannot " + InWhat.toStdString() + ": its entity no longer exists")
        }
        return Found;
    }
}

namespace EditorHistory
{
    // === Snapshots ===========================================================

    std::vector<ComponentTypeID> ComponentTypesOf(World& InWorld, const EntityID InEntity)
    {
        std::vector<ComponentTypeID> Types;
        if(!InWorld.IsAlive(InEntity))
        {
            return Types;
        }
        const GameCore::Entity E = InWorld.GetEntity(InEntity);
        const size_t TypeCount = GameCore::GetComponentTypeCount();
        for(size_t Index = 0; Index < TypeCount && Index < GameCore::COMPONENT_TYPE_MAX; ++Index)
        {
            if(E.HasComponentByType(ComponentTypeID(Index)))
            {
                Types.push_back(ComponentTypeID(Index));
            }
        }
        return Types;
    }

    std::vector<uint8_t> CaptureProperties(World& InWorld, const EntityID InEntity, const ComponentTypeID InType)
    {
        std::vector<uint8_t> Values;
        const auto* Component = static_cast<const uint8_t*>(InWorld.GetEntity(InEntity).GetComponentByType(InType));
        size_t Count = 0;
        const GameCore::ComponentPropertyEntry* Entries = GameCore::FindComponentProperties(InType, Count);
        if(!Component || !Entries)
        {
            return Values;
        }
        for(size_t Index = 0; Index < Count; ++Index)
        {
            const GameCore::ComponentPropertyEntry& Entry = Entries[Index];
            if(!Entry.IsReadOnly())
            {
                Values.insert(Values.end(), Component + Entry.Offset, Component + Entry.Offset + Entry.GetSpan());
            }
        }
        return Values;
    }

    bool ApplyProperties(World& InWorld, const EntityID InEntity, const ComponentTypeID InType, const std::vector<uint8_t>& InValues)
    {
        auto* Component = static_cast<uint8_t*>(InWorld.GetEntity(InEntity).GetComponentByType(InType));
        size_t Count = 0;
        const GameCore::ComponentPropertyEntry* Entries = GameCore::FindComponentProperties(InType, Count);
        if(!Component || !Entries)
        {
            return false;
        }
        size_t Cursor = 0;
        for(size_t Index = 0; Index < Count; ++Index)
        {
            const GameCore::ComponentPropertyEntry& Entry = Entries[Index];
            if(Entry.IsReadOnly())
            {
                continue;
            }
            if(Cursor + Entry.GetSpan() > InValues.size())
            {
                return false;
            }
            std::memcpy(Component + Entry.Offset, InValues.data() + Cursor, Entry.GetSpan());
            Cursor += Entry.GetSpan();
        }
        return true;
    }

    ComponentSnapshot CaptureComponent(World& InWorld, const EntityID InEntity, const ComponentTypeID InType)
    {
        ComponentSnapshot Snapshot;
        Snapshot.Type = InType;

        void* Component = InWorld.GetEntity(InEntity).GetComponentByType(InType);
        if(!Component)
        {
            return Snapshot;
        }
        if(const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(InType))
        {
            SnapshotArchive Ar(InWorld, Snapshot.Data, false, Serializer->Version);
            Serializer->Fn(Ar, Component);
            Snapshot.bSerialized = true;
        }
        else
        {
            /* No serializer: a save would drop it too. Its editable values are
             * the best there is. */
            Snapshot.Data = CaptureProperties(InWorld, InEntity, InType);
        }
        return Snapshot;
    }

    bool RestoreComponent(World& InWorld, const EntityID InEntity, const ComponentSnapshot& InSnapshot)
    {
        std::vector<uint8_t> Defaults;
        if(!InWorld.IsAlive(InEntity) || !DefaultsFor(InSnapshot.Type, Defaults))
        {
            CORE_LOG_WARN("[Editor] undo: cannot recreate a '" + TypeName(InSnapshot.Type).toStdString() + "' component")
            return false;
        }

        void* Component = InWorld.GetEntity(InEntity).AddComponentByType(InSnapshot.Type, Defaults.data());
        if(!Component)
        {
            return false;
        }
        if(InSnapshot.bSerialized)
        {
            const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(InSnapshot.Type);
            std::vector<uint8_t> Data = InSnapshot.Data;
            SnapshotArchive Ar(InWorld, Data, true, Serializer ? Serializer->Version : 0);
            if(Serializer)
            {
                Serializer->Fn(Ar, Component);
            }
        }
        else
        {
            ApplyProperties(InWorld, InEntity, InSnapshot.Type, InSnapshot.Data);
        }
        return true;
    }

    EntityMembers CaptureMembers(World& InWorld, const EntityID InEntity)
    {
        EntityMembers Members;
        const GameCore::Entity E = InWorld.GetEntity(InEntity);
        Members.Name       = E.GetName();
        Members.ScriptPath = E.GetScriptPath();
        if(const GameCore::EntityScriptState* Script = E.GetScriptState())
        {
            Members.bCullOutsideActiveRegion = Script->bCullOutsideActiveRegion;
        }
        return Members;
    }

    EntitySnapshot CaptureEntity(World& InWorld, const EntityID InEntity)
    {
        EntitySnapshot Snapshot;
        if(!InWorld.IsAlive(InEntity))
        {
            return Snapshot;
        }
        const GameCore::Entity E = InWorld.GetEntity(InEntity);
        const EntityMembers Members = CaptureMembers(InWorld, InEntity);
        Snapshot.Guid                     = E.GetGuid();
        Snapshot.ParentGuid               = InWorld.GetEntity(E.GetParent()).GetGuid();
        Snapshot.Name                     = Members.Name;
        Snapshot.ScriptPath               = Members.ScriptPath;
        Snapshot.bCullOutsideActiveRegion = Members.bCullOutsideActiveRegion;
        for(const ComponentTypeID Type : ComponentTypesOf(InWorld, InEntity))
        {
            Snapshot.Components.push_back(CaptureComponent(InWorld, InEntity, Type));
        }
        for(EntityID Child = E.GetFirstChild(); Child.IsSet(); Child = InWorld.GetEntity(Child).GetNextSibling())
        {
            Snapshot.Children.push_back(CaptureEntity(InWorld, Child));
        }
        return Snapshot;
    }

    EntityID RestoreEntity(World& InWorld, const EntitySnapshot& InSnapshot)
    {
        const GameCore::Entity E = InWorld.CreateEntity(InSnapshot.Name.c_str(), InSnapshot.Guid);
        for(const ComponentSnapshot& Component : InSnapshot.Components)
        {
            RestoreComponent(InWorld, E.GetID(), Component);
        }
        if(!InSnapshot.ScriptPath.empty())
        {
            E.SetScript(InSnapshot.ScriptPath.c_str());
            if(GameCore::EntityScriptState* Script = E.GetScriptState())
            {
                Script->bCullOutsideActiveRegion = InSnapshot.bCullOutsideActiveRegion;
            }
        }
        /* The Transform was captured local to this parent: keep it as is. */
        if(InSnapshot.ParentGuid.IsSet())
        {
            const EntityID Parent = InWorld.FindEntityByGuid(InSnapshot.ParentGuid);
            if(Parent.IsSet()) { E.SetParent(Parent, false); }
        }
        for(const EntitySnapshot& Child : InSnapshot.Children)
        {
            RestoreEntity(InWorld, Child);
        }
        InWorld.RefreshEntityBounds(E.GetID());
        return E.GetID();
    }

    // === Command =============================================================

    Command::Command(EditorContext& InContext, const QString& InText)
        : Context(InContext)
    {
        setText(InText);
    }

    void Command::redo()
    {
        if(bRecorded)
        {
            bRecorded = false;   // QUndoStack::push's call: already done
            return;
        }
        Apply(true);
    }

    void Command::undo()
    {
        Apply(false);
    }

    // === PropertyEditCommand =================================================

    PropertyEditCommand::PropertyEditCommand(EditorContext& InContext, const QString& InText, const EntityGuid& InGuid,
                                             const ComponentTypeID InType, std::vector<uint8_t> InBefore,
                                             std::vector<uint8_t> InAfter, const uint64_t InGesture)
        : Command(InContext, InText)
        , Guid(InGuid)
        , Type(InType)
        , Before(std::move(InBefore))
        , After(std::move(InAfter))
        , Gesture(InGesture)
        , TimeMs(NowMs())
    {
        setObsolete(Before == After);
    }

    bool PropertyEditCommand::mergeWith(const QUndoCommand* InOther)
    {
        const auto* Other = static_cast<const PropertyEditCommand*>(InOther);

        /* Only into the entry just recorded -- never one an undo or redo has
         * since moved past -- and only the same component. */
        if(!Context.IsLatestRecorded(this) || !(Other->Guid == Guid) || Other->Type != Type)
        {
            return false;
        }
        const bool bSameGesture = Gesture != 0 && Other->Gesture == Gesture;
        const bool bQuickEdits  = Gesture == 0 && Other->Gesture == 0 && Other->TimeMs - TimeMs <= MergeWindowMs;
        if(!bSameGesture && !bQuickEdits)
        {
            return false;
        }
        After  = Other->After;
        TimeMs = Other->TimeMs;
        setObsolete(Before == After);
        return true;
    }

    void PropertyEditCommand::Apply(const bool bInForward)
    {
        World& W = Context.GetEditedWorld();
        const EntityID Entity = Resolve(W, Guid, text());
        if(!Entity.IsSet())
        {
            return;
        }
        ApplyProperties(W, Entity, Type, bInForward ? After : Before);
        W.RefreshEntityBounds(Entity);
        Context.SetSelection(Entity);
        Context.NotifyPropertyEdited(Entity, Type);
    }

    // === EntityMembersCommand ================================================

    EntityMembersCommand::EntityMembersCommand(EditorContext& InContext, const QString& InText, const EntityGuid& InGuid,
                                               EntityMembers InBefore, EntityMembers InAfter)
        : Command(InContext, InText)
        , Guid(InGuid)
        , Before(std::move(InBefore))
        , After(std::move(InAfter))
    {
        setObsolete(Before == After);
    }

    void EntityMembersCommand::Apply(const bool bInForward)
    {
        World& W = Context.GetEditedWorld();
        const EntityID Entity = Resolve(W, Guid, text());
        if(!Entity.IsSet())
        {
            return;
        }
        const EntityMembers& Target = bInForward ? After : Before;
        Context.RenameEntity(Entity, QString::fromStdString(Target.Name));
        Context.SetEntityScript(Entity, QString::fromStdString(Target.ScriptPath));
        Context.SetEntityScriptCull(Entity, Target.bCullOutsideActiveRegion != 0);
        Context.SetSelection(Entity);
    }

    // === EntityLifetimeCommand ===============================================

    EntityLifetimeCommand::EntityLifetimeCommand(EditorContext& InContext, const QString& InText, const bool bInCreated,
                                                 EntitySnapshot InSnapshot)
        : Command(InContext, InText)
        , bCreated(bInCreated)
        , Snapshot(std::move(InSnapshot))
    {
    }

    void EntityLifetimeCommand::Apply(const bool bInForward)
    {
        World& W = Context.GetEditedWorld();
        const bool bShouldExist = bInForward == bCreated;

        if(bShouldExist)
        {
            Context.SetSelection(RestoreEntity(W, Snapshot));
            return;
        }
        const EntityID Entity = Resolve(W, Snapshot.Guid, text());
        if(Entity.IsSet())
        {
            Snapshot = CaptureEntity(W, Entity);
            Context.DestroyEntity(Entity);
        }
    }

    // === ParentCommand =======================================================

    ParentCommand::ParentCommand(EditorContext& InContext, const QString& InText, const EntityGuid& InGuid,
                                 const EntityGuid& InBeforeParent, std::vector<uint8_t> InBeforeTransform,
                                 const EntityGuid& InAfterParent, std::vector<uint8_t> InAfterTransform)
        : Command(InContext, InText)
        , Guid(InGuid)
        , BeforeParent(InBeforeParent)
        , AfterParent(InAfterParent)
        , BeforeTransform(std::move(InBeforeTransform))
        , AfterTransform(std::move(InAfterTransform))
    {
    }

    void ParentCommand::Apply(const bool bInForward)
    {
        World& W = Context.GetEditedWorld();
        const EntityID Entity = Resolve(W, Guid, text());
        if(!Entity.IsSet())
        {
            return;
        }
        const EntityGuid& ParentGuid = bInForward ? AfterParent : BeforeParent;
        const EntityID    Parent     = ParentGuid.IsSet() ? Resolve(W, ParentGuid, text()) : EntityID();
        if(ParentGuid.IsSet() && !Parent.IsSet())
        {
            return;
        }

        /* The local values recorded for that parent, not recomputed: undo
         * puts it back exactly. */
        W.GetEntity(Entity).SetParent(Parent, false);
        const std::vector<uint8_t>& Values = bInForward ? AfterTransform : BeforeTransform;
        if(!Values.empty())
        {
            ApplyProperties(W, Entity, GameCore::TransformComponent::StaticType().ID, Values);
        }
        W.RefreshEntityBounds(Entity);
        Context.SetSelection(Entity);
        Context.NotifyHierarchyEdited(Entity);
    }

    // === ComponentLifetimeCommand ============================================

    ComponentLifetimeCommand::ComponentLifetimeCommand(EditorContext& InContext, const QString& InText, const bool bInAdded,
                                                       const EntityGuid& InGuid, ComponentSnapshot InSnapshot)
        : Command(InContext, InText)
        , bAdded(bInAdded)
        , Guid(InGuid)
        , Snapshot(std::move(InSnapshot))
    {
    }

    void ComponentLifetimeCommand::Apply(const bool bInForward)
    {
        World& W = Context.GetEditedWorld();
        const EntityID Entity = Resolve(W, Guid, text());
        if(!Entity.IsSet())
        {
            return;
        }
        if(bInForward == bAdded)
        {
            RestoreComponent(W, Entity, Snapshot);
        }
        else
        {
            Snapshot = CaptureComponent(W, Entity, Snapshot.Type);
            W.GetEntity(Entity).RemoveComponentByType(Snapshot.Type);
        }
        W.RefreshEntityBounds(Entity);
        Context.SetSelection(Entity);
    }
}

#endif // WITH_EDITOR
