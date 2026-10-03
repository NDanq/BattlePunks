#include "Private/RenderCore/WorldBridge.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/Components.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/RenderScene.h"
#include "Private/RenderCore/ShadowSetup.h"

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

    namespace
    {
        /* The world pose of a component's entity: its Transform's, through
         * its parents; the identity with neither. */
        GameCore::TransformPose PoseOf(const GameCore::ComponentOwner& InOwner)
        {
            if(InOwner.HasSibling<TransformComponent>() || InOwner.GetEntity().HasParent())
            {
                return InOwner.GetEntity().GetWorldPose();
            }
            return GameCore::TransformPose();
        }

        /* The world box a perspective view sees, out to its far plane. */
        GameCore::AABB PerspectiveBounds(const RenderView& InView)
        {
            const EngineCore::Matrix4x4& P = InView.Projection;
            const EngineCore::Matrix4x4& V = InView.View;
            const EngineCore::Vector3 Right(V.Data[0][0], V.Data[0][1], V.Data[0][2]);
            const EngineCore::Vector3 Up(V.Data[1][0], V.Data[1][1], V.Data[1][2]);
            const EngineCore::Vector3 Forward(V.Data[2][0], V.Data[2][1], V.Data[2][2]);
            const double HalfX = 1.0 / std::abs(double(P.Data[0][0]));
            const double HalfY = 1.0 / std::abs(double(P.Data[1][1]));

            GameCore::AABB Bounds(InView.Origin, InView.Origin);
            for(int Corner = 0; Corner < 4; ++Corner)
            {
                const double SignX = (Corner & 1) ? 1.0 : -1.0;
                const double SignY = (Corner & 2) ? 1.0 : -1.0;
                const EngineCore::Vector3 Point = InView.Origin
                    + (Forward + Right * (SignX * HalfX) + Up * (SignY * HalfY)) * double(InView.ZFar);
                Bounds = GameCore::AABB::Union(Bounds, GameCore::AABB(Point, Point));
            }
            return Bounds;
        }
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

                    const GameCore::TransformPose Pose = PoseOf(Camera.Owner);
                    if(Camera.Owner.HasSibling<TransformComponent>() || Camera.Owner.GetEntity().HasParent())
                    {
                        Origin = Pose.Position;
                    }

                    if(Camera.Projection == CP_Perspective)
                    {
                        /* Looks along its forward, as a light does; never straight
                         * up or down, which MakePerspective cannot aim along. */
                        EngineCore::Vector3 Forward = LightForward(Pose.Rotation);
                        if(std::abs(Forward.GetY()) > 0.9999)
                        {
                            Forward = EngineCore::Vector3(0.0, Forward.GetY() > 0.0 ? 0.9999 : -0.9999, 0.01).GetNormalize();
                        }
                        const float ZNear = std::max(Camera.ZNear, 1.0f);
                        const float ZFar  = std::max(Camera.ZFar, ZNear + 1.0f);
                        const float FovY  = std::clamp(Camera.FovY, 1.0f, 170.0f);
                        RenderView View = RenderView::MakePerspective(InFrame.ViewportPixels, Origin, Forward, FovY, ZNear, ZFar);
                        View.Bounds = PerspectiveBounds(View);
                        return View;
                    }

                    return RenderView::MakeOrthographic(InFrame.ViewportPixels, Origin, Camera.ZNear, Camera.ZFar);
                }
            }
        }

        return RenderView::MakeOrthographic(InFrame.ViewportPixels, Origin, -1.0f, 1000.0f);
    }

    RenderLighting WorldBridge::BuildLighting(const EngineCore::Vector3& InEye) const
    {
        RenderLighting Lighting;
        Lighting.Shadows = ReadShadowSettings();
        if(!BoundWorld)
        {
            return Lighting;
        }

        if(ComponentPool* Pool = BoundWorld->FindPool(DirectionalLightComponent::StaticType().ID))
        {
            const auto* Lights = static_cast<const DirectionalLightComponent*>(Pool->GetDenseData());
            for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
            {
                const DirectionalLightComponent& Light = Lights[Index];
                if(!Light.bActive) { continue; }
                Lighting.LightDirection          = LightForward(PoseOf(Light.Owner).Rotation);
                Lighting.LightColor              = Light.Color;
                Lighting.LightIntensity          = Light.Intensity;
                Lighting.bLightCastShadows       = Light.bCastShadows;
                Lighting.LightShadowTechnique    = Light.ShadowTechnique;
                Lighting.LightShadowTint         = Light.ShadowTint;
                Lighting.LightShadowBias         = Light.ShadowBias;
                Lighting.LightShadowNormalBias   = Light.ShadowNormalBias;
                Lighting.LightShadowDistance     = Light.ShadowDistance;
                Lighting.LightCascadeCount       = Light.CascadeCount;
                Lighting.LightCascadeSplitLambda = Light.CascadeSplitLambda;
                break;
            }
        }

        if(ComponentPool* Pool = BoundWorld->FindPool(SkyLightComponent::StaticType().ID))
        {
            const auto* Skies = static_cast<const SkyLightComponent*>(Pool->GetDenseData());
            for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
            {
                if(!Skies[Index].bActive) { continue; }
                Lighting.SkyColor     = Skies[Index].SkyColor;
                Lighting.GroundColor  = Skies[Index].GroundColor;
                Lighting.SkyIntensity = Skies[Index].Intensity;
                break;
            }
        }

        /* Every active point and spot light; then shadow casters first, each
         * group nearest the eye first, and the first RENDER_LOCAL_LIGHT_MAX. */
        std::vector<RenderLocalLight> Found;
        if(ComponentPool* Pool = BoundWorld->FindPool(PointLightComponent::StaticType().ID))
        {
            const auto* Lights = static_cast<const PointLightComponent*>(Pool->GetDenseData());
            for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
            {
                const PointLightComponent& Light = Lights[Index];
                if(!Light.bActive || Light.Intensity <= 0.0f || Light.Radius <= 0.0f) { continue; }
                RenderLocalLight Local;
                Local.Type             = RLL_Point;
                Local.Position         = PoseOf(Light.Owner).Position;
                Local.Color            = Light.Color;
                Local.Intensity        = Light.Intensity;
                Local.Range            = Light.Radius;
                Local.bCastShadows     = Light.bCastShadows;
                Local.ShadowTechnique  = Light.ShadowTechnique == LST_Modulated ? LST_Modulated : LST_Standard;
                Local.ShadowTint       = Light.ShadowTint;
                Local.ShadowBias       = Light.ShadowBias;
                Local.ShadowNormalBias = Light.ShadowNormalBias;
                Found.push_back(Local);
            }
        }
        if(ComponentPool* Pool = BoundWorld->FindPool(SpotLightComponent::StaticType().ID))
        {
            const auto* Lights = static_cast<const SpotLightComponent*>(Pool->GetDenseData());
            for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
            {
                const SpotLightComponent& Light = Lights[Index];
                if(!Light.bActive || Light.Intensity <= 0.0f || Light.Range <= 0.0f) { continue; }
                const GameCore::TransformPose Pose = PoseOf(Light.Owner);
                RenderLocalLight Local;
                Local.Type             = RLL_Spot;
                Local.Position         = Pose.Position;
                Local.Direction        = LightForward(Pose.Rotation);
                Local.Color            = Light.Color;
                Local.Intensity        = Light.Intensity;
                Local.Range            = Light.Range;
                Local.OuterConeAngle   = std::clamp(Light.OuterConeAngle, 0.5f, 89.0f);
                Local.InnerConeAngle   = std::clamp(Light.InnerConeAngle, 0.0f, Local.OuterConeAngle);
                Local.bCastShadows     = Light.bCastShadows;
                Local.ShadowTechnique  = Light.ShadowTechnique == LST_Modulated ? LST_Modulated : LST_Standard;
                Local.ShadowTint       = Light.ShadowTint;
                Local.ShadowBias       = Light.ShadowBias;
                Local.ShadowNormalBias = Light.ShadowNormalBias;
                Found.push_back(Local);
            }
        }

        const bool bShadows = Lighting.Shadows.bEnabled != 0;
        std::stable_sort(Found.begin(), Found.end(), [&InEye, bShadows](const RenderLocalLight& InA, const RenderLocalLight& InB)
        {
            const bool bCastA = bShadows && InA.bCastShadows;
            const bool bCastB = bShadows && InB.bCastShadows;
            if(bCastA != bCastB) { return bCastA; }
            return (InA.Position - InEye).GetLength() < (InB.Position - InEye).GetLength();
        });

        if(Found.size() > RENDER_LOCAL_LIGHT_MAX && !bWarnedLocalLights)
        {
            bWarnedLocalLights = true;
            RENDERCORE_LOG_WARN("WorldBridge: " + std::to_string(Found.size()) + " point and spot lights are active; "
                                "only " + std::to_string(RENDER_LOCAL_LIGHT_MAX) + " light a frame (shadow casters, then the nearest)")
        }
        Lighting.LocalCount = uint32_t(std::min<size_t>(Found.size(), RENDER_LOCAL_LIGHT_MAX));
        for(uint32_t Index = 0; Index < Lighting.LocalCount; ++Index)
        {
            Lighting.Locals[Index] = Found[Index];
        }
        return Lighting;
    }

    RenderPostProcess WorldBridge::BuildPostProcess() const
    {
        RenderPostProcess List;
        ComponentPool* Pool = BoundWorld ? BoundWorld->FindPool(PostProcessComponent::StaticType().ID) : nullptr;
        if(!Pool)
        {
            return List;
        }

        const auto* Passes = static_cast<const PostProcessComponent*>(Pool->GetDenseData());
        std::vector<const PostProcessComponent*> Enabled;
        for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
        {
            if(Passes[Index].bEnabled && Passes[Index].Material[0] != '\0') { Enabled.push_back(&Passes[Index]); }
        }
        std::stable_sort(Enabled.begin(), Enabled.end(), [](const PostProcessComponent* InA, const PostProcessComponent* InB)
        {
            return InA->Priority < InB->Priority;
        });

        if(Enabled.size() > RENDER_POST_PROCESS_MAX)
        {
            RENDERCORE_LOG_WARN("WorldBridge: " + std::to_string(Enabled.size()) + " Post Process components are enabled; "
                                "only the first " + std::to_string(RENDER_POST_PROCESS_MAX) + " by Priority run")
        }
        for(const PostProcessComponent* Each : Enabled)
        {
            if(List.Count == RENDER_POST_PROCESS_MAX) { break; }
            static_assert(sizeof(List.Materials[0]) == sizeof(Each->Material), "a post-process name fits its slot");
            std::memcpy(List.Materials[List.Count], Each->Material, sizeof(List.Materials[0]));
            List.Materials[List.Count][sizeof(List.Materials[0]) - 1] = '\0';
            ++List.Count;
        }
        return List;
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

                /* Where it is in the world: under a parent, the composed
                 * pose (a parent without a Transform still carries it). */
                const GameCore::Entity OwnerEntity = Owner.GetEntity();
                if(OwnerEntity.HasParent())
                {
                    const GameCore::TransformPose Pose = OwnerEntity.GetWorldPose();
                    Record.Transform.Position = Pose.Position;
                    Record.Transform.Rotation = Pose.Rotation;
                    Record.Transform.Scale    = Pose.Scale;
                }
                else if(const TransformComponent* Transform = Owner.GetSibling<TransformComponent>())
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
        const RenderLighting    Lighting    = BuildLighting(View.Origin);
        const RenderPostProcess PostProcess = BuildPostProcess();
        RenderScene* SceneCapture = Scene;

        ENQUEUE_RENDER_COMMAND(SetViewAndRender)([SceneCapture, View, Lighting, PostProcess](RenderContext& Ctx)
        {
            SceneCapture->SetView(View);
            SceneCapture->SetLighting(Lighting);
            SceneCapture->SetPostProcess(PostProcess);
            SceneCapture->Render(Ctx);
        });
    }
}
