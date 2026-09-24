#if WITH_EDITOR

#include "Public/Editor/EditorContext.h"

#include <chrono>
#include <cstring>
#include <string>

#include <QTimer>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldSerializer.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/RenderCore/Renderer.h"

using GameCore::EntityID;

EditorContext::EditorContext(QObject* InParent)
    : QObject(InParent)
{
    /* Before anything asks for an asset -- the Renderer loads its cooked
     * shaders through this on the render thread. */
    EngineCore::SetFileProvider(&AssetProvider);

    CreateWorld();
}

EditorContext::~EditorContext()
{
    Shutdown();
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

void EditorContext::Shutdown()
{
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

    /* No World::Tick -- but an edited Transform or Bounds still has to reach
     * the spatial index, which picking and a later play mode read. */
    if(EditedWorld)
    {
        EditedWorld->RefreshSpatialIndex();
    }

    /* Application::RunFrame's order, and for its reasons: the fence, then the
     * only point where collecting is sound, then present the frame the fence
     * just completed, then kick the next. */
    Renderer->WaitForPreviousFrame();
    EngineCore::Memory::CollectGarbage();
    Renderer->PresentToWindow(InWindow);

    Renderer->SetViewOverride(&InView);

    RenderCore::FrameInfo Info;
    Info.FrameNumber    = FrameNumber;
    Info.DeltaSeconds   = FrameNumber == 0 ? 0.0 : double(NowNs - PreviousFrameNs) / 1'000'000'000.0;
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
    ReplaceWorld();
}

void EditorContext::CreateDemoScene()
{
    ReplaceWorld();

    struct DemoPrimitive
    {
        const char*       Name;
        double            X, Y, Z;
        float             Width, Height;
        EngineCore::Color Tint;
        double            RotationZ;
    };

    /* World units are the game's pixels, +Y down -- RenderView's convention. */
    const DemoPrimitive Demo[] =
    {
        { "Floor",      400.0, 520.0, 60.0, 900.0,  40.0, EngineCore::Color( 70,  76,  90),  0.0 },
        { "RedBox",     200.0, 400.0,  0.0, 120.0, 120.0, EngineCore::Color(220,  70,  60),  0.0 },
        { "GreenBox",   420.0, 380.0, 30.0, 140.0, 180.0, EngineCore::Color( 80, 200, 110), 15.0 },
        { "BlueBox",    640.0, 420.0,-40.0, 100.0, 100.0, EngineCore::Color( 70, 130, 230), 45.0 },
        { "Sun",        650.0, 120.0,120.0,  80.0,  80.0, EngineCore::Color(250, 210,  80),  0.0 },
    };

    for(const DemoPrimitive& Item : Demo)
    {
        const EntityID ID = CreateEntity(QString::fromLatin1(Item.Name), EngineCore::Vector3(Item.X, Item.Y, Item.Z));
        const GameCore::Entity E = EditedWorld->GetEntity(ID);

        if(GameCore::TransformComponent* Transform = E.GetComponent<GameCore::TransformComponent>())
        {
            Transform->Rotation = EngineCore::Vector3(0.0, 0.0, Item.RotationZ);
        }

        RenderCore::PrimitiveComponent Primitive;
        Primitive.Width  = Item.Width;
        Primitive.Height = Item.Height;
        Primitive.Tint   = Item.Tint;
        E.AddComponent(Primitive);

        /* The quad's own extent in entity space -- the refit carries it
         * through the Transform, Scale included, as the proxy draws it. */
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(EngineCore::Vector3(-0.5 * Item.Width, -0.5 * Item.Height, 0.0),
                                      EngineCore::Vector3( 0.5 * Item.Width,  0.5 * Item.Height, 0.0));
        E.AddComponent(Bounds);
    }

    /* The game's camera -- what Sandbox-style hosts render through. The editor
     * viewport overrides it, but it is still an entity to select and edit. */
    const EntityID CameraID = CreateEntity(QStringLiteral("Camera"), EngineCore::Vector3(0.0, 0.0, -1.0));
    EditedWorld->GetEntity(CameraID).AddComponent<RenderCore::CameraComponent>();

    EditedWorld->RefreshSpatialIndex();
    QueueEntitiesChanged();
}

bool EditorContext::LoadWorld(const QString& InPath)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
    if(!EngineCore::LoadArchiveFromFile(InPath.toStdString(), Ar))
    {
        return false;
    }

    ReplaceWorld();

    if(!GameCore::SerializeWorld(Ar, *EditedWorld) || Ar.HasError())
    {
        CORE_LOG_ERROR("[Editor] '" + InPath.toStdString() + "' is not a readable world; starting an empty scene")
        ReplaceWorld();
        return false;
    }

    EditedWorld->RefreshSpatialIndex();
    QueueEntitiesChanged();
    return true;
}

