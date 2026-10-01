#if WITH_EDITOR

#include "Public/Editor/EditorContext.h"

#include <chrono>
#include <cstring>
#include <string>

#include <QTimer>
#include <QUndoStack>

#include "CoreQtConversions.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreImage.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/RenderCore/Renderer.h"
#include "Private/Editor/EditorHistory.h"
#include "Private/Editor/PlaySession.h"

using GameCore::EntityID;

namespace
{
    /* Undo entries are capped; each holds a component's values or an
     * entity's serialized components -- small. */
    constexpr int HistoryLimit = 500;

    QString ComponentName(const GameCore::ComponentTypeID InType)
    {
        const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(InType);
        return Info ? QString::fromLatin1(Info->Name) : QStringLiteral("Component");
    }

    /* Sets a flag for a scope: recording suspended while a scene is built. */
    struct FlagScope
    {
        explicit FlagScope(bool& InFlag) : Flag(InFlag), bPrevious(InFlag) { Flag = true; }
        ~FlagScope() { Flag = bPrevious; }
        bool& Flag;
        bool  bPrevious;
    };
}

EditorContext::EditorContext(QObject* InParent)
    : QObject(InParent)
{
    /* Before anything asks for an asset -- the Renderer loads its cooked
     * shaders through this on the render thread. */
    EngineCore::SetFileProvider(&LayeredAssets);

    History = new QUndoStack(this);
    History->setUndoLimit(HistoryLimit);
    connect(History, &QUndoStack::indexChanged, this, &EditorContext::HistoryChanged);
    connect(History, &QUndoStack::cleanChanged, this, &EditorContext::HistoryChanged);
    connect(this, &EditorContext::PlayStateChanged, this, &EditorContext::HistoryChanged);

    /* The "before" of the next property write follows the selection and
     * anything that adds or removes its components. */
    connect(this, &EditorContext::SelectionChanged, this, &EditorContext::RefreshShadow);
    connect(this, &EditorContext::EntitiesChanged,  this, &EditorContext::RefreshShadow);
    connect(this, &EditorContext::WorldReset,       this, &EditorContext::RefreshShadow);

    CreateWorld();
}

EditorContext::~EditorContext()
{
    Shutdown();   // stops a run first
    DestroyWorld();

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

    Renderer->SetClearColor(EngineCore::Color(38, 40, 46));
    if(EditedWorld)
    {
        Renderer->BindWorld(*EditedWorld);
    }
    return true;
}

void EditorContext::ReloadMaterial(const QString& InName)
{
    if(Renderer && bRendererReady)
    {
        Renderer->ReloadMaterial(InName.toUtf8().constData());
    }
}

