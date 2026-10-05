#if WITH_EDITOR

#include "Public/Editor/EditorContext.h"

#include <algorithm>
#include <chrono>
#include <string>

#include "CoreQtConversions.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/RenderCore/Renderer.h"
#include "Private/Editor/PlaySession.h"
#include "Private/Editor/PlayProcesses.h"
#include "Public/NimbleEngine/GameModule.h"
#include "Private/Editor/ViewportSDLInput.h"

#include <QColor>

#include "Public/Core/CoreConfig.h"

/* The editor viewports' background (Edit > Editor Settings > Viewport): a
 * vertical gradient, top to bottom, or the top colour alone. "#rrggbb". */
NIMBLE_CONFIG_BOOL(GEditorViewportGradient,     "Editor", "ViewportGradient",    true,      nullptr);
NIMBLE_CONFIG_STRING(GEditorViewportTopColor,   "Editor", "ViewportTopColor",    "#4b515d", nullptr);
NIMBLE_CONFIG_STRING(GEditorViewportBottomColor, "Editor", "ViewportBottomColor", "#1b1d22", nullptr);

EditorContext::EditorContext(QObject* InParent)
    : QObject(InParent)
{
    /* Before anything asks for an asset -- the Renderer loads its cooked
     * shaders through this on the render thread. */
    EngineCore::SetFileProvider(&LayeredAssets);

    Level = new EditorDocument(this);

    FrameTimer.setTimerType(Qt::PreciseTimer);
    FrameTimer.setInterval(16);
    connect(&FrameTimer, &QTimer::timeout, this, &EditorContext::RunFrame);

    /* The Renderer follows the level's edited World as New/Open replace it.
     * While playing the edited World is never replaced (the document refuses),
     * and the Renderer is the game's. */
    connect(Level, &EditorDocument::EditedWorldAboutToBeDestroyed, this, [this]()
    {
        if(Renderer && !Play)
        {
            Renderer->UnbindWorld();
        }
    });
    connect(Level, &EditorDocument::EditedWorldCreated, this, [this]()
    {
        if(bRendererReady && !Play)
        {
            Renderer->BindWorld(Level->GetEditedWorld());
        }
    });
}

EditorContext::~EditorContext()
{
    Shutdown();   // stops a run first

    /* Explicitly, while this context is still whole: its World-replacement
     * hooks run as the document frees its World. */
    delete Level;
    Level = nullptr;

    /* Last: the render thread's shader loads read through it until Shutdown. */
    EngineCore::SetFileProvider(nullptr);
}

bool EditorContext::InitializeRenderer(const RenderCore::ERenderBackend InBackend, const EngineCore::WindowHandle InWindow)
{
    if(Renderer)
    {
        return bRendererReady;
    }

    Backend  = InBackend;
    Renderer = EngineCore::Memory::Alloc<RenderCore::Renderer>();
    bRendererReady = Renderer->Initialize(InBackend, InWindow);
    if(!bRendererReady)
    {
        CORE_LOG_ERROR("[Editor] RenderCore failed to initialize; the viewport will not draw")
        return false;
    }

    Renderer->BindWorld(Level->GetEditedWorld());
    Renderer->SetViewMode(RenderCore::RENDER_DEFAULT_SCENE, ViewMode, ViewTarget.toStdString());
    AppliedBackground.clear();   // ApplyBackground sets every scene's on the first frame

    FrameClock.start();
    FrameTimer.start();
    return true;
}

void EditorContext::ReloadMaterial(const QString& InName)
{
    if(Renderer && bRendererReady)
    {
        Renderer->ReloadMaterial(InName.toUtf8().constData());
    }
}

uint64_t EditorContext::RequestMaterialPreview(const QString& InName, const int InSize, const EngineCore::Color InTint,
                                              const RenderCore::EMaterialPreviewChannel InChannel)
{
    if(!Renderer || !bRendererReady)
    {
        return 0;
    }
    return Renderer->RequestMaterialPreview(InName.toUtf8().constData(), InSize, InTint, InChannel);
}

