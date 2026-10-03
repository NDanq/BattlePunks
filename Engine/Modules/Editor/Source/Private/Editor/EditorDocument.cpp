#if WITH_EDITOR

#include "Public/Editor/EditorDocument.h"

#include <cstring>
#include <string>

#include <QTimer>
#include <QUndoStack>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Private/Editor/EditorHistory.h"

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

EditorDocument::EditorDocument(QObject* InParent)
    : QObject(InParent)
{
    History = new QUndoStack(this);
    History->setUndoLimit(HistoryLimit);
    connect(History, &QUndoStack::indexChanged, this, &EditorDocument::HistoryChanged);
    connect(History, &QUndoStack::cleanChanged, this, &EditorDocument::HistoryChanged);

    /* The "before" of the next property write follows the selection and
     * anything that adds or removes its components. */
    connect(this, &EditorDocument::SelectionChanged, this, &EditorDocument::RefreshShadow);
    connect(this, &EditorDocument::EntitiesChanged,  this, &EditorDocument::RefreshShadow);
    connect(this, &EditorDocument::WorldReset,       this, &EditorDocument::RefreshShadow);

    CreateWorld();
}

EditorDocument::~EditorDocument()
{
    /* A borrowed World is its owner's to free; just let go of it. */
    if(IsWorldBorrowed())
    {
        UnhookActiveWorld();
        ActiveWorld = EditedWorld;
    }
    DestroyWorld();
}

// === Scene ===================================================================

void EditorDocument::NewScene()
{
    if(RefuseWhileBorrowed("New Scene"))
    {
        return;
    }
    ReplaceWorld();
    ResetHistory();
}

void EditorDocument::CreateSandboxScene()
{
    if(RefuseWhileBorrowed("New Sandbox Scene"))
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
        Marker.GetComponent<GameCore::TransformComponent>()->Scale = EngineCore::Vector3(32.0, 32.0, 1.0);

        Marker.AddComponent<RenderCore::RenderComponent>();

        /* The unit quad's extent, as Sandbox does. */
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0));
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

bool EditorDocument::LoadWorld(const QString& InPath, bool* OutbCooked)
{
    if(OutbCooked) { *OutbCooked = false; }
    if(RefuseWhileBorrowed("Open World"))
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

bool EditorDocument::SaveWorld(const QString& InPath)
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

EntityID EditorDocument::CreateEntity(const QString& InName, const EngineCore::Vector3& InPosition,
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

    /* A prefab keeps one root: anything new goes under it, where it stood. */
    if(bSingleRoot && !E.GetParent().IsSet())
    {
        for(const EntityID Root : GetRootEntities())
        {
            if(Root != E.GetID())
            {
                E.SetParent(Root, true);
                ActiveWorld->RefreshEntityBounds(E.GetID());
                break;
            }
        }
    }

    if(IsRecording())
    {
        Record(new EditorHistory::EntityLifetimeCommand(*this, QStringLiteral("Create %1").arg(GetDisplayName(E.GetID())),
                                                        true, EditorHistory::CaptureEntity(*EditedWorld, E.GetID())));
    }
    return E.GetID();
}

void EditorDocument::RenameEntity(const EntityID InEntity, const QString& InName)
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

void EditorDocument::SetEntityScript(const EntityID InEntity, const QString& InPath)
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

void EditorDocument::SetEntityScriptCull(const EntityID InEntity, const bool bInCull)
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

void EditorDocument::RecordMembersChange(const EntityID InEntity, const EditorHistory::EntityMembers& InBefore, const QString& InText)
{
    if(IsRecording())
    {
        Record(new EditorHistory::EntityMembersCommand(*this, InText, EditedWorld->GetEntity(InEntity).GetGuid(), InBefore,
                                                       EditorHistory::CaptureMembers(*EditedWorld, InEntity)));
    }
}

bool EditorDocument::SetEntityParent(const EntityID InEntity, const EntityID InParent)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity) || (InParent.IsSet() && !ActiveWorld->IsAlive(InParent)))
    {
        return false;
    }
    const GameCore::Entity E = ActiveWorld->GetEntity(InEntity);
    const EntityID OldParent = E.GetParent();
    if(OldParent == InParent)
    {
        return false;
    }
    const GameCore::ComponentTypeID TransformType = GameCore::TransformComponent::StaticType().ID;
    const std::vector<uint8_t> BeforeTransform = EditorHistory::CaptureProperties(*ActiveWorld, InEntity, TransformType);

    if(!E.SetParent(InParent, true))
    {
        return false;
    }
    ActiveWorld->RefreshEntityBounds(InEntity);

    if(IsRecording())
    {
        const QString Text = InParent.IsSet()
            ? QStringLiteral("Parent %1 to %2").arg(GetDisplayName(InEntity), GetDisplayName(InParent))
            : QStringLiteral("Detach %1").arg(GetDisplayName(InEntity));
        Record(new EditorHistory::ParentCommand(*this, Text, E.GetGuid(),
                                                EditedWorld->GetEntity(OldParent).GetGuid(), BeforeTransform,
                                                EditedWorld->GetEntity(InParent).GetGuid(),
                                                EditorHistory::CaptureProperties(*EditedWorld, InEntity, TransformType)));
    }
    NotifyHierarchyEdited(InEntity);
    return true;
}