uint64_t EditorContext::RequestMaterialPreview(const QString& InName, const int InSize, const EngineCore::Color InTint)
{
    if(!Renderer || !bRendererReady)
    {
        return 0;
    }
    return Renderer->RequestMaterialPreview(InName.toUtf8().constData(), InSize, InTint);
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

void EditorContext::Shutdown()
{
    /* First: the game's level and module go while the Renderer still runs. */
    StopPlay();

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

void EditorContext::RenderFrame(const RenderCore::RenderView& InView, const EngineCore::Size InViewportPixels,
                                const EngineCore::WindowHandle InWindow)
{
    if(!bRendererReady || InViewportPixels.IsEmpty())
    {
        return;
    }

    using Clock = std::chrono::steady_clock;
    const int64_t NowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    const double  DeltaSeconds = FrameNumber == 0 ? 0.0 : double(NowNs - PreviousFrameNs) / 1'000'000'000.0;

    /* The game's frame, where Application::RunFrame has it: before the fence,
     * so its render commands join this frame. */
    if(Play)
    {
        /* Unhook before the swap frees the World; SyncPlayWorld hooks the new one. */
        if(Play->IsLevelSwapPending())
        {
            UnhookActiveWorld();
        }
        Play->Tick(DeltaSeconds, InViewportPixels);
        SyncPlayWorld();
    }

    /* No Tick, but edits must still reach the spatial index. */
    else if(EditedWorld)
    {
        EditedWorld->RefreshSpatialIndex();
    }

    /* Application::RunFrame's order: fence, collect, present, kick. */
    Renderer->WaitForPreviousFrame();
    EngineCore::Memory::CollectGarbage();
    Renderer->PresentToWindow(InWindow);

    /* Null hands the view back to the World's CameraComponent: the game's. */
    Renderer->SetViewOverride(IsEditorViewActive() ? &InView : nullptr);

    RenderCore::FrameInfo Info;
    Info.FrameNumber    = FrameNumber;
    Info.DeltaSeconds   = DeltaSeconds;
    Info.ViewportPixels = InViewportPixels;
    Renderer->EndFrame(Info);

    PreviousFrameNs = NowNs;
    ++FrameNumber;
}

void EditorContext::DrawDebugLine(const RenderCore::DebugLine& InLine)
{
    if(bRendererReady)
    {
        Renderer->DrawDebugLine(InLine);
    }
}

void EditorContext::DrawDebugBox(const GameCore::AABB& InBox, const EngineCore::Color InColor, const float InThicknessPixels)
{
    if(bRendererReady)
    {
        Renderer->DrawDebugBox(InBox, InColor, InThicknessPixels);
    }
}

// === Scene ===================================================================

void EditorContext::NewScene()
{
    if(RefuseWhilePlaying("New Scene"))
    {
        return;
    }
    ReplaceWorld();
    ResetHistory();
}

void EditorContext::CreateSandboxScene()
{
    if(RefuseWhilePlaying("New Sandbox Scene"))
    {
        return;
    }
    ReplaceWorld();
    ResetHistory();
    const FlagScope Building(bSuspendRecording);   // the scene is the starting point, not an edit

    /*
     * Sandbox's level as loaded, before its first tick. A hand-kept MIRROR of:
     *
     *   Games/Sandbox/SandboxGame.cpp   SandboxLevel::OnLoad -- Marker, Camera
     *   Games/Sandbox/Scripts/Main.lua  the startup script   -- Scripted
     *
     * Keep it in step. Behaviour (systems, the mode, scripts, the logger) is
     * not mirrored.
     */

    // --- Marker (SandboxLevel::OnLoad) ---------------------------------------
    const EntityID MarkerID = CreateEntity(QStringLiteral("Marker"), EngineCore::Vector3(0.0, 360.0, 0.0));
    {
        const GameCore::Entity Marker = EditedWorld->GetEntity(MarkerID);

        RenderCore::RenderComponent Render;
        Render.Width  = 32.0f;
        Render.Height = 32.0f;
        Render.Tint   = EngineCore::Color(255, 200, 40);
        Marker.AddComponent(Render);

        /* The quad's extent, derived from the RenderComponent as Sandbox does. */
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(
            EngineCore::Vector3(-0.5 * Render.Width, -0.5 * Render.Height, 0.0),
            EngineCore::Vector3( 0.5 * Render.Width,  0.5 * Render.Height, 0.0));
        Marker.AddComponent(Bounds);
    }

    // --- Camera (SandboxLevel::OnLoad) ---------------------------------------
    /* The camera the game renders through (the editor uses its own). */
    const EntityID CameraID = CreateEntity(QStringLiteral("Camera"), EngineCore::Vector3(0.0, 0.0, 0.0));
    EditedWorld->GetEntity(CameraID).AddComponent<RenderCore::CameraComponent>();

    // --- Scripted (Scripts/Main.lua) ------------------------------------------
    /* Only the script path: the editor runs no Lua. */
    const EntityID ScriptedID = CreateEntity(QStringLiteral("Scripted"), EngineCore::Vector3(0.0, 0.0, 0.0));
    {
        const GameCore::Entity Scripted = EditedWorld->GetEntity(ScriptedID);

        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, -0.5), EngineCore::Vector3(0.5, 0.5, 0.5));
        Scripted.AddComponent(Bounds);

        Scripted.SetScript("Scripts/Mover.lua");   // Main.lua's scripted:SetScript(...)
    }

    EditedWorld->RefreshSpatialIndex();
    QueueEntitiesChanged();
}