uint64_t EditorContext::RequestMeshPreview(const QString& InMesh, const QString& InMaterial, const QStringList& InSections,
                                           const int InWidth, const int InHeight, const float InYaw, const float InPitch,
                                           const float InZoom, const EngineCore::Color InTint)
{
    if(!Renderer || !bRendererReady)
    {
        return 0;
    }
    RenderCore::Renderer::MeshPreview Preview;
    Preview.Mesh     = InMesh.toStdString();
    Preview.Material = InMaterial.toStdString();
    for(const QString& Each : InSections) { Preview.Sections.push_back(Each.toStdString()); }
    Preview.Width  = InWidth;
    Preview.Height = InHeight;
    Preview.Yaw    = InYaw;
    Preview.Pitch  = InPitch;
    Preview.Zoom   = InZoom;
    Preview.Tint   = InTint;
    return Renderer->RequestMeshPreview(Preview);
}

bool EditorContext::TakeMaterialPreview(const uint64_t InRequest, QImage& OutImage, QString& OutError)
{
    if(!Renderer)
    {
        return false;
    }

    EngineCore::Image Pixels;
    std::string       Error;
    if(!Renderer->TakeMaterialPreview(InRequest, Pixels, Error))
    {
        return false;
    }
    OutImage = Pixels.GetWidth() > 0 ? EngineCore::QtBridge::ToQImage(Pixels) : QImage();
    OutError = QString::fromUtf8(Error.c_str());
    return true;
}

void EditorContext::SetViewMode(const RenderCore::ERenderViewMode InMode, const QString& InTarget)
{
    ViewMode   = InMode;
    ViewTarget = InMode == RenderCore::ERenderViewMode::RenderTarget ? InTarget : QString();
    if(Renderer && bRendererReady)
    {
        Renderer->SetViewMode(RenderCore::RENDER_DEFAULT_SCENE, ViewMode, ViewTarget.toStdString());
    }
}

QStringList EditorContext::GetViewableTargets() const
{
    QStringList Names;
    if(Renderer && bRendererReady)
    {
        for(const std::string& Each : Renderer->GetViewableTargets(RenderCore::RENDER_DEFAULT_SCENE))
        {
            Names << QString::fromStdString(Each);
        }
    }
    return Names;
}

uint64_t EditorContext::RequestSceneCapture()
{
    if(!Renderer || !bRendererReady)
    {
        return 0;
    }
    return Renderer->RequestSceneCapture();
}

uint64_t EditorContext::RequestSceneCapture(const RenderCore::RenderSceneID InScene)
{
    if(!Renderer || !bRendererReady)
    {
        return 0;
    }
    return Renderer->RequestSceneCapture(InScene);
}

uint64_t EditorContext::RequestRenderTargetReadback(const QString& InRuntimePath)
{
    if(!Renderer || !bRendererReady)
    {
        return 0;
    }
    return Renderer->RequestRenderTargetReadback(InRuntimePath.toStdString());
}

bool EditorContext::TakeRenderTargetReadback(const uint64_t InRequest, EngineCore::Image& OutPixels, QString& OutError)
{
    std::string Error;
    if(!Renderer || !Renderer->TakeMaterialPreview(InRequest, OutPixels, Error))
    {
        return false;
    }
    OutError = QString::fromStdString(Error);
    return true;
}

void EditorContext::Shutdown()
{
    /* First: the game's level and module go while the Renderer still runs. */
    StopPlay();
    UnloadAuthoringGame();
    FrameTimer.stop();

    /* The Renderer's Shutdown destroys every scene; the documents stay. */
    for(const DocumentScene& Each : DocumentScenes)
    {
        disconnect(Each.Document, nullptr, this, nullptr);
    }
    DocumentScenes.clear();

    /* The Renderer first: Shutdown() unbinds the World and drains the scene,
     * which needs the World still alive. Null-safe on a failed Initialize. */
    if(Renderer)
    {
        Renderer->Shutdown();
        EngineCore::Memory::Dealloc(Renderer);
        Renderer = nullptr;
    }
    bRendererReady = false;
}

bool EditorContext::GetRenderStats(RenderCore::RenderStats& OutStats) const
{
    if(!bRendererReady)
    {
        return false;
    }
    Renderer->GetStats(OutStats);
    return true;
}

// === Viewports ===============================================================

void EditorContext::AddViewport(EditorViewportClient* InClient)
{
    if(InClient && std::find(Viewports.begin(), Viewports.end(), InClient) == Viewports.end())
    {
        Viewports.push_back(InClient);
    }
}

