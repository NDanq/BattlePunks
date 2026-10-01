#ifndef NIMBLEEDITOR_EDITORCONTEXT_H
#define NIMBLEEDITOR_EDITORCONTEXT_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <vector>

#include <QImage>
#include <QObject>
#include <QString>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/RenderTypes.h"

namespace GameCore   { class World; }
namespace RenderCore { class Renderer; struct RenderView; }

class PlaySession;
class QUndoCommand;
class QUndoStack;
namespace EditorHistory { struct EntityMembers; }

Q_DECLARE_METATYPE(GameCore::EntityID)

/*
 * The editor's model: the edited World, its Renderer, the selection and
 * Play-in-Editor. Panels read and write through this object and listen to its
 * signals; no panel talks to another directly.
 *
 * THREADING. The Qt main thread is the game thread, so nothing needs a lock.
 *
 * THE FRAME. Application::RunFrame's sequence. While editing the World never
 * ticks (no scripts, no systems); while playing, the game's session ticks first.
 *
 * TWO WORLDS. GetWorld() is the ACTIVE World: the edited one, or the playing
 * level's between StartPlay and StopPlay. Playing never touches the edited
 * World; StopPlay restores it and its selection. Switching emits WorldReset.
 */
class EditorContext : public QObject
{
    Q_OBJECT

public:
    explicit EditorContext(QObject* InParent = nullptr);
    ~EditorContext() override;

    /* Starts the Renderer on InWindow and binds the current World. False if the
     * backend refused; the editor still runs with a blank viewport. */
    bool InitializeRenderer(RenderCore::ERenderBackend InBackend, EngineCore::WindowHandle InWindow);

    /* Unbinds and stops the Renderer. Idempotent. The World lives until the
     * context is destroyed; the viewport's native window must still exist. */
    void Shutdown();

    bool IsRendererReady() const { return bRendererReady; }

    /* The backend materials must be cooked for. Null before InitializeRenderer. */
    RenderCore::ERenderBackend GetRenderBackend() const { return Backend; }

    /* Reloads material InName (empty: all) from disk on the next frame, e.g.
     * after a recook. */
    void ReloadMaterial(const QString& InName);

    /* See Renderer::RequestMaterialPreview. 0 when the Renderer is not
     * running; otherwise poll TakeMaterialPreview until it returns true. */
    uint64_t RequestMaterialPreview(const QString& InName, int InSize, EngineCore::Color InTint);
    bool     TakeMaterialPreview(uint64_t InRequest, QImage& OutImage, QString& OutError);

    /* False, leaving OutStats untouched, when the Renderer is not running. */
    bool GetRenderStats(RenderCore::RenderStats& OutStats) const;

    /* The active World (see TWO WORLDS). Re-fetch after WorldReset. */
    GameCore::World& GetWorld() { return *ActiveWorld; }

    /* The edited World, even while playing. */
    GameCore::World& GetEditedWorld() { return *EditedWorld; }

    /* One editor frame: the game's tick (or a spatial refit while editing),
     * then render in Application::RunFrame's order. InView is used unless
     * playing with the view not ejected. */
    void RenderFrame(const RenderCore::RenderView& InView, EngineCore::Size InViewportPixels,
                     EngineCore::WindowHandle InWindow);

    /* Overlay lines for the next RenderFrame(); see Renderer::DrawDebugLine. */
    void DrawDebugLine(const RenderCore::DebugLine& InLine);
    void DrawDebugBox(const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels);

    // === Play-in-Editor =====================================================

    /* Runs the [Game] Module game from its start level in the viewport (see
     * PlaySession). False, with the reason logged, if it would not start. */
    bool StartPlay();

    /* Restores the edited World and selection. Also runs when the game quits.
     * Idempotent. */
    void StopPlay();

    bool IsPlaying() const { return Play != nullptr; }

    /* "SandboxGame / Sandbox" while playing; empty otherwise. */
    QString DescribePlay() const;

    /* EJECTED: while playing, show the editor camera and overlays instead of
     * the game's camera. Reset by StartPlay. */
    void SetPlayViewEjected(bool bInEjected);
    bool IsPlayViewEjected() const { return bPlayViewEjected; }

    /* The editor camera's view and the overlays apply: editing, or ejected. */
    bool IsEditorViewActive() const { return !Play || bPlayViewEjected; }

    // === Scene ==============================================================
    // On the EDITED World; refused while playing.

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

    // === Entities ===========================================================
    // On the active World: while playing, these reach into the running game.

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
    // through this object is recorded, except while playing, when nothing is
    // recorded and undo/redo are refused. New/Open reset it; clean = saved.

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

    /* After StartPlay succeeds, and after StopPlay. */
    void PlayStateChanged(bool bInPlaying);

    void PlayViewEjectedChanged(bool bInEjected);

    /* Entities or components added/removed. Coalesced to once per event-loop
     * turn, never from inside a World delegate. */
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
     * ForgetHooks once the game has freed it. */
    void HookActiveWorld();
    void UnhookActiveWorld();
    void ForgetHooks();

    /* Hooks InWorld, selects InSelection, emits WorldReset and SelectionChanged. */
    void ActivateWorld(GameCore::World* InWorld, GameCore::EntityID InSelection);

    /* After a play Tick: follow a level swap; stop if the game quit. */
    void SyncPlayWorld();

    /* Logs and returns true while playing. */
    bool RefuseWhilePlaying(const char* InWhat) const;

    /* Not playing, not in undo/redo, not building or loading a scene. */
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

    /* Destroys the World and binds a fresh empty one. Emits WorldReset. */
    void ReplaceWorld();
    void CreateWorld();
    void DestroyWorld();

    /* Resolves cooked shaders against the working directory (the exe's
     * folder). A member: render-thread loads outlive any call. */
    EngineCore::FilesystemProvider  AssetProvider;
    /* The disk first, then mounted shader caches: a material the editor just
     * cooked beats the last build's cache. */
    EngineCore::LayeredFileProvider LayeredAssets{ &AssetProvider, true };

    GameCore::World*      EditedWorld = nullptr;
    GameCore::World*      ActiveWorld = nullptr;   // EditedWorld, or the playing level's
    RenderCore::Renderer* Renderer    = nullptr;
    bool                  bRendererReady = false;
    RenderCore::ERenderBackend Backend   = RenderCore::ERenderBackend::Null;

    GameCore::EntityID    Selection;

    GameCore::DelegateHandle CreatedHandle;
    GameCore::DelegateHandle DestroyedHandle;
    GameCore::DelegateHandle ComponentAddedHandle;
    GameCore::DelegateHandle ComponentRemovedHandle;
    bool                     bEntitiesChangedQueued = false;
    GameCore::World*         HookedWorld = nullptr;

    PlaySession*       Play = nullptr;
    GameCore::EntityID EditedSelection;     // restored by StopPlay
    uint32_t           PlayLoadCount = 0;   // PlaySession::GetLoadCount at the last sync
    bool               bPlayViewEjected = false;

    uint64_t FrameNumber     = 0;
    int64_t  PreviousFrameNs = 0;

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

#endif //NIMBLEEDITOR_EDITORCONTEXT_H