bool EditorContext::LoadWorld(const QString& InPath, bool* OutbCooked)
{
    if(OutbCooked) { *OutbCooked = false; }
    if(RefuseWhilePlaying("Open World"))
    {
        return false;
    }

    /* Checked before the current scene is replaced, so an unreadable path
     * leaves it alone. */
    const QByteArray Path = InPath.toUtf8();
    if(GameCore::DetectWorldFileFormat(Path.constData()) == GameCore::EWorldFileFormat::Unknown)
    {
        CORE_LOG_ERROR("[Editor] could not read '" + std::string(Path.constData()) + "'")
        return false;
    }

    ReplaceWorld();
    ResetHistory();

    GameCore::EWorldFileFormat Format = GameCore::EWorldFileFormat::Unknown;
    if(!GameCore::LoadWorldFile(Path.constData(), *EditedWorld, &Format))
    {
        CORE_LOG_ERROR("[Editor] '" + std::string(Path.constData()) + "' is not a readable world; starting an empty scene")
        ReplaceWorld();
        ResetHistory();
        return false;
    }
    if(OutbCooked) { *OutbCooked = Format == GameCore::EWorldFileFormat::Binary; }

    QueueEntitiesChanged();
    return true;
}

bool EditorContext::SaveWorld(const QString& InPath)
{
    /* The edited World, not the playing one -- allowed while playing, since
     * the edited World is untouched by the run. */
    if(!GameCore::SaveWorldPackage(InPath.toUtf8().constData(), *EditedWorld))
    {
        return false;
    }
    History->setClean();
    return true;
}

// === Entities ================================================================

EntityID EditorContext::CreateEntity(const QString& InName, const EngineCore::Vector3& InPosition,
                                     const std::function<void(const GameCore::Entity&)>& InSetup)
{
    const QByteArray Name = InName.toUtf8();
    const GameCore::Entity E = ActiveWorld->CreateEntity(Name.constData());

    GameCore::TransformComponent Transform;
    Transform.Position = InPosition;
    E.AddComponent(Transform);

    if(InSetup)
    {
        InSetup(E);
    }

    if(IsRecording())
    {
        Record(new EditorHistory::EntityLifetimeCommand(*this, QStringLiteral("Create %1").arg(GetDisplayName(E.GetID())),
                                                        true, EditorHistory::CaptureEntity(*EditedWorld, E.GetID())));
    }
    return E.GetID();
}

void EditorContext::RenameEntity(const EntityID InEntity, const QString& InName)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return;
    }
    const QString OldName = GetDisplayName(InEntity);
    const EditorHistory::EntityMembers Before = EditorHistory::CaptureMembers(*ActiveWorld, InEntity);
    if(ActiveWorld->GetEntity(InEntity).SetName(InName.toUtf8().constData()))
    {
        QueueEntitiesChanged();
        RecordMembersChange(InEntity, Before, QStringLiteral("Rename %1 to %2").arg(OldName, GetDisplayName(InEntity)));
    }
}

void EditorContext::SetEntityScript(const EntityID InEntity, const QString& InPath)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return;
    }
    const EditorHistory::EntityMembers Before = EditorHistory::CaptureMembers(*ActiveWorld, InEntity);
    const QString Path = InPath.trimmed();
    if(ActiveWorld->GetEntity(InEntity).SetScript(Path.toUtf8().constData()))
    {
        QueueEntitiesChanged();
        RecordMembersChange(InEntity, Before, (Path.isEmpty() ? QStringLiteral("Clear Script on %1")
                                                              : QStringLiteral("Set Script on %1")).arg(GetDisplayName(InEntity)));
    }
}

void EditorContext::SetEntityScriptCull(const EntityID InEntity, const bool bInCull)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return;
    }
    GameCore::EntityScriptState* Script = ActiveWorld->GetEntity(InEntity).GetScriptState();
    if(!Script || (Script->bCullOutsideActiveRegion != 0) == bInCull)
    {
        return;
    }
    const EditorHistory::EntityMembers Before = EditorHistory::CaptureMembers(*ActiveWorld, InEntity);
    Script->bCullOutsideActiveRegion = bInCull ? 1 : 0;
    RecordMembersChange(InEntity, Before, (bInCull ? QStringLiteral("Cull Script on %1")
                                                   : QStringLiteral("Stop Culling Script on %1")).arg(GetDisplayName(InEntity)));
}

