#include "Private/RenderCore/WorldBridge.h"

#include <cstddef>
#include <cstring>

#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/Components.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"

// GameCore is this file's whole subject -- it reads World's pools and
// components on every line.
using namespace GameCore;

/*
 * See WorldBridge.h for the contract; this is the "how". Three pieces:
 * proxy lifecycle (bound to World's own delegates), the per-frame extraction
 * batch, and the view build -- each documented in DESIGN.md under its own
 * heading, referenced from the matching function below rather than repeated
 * here.
 */

namespace RenderCore
{
    namespace
    {
        /* One record per component extracted this frame -- see DESIGN.md's
         * "Extraction". Fixed layout: RenderProxyHandle then RenderTransform
         * then the component's own raw bytes, all in one Memory::Allocate'd
         * blob so a single Free tears the whole batch down. */
        struct ExtractHeader
        {
            RenderProxyHandle Handle;
            RenderTransform   Transform;
            // component bytes follow immediately after this header
        };

        size_t RecordStrideFor(const ComponentTypeInfo& InType)
        {
            /* Rounded up to InType's own alignment so the component bytes
             * that follow ExtractHeader land aligned -- the same rounding
             * CoreMemory.h's FixedBlockBytesFor<T>() applies for its header. */
            const size_t Align = InType.Alignment > alignof(ExtractHeader) ? InType.Alignment : alignof(ExtractHeader);
            const size_t Raw   = sizeof(ExtractHeader) + InType.SizeBytes;
            return (Raw + Align - 1) & ~(Align - 1);
        }
    }

    WorldBridge::WorldBridge(RenderScene& InScene) : Scene(&InScene)
    {
    }

    WorldBridge::~WorldBridge()
    {
        UnbindWorld();
    }

    void WorldBridge::BindWorld(World& InWorld)
    {
        if(BoundWorld)
        {
            RENDERCORE_LOG_WARN("WorldBridge::BindWorld called while already bound to a World; unbinding the previous one first")
            UnbindWorld();
        }

        BoundWorld = &InWorld;

        AddedHandle   = InWorld.OnComponentAdded().Add(&WorldBridge::OnComponentAdded, this);
        RemovedHandle = InWorld.OnComponentRemoved().Add(&WorldBridge::OnComponentRemoved, this);

        /* Sweep every registered proxy type's pool for components that
         * predate this call -- BindWorld runs after a game has likely
         * already populated its World (Renderer::BindWorld, a later
         * integration step, runs once at startup). A component added from
         * here on is caught by the delegate above instead. */
        for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
        {
            if(!FindProxyFactory(Type))
            {
                continue;
            }

            ComponentPool* Pool = InWorld.FindPool(Type);
            if(!Pool)
            {
                continue;
            }

            const size_t Count = Pool->GetCount();
            for(size_t Index = 0; Index < Count; ++Index)
            {
                OnComponentAdded(Pool->GetEntityAt(Index), Type, this);
            }
        }
    }

    void WorldBridge::UnbindWorld()
    {
        if(!BoundWorld)
        {
            return;
        }

        /* Every proxy this bridge created carries a RenderProxyHandle back
         * on its owning component -- walk every registered type's pool one
         * more time and remove what is still there, the same sweep BindWorld
         * uses to catch what came before it. */
        for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
        {
            if(!FindProxyFactory(Type))
            {
                continue;
            }

            ComponentPool* Pool = BoundWorld->FindPool(Type);
            if(!Pool)
            {
                continue;
            }

            const size_t Count = Pool->GetCount();
            for(size_t Index = 0; Index < Count; ++Index)
            {
                OnComponentRemoved(Pool->GetEntityAt(Index), Type, this);
            }
        }

        FlushRenderCommands();

        BoundWorld->OnComponentAdded().Remove(AddedHandle);
        BoundWorld->OnComponentRemoved().Remove(RemovedHandle);
        AddedHandle   = DelegateHandle();
        RemovedHandle = DelegateHandle();

        BoundWorld = nullptr;
    }

    bool WorldBridge::IsBound() const
    {
        return BoundWorld != nullptr;
    }

