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
 * Undo / redo for the edited World: the snapshots commands hold, and the
 * commands themselves. EditorContext is the only user -- it records a
 * command wherever it changes the World (see "History" in EditorContext.h),
 * and runs undo and redo.
 *
 * ENTITIES ARE NAMED BY GUID, never EntityID. Undoing a delete recreates the
 * entity under a new EntityID but its old GUID (World::CreateEntity(name,
 * guid)), so every older command still finds it, and a save after the undo
 * writes the same .nmentity it would have before the delete.
 *
 * TWO KINDS OF SNAPSHOT:
 *   Property values   the writable fields of a component's property list
 *                     (ComponentProperties.h), back to back -- exactly what
 *                     the Details panel and the transform handles can change.
 *   Whole components  through the type's serializer, what a save holds --
 *                     for recreating a deleted entity or removed component.
 *                     Never the raw bytes: those carry the Owner header and
 *                     per-run state (a render proxy handle, a spatial index
 *                     leaf) that must be made fresh, not copied back.
 *
 * RECORDED AFTER THE FACT. The editor changes the World first and records
 * second (a property editor has already written its field when it says so),
 * so each command's first redo() -- the one QUndoStack::push runs -- does
 * nothing.
 */
namespace EditorHistory
{
    struct ComponentSnapshot
    {
        GameCore::ComponentTypeID Type = GameCore::COMPONENT_TYPE_INVALID;
        std::vector<uint8_t>      Data;
        bool                      bSerialized = false;   // else property values over defaults
    };

    struct EntitySnapshot
    {
        GameCore::EntityGuid           Guid;
        std::string                    Name;
        std::string                    ScriptPath;
        uint8_t                        bCullOutsideActiveRegion = 0;
        std::vector<ComponentSnapshot> Components;
    };

    /* Every component type InEntity has, in type order. */
    std::vector<GameCore::ComponentTypeID> ComponentTypesOf(GameCore::World& InWorld, GameCore::EntityID InEntity);

    /* The writable property values; empty if the component or its property
     * list is missing. */
    std::vector<uint8_t> CaptureProperties(GameCore::World& InWorld, GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);
    bool ApplyProperties(GameCore::World& InWorld, GameCore::EntityID InEntity, GameCore::ComponentTypeID InType,
                         const std::vector<uint8_t>& InValues);

    ComponentSnapshot CaptureComponent(GameCore::World& InWorld, GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    /* Adds the component (from its defaults) and loads the snapshot into it. */
    bool RestoreComponent(GameCore::World& InWorld, GameCore::EntityID InEntity, const ComponentSnapshot& InSnapshot);

    EntitySnapshot CaptureEntity(GameCore::World& InWorld, GameCore::EntityID InEntity);

    /* Recreates the entity with its GUID, name, script and components. */
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

    /* One component's property values, before and after. Consecutive edits
     * to the same component merge: all of one open gesture (a handle drag),
     * or edits outside any gesture less than a second apart. A merge that
     * lands back on the starting values makes the command obsolete, so a
     * cancelled drag leaves no entry. */
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

    /* Creating (bInCreated) or deleting an entity. The snapshot is taken when
     * it goes away -- at the delete, or at the undo of a create -- so it holds
     * whatever the entity had become by then. */
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