void EditorContext::RecordMembersChange(const EntityID InEntity, const EditorHistory::EntityMembers& InBefore, const QString& InText)
{
    if(IsRecording())
    {
        Record(new EditorHistory::EntityMembersCommand(*this, InText, EditedWorld->GetEntity(InEntity).GetGuid(), InBefore,
                                                       EditorHistory::CaptureMembers(*EditedWorld, InEntity)));
    }
}

bool EditorContext::AddComponent(const EntityID InEntity, const GameCore::ComponentTypeID InType)
{
    const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(InType);
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity) || !Info)
    {
        return false;
    }
    const GameCore::Entity E = ActiveWorld->GetEntity(InEntity);
    if(E.HasComponentByType(InType))
    {
        return false;
    }

    /* From the default-constructed bytes -- never zeroes -- exactly as the
     * type's own `T{}` would start. */
    std::vector<uint8_t> Init(Info->SizeBytes);
    if(!GameCore::GetComponentPropertyDefaults(InType, Init.data()) || !E.AddComponentByType(InType, Init.data()))
    {
        return false;
    }
    ActiveWorld->RefreshEntityBounds(InEntity);

    if(IsRecording())
    {
        Record(new EditorHistory::ComponentLifetimeCommand(
            *this, QStringLiteral("Add %1 to %2").arg(ComponentName(InType), GetDisplayName(InEntity)), true,
            EditedWorld->GetEntity(InEntity).GetGuid(), EditorHistory::CaptureComponent(*EditedWorld, InEntity, InType)));
    }
    return true;
}

bool EditorContext::RemoveComponent(const EntityID InEntity, const GameCore::ComponentTypeID InType)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity) || !ActiveWorld->GetEntity(InEntity).HasComponentByType(InType))
    {
        return false;
    }
    const bool bRecord = IsRecording();
    EditorHistory::ComponentSnapshot Snapshot;
    if(bRecord)
    {
        Snapshot = EditorHistory::CaptureComponent(*EditedWorld, InEntity, InType);
    }

    /* Immediate outside a tick; OnComponentRemoved rebuilds the panels on the
     * next turn. */
    if(!ActiveWorld->GetEntity(InEntity).RemoveComponentByType(InType))
    {
        return false;
    }
    if(bRecord)
    {
        Record(new EditorHistory::ComponentLifetimeCommand(
            *this, QStringLiteral("Remove %1 from %2").arg(ComponentName(InType), GetDisplayName(InEntity)), false,
            EditedWorld->GetEntity(InEntity).GetGuid(), std::move(Snapshot)));
    }
    return true;
}

void EditorContext::DestroyEntity(const EntityID InEntity)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return;
    }
    const bool bRecord = IsRecording();
    const QString Text = QStringLiteral("Delete %1").arg(GetDisplayName(InEntity));
    EditorHistory::EntitySnapshot Snapshot;
    if(bRecord)
    {
        Snapshot = EditorHistory::CaptureEntity(*EditedWorld, InEntity);
    }

    if(InEntity == Selection)
    {
        SetSelection(GameCore::InvalidEntityID());
    }
    ActiveWorld->DestroyEntity(InEntity);

    if(bRecord)
    {
        Record(new EditorHistory::EntityLifetimeCommand(*this, Text, false, std::move(Snapshot)));
    }
}

QString EditorContext::GetDisplayName(const EntityID InEntity) const
{
    const char* Name = ActiveWorld ? ActiveWorld->GetEntity(InEntity).GetName() : "";
    if(Name[0] != '\0')
    {
        return QString::fromUtf8(Name);
    }
    return QStringLiteral("Entity %1").arg(InEntity.Index);
}