void EditorDocument::NotifyHierarchyEdited(const EntityID InEntity)
{
    QueueEntitiesChanged();
    emit PropertyEdited(InEntity, GameCore::TransformComponent::StaticType().ID);

    /* The local Transform was rewritten without a property edit: the next
     * edit's "before" must be these values. */
    if(InEntity == ShadowEntity)
    {
        RefreshShadow();
    }
}

bool EditorDocument::AddComponent(const EntityID InEntity, const GameCore::ComponentTypeID InType)
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

bool EditorDocument::RemoveComponent(const EntityID InEntity, const GameCore::ComponentTypeID InType)
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

void EditorDocument::DestroyEntity(const EntityID InEntity)
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

QString EditorDocument::GetDisplayName(const EntityID InEntity) const
{
    const char* Name = ActiveWorld ? ActiveWorld->GetEntity(InEntity).GetName() : "";
    if(Name[0] != '\0')
    {
        return QString::fromUtf8(Name);
    }
    return QStringLiteral("Entity %1").arg(InEntity.Index);
}

GameCore::AABB EditorDocument::GetEntityBounds(const EntityID InEntity) const
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
            return GameCore::AABB::Transformed(Local, E.GetWorldMatrix());
        }
        const GameCore::AABB Quad(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0));   // the unit quad
        return GameCore::AABB::Transformed(Quad, E.GetWorldMatrix());
    }

    /* A marker for anything with only a position (a camera, an empty). */
    const double Half = 25.0;
    const EngineCore::Vector3 Position = E.GetWorldPose().Position;
    return GameCore::AABB(Position - EngineCore::Vector3(Half, Half, Half),
                          Position + EngineCore::Vector3(Half, Half, Half));
}

GameCore::AABB EditorDocument::GetSceneBounds() const
{
    if(!ActiveWorld)
    {
        return GameCore::AABB();
    }

    struct Collect { const EditorDocument* Self; GameCore::AABB Box; } Ctx{ this, GameCore::AABB() };
    ActiveWorld->ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        auto& C = *static_cast<Collect*>(InUserData);
        C.Box = GameCore::AABB::Union(C.Box, C.Self->GetEntityBounds(InEntity));
    }, &Ctx);
    return Ctx.Box;
}

// === Selection ===============================================================

void EditorDocument::SetSelection(EntityID InEntity)
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

void EditorDocument::NotifyPropertyEdited(const EntityID InEntity, const GameCore::ComponentTypeID InType)
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

bool EditorDocument::IsRecording() const
{
    return !IsWorldBorrowed() && !bApplyingHistory && !bSuspendRecording;
}

void EditorDocument::Record(QUndoCommand* InCommand)
{
    History->push(InCommand);   // merged or dropped (obsolete) inside, or appended
    LatestRecorded = History->index() > 0 ? History->command(History->index() - 1) : nullptr;
    emit HistoryChanged();
}

void EditorDocument::ResetHistory()
{
    History->clear();   // also clean
    LatestRecorded = nullptr;
    OpenGesture    = 0;
    emit HistoryChanged();
}

bool EditorDocument::CanUndo() const
{
    return !IsWorldBorrowed() && OpenGesture == 0 && History->canUndo();
}

bool EditorDocument::CanRedo() const
{
    return !IsWorldBorrowed() && OpenGesture == 0 && History->canRedo();
}

void EditorDocument::Undo()
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

void EditorDocument::Redo()
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

void EditorDocument::BeginEditGesture(const QString& InLabel)
{
    OpenGesture  = ++GestureSerial;
    GestureLabel = InLabel;
    emit HistoryChanged();
}