    void WorldBridge::OnComponentAdded(const EntityID InEntity, const ComponentTypeID InType, void* InUserData)
    {
        auto* Bridge = static_cast<WorldBridge*>(InUserData);

        CreateProxyFn Factory = FindProxyFactory(InType);
        if(!Factory)
        {
            return;
        }

        /* The owner header is already written at this point -- World::AddComponentRaw
         * writes it immediately after copying the caller's init bytes, before
         * broadcasting OnComponentAdded (World.cpp) -- so GetComponentByType here
         * sees a fully-formed component. */
        void* Bytes = GameCore::Entity(Bridge->BoundWorld, InEntity).GetComponentByType(InType);
        if(!Bytes)
        {
            return;
        }

        RenderProxy* Proxy = Factory(InEntity, Bytes);
        if(!Proxy)
        {
            return;
        }

        /* Second thing in the struct, right after ComponentOwner --
         * RENDERCORE_PROXY_BODY()'s whole contract, checked at compile time
         * by RENDERCORE_DEFINE_PROXY_COMPONENT's own static_assert. */
        auto* HandleSlot = reinterpret_cast<RenderProxyHandle*>(static_cast<uint8_t*>(Bytes) + sizeof(ComponentOwner));
        HandleSlot->Ptr = Proxy;

        RenderScene* Scene = Bridge->Scene;
        ENQUEUE_RENDER_COMMAND(AddProxy)([Scene, Proxy](RenderContext& Ctx)
        {
            Scene->AddProxy(Proxy, Ctx);
        });
    }

    void WorldBridge::OnComponentRemoved(const EntityID InEntity, const ComponentTypeID InType, void* InUserData)
    {
        auto* Bridge = static_cast<WorldBridge*>(InUserData);

        CreateProxyFn Factory = FindProxyFactory(InType);
        if(!Factory)
        {
            return;
        }

        /* OnComponentRemoved fires BEFORE the pool actually removes the
         * bytes (World::RemoveComponentRaw / TickPhase's flush / World.cpp's
         * DestroyEntityImmediate all broadcast first), so the handle is
         * still readable here. */
        void* Bytes = GameCore::Entity(Bridge->BoundWorld, InEntity).GetComponentByType(InType);
        if(!Bytes)
        {
            return;
        }

        auto* HandleSlot = reinterpret_cast<RenderProxyHandle*>(static_cast<uint8_t*>(Bytes) + sizeof(ComponentOwner));
        RenderProxy* Proxy = HandleSlot->Ptr;
        HandleSlot->Ptr = nullptr;

        if(!Proxy)
        {
            return;
        }

        RenderScene* Scene = Bridge->Scene;
        ENQUEUE_RENDER_COMMAND(RemoveProxy)([Scene, Proxy](RenderContext& Ctx)
        {
            Scene->RemoveProxy(Proxy, Ctx);
        });
    }

    RenderView WorldBridge::BuildView(const FrameInfo& InFrame) const
    {
        /* The first active CameraComponent wins -- see DESIGN.md's "View".
         * (0, 1, -1) matches GDICamera's own default constructor when no
         * camera is active. */
        EngineCore::Vector3 Origin(0.0, 1.0, -1.0);

        if(BoundWorld)
        {
            ComponentPool* CameraPool = BoundWorld->FindPool(CameraComponent::StaticType().ID);
            if(CameraPool)
            {
                const auto* Cameras = static_cast<const CameraComponent*>(CameraPool->GetDenseData());
                const size_t Count  = CameraPool->GetCount();
                for(size_t Index = 0; Index < Count; ++Index)
                {
                    const CameraComponent& Camera = Cameras[Index];
                    if(!Camera.bActive)
                    {
                        continue;
                    }

                    if(const TransformComponent* Transform = Camera.Owner.GetSibling<TransformComponent>())
                    {
                        Origin = Transform->Position;
                    }

                    return RenderView::MakeOrthographic(InFrame.ViewportPixels, Origin, Camera.ZNear, Camera.ZFar);
                }
            }
        }

        return RenderView::MakeOrthographic(InFrame.ViewportPixels, Origin, -1.0f, 1000.0f);
    }

