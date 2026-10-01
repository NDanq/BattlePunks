#ifndef NIMBLEEDITOR_EDITORCONTEXT_H
#define NIMBLEEDITOR_EDITORCONTEXT_H

#if WITH_EDITOR

#include <cstdint>
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
 * The editor's model: the World being edited, the Renderer that draws it, the
 * selection, and Play-in-Editor. Every panel reads and writes through this
 * object and hears about changes through its signals -- no panel talks to
 * another directly.
 *
 * THREADING. The Qt main thread IS the game thread here. World and Renderer
 * are game-thread-only, and every call below happens from a Qt event, so
 * nothing needs a lock -- the same single-thread rule Application keeps with
 * SDL, with Qt's loop in place of PumpEvents.
 *
 * THE FRAME. Application::RunFrame's sequence. While editing there is no
 * simulation: the edited World never ticks, so scripts and systems never run
 * and nothing moves unless the user moves it. While playing, the game's
 * session ticks first, as it would in its own launcher.
 *
 * TWO WORLDS. GetWorld() is the ACTIVE World -- the one the panels show. It is
 * the edited World, except between StartPlay and StopPlay, when it is the
 * playing level's World. Playing never touches the edited World: the game
 * loads its own level into a World of its own, and StopPlay hands the edited
 * one back exactly as it was, selection included. Switching emits WorldReset,
 * which is what every panel already rebuilds on.
 */
class EditorContext : public QObject
{
    Q_OBJECT

public:
    explicit EditorContext(QObject* InParent = nullptr);
    ~EditorContext() override;

    /* Starts the Renderer against InWindow (the viewport's native handle) and
     * binds the current World. False if the backend refused to start; the
     * editor still runs, with a viewport that draws nothing. */
    bool InitializeRenderer(RenderCore::ERenderBackend InBackend, EngineCore::WindowHandle InWindow);

    /* Unbinds and stops the Renderer. Idempotent. The World survives until
     * the context is destroyed, so a panel reacting to a late signal never
     * finds it gone; the viewport's native window must still exist here. */
    void Shutdown();

    bool IsRendererReady() const { return bRendererReady; }

    /* The backend InitializeRenderer was asked for -- what a material has to
     * be cooked for to show in the viewport. Null before then. */
    RenderCore::ERenderBackend GetRenderBackend() const { return Backend; }

    /* Forgets material InName (empty: every material) so the viewport's next
     * frame reads it from disk again -- after a material graph was recooked.
     * Nothing when the Renderer is not running. */
    void ReloadMaterial(const QString& InName);

    /* A picture of material InName drawn by the viewport's own renderer --
     * see Renderer::RequestMaterialPreview. 0 when the Renderer is not
     * running; otherwise poll TakeMaterialPreview, which is true once, with
     * the image or (OutImage null) why there is none. */
    uint64_t RequestMaterialPreview(const QString& InName, int InSize, EngineCore::Color InTint);
    bool     TakeMaterialPreview(uint64_t InRequest, QImage& OutImage, QString& OutError);

    /* False, leaving OutStats untouched, when the Renderer is not running. */
    bool GetRenderStats(RenderCore::RenderStats& OutStats) const;

    /* The active World -- see TWO WORLDS above. Re-ask after WorldReset. */
    GameCore::World& GetWorld() { return *ActiveWorld; }

    /* The World being edited, even while playing -- what the history acts on. */
    GameCore::World& GetEditedWorld() { return *EditedWorld; }

    /*
     * One editor frame: the playing game's tick, or a refit of the edited
     * World's spatial index (edits may have moved bounds); then the fence,
     * garbage collection, present and EndFrame, in Application::RunFrame's
     * order. InView replaces the World's camera -- except while playing with
     * the view not ejected, when the game's own camera draws.
     */
    void RenderFrame(const RenderCore::RenderView& InView, EngineCore::Size InViewportPixels,
                     EngineCore::WindowHandle InWindow);

    /* Queue overlay lines for the next RenderFrame() -- see
     * Renderer::DrawDebugLine. Dropped when the Renderer is not running. */
    void DrawDebugLine(const RenderCore::DebugLine& InLine);
    void DrawDebugBox(const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels);

