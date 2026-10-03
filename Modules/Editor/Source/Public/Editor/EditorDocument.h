#ifndef NIMBLEEDITOR_EDITORDOCUMENT_H
#define NIMBLEEDITOR_EDITORDOCUMENT_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <vector>

#include <QObject>
#include <QString>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; class Entity; class PrefabAsset; }

class QUndoCommand;
class QUndoStack;
namespace EditorHistory { struct EntityMembers; }

Q_DECLARE_METATYPE(GameCore::EntityID)

/*
 * One edited World and everything that belongs to editing it: the selection,
 * the undo history, and the entity operations that record into it. Panels
 * bind to a document and listen to its signals; no panel talks to another
 * directly. The level is one document (EditorContext::GetLevel()); a prefab
 * open in the Prefab Editor will be another.
 *
 * App-wide state -- the Renderer, previews, Play-in-Editor -- is
 * EditorContext's, not a document's.
 *
 * THREADING. The Qt main thread is the game thread, so nothing needs a lock.
 *
 * TWO WORLDS. GetWorld() is the ACTIVE World: the edited one, or a World
 * borrowed from someone else -- Play-in-Editor lends the playing level's
 * (BorrowWorld). A borrowed World is never recorded into the history and is
 * never owned; ReturnToEditedWorld restores the edited World and its
 * selection. Switching emits WorldReset.
 */
class EditorDocument : public QObject
{
    Q_OBJECT

public:
    explicit EditorDocument(QObject* InParent = nullptr);
    ~EditorDocument() override;

    /* The active World (see TWO WORLDS). Re-fetch after WorldReset. */
    GameCore::World& GetWorld() { return *ActiveWorld; }

    /* The edited World, even while one is borrowed. */
    GameCore::World& GetEditedWorld() { return *EditedWorld; }

    // === Borrowed World =====================================================
    // For Play-in-Editor. While borrowed, entity operations reach into the
    // borrowed World and nothing is recorded; scene operations are refused.

    /* Shows InWorld instead, with nothing selected. The edited World's
     * selection is kept for ReturnToEditedWorld. */
    void BorrowWorld(GameCore::World* InWorld);

    /* Unhooks the borrowed World while it is still alive -- before its owner
     * frees it. GetWorld() still returns it until DropBorrowedWorld. */
    void UnhookBorrowedWorld();

    /* The borrowed World was freed (or is about to be): forget it without
     * touching it. GetWorld() must not be called until BorrowWorld or
     * ReturnToEditedWorld. */
    void DropBorrowedWorld();

    /* Back to the edited World and its kept selection. */
    void ReturnToEditedWorld();

    bool IsWorldBorrowed() const { return ActiveWorld != EditedWorld; }

    // === Scene ==============================================================
    // On the EDITED World; refused while one is borrowed.

    /* Replaces the World with an empty one. */
    void NewScene();

    /* NewScene plus the entities Sandbox's loaded level has. */
    void CreateSandboxScene();

    /* Via LoadWorldFile / SaveWorldPackage (WorldPackage.h). Saves the text
     * form; loads either, with OutbCooked set for a cooked world. Scripts are
     * not instantiated. False on error: an unreadable file leaves the World
     * unchanged, a corrupt one leaves it empty. */
    bool LoadWorld(const QString& InPath, bool* OutbCooked = nullptr);
    bool SaveWorld(const QString& InPath);

    /* No edit-time ticking, but edits must still reach the spatial index and
     * overlaps. Once per editor frame while the edited World is shown. */
    void RefreshEditedWorld();

    // === Prefab =============================================================
    // A prefab document's World holds one instance of the prefab; saving
    // captures it back (CapturePrefab), keeping every template's local id.

    /* The prefab at InPath (either format), spawned into a fresh World; its
     * name, script and tick mode in OutAsset. False, with the reason logged,
     * if it cannot be read -- the World is then left as it was. */
    bool LoadPrefab(const QString& InPath, GameCore::PrefabAsset& OutAsset);

    /* Captures the one root entity's subtree into InOutAsset (its name, script
     * and tick mode kept) and writes it to InPath as text. Marks the history
     * clean. False with OutError: nothing or more than one root, a borrowed
     * World, or the file could not be written. */
    bool SavePrefab(const QString& InPath, GameCore::PrefabAsset& InOutAsset, QString& OutError);

    /* The edited World's top-level entities. A prefab saves with exactly one. */
    std::vector<GameCore::EntityID> GetRootEntities() const;

    /* SINGLE ROOT (a prefab): CreateEntity parents a new entity under the
     * first root, so everything made stays in the prefab. */
    void SetSingleRoot(bool bInSingleRoot) { bSingleRoot = bInSingleRoot; }
    bool IsSingleRoot() const { return bSingleRoot; }

    // === Entities ===========================================================
    // On the active World: while borrowed, these reach into the running game.

    /* With a Transform at InPosition. InSetup runs before recording, so the
     * entity and its components are one undo step. */
    GameCore::EntityID CreateEntity(const QString& InName, const EngineCore::Vector3& InPosition,
                                    const std::function<void(const GameCore::Entity&)>& InSetup = nullptr);
    void               DestroyEntity(GameCore::EntityID InEntity);

    /* Entity::SetName / SetScript. On the edited World a script is only stored. */
    void RenameEntity(GameCore::EntityID InEntity, const QString& InName);
    void SetEntityScript(GameCore::EntityID InEntity, const QString& InPath);

    /* The script's bCullOutsideActiveRegion; nothing without a script. */
    void SetEntityScriptCull(GameCore::EntityID InEntity, bool bInCull);

    /* Entity::SetParent, keeping the entity where it is in the world. An
     * unset InParent detaches. False if refused (a cycle) or unchanged. */
    bool SetEntityParent(GameCore::EntityID InEntity, GameCore::EntityID InParent);