GameCore::AABB EditorContext::GetEntityBounds(const EntityID InEntity) const
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return GameCore::AABB();
    }
    const GameCore::Entity E = ActiveWorld->GetEntity(InEntity);

    if(const GameCore::BoundsComponent* Bounds = E.GetComponent<GameCore::BoundsComponent>())
    {
        if(!Bounds->World.IsEmpty())
        {
            return Bounds->World;
        }
    }

    const GameCore::TransformComponent* Transform = E.GetComponent<GameCore::TransformComponent>();
    if(!Transform)
    {
        return GameCore::AABB();
    }

    if(const RenderCore::RenderComponent* Render = E.GetComponent<RenderCore::RenderComponent>())
    {
        /* A mesh's own box, as the renderer culls it (RenderComponentProxy). */
        const std::string MeshPath(Render->Mesh, strnlen(Render->Mesh, RenderCore::RENDER_MESH_PATH_MAX));
        if(const std::shared_ptr<const RenderCore::MeshData> Mesh = MeshPath.empty() ? nullptr : RenderCore::MeshAssetCache::Find(MeshPath))
        {
            const GameCore::AABB Local(EngineCore::Vector3(Mesh->BoundsMin[0], Mesh->BoundsMin[1], Mesh->BoundsMin[2]),
                                       EngineCore::Vector3(Mesh->BoundsMax[0], Mesh->BoundsMax[1], Mesh->BoundsMax[2]));
            return GameCore::AABB::Transformed(Local, Transform->GetLocalMatrix());
        }
        const GameCore::AABB Quad(EngineCore::Vector3(-0.5 * Render->Width, -0.5 * Render->Height, 0.0),
                                  EngineCore::Vector3( 0.5 * Render->Width,  0.5 * Render->Height, 0.0));
        return GameCore::AABB::Transformed(Quad, Transform->GetLocalMatrix());
    }

    /* A marker for anything with only a position (a camera, an empty). */
    const double Half = 25.0;
    return GameCore::AABB(Transform->Position - EngineCore::Vector3(Half, Half, Half),
                          Transform->Position + EngineCore::Vector3(Half, Half, Half));
}

GameCore::AABB EditorContext::GetSceneBounds() const
{
    if(!ActiveWorld)
    {
        return GameCore::AABB();
    }

    struct Collect { const EditorContext* Self; GameCore::AABB Box; } Ctx{ this, GameCore::AABB() };
    ActiveWorld->ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        auto& C = *static_cast<Collect*>(InUserData);
        C.Box = GameCore::AABB::Union(C.Box, C.Self->GetEntityBounds(InEntity));
    }, &Ctx);
    return Ctx.Box;
}

// === Selection ===============================================================

void EditorContext::SetSelection(EntityID InEntity)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        InEntity = GameCore::InvalidEntityID();
    }
    if(InEntity == Selection)
    {
        return;
    }
    Selection = InEntity;
    emit SelectionChanged(Selection);
}

void EditorContext::NotifyPropertyEdited(const EntityID InEntity, const GameCore::ComponentTypeID InType)
{
    emit PropertyEdited(InEntity, InType);

    /* Only the selection's values from before the write are known (the
     * shadow); every panel that writes properties edits the selection. */
    if(!IsRecording() || InEntity != ShadowEntity)
    {
        return;
    }
    for(ShadowComponent& Entry : Shadow)
    {
        if(Entry.Type != InType)
        {
            continue;
        }
        std::vector<uint8_t> After = EditorHistory::CaptureProperties(*EditedWorld, InEntity, InType);
        if(After == Entry.Values)
        {
            return;
        }
        const QString Text = OpenGesture != 0
            ? QStringLiteral("%1 %2").arg(GestureLabel, GetDisplayName(InEntity))
            : QStringLiteral("Edit %1 on %2").arg(ComponentName(InType), GetDisplayName(InEntity));
        Record(new EditorHistory::PropertyEditCommand(*this, Text, EditedWorld->GetEntity(InEntity).GetGuid(), InType,
                                                      Entry.Values, After, OpenGesture));
        Entry.Values = std::move(After);
        return;
    }
}

// === History =================================================================

bool EditorContext::IsRecording() const
{
    return !Play && !bApplyingHistory && !bSuspendRecording && ActiveWorld == EditedWorld;
}

