#if WITH_EDITOR

#include "Public/Editor/EditorDocument.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <functional>
#include <string>

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
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
#include "Public/NimbleEngine/GameSession.h"
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

void EditorDocument::NewWorld()
{
    if(RefuseWhileBorrowed("New Scene"))
    {
        return;
    }
    ReplaceWorld();
    ResetHistory();
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

    /* Physics or networking components this build cannot run: they edit and
     * save as ever, but Play will not simulate or replicate them. */
    NimbleEngine::WarnInactiveModuleComponents(*EditedWorld, Path.constData());

    QueueEntitiesChanged();
    return true;
}

bool EditorDocument::SaveWorld(const QString& InPath)
{
    /* The edited World, not the playing one -- allowed while playing, since
     * the edited World is untouched by the run. */
    emit AboutToSaveWorld();
    const bool bSaved = GameCore::SaveWorldPackage(InPath.toUtf8().constData(), *EditedWorld);
    emit WorldSaved();
    if(!bSaved)
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

EntityID EditorDocument::PlacePrefab(const QString& InRuntimePath, const EngineCore::Vector3& InPosition)
{
    if(bSingleRoot)
    {
        CORE_LOG_WARN("[Editor] a prefab cannot hold an instance of another prefab")
        return EntityID();
    }
    GameCore::TransformPose At;
    At.Position = InPosition;
    return SpawnPrefabRecorded(InRuntimePath, At);
}

EntityID EditorDocument::ReplaceWithPrefab(const EntityID InEntity, const QString& InRuntimePath)
{
    if(bSingleRoot || !ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return EntityID();
    }
    /* Loaded before anything goes, so a bad prefab leaves the scene alone. */
    if(!GameCore::FindOrLoadPrefab(InRuntimePath.toUtf8().constData()))
    {
        return EntityID();
    }
    const GameCore::TransformPose At = ActiveWorld->GetEntity(InEntity).GetWorldPose();
    const QString Name = GetDisplayName(InEntity);

    const bool bMacro = IsRecording();
    if(bMacro) { History->beginMacro(QStringLiteral("Convert %1 to Prefab").arg(Name)); }
    DestroyEntity(InEntity);
    const EntityID Root = SpawnPrefabRecorded(InRuntimePath, At);
    if(bMacro)
    {
        History->endMacro();
        LatestRecorded = nullptr;   // nothing merges across the macro
        emit HistoryChanged();
    }
    return Root;
}

int EditorDocument::RefreshPrefabInstances(const QString& InRuntimePath)
{
    if(IsWorldBorrowed() || !EditedWorld)
    {
        return 0;
    }
    struct Collect { GameCore::World* W; std::string Path; std::vector<GameCore::PrefabInstanceID> Found; };
    Collect Ctx{ EditedWorld, InRuntimePath.toUtf8().constData(), {} };
    EditedWorld->ForEachInstance([](const GameCore::PrefabInstanceID InInstance, void* InUserData)
    {
        auto& C = *static_cast<Collect*>(InUserData);
        if(C.Path == C.W->GetInstance(InInstance).GetPrefabPath()) { C.Found.push_back(InInstance); }
    }, &Ctx);
    if(Ctx.Found.empty())
    {
        return 0;
    }

    /* Entity ids change, GUIDs do not: the selection follows its GUIDs. */
    const GameCore::EntityGuid PrimaryGuid = EditedWorld->GetEntity(Selection).GetGuid();
    std::vector<GameCore::EntityGuid> SelectedGuids;
    for(const EntityID Each : Selected) { SelectedGuids.push_back(EditedWorld->GetEntity(Each).GetGuid()); }
    {
        const FlagScope Refreshing(bSuspendRecording);
        for(const GameCore::PrefabInstanceID Instance : Ctx.Found)
        {
            GameCore::RefreshPrefabInstance(*EditedWorld, Instance);
        }
        EditedWorld->RefreshSpatialIndex();
    }
    std::vector<EntityID> Reselect;
    for(const GameCore::EntityGuid& Guid : SelectedGuids)
    {
        if(Guid.IsSet()) { Reselect.push_back(EditedWorld->FindEntityByGuid(Guid)); }
    }
    ApplySelection(Reselect, PrimaryGuid.IsSet() ? EditedWorld->FindEntityByGuid(PrimaryGuid) : EntityID(), /*bInForceSignal*/ true);
    RefreshShadow();
    QueueEntitiesChanged();
    return int(Ctx.Found.size());
}

std::vector<EntityID> EditorDocument::GetLooseRoots() const
{
    std::vector<EntityID> Loose;
    for(const EntityID Root : GetRootEntities())
    {
        bool bAnyInstance = false;
        std::vector<EntityID> Stack{ Root };
        while(!Stack.empty() && !bAnyInstance)
        {
            const GameCore::Entity E = EditedWorld->GetEntity(Stack.back());
            Stack.pop_back();
            bAnyInstance = E.IsInInstance();
            for(EntityID Child = E.GetFirstChild(); Child.IsSet(); Child = EditedWorld->GetEntity(Child).GetNextSibling())
            {
                Stack.push_back(Child);
            }
        }
        if(!bAnyInstance) { Loose.push_back(Root); }
    }
    return Loose;
}

int EditorDocument::ConvertTreesToPrefabs(const std::vector<std::pair<EntityID, QString>>& InTrees)
{
    if(bSingleRoot || IsWorldBorrowed())
    {
        return 0;
    }
    struct Job
    {
        EditorHistory::EntitySnapshot Old;
        EntityID                      OldRoot;
        EntityID                      NewRoot;
    };
    std::vector<Job> Jobs;
    std::vector<QString> Paths;
    for(const auto& [Root, Path] : InTrees)
    {
        if(EditedWorld->IsAlive(Root) && GameCore::FindOrLoadPrefab(Path.toUtf8().constData()))
        {
            Jobs.push_back({ EditorHistory::CaptureEntity(*EditedWorld, Root), Root, EntityID() });
            Paths.push_back(Path);
        }
    }
    if(Jobs.empty())
    {
        return 0;
    }

    const bool bMacro = IsRecording();
    if(bMacro) { History->beginMacro(QStringLiteral("Convert %1 Entities to Prefabs").arg(Jobs.size())); }

    /* Every instance first, the originals still there for their references. */
    {
        const FlagScope Converting(bSuspendRecording);
        for(size_t Index = 0; Index < Jobs.size(); ++Index)
        {
            const GameCore::TransformPose At = EditedWorld->GetEntity(Jobs[Index].OldRoot).GetWorldPose();
            Jobs[Index].NewRoot = EditedWorld->SpawnPrefab(Paths[Index].toUtf8().constData(), At).GetRoot().GetID();
        }

        /* Old GUID -> its replacement, the trees walked side by side. */
        EditorHistory::GuidRemap Remap;
        const std::function<void(const EditorHistory::EntitySnapshot&, EntityID)> MapTree =
            [&](const EditorHistory::EntitySnapshot& InOld, const EntityID InNew)
        {
            Remap[InOld.Guid] = InNew;
            EntityID Child = EditedWorld->GetEntity(InNew).GetFirstChild();
            for(const EditorHistory::EntitySnapshot& OldChild : InOld.Children)
            {
                if(!Child.IsSet()) { break; }
                MapTree(OldChild, Child);
                Child = EditedWorld->GetEntity(Child).GetNextSibling();
            }
        };
        for(const Job& Each : Jobs)
        {
            if(Each.NewRoot.IsSet()) { MapTree(Each.Old, Each.NewRoot); }
        }

        /* Each tree's own values onto its replacement: they are its overrides. */
        const std::function<void(const EditorHistory::EntitySnapshot&, EntityID)> Apply =
            [&](const EditorHistory::EntitySnapshot& InOld, const EntityID InNew)
        {
            const GameCore::Entity New = EditedWorld->GetEntity(InNew);
            for(const EditorHistory::ComponentSnapshot& Component : InOld.Components)
            {
                EditorHistory::LoadComponent(*EditedWorld, InNew, Component, &Remap);
            }
            if(InOld.Name != New.GetName()) { New.SetName(InOld.Name.c_str()); }
            EntityID Child = New.GetFirstChild();
            for(const EditorHistory::EntitySnapshot& OldChild : InOld.Children)
            {
                if(!Child.IsSet()) { break; }
                Apply(OldChild, Child);
                Child = EditedWorld->GetEntity(Child).GetNextSibling();
            }
        };
        for(const Job& Each : Jobs)
        {
            if(!Each.NewRoot.IsSet()) { continue; }
            Apply(Each.Old, Each.NewRoot);
            EditedWorld->RefreshEntityBounds(Each.NewRoot);
        }
    }

    /* Recorded finished, so a redo brings them back with their values; then
     * the originals go, recorded as deletes. */
    int Converted = 0;
    for(const Job& Each : Jobs)
    {
        if(!Each.NewRoot.IsSet()) { continue; }
        ++Converted;
        if(bMacro)
        {
            Record(new EditorHistory::EntityLifetimeCommand(*this, QStringLiteral("Place %1").arg(GetDisplayName(Each.NewRoot)),
                                                            true, EditorHistory::CaptureEntity(*EditedWorld, Each.NewRoot)));
        }
        DestroyEntity(Each.OldRoot);
    }

    if(bMacro)
    {
        History->endMacro();
        LatestRecorded = nullptr;
        emit HistoryChanged();
    }
    return Converted;
}

EntityID EditorDocument::SpawnPrefabRecorded(const QString& InRuntimePath, const GameCore::TransformPose& InAt)
{
    const GameCore::PrefabInstance Instance = ActiveWorld->SpawnPrefab(InRuntimePath.toUtf8().constData(), InAt);
    if(!Instance.IsValid())
    {
        return EntityID();
    }
    const EntityID Root = Instance.GetRoot().GetID();
    ActiveWorld->RefreshEntityBounds(Root);

    if(IsRecording())
    {
        Record(new EditorHistory::EntityLifetimeCommand(*this, QStringLiteral("Place %1").arg(GetDisplayName(Root)),
                                                        true, EditorHistory::CaptureEntity(*EditedWorld, Root)));
    }
    return Root;
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

const GameCore::WorldSettings& EditorDocument::GetWorldSettings() const
{
    return EditedWorld->GetSettings();
}

bool EditorDocument::SetWorldSettings(const GameCore::WorldSettings& InSettings)
{
    if(RefuseWhileBorrowed("Edit World Settings"))
    {
        return false;
    }
    GameCore::WorldSettings Clamped = InSettings;
    Clamped.Lighting.Clamp();
    const GameCore::WorldSettings Before = EditedWorld->GetSettings();
    if(Clamped == Before)
    {
        return false;
    }
    EditedWorld->GetSettings() = Clamped;
    if(IsRecording())
    {
        Record(new EditorHistory::WorldSettingsCommand(*this, QStringLiteral("Edit World Settings"), Before, Clamped));
    }
    emit WorldSettingsChanged();
    return true;
}

void EditorDocument::SetEntityMobility(const EntityID InEntity, const GameCore::EMobility InMobility)
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity) || ActiveWorld->GetEntity(InEntity).GetAuthoredMobility() == InMobility)
    {
        return;
    }
    const EditorHistory::EntityMembers Before = EditorHistory::CaptureMembers(*ActiveWorld, InEntity);
    ActiveWorld->GetEntity(InEntity).SetMobility(InMobility);
    QueueEntitiesChanged();
    RecordMembersChange(InEntity, Before, QStringLiteral("Make %1 %2").arg(GetDisplayName(InEntity),
                                                                         QString::fromLatin1(GameCore::MobilityName(InMobility))));
}