    /* InEntity's parent changed outside SetEntityParent (undo/redo): the
     * Outliner's tree and the Transform's values are stale. */
    void NotifyHierarchyEdited(GameCore::EntityID InEntity);

    /* Default-constructed. False if already present or not constructible. */
    bool AddComponent(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);
    bool RemoveComponent(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    /* Name if it has one, "Entity <index>" otherwise. */
    QString GetDisplayName(GameCore::EntityID InEntity) const;

    /* World-space box, first available of: BoundsComponent::World, the
     * RenderComponent's mesh or quad through the Transform, or a small cube at
     * the Transform. Empty if none apply. */
    GameCore::AABB GetEntityBounds(GameCore::EntityID InEntity) const;

    /* The union of every entity's GetEntityBounds. */
    GameCore::AABB GetSceneBounds() const;

    // === Selection ==========================================================

    GameCore::EntityID GetSelection() const { return Selection; }

    /* An unset or dead id clears it. Emits SelectionChanged on change only. */
    void SetSelection(GameCore::EntityID InEntity);

    /* A panel wrote InType on InEntity. Recorded only for the selection, the
     * one entity whose previous values are known. */
    void NotifyPropertyEdited(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    // === History ============================================================
    // Undo/redo for the EDITED World (EditorHistory.h). Every change made
    // through this object is recorded, except while a World is borrowed, when
    // nothing is recorded and undo/redo are refused. New/Open reset it; clean
    // = saved.

    QUndoStack& GetHistory() { return *History; }

    bool CanUndo() const;
    bool CanRedo() const;
    void Undo();
    void Redo();

    /* Brackets a continuous edit (a handle drag) into one entry, "<InLabel>
     * <entity>"; none if nothing changed. Undo/redo are refused meanwhile. */
    void BeginEditGesture(const QString& InLabel);
    void EndEditGesture();

    /* InCommand is the latest entry with no undo/redo since: mergeable. */
    bool IsLatestRecorded(const QUndoCommand* InCommand) const;

signals:
    /* The active World was replaced; every cached EntityID is stale. */
    void WorldReset();

    /* The edited World is about to be freed, and a new one was just created.
     * Direct (same-thread) signals: a Renderer bound to the old World must
     * unbind before it goes. */
    void EditedWorldAboutToBeDestroyed();
    void EditedWorldCreated();

    /* Entities or components added/removed, or an entity reparented.
     * Coalesced to once per event-loop turn, never from inside a World
     * delegate. */
    void EntitiesChanged();

    void SelectionChanged(GameCore::EntityID InEntity);

    void PropertyEdited(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    /* CanUndo / CanRedo / clean state may have changed. */
    void HistoryChanged();

private:
    static void OnEntityCreated(GameCore::EntityID InEntity, void* InUserData);
    static void OnEntityDestroyed(GameCore::EntityID InEntity, void* InUserData);
    static void OnComponentChanged(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

    /* Defers EntitiesChanged to the next event-loop turn, once. */
    void QueueEntitiesChanged();

    /* The active World's entity delegates. Unhook only while it is alive;
     * ForgetHooks once its owner has freed it. */
    void HookActiveWorld();
    void UnhookActiveWorld();
    void ForgetHooks();

    /* Hooks InWorld, selects InSelection, emits WorldReset and SelectionChanged. */
    void ActivateWorld(GameCore::World* InWorld, GameCore::EntityID InSelection);

    /* Logs and returns true while a World is borrowed. */
    bool RefuseWhileBorrowed(const char* InWhat) const;

    /* Not borrowed, not in undo/redo, not building or loading a scene. */
    bool IsRecording() const;

    /* Pushes an already-applied command. Takes ownership. */
    void Record(QUndoCommand* InCommand);

    /* Records a name / script / cull change from InBefore, if recording. */
    void RecordMembersChange(GameCore::EntityID InEntity, const EditorHistory::EntityMembers& InBefore, const QString& InText);

    /* Empties the history and marks it clean. */
    void ResetHistory();

    /* Snapshots the selection's values: the "before" of the next
     * NotifyPropertyEdited. Call after any unrecorded change. */
    void RefreshShadow();

    /* Destroys the World and creates a fresh empty one. Emits WorldReset. */
    void ReplaceWorld();
    void CreateWorld();
    void DestroyWorld();

    GameCore::World*      EditedWorld = nullptr;
    GameCore::World*      ActiveWorld = nullptr;   // EditedWorld, or a borrowed one
    bool                  bSingleRoot = false;

    GameCore::EntityID    Selection;
    GameCore::EntityID    EditedSelection;         // kept while a World is borrowed

    GameCore::DelegateHandle CreatedHandle;
    GameCore::DelegateHandle DestroyedHandle;
    GameCore::DelegateHandle ComponentAddedHandle;
    GameCore::DelegateHandle ComponentRemovedHandle;
    bool                     bEntitiesChangedQueued = false;
    GameCore::World*         HookedWorld = nullptr;

    // === History ============================================================
    QUndoStack*         History = nullptr;
    const QUndoCommand* LatestRecorded = nullptr;   // see IsLatestRecorded
    bool                bApplyingHistory  = false;  // inside Undo / Redo
    bool                bSuspendRecording = false;  // building or loading a scene
    uint64_t            GestureSerial = 0;
    uint64_t            OpenGesture   = 0;          // 0: none
    QString             GestureLabel;

    /* The selection's property values as of the last recorded change. */
    struct ShadowComponent
    {
        GameCore::ComponentTypeID Type;
        std::vector<uint8_t>      Values;
    };
    GameCore::EntityID           ShadowEntity;
    std::vector<ShadowComponent> Shadow;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORDOCUMENT_H