void EditorContext::RemoveViewport(EditorViewportClient* InClient)
{
    const auto It = std::find(Viewports.begin(), Viewports.end(), InClient);
    if(It == Viewports.end())
    {
        return;
    }
    Viewports.erase(It);

    /* Its swapchain, while the native window it was made for still exists. */
    if(bRendererReady)
    {
        Renderer->ReleaseWindow(InClient->GetViewportWindow());
    }
}

void EditorContext::ApplyBackground()
{
    /* Read once a frame; sent only when something changed (or a scene is new). */
    const std::string Key = std::string(GEditorViewportGradient.GetBool() ? "1" : "0") + GEditorViewportTopColor.GetString()
                          + "|" + GEditorViewportBottomColor.GetString();
    if(Key == AppliedBackground)
    {
        return;
    }
    AppliedBackground = Key;

    const auto ColorOf = [](const std::string& InText, const EngineCore::Color InFallback)
    {
        const QColor Parsed(QString::fromStdString(InText));
        return Parsed.isValid() ? EngineCore::Color(uint8_t(Parsed.red()), uint8_t(Parsed.green()), uint8_t(Parsed.blue()))
                                : InFallback;
    };
    const EngineCore::Color Top    = ColorOf(GEditorViewportTopColor.GetString(), EngineCore::Color(75, 81, 93));
    const EngineCore::Color Bottom = ColorOf(GEditorViewportBottomColor.GetString(), EngineCore::Color(27, 29, 34));
    const auto Apply = [&](const RenderCore::RenderSceneID InScene)
    {
        if(GEditorViewportGradient.GetBool()) { Renderer->SetClearGradient(InScene, Top, Bottom); }
        else                                  { Renderer->SetClearColor(InScene, Top); }
    };
    Apply(RenderCore::RENDER_DEFAULT_SCENE);
    for(const DocumentScene& Each : DocumentScenes)
    {
        Apply(Each.Scene);
    }
}

void EditorContext::RunFrame()
{
    if(!bRendererReady)
    {
        return;
    }
    ApplyBackground();
    Renderer->BeginGameFrame();   // "stat unit": the editor's and the game's work from here
    const float ViewDeltaSeconds = float(FrameClock.restart()) / 1000.0f;

    /* Each viewport's view and overlays, into its document's scene. The level's
     * view is the editor camera's only while the editor view is active. */
    struct Prepared
    {
        RenderCore::RenderSceneID   Scene  = RenderCore::RENDER_INVALID_SCENE;
        bool                        bLevel = false;
        EditorViewportClient::Frame Frame;
    };
    std::vector<Prepared> Frames;
    EngineCore::Size      LevelPixels;
    for(EditorViewportClient* Client : std::vector<EditorViewportClient*>(Viewports))
    {
        EditorDocument& Document = Client->GetViewportDocument();
        Prepared Each;
        Each.bLevel = &Document == Level;
        Each.Scene  = GetDocumentScene(&Document);
        if(Each.Scene == RenderCore::RENDER_INVALID_SCENE)
        {
            continue;
        }

        OverlayScene = Each.Scene;
        const bool bDrawn = Client->PrepareFrame(ViewDeltaSeconds, !Each.bLevel || IsEditorViewActive(), Each.Frame);
        OverlayScene = RenderCore::RENDER_INVALID_SCENE;
        if(!bDrawn || Each.Frame.Pixels.IsEmpty())
        {
            continue;
        }
        if(Each.bLevel && LevelPixels.IsEmpty())
        {
            LevelPixels = Each.Frame.Pixels;
        }
        Frames.push_back(Each);
    }
    if(Frames.empty())
    {
        return;   // every window hidden or zero-sized: no frame at all
    }

    using Clock = std::chrono::steady_clock;
    const int64_t NowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    const double  DeltaSeconds = FrameNumber == 0 ? 0.0 : double(NowNs - PreviousFrameNs) / 1'000'000'000.0;

    /* The game's frame, where Application::RunFrame has it: before the fence,
     * so its render commands join this frame. Paused while the level's
     * viewport is hidden: the game has nowhere to draw. */
    if(Play)
    {
        if(!LevelPixels.IsEmpty())
        {
            /* Unhook before the swap frees the World; SyncPlayWorld hooks the new one. */
            if(Play->IsLevelSwapPending())
            {
                Level->UnhookBorrowedWorld();
            }
            /* What SDL has queued reaches the router just before the game latches it. */
            if(PlayInput)
            {
                PlayInput->Pump();
            }
            Play->SetInputBlocked(bPlayViewEjected);
            Play->Tick(DeltaSeconds, LevelPixels);
            SyncPlayWorld();
        }
    }

    /* No Tick, but edits must still reach the spatial index -- and a
     * Collision whose Source was just set to the mesh takes its shape now
     * (no scripts run here, so its events go nowhere). */
    else
    {
        Level->RefreshEditedWorld();
    }
    emit BeforeFrame(DeltaSeconds);
    for(const DocumentScene& Each : DocumentScenes)
    {
        Each.Document->RefreshEditedWorld();
    }

    /* Application::RunFrame's order: fence, collect, present, kick. Each
     * window shows its own scene. */
    Renderer->WaitForPreviousFrame();
    EngineCore::Memory::CollectGarbage();
    for(const Prepared& Each : Frames)
    {
        Renderer->PresentToWindow(Each.Scene, Each.Frame.Window);
    }

    /* A scene nobody shows this frame is not rendered at all. */
    for(const DocumentScene& Each : DocumentScenes)
    {
        const bool bShown = std::any_of(Frames.begin(), Frames.end(),
                                        [&Each](const Prepared& InFrame) { return InFrame.Scene == Each.Scene; });
        if(!bShown)
        {
            Renderer->SetViewportPixels(Each.Scene, EngineCore::Size());
        }
    }

    /* One view per scene: two viewports on one document show the last one's. */
    for(const Prepared& Each : Frames)
    {
        if(Each.Scene != RenderCore::RENDER_DEFAULT_SCENE)
        {
            Renderer->SetViewportPixels(Each.Scene, Each.Frame.Pixels);
        }
        /* Null hands the level's view back to the World's CameraComponent: the game's. */
        const bool bEditorView = !Each.bLevel || IsEditorViewActive();
        Renderer->SetViewOverride(Each.Scene, bEditorView ? &Each.Frame.View : nullptr);
    }

    RenderCore::FrameInfo Info;
    Info.FrameNumber    = FrameNumber;
    Info.DeltaSeconds   = DeltaSeconds;
    Info.ViewportPixels = LevelPixels;
    Renderer->EndFrame(Info);

    PreviousFrameNs = NowNs;
    ++FrameNumber;
}

