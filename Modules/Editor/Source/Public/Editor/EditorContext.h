#ifndef NIMBLEEDITOR_EDITORCONTEXT_H
#define NIMBLEEDITOR_EDITORCONTEXT_H

#if WITH_EDITOR

#include <cstdint>

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

Q_DECLARE_METATYPE(GameCore::EntityID)

/*
 * The editor's model: the World being edited, the Renderer that draws it, and
 * the selection. Every panel reads and writes through this object and hears
 * about changes through its signals -- no panel talks to another directly.
 *
 * THREADING. The Qt main thread IS the game thread here. World and Renderer
 * are game-thread-only, and every call below happens from a Qt event, so
 * nothing needs a lock -- the same single-thread rule Application keeps with
 * SDL, with Qt's loop in place of PumpEvents.
 *
 * THE FRAME. Application::RunFrame's sequence, minus the simulation: the
 * editor does not tick the World (there is no play mode yet), so scripts and
 * systems never run and nothing moves unless the user moves it.
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

    /* False, leaving OutStats untouched, when the Renderer is not running. */
    bool GetRenderStats(RenderCore::RenderStats& OutStats) const;

    GameCore::World& GetWorld() { return *EditedWorld; }

    /*
     * One editor frame: refit the spatial index (edits may have moved bounds),
     * then the fence, garbage collection, present and EndFrame, in
     * Application::RunFrame's order. InView replaces the World's camera.
     */
    void RenderFrame(const RenderCore::RenderView& InView, EngineCore::Size InViewportPixels,
                     EngineCore::WindowHandle InWindow);

    /* Queue overlay lines for the next RenderFrame() -- see
     * Renderer::DrawDebugLine. Dropped when the Renderer is not running. */
    void DrawDebugLine(const RenderCore::DebugLine& InLine);
    void DrawDebugBox(const GameCore::AABB& InBox, EngineCore::Color InColor, float InThicknessPixels);

    // === Scene ==============================================================

    /* An empty World. The previous one is destroyed; the selection clears. */
    void NewScene();

    /* NewScene, then the entities Sandbox's level has once it has loaded --
     * Marker, Camera and the startup script's Scripted. See the definition for
     * where each comes from. */
    void CreateSandboxScene();

    /* Through GameCore::SerializeWorld -- the World alone, not a SaveGame's
     * GameState. Script components keep their data but get no Lua instance:
     * the editor runs no scripts. False (World unchanged on a read failure) on
     * error; a corrupt file leaves an empty World. */
    bool LoadWorld(const QString& InPath);
    bool SaveWorld(const QString& InPath);

    // === Entities ===========================================================

    /* With a Transform and a Name, placed at InPosition. */
    GameCore::EntityID CreateEntity(const QString& InName, const EngineCore::Vector3& InPosition);
    void               DestroyEntity(GameCore::EntityID InEntity);

    /* Name if it has one, "Entity <index>" otherwise. */
    QString GetDisplayName(GameCore::EntityID InEntity) const;

    /*
     * The world-space box the editor shows for InEntity, best source first:
     *   BoundsComponent::World     what the spatial index and culling use
     *   a Primitive's quad         its corners through the Transform
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

    /* A panel wrote into InType's component on InEntity. */
    void NotifyPropertyEdited(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

signals:
    /* The World was replaced (new, demo, load). Every cached EntityID is stale. */
    void WorldReset();

    /* Entities were created or destroyed, or a component was added or removed.
     * Coalesced: at most once per event-loop turn, never from inside a
     * World delegate broadcast. */
    void EntitiesChanged();

    void SelectionChanged(GameCore::EntityID InEntity);

    void PropertyEdited(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType);

private:
    static void OnEntityCreated(GameCore::EntityID InEntity, void* InUserData);
    static void OnEntityDestroyed(GameCore::EntityID InEntity, void* InUserData);
    static void OnComponentChanged(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

    /* Defers EntitiesChanged to the next event-loop turn, once. */
    void QueueEntitiesChanged();

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
    RenderCore::Renderer* Renderer    = nullptr;
    bool                  bRendererReady = false;

    GameCore::EntityID    Selection;

    GameCore::DelegateHandle CreatedHandle;
    GameCore::DelegateHandle DestroyedHandle;
    GameCore::DelegateHandle ComponentAddedHandle;
    GameCore::DelegateHandle ComponentRemovedHandle;
    bool                     bEntitiesChangedQueued = false;

    uint64_t FrameNumber     = 0;
    int64_t  PreviousFrameNs = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCONTEXT_H