    // === Play-in-Editor =====================================================

    /*
     * Runs the game named by [Game] Module from its start level, in the
     * viewport, with the game's own camera (see PlaySession). The edited World
     * is set aside, not modified. False, with the reason logged, if the game
     * would not start -- the editor is then exactly as it was.
     */
    bool StartPlay();

    /* Ends the run and restores the edited World and its selection. Also
     * happens by itself when the game asks to quit. Idempotent. */
    void StopPlay();

    bool IsPlaying() const { return Play != nullptr; }

    /* "SandboxGame / Sandbox" while playing; empty otherwise. */
    QString DescribePlay() const;

    /*
     * EJECTED: while playing, the viewport shows the editor camera, with its
     * navigation and overlays, instead of the game's camera -- to look around
     * a running game. Reset by StartPlay.
     */
    void SetPlayViewEjected(bool bInEjected);
    bool IsPlayViewEjected() const { return bPlayViewEjected; }

    /* The editor camera's view and the overlays apply: editing, or ejected. */
    bool IsEditorViewActive() const { return !Play || bPlayViewEjected; }

    // === Scene ==============================================================
    //
    // Scene operations act on the EDITED World and are refused while playing.

    /* An empty World. The previous one is destroyed; the selection clears. */
    void NewScene();

    /* NewScene, then the entities Sandbox's level has once it has loaded --
     * Marker, Camera and the startup script's Scripted. See the definition for
     * where each comes from. */
    void CreateSandboxScene();

    /* Through GameCore::LoadWorldFile / SaveWorldPackage (WorldPackage.h) --
     * the World alone, not a SaveGame's GameState. Saving writes the text
     * form: the .nmworld manifest plus one .nmentity per entity in the sibling
     * folder. Loading reads either form; OutbCooked says it was a cooked
     * (binary) world, which the editor opens but should save elsewhere.
     * Script paths are kept but get no Lua instance: the editor runs no
     * scripts. False (World unchanged if the file cannot be read at all) on
     * error; a corrupt file leaves an empty World. */
    bool LoadWorld(const QString& InPath, bool* OutbCooked = nullptr);
    bool SaveWorld(const QString& InPath);

    // === Entities ===========================================================
    //
    // On the active World: while playing, these reach into the running game.

    /* Named InName, with a Transform placed at InPosition. */
    GameCore::EntityID CreateEntity(const QString& InName, const EngineCore::Vector3& InPosition);
    void               DestroyEntity(GameCore::EntityID InEntity);

    /* The entity members (Entity::SetName / SetScript). Both refresh the
     * outliner. On the edited World a script path is only stored -- it runs
     * when Play loads a World that carries it. */
    void RenameEntity(GameCore::EntityID InEntity, const QString& InName);
    void SetEntityScript(GameCore::EntityID InEntity, const QString& InPath);

    /* The script's bCullOutsideActiveRegion; nothing without a script. */
    void SetEntityScriptCull(GameCore::EntityID InEntity, bool bInCull);