bool EditorContext::SaveWorld(const QString& InPath)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
    if(!GameCore::SerializeWorld(Ar, *EditedWorld) || Ar.HasError())
    {
        CORE_LOG_ERROR("[Editor] could not serialize the world")
        return false;
    }
    return EngineCore::SaveArchiveToFile(InPath.toStdString(), Ar);
}

// === Entities ================================================================

EntityID EditorContext::CreateEntity(const QString& InName, const EngineCore::Vector3& InPosition)
{
    const QByteArray Name = InName.toUtf8();
    const GameCore::Entity E = EditedWorld->CreateEntity(Name.constData());

    GameCore::TransformComponent Transform;
    Transform.Position = InPosition;
    E.AddComponent(Transform);

    return E.GetID();
}

void EditorContext::DestroyEntity(const EntityID InEntity)
{
    if(!EditedWorld || !EditedWorld->IsAlive(InEntity))
    {
        return;
    }
    if(InEntity == Selection)
    {
        SetSelection(GameCore::InvalidEntityID());
    }
    EditedWorld->DestroyEntity(InEntity);
}

QString EditorContext::GetDisplayName(const EntityID InEntity) const
{
    const char* Name = EditedWorld ? EditedWorld->GetEntity(InEntity).GetName() : "";
    if(Name[0] != '\0')
    {
        return QString::fromUtf8(Name);
    }
    return QStringLiteral("Entity %1").arg(InEntity.Index);
}

GameCore::AABB EditorContext::GetEntityBounds(const EntityID InEntity) const
{
    if(!EditedWorld || !EditedWorld->IsAlive(InEntity))
    {
        return GameCore::AABB();
    }
    const GameCore::Entity E = EditedWorld->GetEntity(InEntity);

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

    if(const RenderCore::PrimitiveComponent* Primitive = E.GetComponent<RenderCore::PrimitiveComponent>())
    {
        const GameCore::AABB Quad(EngineCore::Vector3(-0.5 * Primitive->Width, -0.5 * Primitive->Height, 0.0),
                                  EngineCore::Vector3( 0.5 * Primitive->Width,  0.5 * Primitive->Height, 0.0));
        return GameCore::AABB::Transformed(Quad, Transform->GetLocalMatrix());
    }

    /* A marker for anything with only a position (a camera, an empty). */
    const double Half = 25.0;
    return GameCore::AABB(Transform->Position - EngineCore::Vector3(Half, Half, Half),
                          Transform->Position + EngineCore::Vector3(Half, Half, Half));
}

GameCore::AABB EditorContext::GetSceneBounds() const
{
    if(!EditedWorld)
    {
        return GameCore::AABB();
    }

    struct Collect { const EditorContext* Self; GameCore::AABB Box; } Ctx{ this, GameCore::AABB() };
    EditedWorld->ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        auto& C = *static_cast<Collect*>(InUserData);
        C.Box = GameCore::AABB::Union(C.Box, C.Self->GetEntityBounds(InEntity));
    }, &Ctx);
    return Ctx.Box;
}

// === Selection ===============================================================

void EditorContext::SetSelection(EntityID InEntity)
{
    if(!EditedWorld || !EditedWorld->IsAlive(InEntity))
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
    /* A rename changes what the outliner shows. */
    if(InType == GameCore::NameComponent::StaticType().ID)
    {
        QueueEntitiesChanged();
    }
    emit PropertyEdited(InEntity, InType);
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

    CreatedHandle          = EditedWorld->OnEntityCreated().Add(&EditorContext::OnEntityCreated, this);
    DestroyedHandle        = EditedWorld->OnEntityDestroyed().Add(&EditorContext::OnEntityDestroyed, this);
    ComponentAddedHandle   = EditedWorld->OnComponentAdded().Add(&EditorContext::OnComponentChanged, this);
    ComponentRemovedHandle = EditedWorld->OnComponentRemoved().Add(&EditorContext::OnComponentChanged, this);

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

    /* The Renderer's proxies must go before the components they mirror. */
    if(Renderer)
    {
        Renderer->UnbindWorld();
    }

    EditedWorld->OnEntityCreated().Remove(CreatedHandle);
    EditedWorld->OnEntityDestroyed().Remove(DestroyedHandle);
    EditedWorld->OnComponentAdded().Remove(ComponentAddedHandle);
    EditedWorld->OnComponentRemoved().Remove(ComponentRemovedHandle);

    EngineCore::Memory::Dealloc(EditedWorld);
    EditedWorld = nullptr;
}

#endif // WITH_EDITOR
