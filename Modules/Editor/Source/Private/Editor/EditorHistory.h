#ifndef NIMBLEEDITOR_EDITORHISTORY_H
#define NIMBLEEDITOR_EDITORHISTORY_H

#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <vector>

#include <QUndoCommand>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

class EditorContext;

/*
 * Undo / redo commands and snapshots for the edited World, used only by
 * EditorContext.
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

    /* An entity and its whole subtree: deleting a parent deletes its
     * children, so restoring it restores them. */
    struct EntitySnapshot
    {
        GameCore::EntityGuid           Guid;
        GameCore::EntityGuid           ParentGuid;   // unset: a root
        std::string                    Name;
        std::string                    ScriptPath;
        uint8_t                        bCullOutsideActiveRegion = 0;
        std::vector<ComponentSnapshot> Components;
        std::vector<EntitySnapshot>    Children;     // in child order
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

    EntitySnapshot CaptureEntity(GameCore::World& InWorld, GameCore::EntityID InEntity);

    /* Recreates the entity with its GUID, name, script and components, under
     * its parent if that still exists, then its children. */
    GameCore::EntityID RestoreEntity(GameCore::World& InWorld, const EntitySnapshot& InSnapshot);

    // === Commands ============================================================

    /* Common ground: the context, and the skipped first redo. */
    class Command : public QUndoCommand
    {
    public:
        Command(EditorContext& InContext, const QString& InText);

        void undo() final;
        void redo() final;

    protected:
        /* bInForward: redo; otherwise undo. */
        virtual void Apply(bool bInForward) = 0;

        EditorContext& Context;

    private:
        bool bRecorded = true;   // the change is already in the World
    };

    /* Before/after property values. Merges within a gesture, or edits under a
     * second apart; becomes obsolete if it returns to the start. */
    class PropertyEditCommand final : public Command
    {
    public:
        enum { ID = 1 };

        PropertyEditCommand(EditorContext& InContext, const QString& InText, const GameCore::EntityGuid& InGuid,
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

    /* What an entity carries besides components: name, script path and the
     * script's cull flag. */
    struct EntityMembers
    {
        std::string Name;
        std::string ScriptPath;
        uint8_t     bCullOutsideActiveRegion = 0;

        bool operator==(const EntityMembers& InOther) const
        {
            return Name == InOther.Name && ScriptPath == InOther.ScriptPath
                && bCullOutsideActiveRegion == InOther.bCullOutsideActiveRegion;
        }
    };

    EntityMembers CaptureMembers(GameCore::World& InWorld, GameCore::EntityID InEntity);

    class EntityMembersCommand final : public Command
    {
    public:
        EntityMembersCommand(EditorContext& InContext, const QString& InText, const GameCore::EntityGuid& InGuid,
                             EntityMembers InBefore, EntityMembers InAfter);

    protected:
        void Apply(bool bInForward) override;

    private:
        GameCore::EntityGuid Guid;
        EntityMembers        Before;
        EntityMembers        After;
    };

    /* Create (bInCreated) or delete. Snapshotted when the entity goes away. */
    class EntityLifetimeCommand final : public Command
    {
    public:
        EntityLifetimeCommand(EditorContext& InContext, const QString& InText, bool bInCreated, EntitySnapshot InSnapshot);

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
        ParentCommand(EditorContext& InContext, const QString& InText, const GameCore::EntityGuid& InGuid,
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
        ComponentLifetimeCommand(EditorContext& InContext, const QString& InText, bool bInAdded,
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
