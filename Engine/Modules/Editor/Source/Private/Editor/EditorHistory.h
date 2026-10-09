#ifndef NIMBLEEDITOR_EDITORHISTORY_H
#define NIMBLEEDITOR_EDITORHISTORY_H

#if WITH_EDITOR

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QUndoCommand>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/Mobility.h"
#include "Public/GameCore/WorldSettings.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

class EditorDocument;

/*
 * Undo / redo commands and snapshots for the edited World, used only by
 * EditorDocument.
 *
 * ENTITIES ARE NAMED BY GUID: an undone delete recreates the entity with its
 * old GUID, so older commands still find it.
 *
 * SNAPSHOTS:
 *   Property values   the writable registered properties, back to back.
 *   Whole components  via the serializer (never raw bytes, which hold the
 *                     Owner and per-run state).
 *
 * Commands are recorded after the change, so the first redo() is a no-op.
 */
namespace EditorHistory
{
    struct ComponentSnapshot
    {
        GameCore::ComponentTypeID Type = GameCore::COMPONENT_TYPE_INVALID;
        std::vector<uint8_t>      Data;
        bool                      bSerialized = false;   // else property values over defaults
    };

    /* A prefab instance as its root's snapshot carries it (EditorHistory.cpp). */
    struct InstanceSnapshot;

    /* An entity and its whole subtree: deleting a parent deletes its
     * children, so restoring it restores them. Instance membership travels
     * too: a restored instance root brings its instance back, and a restored
     * member rejoins its instance if that is still there. */
    struct EntitySnapshot
    {
        GameCore::EntityGuid           Guid;
        GameCore::EntityGuid           ParentGuid;   // unset: a root
        std::string                    Name;
        GameCore::EMobility            Mobility = GameCore::EMobility::Dynamic;
        std::string                    Folder;       // the Outliner's (Entity::GetFolder)
        std::vector<ComponentSnapshot> Components;
        std::vector<EntitySnapshot>    Children;     // in child order

        GameCore::EntityGuid              InstanceGuid;   // the instance it is a member of; unset if loose
        uint32_t                          LocalId = 0;    // its template's id in that instance
        std::shared_ptr<InstanceSnapshot> Instance;       // set on an instance's root
    };

    /* Every component type InEntity has, in type order. */
    std::vector<GameCore::ComponentTypeID> ComponentTypesOf(GameCore::World& InWorld, GameCore::EntityID InEntity);

    /* Empty if the component or its property list is missing. */
    std::vector<uint8_t> CaptureProperties(GameCore::World& InWorld, GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);
    bool ApplyProperties(GameCore::World& InWorld, GameCore::EntityID InEntity, GameCore::ComponentTypeID InType,
                         const std::vector<uint8_t>& InValues);