    void WorldBridge::ExtractAndRender(const FrameInfo& InFrame, const RenderView* InViewOverride)
    {
        if(!BoundWorld)
        {
            return;
        }

        for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
        {
            if(!FindProxyFactory(Type))
            {
                continue;
            }

            ComponentPool* Pool = BoundWorld->FindPool(Type);
            if(!Pool)
            {
                continue;
            }

            const size_t Count = Pool->GetCount();
            if(Count == 0)
            {
                continue;
            }

            const ComponentTypeInfo& TypeInfo = Pool->GetType();
            const size_t Stride = RecordStrideFor(TypeInfo);
            const uint8_t* Dense = static_cast<const uint8_t*>(Pool->GetDenseData());

            /* One Memory::Allocate'd blob for the whole batch -- see
             * DESIGN.md's "Extraction". Trivially destructible records (a
             * RenderProxyHandle, a RenderTransform and POD component bytes),
             * so Free alone is correct with no destructor to run, the same
             * reasoning RenderCommand.h's own capture rule states. */
            void* Blob = EngineCore::Memory::Allocate(Stride * Count, alignof(ExtractHeader), EngineCore::POOL_Default,
                                                       nullptr, EngineCore::AF_None, EngineCore::GImageID);
            if(!Blob)
            {
                RENDERCORE_LOG_ERROR("WorldBridge::ExtractAndRender: extraction batch allocation failed; skipping this type this frame")
                continue;
            }

            uint8_t* Write = static_cast<uint8_t*>(Blob);
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const uint8_t* Component = Dense + Index * TypeInfo.SizeBytes;
                const auto&    Owner     = *reinterpret_cast<const ComponentOwner*>(Component);
                const auto&    Handle    = *reinterpret_cast<const RenderProxyHandle*>(Component + sizeof(ComponentOwner));

                /* Built as a real, fully-constructed local -- not assigned
                 * into Blob's raw, never-constructed bytes, which would be
                 * writing through an object that does not exist yet -- then
                 * memcpy'd in, the same "build it properly, then memcpy the
                 * whole thing" shape ComponentPool::Add uses for the
                 * component bytes themselves (ComponentPool.cpp). */
                ExtractHeader Record;
                Record.Handle = Handle;

                if(const TransformComponent* Transform = Owner.GetSibling<TransformComponent>())
                {
                    Record.Transform.Position = Transform->Position;
                    Record.Transform.Rotation = Transform->Rotation;
                    Record.Transform.Scale    = Transform->Scale;
                }
                else
                {
                    Record.Transform = RenderTransform();
                }

                std::memcpy(Write, &Record, sizeof(ExtractHeader));
                std::memcpy(Write + sizeof(ExtractHeader), Component, TypeInfo.SizeBytes);

                Write += Stride;
            }

            const size_t   BlobStride = Stride;
            const size_t   BlobCount  = Count;
            void* const    BlobPtr    = Blob;

            ENQUEUE_RENDER_COMMAND(ApplyExtraction)([BlobPtr, BlobStride, BlobCount](RenderContext&)
            {
                const uint8_t* Read = static_cast<const uint8_t*>(BlobPtr);
                for(size_t Index = 0; Index < BlobCount; ++Index)
                {
                    const auto* Record = reinterpret_cast<const ExtractHeader*>(Read);
                    if(RenderProxy* Proxy = Record->Handle.Ptr)
                    {
                        Proxy->SetTransform(Record->Transform);
                        Proxy->ApplyComponent(Read + sizeof(ExtractHeader));
                    }
                    Read += BlobStride;
                }
                EngineCore::Memory::Free(BlobPtr);
            });
        }

        RenderView View = InViewOverride ? *InViewOverride : BuildView(InFrame);
        View.ViewportPixels = InFrame.ViewportPixels;   // the render targets are sized from this
        RenderScene* SceneCapture = Scene;

        ENQUEUE_RENDER_COMMAND(SetViewAndRender)([SceneCapture, View](RenderContext& Ctx)
        {
            SceneCapture->SetView(View);
            SceneCapture->Render(Ctx);
        });
    }
}