RenderCore::RenderSceneID EditorContext::GetDocumentScene(EditorDocument* InDocument)
{
    if(InDocument == Level)
    {
        return RenderCore::RENDER_DEFAULT_SCENE;
    }
    for(const DocumentScene& Each : DocumentScenes)
    {
        if(Each.Document == InDocument)
        {
            return Each.Scene;
        }
    }
    if(!bRendererReady)
    {
        return RenderCore::RENDER_INVALID_SCENE;
    }

    const RenderCore::RenderSceneID Scene = Renderer->CreateScene();
    if(Scene == RenderCore::RENDER_INVALID_SCENE)
    {
        return Scene;
    }
    Renderer->BindWorld(Scene, InDocument->GetWorld());
    DocumentScenes.push_back({ InDocument, Scene });
    AppliedBackground.clear();   // the new scene gets it next frame

    /* The scene follows the document's edited World as New/Open replace it,
     * and goes with the document. A World is bound to one scene at a time,
     * which is why scenes are per document, not per viewport. */
    connect(InDocument, &EditorDocument::EditedWorldAboutToBeDestroyed, this, [this, Scene]()
    {
        if(bRendererReady) { Renderer->UnbindWorld(Scene); }
    });
    connect(InDocument, &EditorDocument::EditedWorldCreated, this, [this, Scene, InDocument]()
    {
        if(bRendererReady) { Renderer->BindWorld(Scene, InDocument->GetEditedWorld()); }
    });
    connect(InDocument, &QObject::destroyed, this, [this, InDocument]()
    {
        ForgetDocumentScene(InDocument);
    });
    return Scene;
}

void EditorContext::BindDocumentScene(EditorDocument& InDocument)
{
    if(&InDocument == Level || !bRendererReady)
    {
        return;
    }
    for(const DocumentScene& Each : DocumentScenes)
    {
        if(Each.Document == &InDocument)
        {
            Renderer->UnbindWorld(Each.Scene);
            Renderer->BindWorld(Each.Scene, InDocument.GetWorld());
            return;
        }
    }
}

void EditorContext::UnbindDocumentScene(EditorDocument& InDocument)
{
    if(&InDocument == Level || !bRendererReady)
    {
        return;
    }
    for(const DocumentScene& Each : DocumentScenes)
    {
        if(Each.Document == &InDocument)
        {
            Renderer->UnbindWorld(Each.Scene);
            return;
        }
    }
}