void EditorContext::Record(QUndoCommand* InCommand)
{
    History->push(InCommand);   // merged or dropped (obsolete) inside, or appended
    LatestRecorded = History->index() > 0 ? History->command(History->index() - 1) : nullptr;
    emit HistoryChanged();
}

void EditorContext::ResetHistory()
{
    History->clear();   // also clean
    LatestRecorded = nullptr;
    OpenGesture    = 0;
    emit HistoryChanged();
}

bool EditorContext::CanUndo() const
{
    return !Play && OpenGesture == 0 && History->canUndo();
}

bool EditorContext::CanRedo() const
{
    return !Play && OpenGesture == 0 && History->canRedo();
}

void EditorContext::Undo()
{
    if(!CanUndo())
    {
        return;
    }
    {
        const FlagScope Applying(bApplyingHistory);
        History->undo();
    }
    LatestRecorded = nullptr;   // nothing merges into an entry once it has been stepped over
    RefreshShadow();
    emit HistoryChanged();
}

void EditorContext::Redo()
{
    if(!CanRedo())
    {
        return;
    }
    {
        const FlagScope Applying(bApplyingHistory);
        History->redo();
    }
    LatestRecorded = nullptr;
    RefreshShadow();
    emit HistoryChanged();
}

void EditorContext::BeginEditGesture(const QString& InLabel)
{
    OpenGesture  = ++GestureSerial;
    GestureLabel = InLabel;
    emit HistoryChanged();
}

void EditorContext::EndEditGesture()
{
    OpenGesture = 0;
    emit HistoryChanged();
}

bool EditorContext::IsLatestRecorded(const QUndoCommand* InCommand) const
{
    return InCommand && InCommand == LatestRecorded;
}

void EditorContext::RefreshShadow()
{
    Shadow.clear();
    ShadowEntity = GameCore::InvalidEntityID();
    if(Play || !EditedWorld || !EditedWorld->IsAlive(Selection))
    {
        return;
    }
    ShadowEntity = Selection;
    for(const GameCore::ComponentTypeID Type : EditorHistory::ComponentTypesOf(*EditedWorld, Selection))
    {
        Shadow.push_back({ Type, EditorHistory::CaptureProperties(*EditedWorld, Selection, Type) });
    }
}

// === World lifetime ==========================================================

void EditorContext::OnEntityCreated(const EntityID /*InEntity*/, void* InUserData)
{
    static_cast<EditorContext*>(InUserData)->QueueEntitiesChanged();
}

void EditorContext::OnEntityDestroyed(const EntityID InEntity, void* InUserData)
{
    auto* Self = static_cast<EditorContext*>(InUserData);

    /* Inside the World's broadcast: record, never re-enter the World. The
     * signal itself waits for the queued turn. */
    if(InEntity == Self->Selection)
    {
        Self->Selection = GameCore::InvalidEntityID();
        QTimer::singleShot(0, Self, [Self]() { emit Self->SelectionChanged(Self->Selection); });
    }
    Self->QueueEntitiesChanged();
}

void EditorContext::OnComponentChanged(const EntityID /*InEntity*/, const GameCore::ComponentTypeID /*InType*/, void* InUserData)
{
    static_cast<EditorContext*>(InUserData)->QueueEntitiesChanged();
}

void EditorContext::QueueEntitiesChanged()
{
    if(bEntitiesChangedQueued)
    {
        return;
    }
    bEntitiesChangedQueued = true;
    QTimer::singleShot(0, this, [this]()
    {
        bEntitiesChangedQueued = false;
        emit EntitiesChanged();
    });
}

void EditorContext::ReplaceWorld()
{
    Selection = GameCore::InvalidEntityID();

    DestroyWorld();
    CreateWorld();

    emit WorldReset();
    emit SelectionChanged(Selection);
}

void EditorContext::CreateWorld()
{
    EditedWorld = EngineCore::Memory::Alloc<GameCore::World>();
    ActiveWorld = EditedWorld;
    HookActiveWorld();

    if(bRendererReady)
    {
        Renderer->BindWorld(*EditedWorld);
    }
}