void EditorDocument::SetEntitiesMobility(const std::vector<EntityID>& InEntities, const GameCore::EMobility InMobility)
{
    std::vector<EntityID> Changing;
    for(const EntityID Each : InEntities)
    {
        if(ActiveWorld->IsAlive(Each) && ActiveWorld->GetEntity(Each).GetAuthoredMobility() != InMobility) { Changing.push_back(Each); }
    }
    if(Changing.size() <= 1)
    {
        if(!Changing.empty()) { SetEntityMobility(Changing.front(), InMobility); }
        return;
    }
    const bool bMacro = IsRecording();
    if(bMacro) { History->beginMacro(QStringLiteral("Make %1 entities %2").arg(Changing.size()).arg(GameCore::MobilityName(InMobility))); }
    for(const EntityID Each : Changing)
    {
        SetEntityMobility(Each, InMobility);
    }
    if(bMacro)
    {
        History->endMacro();
        LatestRecorded = nullptr;
        emit HistoryChanged();
    }
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

    if(IsSelected(InEntity))
    {
        std::vector<EntityID> Rest = Selected;
        Rest.erase(std::remove(Rest.begin(), Rest.end(), InEntity), Rest.end());
        SetSelection(Rest);
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

    /* Its own shape and its children's, so a prefab's root (or any parent)
     * frames the whole of it. */
    GameCore::AABB Box = GetOwnBounds(InEntity);
    for(EntityID Child = E.GetFirstChild(); Child.IsSet(); Child = ActiveWorld->GetEntity(Child).GetNextSibling())
    {
        Box = GameCore::AABB::Union(Box, GetEntityBounds(Child));
    }
    if(!Box.IsEmpty() || !E.GetComponent<GameCore::TransformComponent>())
    {
        return Box;
    }

    /* A marker for anything with only a position (a camera, an empty). */
    const double Half = 25.0;
    const EngineCore::Vector3 Position = E.GetWorldPose().Position;
    return GameCore::AABB(Position - EngineCore::Vector3(Half, Half, Half),
                          Position + EngineCore::Vector3(Half, Half, Half));
}

GameCore::AABB EditorDocument::GetOwnBounds(const EntityID InEntity) const
{
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
    return GameCore::AABB();
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

// === Clipboard ===============================================================

namespace
{
    /* What CopySelection took, shared by every document. Serial matches the
     * marker on the system clipboard. */
    struct EntityClipboard
    {
        std::vector<EditorHistory::EntitySnapshot> Roots;
        std::vector<GameCore::TransformPose>       Poses;   // each root's world pose
        uint64_t                                   Serial = 0;
    };
    EntityClipboard GClipboard;
    uint64_t        GClipboardSerial = 0;

    const char* const ClipboardFormat = "application/x-nimbleengine-entities";

    /* The system clipboard -- none without a GUI application (the tests). */
    QClipboard* SystemClipboard()
    {
        return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
    }
}

int EditorDocument::CopySelection()
{
    const std::vector<EntityID> Roots = GetSelectedRoots();
    if(Roots.empty())
    {
        return 0;
    }
    EntityClipboard Copied;
    for(const EntityID Root : Roots)
    {
        Copied.Roots.push_back(EditorHistory::CaptureEntity(*ActiveWorld, Root));
        Copied.Poses.push_back(ActiveWorld->GetEntity(Root).GetWorldPose());
    }
    Copied.Serial = ++GClipboardSerial;
    GClipboard    = std::move(Copied);
    if(QClipboard* System = SystemClipboard())
    {
        auto* Data = new QMimeData();
        Data->setData(QString::fromLatin1(ClipboardFormat), QByteArray::number(qulonglong(GClipboard.Serial)));
        Data->setText(QStringLiteral("%1 entities").arg(GClipboard.Roots.size()));
        System->setMimeData(Data);
    }
    return int(Roots.size());
}

int EditorDocument::CutSelection()
{
    const int Copied = CopySelection();
    if(Copied > 0)
    {
        DeleteSelection();
    }
    return Copied;
}

int EditorDocument::DeleteSelection()
{
    const std::vector<EntityID> Roots = GetSelectedRoots();
    if(Roots.empty())
    {
        return 0;
    }
    if(Roots.size() == 1)
    {
        DestroyEntity(Roots.front());
        return 1;
    }
    const bool bMacro = IsRecording();
    if(bMacro) { History->beginMacro(QStringLiteral("Delete %1 entities").arg(Roots.size())); }
    for(const EntityID Root : Roots)
    {
        DestroyEntity(Root);
    }
    if(bMacro)
    {
        History->endMacro();
        LatestRecorded = nullptr;
        emit HistoryChanged();
    }
    return int(Roots.size());
}

bool EditorDocument::CanPaste() const
{
    if(GClipboard.Roots.empty())
    {
        return false;
    }
    if(const QClipboard* System = SystemClipboard())
    {
        const QMimeData* Data = System->mimeData();
        return Data && Data->data(QString::fromLatin1(ClipboardFormat)) == QByteArray::number(qulonglong(GClipboard.Serial));
    }
    return true;
}

std::vector<EntityID> EditorDocument::Paste()
{
    if(!CanPaste())
    {
        return {};
    }
    const size_t Count = GClipboard.Roots.size();
    return PasteSnapshots(GClipboard.Roots, GClipboard.Poses,
                          Count == 1 ? QStringLiteral("Paste %1").arg(QString::fromStdString(GClipboard.Roots.front().Name))
                                     : QStringLiteral("Paste %1 entities").arg(Count));
}

std::vector<EntityID> EditorDocument::DuplicateSelection()
{
    std::vector<EditorHistory::EntitySnapshot> Roots;
    std::vector<GameCore::TransformPose>       Poses;
    for(const EntityID Root : GetSelectedRoots())
    {
        Roots.push_back(EditorHistory::CaptureEntity(*ActiveWorld, Root));
        Poses.push_back(ActiveWorld->GetEntity(Root).GetWorldPose());
    }
    if(Roots.empty())
    {
        return {};
    }
    return PasteSnapshots(Roots, Poses, Roots.size() == 1 ? QStringLiteral("Duplicate %1").arg(QString::fromStdString(Roots.front().Name))
                                                          : QStringLiteral("Duplicate %1 entities").arg(Roots.size()));
}

std::vector<EntityID> EditorDocument::PasteSnapshots(const std::vector<EditorHistory::EntitySnapshot>& InRoots,
                                                     const std::vector<GameCore::TransformPose>& InPoses, const QString& InLabel)
{
    /* Prefabs do not nest: in a prefab, copies of instances come in loose. */
    std::vector<EntityID> Made = EditorHistory::RestoreCopies(*ActiveWorld, InRoots, InPoses, !bSingleRoot);

    /* A prefab keeps one root: a copy left at the top goes under it. */
    if(bSingleRoot)
    {
        const std::vector<EntityID> Tops = GetRootEntities();
        const auto Keep = std::find_if(Tops.begin(), Tops.end(), [&Made](const EntityID InTop)
        {
            return std::find(Made.begin(), Made.end(), InTop) == Made.end();
        });
        for(const EntityID Each : Made)
        {
            if(Keep != Tops.end() && !ActiveWorld->GetEntity(Each).GetParent().IsSet())
            {
                ActiveWorld->GetEntity(Each).SetParent(*Keep, true);
                ActiveWorld->RefreshEntityBounds(Each);
            }
        }
    }

    if(IsRecording())
    {
        History->beginMacro(InLabel);
        for(const EntityID Each : Made)
        {
            Record(new EditorHistory::EntityLifetimeCommand(*this, QStringLiteral("Create %1").arg(GetDisplayName(Each)), true,
                                                            EditorHistory::CaptureEntity(*EditedWorld, Each)));
        }
        History->endMacro();
        LatestRecorded = nullptr;
        emit HistoryChanged();
    }
    ActiveWorld->RefreshSpatialIndex();
    QueueEntitiesChanged();
    SetSelection(Made, Made.empty() ? EntityID() : Made.back());
    return Made;
}

// === Selection ===============================================================

GameCore::EntityID EditorDocument::GetPickTarget(EntityID InEntity) const
{
    if(!ActiveWorld || !ActiveWorld->IsAlive(InEntity))
    {
        return InEntity;
    }
    /* Up from instance to instance: a nested prefab's root sits inside its
     * outer instance's hierarchy, so the climb goes on from its parent. */
    GameCore::World& World = *ActiveWorld;
    for(EntityID At = InEntity; At.IsSet(); )
    {
        const GameCore::PrefabInstanceID Instance = World.GetEntity(At).GetInstance();
        if(Instance.IsSet())
        {
            InEntity = World.GetInstance(Instance).GetRoot().GetID();
            At       = InEntity;
        }
        At = World.GetEntity(At).GetParent();
    }
    return InEntity;
}

bool EditorDocument::IsSelected(const EntityID InEntity) const
{
    return InEntity.IsSet() && std::find(Selected.begin(), Selected.end(), InEntity) != Selected.end();
}

std::vector<EntityID> EditorDocument::GetSelectedRoots() const
{
    std::vector<EntityID> Roots;
    for(const EntityID Each : Selected)
    {
        bool bUnderSelected = false;
        for(EntityID Up = ActiveWorld->GetEntity(Each).GetParent(); Up.IsSet() && !bUnderSelected;
            Up = ActiveWorld->GetEntity(Up).GetParent())
        {
            bUnderSelected = IsSelected(Up);
        }
        if(!bUnderSelected) { Roots.push_back(Each); }
    }
    return Roots;
}

void EditorDocument::SetSelection(const EntityID InEntity)
{
    ApplySelection({ InEntity }, InEntity);
}

void EditorDocument::SetSelection(const std::vector<EntityID>& InEntities, const EntityID InPrimary)
{
    ApplySelection(InEntities, InPrimary);
}

void EditorDocument::ToggleSelected(const EntityID InEntity)
{
    std::vector<EntityID> Next = Selected;
    if(IsSelected(InEntity))
    {
        Next.erase(std::remove(Next.begin(), Next.end(), InEntity), Next.end());
        ApplySelection(Next, InEntity == Selection ? EntityID() : Selection);
        return;
    }
    Next.push_back(InEntity);
    ApplySelection(Next, InEntity);
}

void EditorDocument::AddToSelection(const EntityID InEntity)
{
    std::vector<EntityID> Next = Selected;
    Next.push_back(InEntity);
    ApplySelection(Next, InEntity);
}

void EditorDocument::ApplySelection(const std::vector<EntityID>& InEntities, EntityID InPrimary, const bool bInForceSignal)
{
    /* Alive, once each, in order; the primary moved to the end. */
    std::vector<EntityID> Next;
    for(const EntityID Each : InEntities)
    {
        if(ActiveWorld && ActiveWorld->IsAlive(Each) && std::find(Next.begin(), Next.end(), Each) == Next.end())
        {
            Next.push_back(Each);
        }
    }
    const auto Primary = std::find(Next.begin(), Next.end(), InPrimary);
    if(Primary == Next.end())
    {
        InPrimary = Next.empty() ? GameCore::InvalidEntityID() : Next.back();
    }
    else
    {
        Next.erase(Primary);
        Next.push_back(InPrimary);
    }
    if(!bInForceSignal && Next == Selected && InPrimary == Selection)
    {
        return;
    }
    Selected  = std::move(Next);
    Selection = InPrimary;
    emit SelectionChanged(Selection);
}

void EditorDocument::NotifyPropertyEdited(const EntityID InEntity, const GameCore::ComponentTypeID InType)
{
    emit PropertyEdited(InEntity, InType);

    /* Only the selection's values from before the write are known (the
     * shadow); every panel that writes properties edits the selection. A
     * group edit records once, at its end. */
    if(!IsRecording() || bGroupEditing || InEntity != ShadowEntity)
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
    const QUndoCommand* Previous = LatestRecorded;
    History->push(InCommand);   // merged or dropped (obsolete) inside, or appended
    LatestRecorded = History->index() > 0 ? History->command(History->index() - 1) : nullptr;
    emit HistoryChanged();
    if(LatestRecorded && LatestRecorded != Previous)
    {
        emit CommandRecorded(LatestRecorded->text());
    }
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

void EditorDocument::BeginGroupEdit(const std::vector<EntityID>& InEntities, const GameCore::ComponentTypeID InType,
                                    const QString& InLabel)
{
    BeginEditGesture(InLabel);
    bGroupEditing  = true;
    GroupEditType  = InType;
    GroupEditLabel = InLabel;
    GroupEdit.clear();
    for(const EntityID Each : InEntities)
    {
        if(ActiveWorld->IsAlive(Each))
        {
            GroupEdit.push_back({ ActiveWorld->GetEntity(Each).GetGuid(), EditorHistory::CaptureProperties(*ActiveWorld, Each, InType) });
        }
    }
}

void EditorDocument::EndGroupEdit(const bool bInCancel)
{
    if(!bGroupEditing)
    {
        return;
    }
    bGroupEditing = false;
    struct Changed { const GroupEditEntry* Entry; std::vector<uint8_t> After; };
    std::vector<Changed> Changes;
    for(const GroupEditEntry& Entry : GroupEdit)
    {
        const EntityID Entity = ActiveWorld->FindEntityByGuid(Entry.Guid);
        if(!Entity.IsSet())
        {
            continue;
        }
        if(bInCancel)
        {
            EditorHistory::ApplyProperties(*ActiveWorld, Entity, GroupEditType, Entry.Before);
            ActiveWorld->RefreshEntityBounds(Entity);
            emit PropertyEdited(Entity, GroupEditType);
            continue;
        }
        std::vector<uint8_t> After = EditorHistory::CaptureProperties(*ActiveWorld, Entity, GroupEditType);
        if(After != Entry.Before) { Changes.push_back({ &Entry, std::move(After) }); }
    }
    if(!bInCancel && !Changes.empty() && IsRecording())
    {
        History->beginMacro(QStringLiteral("%1 %2 entities").arg(GroupEditLabel).arg(Changes.size()));
        for(Changed& Each : Changes)
        {
            const EntityID Entity = EditedWorld->FindEntityByGuid(Each.Entry->Guid);
            Record(new EditorHistory::PropertyEditCommand(*this, QStringLiteral("%1 %2").arg(GroupEditLabel, GetDisplayName(Entity)),
                                                          Each.Entry->Guid, GroupEditType, Each.Entry->Before, std::move(Each.After), 0));
        }
        History->endMacro();
        LatestRecorded = nullptr;
    }
    GroupEdit.clear();
    RefreshShadow();
    EndEditGesture();
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
    const auto Found = std::find(Self->Selected.begin(), Self->Selected.end(), InEntity);
    if(Found != Self->Selected.end())
    {
        Self->Selected.erase(Found);
        if(InEntity == Self->Selection)
        {
            Self->Selection = Self->Selected.empty() ? GameCore::InvalidEntityID() : Self->Selected.back();
        }
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
    Selected.clear();

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

void EditorDocument::ActivateWorld(GameCore::World* InWorld, const EntityID InSelection, const std::vector<EntityID>& InSelected)
{
    ActiveWorld = InWorld;
    HookActiveWorld();

    Selected.clear();
    for(const EntityID Each : InSelected.empty() ? std::vector<EntityID>{ InSelection } : InSelected)
    {
        if(ActiveWorld && ActiveWorld->IsAlive(Each)) { Selected.push_back(Each); }
    }
    Selection = ActiveWorld && ActiveWorld->IsAlive(InSelection) ? InSelection
              : Selected.empty() ? GameCore::InvalidEntityID() : Selected.back();
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
    const bool bNative = InPath.startsWith(QString::fromLatin1(GameCore::NATIVE_PREFAB_PREFIX));
    const GameCore::PrefabAsset* Registered = bNative ? GameCore::FindOrLoadPrefab(Path.constData()) : nullptr;
    if(Registered) { Loaded = *Registered; }
    if(bNative ? !Registered : !GameCore::LoadPrefabFile(Path.constData(), Loaded))
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
        EditedSelected  = Selected;
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
    ActivateWorld(EditedWorld, EditedSelection, EditedSelected);
    EditedSelection = GameCore::InvalidEntityID();
    EditedSelected.clear();
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
