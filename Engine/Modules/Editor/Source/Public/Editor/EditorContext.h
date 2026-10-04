#ifndef NIMBLEEDITOR_EDITORCONTEXT_H
#define NIMBLEEDITOR_EDITORCONTEXT_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"

namespace RenderCore { class Renderer; }

class EditorDocument;
class EditorViewportClient;
class PlaySession;
class QWidget;
class ViewportSDLInput;
namespace NimbleEngine { class InputState; }

/*
 * The editor's app-wide state: the Renderer, asset previews and
 * Play-in-Editor, and the level document (GetLevel()). What belongs to one
 * edited World -- its selection, history and entity operations -- is an
 * EditorDocument's.
 *
 * THREADING. The Qt main thread is the game thread, so nothing needs a lock.
 *
 * THE FRAME. Application::RunFrame's sequence. While editing the World never
 * ticks (no scripts, no systems); while playing, the game's session ticks first.
 *
 * THE RENDERER draws one World: the level's edited World, or the playing
 * level's between StartPlay and StopPlay. Playing lends that World to the
 * level document (EditorDocument::BorrowWorld) and never touches the edited
 * one; StopPlay hands the edited World and its selection back.
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

    /* Unbinds and stops the Renderer. Idempotent. The level lives until the
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

    /* A render target's pixels as materials sample them after the next frame's
     * captures (Renderer::RequestRenderTargetReadback): what a bake stores.
     * 0 when the Renderer is not running; poll TakeRenderTargetReadback. */
    uint64_t RequestRenderTargetReadback(const QString& InRuntimePath);
    bool     TakeRenderTargetReadback(uint64_t InRequest, EngineCore::Image& OutPixels, QString& OutError);

    /* The viewport's next frame, without debug lines (Renderer::RequestSceneCapture):
     * the "screenshot" command. 0 when the Renderer is not running; poll
     * TakeMaterialPreview. */
    uint64_t RequestSceneCapture();

    /* See Renderer::RequestMeshPreview: InMesh (a runtime path) framed by an
     * orbiting camera, with InMaterial on every section, else InSections.
     * Answered through TakeMaterialPreview. 0 when the Renderer is not running. */
    uint64_t RequestMeshPreview(const QString& InMesh, const QString& InMaterial, const QStringList& InSections,
                                int InWidth, int InHeight, float InYaw, float InPitch, float InZoom, EngineCore::Color InTint);

    /* False, leaving OutStats untouched, when the Renderer is not running. */
    bool GetRenderStats(RenderCore::RenderStats& OutStats) const;

    // === Viewports ==========================================================
    // The context drives ONE frame for every viewport, on its own timer (from
    // InitializeRenderer): the game's tick, each viewport's view and overlay,
    // then render and present in Application::RunFrame's order.

    /* InClient is drawn every frame until RemoveViewport; it shows its
     * document's World. Its destructor must call RemoveViewport while its
     * native window still exists. */
    void AddViewport(EditorViewportClient* InClient);
    void RemoveViewport(EditorViewportClient* InClient);

    /* A document other than the level shows a World it does not own (a
     * prefab simulation, EditorDocument::BorrowWorld): Unbind its scene before
     * that World goes or is swapped, Bind afterwards to draw GetWorld(). The
     * level's scene is Play-in-Editor's to manage; it is ignored here. */
    void BindDocumentScene(EditorDocument& InDocument);
    void UnbindDocumentScene(EditorDocument& InDocument);

    /* Overlay lines for this frame, into the scene of the viewport whose
     * PrepareFrame is running; nothing outside one. See Renderer::DrawDebugLine. */
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

    /* The widget whose input the game reads while playing: the viewport. Set
     * once, by its owner; StartPlay wraps it in SDL (ViewportSDLInput) so the
     * game's input runs the launcher's path. Null plays without input. */
    void SetPlayInputWidget(QWidget* InWidget) { PlayInputWidget = InWidget; }

    /* While playing WITHOUT SDL input (it could not wrap the viewport): the
     * state the viewport's Qt events feed instead. Null otherwise -- SDL has
     * the input, or nothing is playing, or the view is ejected. */
    NimbleEngine::InputState* GetPlayFallbackInput();

    /* While playing through that fallback, the game has captured the mouse:
     * the viewport hides the cursor and holds it at its centre itself,
     * reporting each move as a delta (SDL's relative mode, by hand). */
    bool IsPlayMouseHeldByViewport() const;

    // === Documents ==========================================================

    /* The level: the World the main window edits and Play-in-Editor runs in
     * place of. Owned; lives as long as the context. */
    EditorDocument& GetLevel() { return *Level; }