void EditorContext::DestroyWorld()
{
    if(!EditedWorld)
    {
        return;
    }

    /* Before the World goes: the bridge holds its delegate handles. */
    if(Renderer)
    {
        Renderer->UnbindWorld();
    }

    if(ActiveWorld == EditedWorld)
    {
        UnhookActiveWorld();
    }
    ActiveWorld = nullptr;

    EngineCore::Memory::Dealloc(EditedWorld);
    EditedWorld = nullptr;
}

void EditorContext::HookActiveWorld()
{
    if(!ActiveWorld || HookedWorld == ActiveWorld)
    {
        return;
    }
    HookedWorld = ActiveWorld;
    CreatedHandle          = ActiveWorld->OnEntityCreated().Add(&EditorContext::OnEntityCreated, this);
    DestroyedHandle        = ActiveWorld->OnEntityDestroyed().Add(&EditorContext::OnEntityDestroyed, this);
    ComponentAddedHandle   = ActiveWorld->OnComponentAdded().Add(&EditorContext::OnComponentChanged, this);
    ComponentRemovedHandle = ActiveWorld->OnComponentRemoved().Add(&EditorContext::OnComponentChanged, this);
}

void EditorContext::UnhookActiveWorld()
{
    if(!HookedWorld)
    {
        return;
    }
    HookedWorld->OnEntityCreated().Remove(CreatedHandle);
    HookedWorld->OnEntityDestroyed().Remove(DestroyedHandle);
    HookedWorld->OnComponentAdded().Remove(ComponentAddedHandle);
    HookedWorld->OnComponentRemoved().Remove(ComponentRemovedHandle);
    ForgetHooks();
}

void EditorContext::ForgetHooks()
{
    HookedWorld            = nullptr;
    CreatedHandle          = GameCore::DelegateHandle();
    DestroyedHandle        = GameCore::DelegateHandle();
    ComponentAddedHandle   = GameCore::DelegateHandle();
    ComponentRemovedHandle = GameCore::DelegateHandle();
}

void EditorContext::ActivateWorld(GameCore::World* InWorld, const EntityID InSelection)
{
    ActiveWorld = InWorld;
    HookActiveWorld();

    Selection = ActiveWorld && ActiveWorld->IsAlive(InSelection) ? InSelection : GameCore::InvalidEntityID();
    emit WorldReset();
    emit SelectionChanged(Selection);
}

// === Play-in-Editor ==========================================================

bool EditorContext::RefuseWhilePlaying(const char* InWhat) const
{
    if(!Play)
    {
        return false;
    }
    CORE_LOG_WARN(std::string("[Editor] ") + InWhat + " is not available while playing; stop first")
    return true;
}

bool EditorContext::StartPlay()
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

    /* Set the edited World aside: no panel hooks, no renderer binding. The
     * session binds its own level's World. */
    EditedSelection = Selection;
    UnhookActiveWorld();
    Renderer->UnbindWorld();

    Play = EngineCore::Memory::Alloc<PlaySession>(*Renderer);
    if(!Play->Start())
    {
        EngineCore::Memory::Dealloc(Play);
        Play = nullptr;

        Renderer->BindWorld(*EditedWorld);
        HookActiveWorld();   // ActiveWorld is still EditedWorld
        return false;
    }

    PlayLoadCount    = Play->GetLoadCount();
    bPlayViewEjected = false;
    ActivateWorld(Play->GetWorld(), GameCore::InvalidEntityID());

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
    UnhookActiveWorld();
    ActiveWorld = nullptr;

    Play->Stop();   // unbinds the renderer from the game's World
    EngineCore::Memory::Dealloc(Play);
    Play = nullptr;
    bPlayViewEjected = false;

    if(bRendererReady)
    {
        Renderer->BindWorld(*EditedWorld);
    }
    ActivateWorld(EditedWorld, EditedSelection);

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
        ForgetHooks();
        ActiveWorld = nullptr;
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
        ActivateWorld(Play->GetWorld(), GameCore::InvalidEntityID());
    }
}

QString EditorContext::DescribePlay() const
{
    return Play ? QString::fromStdString(Play->Describe()) : QString();
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

#endif // WITH_EDITOR