void EditorDocument::EndEditGesture()
{
    OpenGesture = 0;
    emit HistoryChanged();
}

bool EditorDocument::IsLatestRecorded(const QUndoCommand* InCommand) const
{
    return InCommand && InCommand == LatestRecorded;
}

void EditorDocument::RefreshShadow()
{
    Shadow.clear();
    ShadowEntity = GameCore::InvalidEntityID();
    if(IsWorldBorrowed() || !EditedWorld || !EditedWorld->IsAlive(Selection))
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

void EditorDocument::OnEntityCreated(const EntityID /*InEntity*/, void* InUserData)
{
    static_cast<EditorDocument*>(InUserData)->QueueEntitiesChanged();
}

void EditorDocument::OnEntityDestroyed(const EntityID InEntity, void* InUserData)
{
    auto* Self = static_cast<EditorDocument*>(InUserData);

    /* Inside the World's broadcast: record, never re-enter the World. The
     * signal itself waits for the queued turn. */
    if(InEntity == Self->Selection)
    {
        Self->Selection = GameCore::InvalidEntityID();
        QTimer::singleShot(0, Self, [Self]() { emit Self->SelectionChanged(Self->Selection); });
    }
    Self->QueueEntitiesChanged();
}

void EditorDocument::OnComponentChanged(const EntityID /*InEntity*/, const GameCore::ComponentTypeID /*InType*/, void* InUserData)
{
    static_cast<EditorDocument*>(InUserData)->QueueEntitiesChanged();
}

void EditorDocument::QueueEntitiesChanged()
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

void EditorDocument::ReplaceWorld()
{
    Selection = GameCore::InvalidEntityID();

    DestroyWorld();
    CreateWorld();

    emit WorldReset();
    emit SelectionChanged(Selection);
}

void EditorDocument::CreateWorld()
{
    EditedWorld = EngineCore::Memory::Alloc<GameCore::World>();
    ActiveWorld = EditedWorld;
    HookActiveWorld();

    emit EditedWorldCreated();
}

void EditorDocument::DestroyWorld()
{
    if(!EditedWorld)
    {
        return;
    }

    /* Before the World goes: a Renderer's bridge holds its delegate handles. */
    emit EditedWorldAboutToBeDestroyed();

    if(ActiveWorld == EditedWorld)
    {
        UnhookActiveWorld();
    }
    ActiveWorld = nullptr;

    EngineCore::Memory::Dealloc(EditedWorld);
    EditedWorld = nullptr;
}

void EditorDocument::HookActiveWorld()
{
    if(!ActiveWorld || HookedWorld == ActiveWorld)
    {
        return;
    }
    HookedWorld = ActiveWorld;
    CreatedHandle          = ActiveWorld->OnEntityCreated().Add(&EditorDocument::OnEntityCreated, this);
    DestroyedHandle        = ActiveWorld->OnEntityDestroyed().Add(&EditorDocument::OnEntityDestroyed, this);
    ComponentAddedHandle   = ActiveWorld->OnComponentAdded().Add(&EditorDocument::OnComponentChanged, this);
    ComponentRemovedHandle = ActiveWorld->OnComponentRemoved().Add(&EditorDocument::OnComponentChanged, this);
}

void EditorDocument::UnhookActiveWorld()
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

void EditorDocument::ForgetHooks()
{
    HookedWorld            = nullptr;
    CreatedHandle          = GameCore::DelegateHandle();
    DestroyedHandle        = GameCore::DelegateHandle();
    ComponentAddedHandle   = GameCore::DelegateHandle();
    ComponentRemovedHandle = GameCore::DelegateHandle();
}

void EditorDocument::ActivateWorld(GameCore::World* InWorld, const EntityID InSelection)
{
    ActiveWorld = InWorld;
    HookActiveWorld();

    Selection = ActiveWorld && ActiveWorld->IsAlive(InSelection) ? InSelection : GameCore::InvalidEntityID();
    emit WorldReset();
    emit SelectionChanged(Selection);
}

// === Prefab ==================================================================

bool EditorDocument::LoadPrefab(const QString& InPath, GameCore::PrefabAsset& OutAsset)
{
    if(RefuseWhileBorrowed("Open Prefab"))
    {
        return false;
    }

    /* Read before the current World is replaced, so a bad file leaves it alone. */
    const QByteArray Path = InPath.toUtf8();
    GameCore::PrefabAsset Loaded;
    if(!GameCore::LoadPrefabFile(Path.constData(), Loaded))
    {
        CORE_LOG_ERROR("[Editor] '" + std::string(Path.constData()) + "' is not a readable prefab")
        return false;
    }

    ReplaceWorld();
    ResetHistory();
    {
        const FlagScope Building(bSuspendRecording);   // the prefab as saved is the starting point
        if(!EditedWorld->SpawnPrefab(Loaded).IsValid())
        {
            CORE_LOG_ERROR("[Editor] '" + std::string(Path.constData()) + "' has no entities")
        }
        EditedWorld->RefreshSpatialIndex();
    }
    OutAsset = Loaded;
    QueueEntitiesChanged();
    return true;
}

bool EditorDocument::SavePrefab(const QString& InPath, GameCore::PrefabAsset& InOutAsset, QString& OutError)
{
    if(IsWorldBorrowed())
    {
        OutError = QStringLiteral("Stop simulating before saving");
        return false;
    }
    const std::vector<EntityID> Roots = GetRootEntities();
    if(Roots.empty())
    {
        OutError = QStringLiteral("The prefab is empty: it needs a root entity");
        return false;
    }
    if(Roots.size() > 1)
    {
        QStringList Names;
        for(const EntityID Root : Roots) { Names << GetDisplayName(Root); }
        OutError = QStringLiteral("A prefab has exactly one root entity. Parent the others under one of: %1")
                       .arg(Names.join(QStringLiteral(", ")));
        return false;
    }

    /* Adopting: entities added here join the instance with the ids they get,
     * so the next save keeps them -- placed instances key overrides on them. */
    GameCore::PrefabAsset Captured = InOutAsset;
    if(!GameCore::CapturePrefab(*EditedWorld, Roots.front(), Captured, true))
    {
        OutError = QStringLiteral("Could not capture the prefab");
        return false;
    }
    if(!GameCore::SavePrefabFile(InPath.toUtf8().constData(), Captured))
    {
        OutError = QStringLiteral("Could not write %1").arg(InPath);
        return false;
    }
    InOutAsset = Captured;
    History->setClean();
    return true;
}

std::vector<EntityID> EditorDocument::GetRootEntities() const
{
    std::vector<EntityID> Roots;
    if(!EditedWorld)
    {
        return Roots;
    }
    struct Collect { GameCore::World* W; std::vector<EntityID>* Out; } Ctx{ EditedWorld, &Roots };
    EditedWorld->ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        auto& C = *static_cast<Collect*>(InUserData);
        if(!C.W->GetEntity(InEntity).GetParent().IsSet())
        {
            C.Out->push_back(InEntity);
        }
    }, &Ctx);
    return Roots;
}