void EditorContext::ForgetDocumentScene(EditorDocument* InDocument)
{
    for(auto It = DocumentScenes.begin(); It != DocumentScenes.end(); ++It)
    {
        if(It->Document == InDocument)
        {
            /* Its World was unbound by EditedWorldAboutToBeDestroyed. */
            if(bRendererReady)
            {
                Renderer->DestroyScene(It->Scene);
            }
            DocumentScenes.erase(It);
            return;
        }
    }
}

void EditorContext::DrawDebugLine(const RenderCore::DebugLine& InLine)
{
    if(bRendererReady && OverlayScene != RenderCore::RENDER_INVALID_SCENE)
    {
        Renderer->DrawDebugLine(OverlayScene, InLine);
    }
}

void EditorContext::DrawDebugBox(const GameCore::AABB& InBox, const EngineCore::Color InColor, const float InThicknessPixels)
{
    if(bRendererReady && OverlayScene != RenderCore::RENDER_INVALID_SCENE)
    {
        Renderer->DrawDebugBox(OverlayScene, InBox, InColor, InThicknessPixels);
    }
}

// === Play-in-Editor ==========================================================

bool EditorContext::StartPlay(const QString& InOpenWorld)
{
    if(Play)
    {
        return true;
    }
    if(!bRendererReady)
    {
        CORE_LOG_ERROR("[PIE] the renderer is not running")
        return false;
    }

    /* A client of a server this machine runs: start it first. The viewport's
     * client keeps trying to connect while the server comes up. */
    const PlayNetSettings NetSettings = PlayNetSettings::FromConfig();
    if(!PlayNet) { PlayNet = new PlayProcesses(this); }
    if(NetSettings.Mode == NimbleEngine::ENetMode::Client && NetSettings.bLaunchServer)
    {
        if(!PlayNet->StartServer(NetSettings.Port))
        {
            CORE_LOG_WARN("[PIE] no dedicated server started; connecting to " + NetSettings.ServerAddress + " anyway")
        }
    }

    /* The run loads the game module itself; the editor's copy makes way. */
    UnloadAuthoringGame();

    /* Set the edited World aside: the session binds its own level's World. */
    Renderer->UnbindWorld();

    Play = EngineCore::Memory::Alloc<PlaySession>(*Renderer);
    if(!Play->Start(InOpenWorld.toStdString()))
    {
        EngineCore::Memory::Dealloc(Play);
        Play = nullptr;
        PlayNet->StopAll();
        ReloadAuthoringGame();

        Renderer->BindWorld(Level->GetEditedWorld());
        return false;
    }

    /* More clients, each a window of its own. */
    for(int Index = 0; Index < NetSettings.ClientWindows && IsPlayNetworked(); ++Index)
    {
        LaunchPlayClientWindow();
    }

    /* The game's input takes the launcher's path: the viewport as an SDL
     * window, into the session's router. Without it, the viewport's Qt events
     * feed the router instead (GetPlayFallbackInput). */
    if(PlayInputWidget)
    {
        PlayInput = new ViewportSDLInput(this);
        if(PlayInput->Start(PlayInputWidget))
        {
            Play->AttachInputWindow(PlayInput->GetWindow());
            CORE_LOG("[PIE] input: SDL, through the viewport")
        }
        else
        {
            delete PlayInput;
            PlayInput = nullptr;
            CORE_LOG_WARN("[PIE] input: SDL could not take the viewport; using its Qt events")
        }
    }

    PlayLoadCount    = Play->GetLoadCount();
    bPlayViewEjected = false;
    Level->BorrowWorld(Play->GetWorld());

    emit PlayStateChanged(true);
    return true;
}

void EditorContext::StopPlay()
{
    if(!Play)
    {
        return;
    }

    /* The playing World is still alive here; Stop frees it. */
    Level->UnhookBorrowedWorld();
    Level->DropBorrowedWorld();

    /* Before the run: the router detaches from the wrapped window, then SDL
     * lets the viewport go. */
    Play->GetInputRouter().Detach();
    if(PlayInput)
    {
        PlayInput->Stop();
        delete PlayInput;
        PlayInput = nullptr;
    }

    Play->Stop();   // unbinds the renderer from the game's World
    EngineCore::Memory::Dealloc(Play);
    Play = nullptr;
    bPlayViewEjected = false;

    /* The editor's copy of the module comes back for editing. */
    ReloadAuthoringGame();

    /* The run's server and client windows end with it. */
    if(PlayNet) { PlayNet->StopAll(); }

    if(bRendererReady)
    {
        Renderer->BindWorld(Level->GetEditedWorld());
    }
    Level->ReturnToEditedWorld();

    emit PlayStateChanged(false);
}