signals:
    /* After StartPlay succeeds, and after StopPlay. */
    void PlayStateChanged(bool bInPlaying);

    void PlayViewEjectedChanged(bool bInEjected);

    /* Every drawn frame, after the game's tick and before the render: where
     * anything else that ticks a World (a prefab simulation) does so, so its
     * render commands join this frame. */
    void BeforeFrame(double InDeltaSeconds);

private:
    /* After a play Tick: follow a level swap; stop if the game quit. */
    void SyncPlayWorld();

    /* The frame timer's tick: see Viewports. */
    void RunFrame();

    /* InDocument's scene: the default for the level, otherwise one created and
     * bound on first use and destroyed with the document. INVALID without a
     * Renderer. */
    RenderCore::RenderSceneID GetDocumentScene(EditorDocument* InDocument);
    void                      ForgetDocumentScene(EditorDocument* InDocument);

    struct DocumentScene
    {
        EditorDocument*           Document = nullptr;
        RenderCore::RenderSceneID Scene    = RenderCore::RENDER_INVALID_SCENE;
    };
    std::vector<DocumentScene>         DocumentScenes;   // documents other than the level

    /* Editor.Viewport* (the background gradient) onto every scene, when they
     * change; AppliedBackground is what was last sent ("" sends again). */
    void        ApplyBackground();
    std::string AppliedBackground;
    std::vector<EditorViewportClient*> Viewports;

    QTimer                    FrameTimer;
    QElapsedTimer             FrameClock;
    RenderCore::RenderSceneID OverlayScene = RenderCore::RENDER_INVALID_SCENE;   // see DrawDebugLine

    /* Resolves cooked shaders against the working directory (the exe's
     * folder). A member: render-thread loads outlive any call. */
    EngineCore::FilesystemProvider  AssetProvider;
    /* The disk first, then mounted shader caches: a material the editor just
     * cooked beats the last build's cache. */
    EngineCore::LayeredFileProvider LayeredAssets{ &AssetProvider, true };

    EditorDocument*       Level    = nullptr;
    RenderCore::Renderer* Renderer = nullptr;
    bool                  bRendererReady = false;
    RenderCore::ERenderBackend Backend   = RenderCore::ERenderBackend::Null;

    PlaySession*       Play = nullptr;
    QWidget*           PlayInputWidget = nullptr;
    ViewportSDLInput*  PlayInput = nullptr;   // the viewport as an SDL window, while playing
    uint32_t           PlayLoadCount = 0;   // PlaySession::GetLoadCount at the last sync
    bool               bPlayViewEjected = false;

    uint64_t FrameNumber     = 0;
    int64_t  PreviousFrameNs = 0;
};

/*
 * A view the editor draws each frame: a viewport widget, or anything else
 * with a native window, a camera and a document.
 */
class EditorViewportClient
{
public:
    virtual ~EditorViewportClient() = default;

    /* The document whose World it shows. */
    virtual EditorDocument& GetViewportDocument() = 0;

    /* The native window it presents to. */
    virtual EngineCore::WindowHandle GetViewportWindow() = 0;

    struct Frame
    {
        RenderCore::RenderView   View;
        EngineCore::Size         Pixels;   // physical
        EngineCore::WindowHandle Window;
    };

    /* Fills OutFrame and queues overlays (EditorContext::DrawDebugLine).
     * InDeltaSeconds is since the last frame. False skips it this frame (a
     * hidden or zero-sized window). bInEditorView is false while the game's
     * camera has the level: OutFrame.View is then ignored and nothing should
     * be queued. */
    virtual bool PrepareFrame(float InDeltaSeconds, bool bInEditorView, Frame& OutFrame) = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCONTEXT_H