// === Borrowed World ==========================================================

void EditorDocument::BorrowWorld(GameCore::World* InWorld)
{
    if(!IsWorldBorrowed())
    {
        EditedSelection = Selection;
    }
    UnhookActiveWorld();
    ActivateWorld(InWorld, GameCore::InvalidEntityID());
    emit HistoryChanged();
}

void EditorDocument::UnhookBorrowedWorld()
{
    if(IsWorldBorrowed())
    {
        UnhookActiveWorld();
    }
}

void EditorDocument::DropBorrowedWorld()
{
    if(!IsWorldBorrowed())
    {
        return;
    }
    if(HookedWorld != EditedWorld)
    {
        ForgetHooks();
    }
    ActiveWorld = nullptr;
}

void EditorDocument::ReturnToEditedWorld()
{
    if(!IsWorldBorrowed())
    {
        return;
    }
    if(ActiveWorld)
    {
        UnhookActiveWorld();
    }
    else if(HookedWorld != EditedWorld)
    {
        ForgetHooks();
    }
    ActivateWorld(EditedWorld, EditedSelection);
    EditedSelection = GameCore::InvalidEntityID();
    emit HistoryChanged();
}

bool EditorDocument::RefuseWhileBorrowed(const char* InWhat) const
{
    if(!IsWorldBorrowed())
    {
        return false;
    }
    CORE_LOG_WARN(std::string("[Editor] ") + InWhat + " is not available while playing; stop first")
    return true;
}

void EditorDocument::RefreshEditedWorld()
{
    if(EditedWorld && !IsWorldBorrowed())
    {
        EditedWorld->RefreshSpatialIndex();
        EditedWorld->UpdateOverlaps();
    }
}

#endif // WITH_EDITOR