    /* From the type's default-constructed bytes, as "+ Add Component" does.
     * False if it is already there or cannot be made. */
    bool AddComponent(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);
    bool RemoveComponent(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    /* Name if it has one, "Entity <index>" otherwise. */
    QString GetDisplayName(GameCore::EntityID InEntity) const;

    /*
     * The world-space box the editor shows for InEntity, best source first:
     *   BoundsComponent::World     what the spatial index and culling use
     *   a RenderComponent's quad   its corners through the Transform
     *   the Transform alone        a small cube at its position
     * An empty box for a dead entity or one with none of these.
     */
    GameCore::AABB GetEntityBounds(GameCore::EntityID InEntity) const;

    /* The union of every entity's GetEntityBounds. */
    GameCore::AABB GetSceneBounds() const;

    // === Selection ==========================================================

    GameCore::EntityID GetSelection() const { return Selection; }

    /* An unset or dead id clears the selection. Emits SelectionChanged only
     * when it actually changes. */
    void SetSelection(GameCore::EntityID InEntity);

    /* A panel wrote into InType's component on InEntity. Records the change
     * (see History) when InEntity is the selection -- the only entity whose
     * values before the write are known. */
    void NotifyPropertyEdited(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    // === History ============================================================
    //
    // Undo and redo for the EDITED World (EditorHistory.h). Every change made
    // through this object is recorded -- create, delete, rename, script, add
    // or remove a component, and property writes reported through
    // NotifyPropertyEdited -- except while playing, when changes go to the
    // running game and are not recorded, and undo and redo are refused.
    //
    // New, Open and the Sandbox scene start an empty history. The history is
    // "clean" at the last save or load; the title shows when it is not.

    QUndoStack& GetHistory() { return *History; }

    bool CanUndo() const;
    bool CanRedo() const;
    void Undo();
    void Redo();

    /* Brackets one continuous edit -- a handle drag -- so every property write
     * between them is ONE history entry, named "<InLabel> <entity>". A
     * gesture that ends where it began leaves no entry. Undo and redo are
     * refused while one is open. */
    void BeginEditGesture(const QString& InLabel);
    void EndEditGesture();

    /* True if InCommand is the entry most recently recorded and nothing has
     * been undone or redone since -- the only one a new edit may merge into. */
    bool IsLatestRecorded(const QUndoCommand* InCommand) const;

signals:
    /* The active World was replaced (new, Sandbox scene, load, Play, Stop, or
     * the game swapping its level). Every cached EntityID is stale. */
    void WorldReset();

    /* After StartPlay succeeds, and after StopPlay. */
    void PlayStateChanged(bool bInPlaying);

    void PlayViewEjectedChanged(bool bInEjected);

    /* Entities were created or destroyed, or a component was added or removed.
     * Coalesced: at most once per event-loop turn, never from inside a
     * World delegate broadcast. */
    void EntitiesChanged();

    void SelectionChanged(GameCore::EntityID InEntity);

    void PropertyEdited(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

    /* Something undo / redo depends on changed: an entry recorded, undone or
     * redone, the history cleared or saved, play started or stopped. */
    void HistoryChanged();

private:
    static void OnEntityCreated(GameCore::EntityID InEntity, void* InUserData);
    static void OnEntityDestroyed(GameCore::EntityID InEntity, void* InUserData);
    static void OnComponentChanged(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

    /* Defers EntitiesChanged to the next event-loop turn, once. */
    void QueueEntitiesChanged();

    /* Subscribe to / drop the active World's entity delegates. Unhook only
     * while that World is alive; a World the game has already freed took its
     * delegates with it, and ForgetHooks is the answer then. */
    void HookActiveWorld();
    void UnhookActiveWorld();
    void ForgetHooks();

    /* Makes InWorld the active one: hooks it, clears the selection (or sets
     * InSelection), emits WorldReset and SelectionChanged. */
    void ActivateWorld(GameCore::World* InWorld, GameCore::EntityID InSelection);

    /* After a play Tick: follows the game to a new World if it replaced its
     * level, and stops the run if the game asked to quit. */
    void SyncPlayWorld();

    /* Logs and returns true while playing -- the scene operations' guard. */
    bool RefuseWhilePlaying(const char* InWhat) const;

    /* Changes are recorded: editing (not playing), not inside an undo or redo,
     * and not while a scene is being built or loaded. */
    bool IsRecording() const;

    /* Pushes an already-applied command. Takes ownership. */
    void Record(QUndoCommand* InCommand);

    /* Records a name / script / cull change from InBefore to what InEntity
     * has now, if recording. */
    void RecordMembersChange(GameCore::EntityID InEntity, const EditorHistory::EntityMembers& InBefore, const QString& InText);

    /* Empties the history and marks it clean -- a new scene. */
    void ResetHistory();

    /* Re-takes the selection's property values: the "before" of the next
     * NotifyPropertyEdited. Called whenever they may have changed other than
     * by a recorded write. */
    void RefreshShadow();

    /* Destroys the current World (unbinding the Renderer first) and creates an
     * empty one bound in its place. Emits WorldReset. */
    void ReplaceWorld();
    void CreateWorld();
    void DestroyWorld();

    /* Resolves bundle-relative asset paths (the cooked shaders) against the
     * working directory, which EditorMain sets to the executable's folder. A
     * member because the render thread's shader loads outlive every call. */
    EngineCore::FilesystemProvider AssetProvider;

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