void EditorContext::SyncPlayWorld()
{
    /* The game replaced its World (e.g. a save restore). Compare load counts,
     * not pointers, which can be reused. */
    const bool bReplaced = Play->GetLoadCount() != PlayLoadCount;
    if(bReplaced)
    {
        PlayLoadCount = Play->GetLoadCount();
        Level->DropBorrowedWorld();
    }

    /* Quit requested or no World left: stop now, before the fence, as
     * Application would; the rest of the frame draws the edited World. */
    if(Play->IsQuitRequested() || !Play->GetWorld())
    {
        CORE_LOG("[PIE] the game ended its session; stopping")
        StopPlay();
        return;
    }

    if(bReplaced)
    {
        Level->BorrowWorld(Play->GetWorld());
    }
}

QString EditorContext::DescribePlay() const
{
    return Play ? QString::fromStdString(Play->Describe()) : QString();
}

NimbleEngine::InputState* EditorContext::GetPlayFallbackInput()
{
    return Play && !PlayInput && !bPlayViewEjected ? &Play->GetInputRouter().GetPending() : nullptr;
}

bool EditorContext::IsPlayMouseHeldByViewport() const
{
    return Play && !PlayInput && !bPlayViewEjected && Play->WantsViewportMouseHold();
}

void EditorContext::SetPlayViewEjected(const bool bInEjected)
{
    const bool bEjected = Play && bInEjected;
    if(bEjected == bPlayViewEjected)
    {
        return;
    }
    bPlayViewEjected = bEjected;
    emit PlayViewEjectedChanged(bPlayViewEjected);
}



bool EditorContext::IsPlayNetworked() const
{
    return Play && Play->GetNetMode() != NimbleEngine::ENetMode::Standalone;
}

bool EditorContext::LaunchPlayClientWindow()
{
    if(!IsPlayNetworked())
    {
        CORE_LOG_ERROR("[PIE] a client window needs a networked Play: set Editor Settings > Play > Network mode, then Play")
        return false;
    }
    if(!PlayNet) { PlayNet = new PlayProcesses(this); }

    /* The viewport's own server, or the one it is a client of. */
    const PlayNetSettings NetSettings = PlayNetSettings::FromConfig();
    if(Play->GetNetMode() == NimbleEngine::ENetMode::Server)
    {
        return PlayNet->StartClient(QStringLiteral("127.0.0.1"), Play->GetNetPort(), QString::fromStdString(NetSettings.ClientArgs));
    }
    return PlayNet->StartClient(QString::fromStdString(NetSettings.ServerAddress), NetSettings.Port,
                                QString::fromStdString(NetSettings.ClientArgs));
}

bool EditorContext::ReloadAuthoringGame()
{
    UnloadAuthoringGame();
    if(Play) { return false; }   // the run has its own copy

    const EngineCore::ConfigVar* ModuleVar = EngineCore::ConfigFindVar("Game", "Module");
    if(!ModuleVar || ModuleVar->GetString()[0] == '\0') { return false; }

    AuthoringGame = EngineCore::Memory::Alloc<NimbleEngine::GameModuleHost>();
    if(bRendererReady) { AuthoringGame->SetRenderer(Renderer); }
    if(!AuthoringGame->Load(""))
    {
        EngineCore::Memory::Dealloc(AuthoringGame);
        AuthoringGame = nullptr;
        CORE_LOG_WARN("[Editor] the game module did not load: its components will not open with a world or prefab "
                      "until it is built (Build > Compile Source)")
        return false;
    }
    CORE_LOG("[Editor] " + AuthoringGame->GetModuleName() + " loaded for editing: its components open and save with the world")
    return true;
}

void EditorContext::UnloadAuthoringGame()
{
    if(!AuthoringGame) { return; }
    AuthoringGame->Unload();
    EngineCore::Memory::Dealloc(AuthoringGame);
    AuthoringGame = nullptr;
}

#endif // WITH_EDITOR