    ComponentSnapshot CaptureComponent(GameCore::World& InWorld, GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    /* Adds the component (from its defaults) and loads the snapshot into it. */
    bool RestoreComponent(GameCore::World& InWorld, GameCore::EntityID InEntity, const ComponentSnapshot& InSnapshot);

    /* Entity GUID -> the entity a reference to it should now name. */
    using GuidRemap = std::unordered_map<GameCore::EntityGuid, GameCore::EntityID, GameCore::EntityGuidHash>;

    /* Loads the snapshot into InEntity's component of its type, added from
     * its defaults if missing. A reference to a GUID in InRemap names that
     * entity instead (a converted entity's replacement). */
    bool LoadComponent(GameCore::World& InWorld, GameCore::EntityID InEntity, const ComponentSnapshot& InSnapshot,
                       const GuidRemap* InRemap = nullptr);

    EntitySnapshot CaptureEntity(GameCore::World& InWorld, GameCore::EntityID InEntity);

    /* Recreates the entity with its GUID, name and components, under
     * its parent if that still exists, then its children; then the instances
     * they were in (see EntitySnapshot). */
    GameCore::EntityID RestoreEntity(GameCore::World& InWorld, const EntitySnapshot& InSnapshot);

    /*
     * Copy and paste: InSnapshots' subtrees made again as NEW entities, with
     * fresh GUIDs -- an instance member's derived from its instance's new GUID,
     * as a load derives them -- and references between them moved to the
     * copies (references to anything else are kept). A copied instance root
     * brings a new instance; a member copied without its root becomes loose,
     * as does everything if !bInKeepInstances.
     *
     * Each root goes under its snapshot's parent if that is in InWorld and in
     * no instance, its local Transform kept; otherwise it is made a root, at
     * InWorldPoses[i] (its world pose when captured). Returns the new roots.
     */
    std::vector<GameCore::EntityID> RestoreCopies(GameCore::World& InWorld, const std::vector<EntitySnapshot>& InSnapshots,
                                                  const std::vector<GameCore::TransformPose>& InWorldPoses,
                                                  bool bInKeepInstances = true);

    // === Commands ============================================================

    /* Common ground: the context, and the skipped first redo. */
    class Command : public QUndoCommand
    {
    public:
        Command(EditorDocument& InDocument, const QString& InText);

        void undo() final;
        void redo() final;

    protected:
        /* bInForward: redo; otherwise undo. */
        virtual void Apply(bool bInForward) = 0;

        EditorDocument& Document;

    private:
        bool bRecorded = true;   // the change is already in the World
    };

    /* Before/after property values. Merges within a gesture, or edits under a
     * second apart; becomes obsolete if it returns to the start. */
    class PropertyEditCommand final : public Command
    {
    public:
        enum { ID = 1 };

        PropertyEditCommand(EditorDocument& InDocument, const QString& InText, const GameCore::EntityGuid& InGuid,
                            GameCore::ComponentTypeID InType, std::vector<uint8_t> InBefore, std::vector<uint8_t> InAfter,
                            uint64_t InGesture);

        int  id() const override { return ID; }
        bool mergeWith(const QUndoCommand* InOther) override;

    protected:
        void Apply(bool bInForward) override;

    private:
        GameCore::EntityGuid      Guid;
        GameCore::ComponentTypeID Type;
        std::vector<uint8_t>      Before;
        std::vector<uint8_t>      After;
        uint64_t                  Gesture;     // 0 outside a gesture
        int64_t                   TimeMs;      // of the latest edit merged in
    };

    /* What an entity carries besides components: its name, mobility and
     * Outliner folder. */
    struct EntityMembers
    {
        std::string         Name;
        GameCore::EMobility Mobility = GameCore::EMobility::Dynamic;
        std::string         Folder;

        bool operator==(const EntityMembers& InOther) const
        {
            return Name == InOther.Name && Mobility == InOther.Mobility && Folder == InOther.Folder;
        }
    };

    EntityMembers CaptureMembers(GameCore::World& InWorld, GameCore::EntityID InEntity);

    class EntityMembersCommand final : public Command
    {
    public:
        EntityMembersCommand(EditorDocument& InDocument, const QString& InText, const GameCore::EntityGuid& InGuid,
                             EntityMembers InBefore, EntityMembers InAfter);

    protected:
        void Apply(bool bInForward) override;

    private:
        GameCore::EntityGuid Guid;
        EntityMembers        Before;
        EntityMembers        After;
    };

    /* The edited World's settings, before and after. */
    class WorldSettingsCommand final : public Command
    {
    public:
        WorldSettingsCommand(EditorDocument& InDocument, const QString& InText, const GameCore::WorldSettings& InBefore,
                             const GameCore::WorldSettings& InAfter);

    protected:
        void Apply(bool bInForward) override;

    private:
        GameCore::WorldSettings Before;
        GameCore::WorldSettings After;
    };

    /* Create (bInCreated) or delete. Snapshotted when the entity goes away. */
    class EntityLifetimeCommand final : public Command
    {
    public:
        EntityLifetimeCommand(EditorDocument& InDocument, const QString& InText, bool bInCreated, EntitySnapshot InSnapshot);

    protected:
        void Apply(bool bInForward) override;

    private:
        bool           bCreated;
        EntitySnapshot Snapshot;
    };

    /* A new parent (unset: detached). Reparenting keeps the world pose by
     * rewriting the local Transform, so its values travel too. */
    class ParentCommand final : public Command
    {
    public:
        ParentCommand(EditorDocument& InDocument, const QString& InText, const GameCore::EntityGuid& InGuid,
                      const GameCore::EntityGuid& InBeforeParent, std::vector<uint8_t> InBeforeTransform,
                      const GameCore::EntityGuid& InAfterParent, std::vector<uint8_t> InAfterTransform);

    protected:
        void Apply(bool bInForward) override;

    private:
        GameCore::EntityGuid Guid;
        GameCore::EntityGuid BeforeParent;
        GameCore::EntityGuid AfterParent;
        std::vector<uint8_t> BeforeTransform;   // Transform property values; empty without one
        std::vector<uint8_t> AfterTransform;
    };

    /* Adding (bInAdded) or removing one component, the same way. */
    class ComponentLifetimeCommand final : public Command
    {
    public:
        ComponentLifetimeCommand(EditorDocument& InDocument, const QString& InText, bool bInAdded,
                                 const GameCore::EntityGuid& InGuid, ComponentSnapshot InSnapshot);

    protected:
        void Apply(bool bInForward) override;

    private:
        bool                 bAdded;
        GameCore::EntityGuid Guid;
        ComponentSnapshot    Snapshot;
    };
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORHISTORY_H
