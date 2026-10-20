#include "Private/RenderCore/WorldBridge.h"
#include "Private/RenderCore/CustomRenderPassInternal.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/MaterialDescription.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/Components.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Public/RenderCore/RenderScene.h"
#include "Private/RenderCore/ShadowSetup.h"
#include "Public/RenderCore/RenderTargetAsset.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Private/RenderCore/RenderComponentProxy.h"

/* SPLITSCREEN, for testing views far apart before a game gives each its own
 * camera: "x,y,z" moves each view i > 0 that has no camera of its own by i
 * times that, from the camera it shares. Empty: none. Development only. */
NIMBLE_CONFIG_STRING(GSplitscreenDebugOffset, "Splitscreen", "DebugOffset", "", nullptr);

// GameCore is this file's whole subject -- it reads World's pools and
// components on every line.
using namespace GameCore;

/* Off: every light lit live, the bake left loaded but unused -- the Lit view
 * the editor's Show > Lighting > Live and LightLab's L key switch to. */
NIMBLE_CONFIG_BOOL(GBakedLighting, "Rendering", "BakedLighting", true, nullptr);

/* A world that does not tick -- the editor's, outside Play -- still plays its
 * animations in the viewport; off, each holds its first frame (ANIMATION.md). */
NIMBLE_CONFIG_BOOL(GAnimationPreview, "Animation", "PreviewInEditor", true, nullptr);

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
            uint8_t           bStatic = 0;   // the owner's mobility
            RenderBakedDraw   Baked;         // a Render component's; empty for the rest
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
        Animation     = AnimationSystem::Attach(InWorld);

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
        for(EntityID& Each : ViewCameras) { Each = EntityID(); }
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
        Procedural.Reset();

        BoundWorld->OnComponentAdded().Remove(AddedHandle);
        BoundWorld->OnComponentRemoved().Remove(RemovedHandle);
        AddedHandle   = DelegateHandle();
        RemovedHandle = DelegateHandle();
        AnimationSystem::Detach(*BoundWorld);
        Animation = nullptr;
        PosedProxies.clear();

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
        Bridge->StaticRecords.erase(Proxy);   // a later proxy at this address starts fresh
        Bridge->PosedProxies.erase(Proxy);    // and is never sent a pose meant for this one

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

    RenderView WorldBridge::BuildView(const FrameInfo& InFrame, const uint32_t InViewIndex) const
    {
        /* The view's camera if one is set and still there, else view 0's,
         * else the first active CameraComponent -- see DESIGN.md's "View".
         * (0, 1, -1) matches GDICamera's own default constructor when no
         * camera is active. */
        EngineCore::Vector3 Shared(0.0, 0.0, 0.0);   // [Splitscreen] DebugOffset, for a view on another's camera
        double X = 0.0, Y = 0.0, Z = 0.0;
        if(InViewIndex > 0 && std::sscanf(GSplitscreenDebugOffset.GetString(), "%lf,%lf,%lf", &X, &Y, &Z) == 3)
        {
            Shared = EngineCore::Vector3(X, Y, Z) * double(InViewIndex);
        }
        for(const uint32_t Index : { InViewIndex, 0u })
        {
            const EntityID Wanted = Index < RENDER_VIEWS_MAX ? ViewCameras[Index] : EntityID();
            if(BoundWorld && Wanted.IsSet() && BoundWorld->IsAlive(Wanted))
            {
                if(const CameraComponent* Camera = BoundWorld->GetEntity(Wanted).GetComponent<CameraComponent>())
                {
                    return ViewThrough(*Camera, InFrame, Index == InViewIndex ? EngineCore::Vector3(0.0, 0.0, 0.0) : Shared);
                }
            }
        }

        if(BoundWorld)
        {
            ComponentPool* CameraPool = BoundWorld->FindPool(CameraComponent::StaticType().ID);
            if(CameraPool)
            {
                const auto* Cameras = static_cast<const CameraComponent*>(CameraPool->GetDenseData());
                const size_t Count  = CameraPool->GetCount();
                for(size_t Index = 0; Index < Count; ++Index)
                {
                    if(Cameras[Index].bActive)
                    {
                        return ViewThrough(Cameras[Index], InFrame, Shared);
                    }
                }
            }
        }

        return RenderView::MakeOrthographic(InFrame.ViewportPixels, EngineCore::Vector3(0.0, 1.0, -1.0), -1.0f, 1000.0f);
    }

    RenderView WorldBridge::ViewThrough(const CameraComponent& Camera, const FrameInfo& InFrame, const EngineCore::Vector3& InOffset) const
    {
        EngineCore::Vector3 Origin(0.0, 1.0, -1.0);
        const GameCore::TransformPose Pose = PoseOf(Camera.Owner);
        if(Camera.Owner.HasSibling<TransformComponent>() || Camera.Owner.GetEntity().HasParent())
        {
            Origin = Pose.Position;
        }
        Origin = Origin + InOffset;   // [Splitscreen] DebugOffset; zero otherwise

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

    RenderLighting WorldBridge::BuildLighting(const EngineCore::Vector3& InEye) const
    {
        RenderLighting Lighting;
        Lighting.Shadows = ReadShadowSettings();
        if(!BoundWorld)
        {
            return Lighting;
        }

        /* Drawing with a bake: its static lights are in it, so they are left
         * out here -- the sun's default too, if a static one was the only one. */
        const auto Baked = [this](const ComponentOwner& InOwner) { return bBakeActive && InOwner.GetEntity().IsStatic(); };
        if(bBakeActive)
        {
            Lighting.bBaked    = 1;
            Lighting.bBakedSky = Bake->bSkyBaked ? 1 : 0;
            Lighting.bBakedDirectional = Bake->bDirectional ? 1 : 0;
            Lighting.BakedAtlasCount = uint32_t(std::min<size_t>(Bake->Atlases.size(), RENDER_BAKED_ATLASES_MAX));
            for(uint32_t Atlas = 0; Atlas < Lighting.BakedAtlasCount; ++Atlas)
            {
                std::snprintf(Lighting.BakedAtlases[Atlas], sizeof(Lighting.BakedAtlases[Atlas]), "%s", Bake->Atlases[Atlas].c_str());
            }
        }

        if(ComponentPool* Pool = BoundWorld->FindPool(DirectionalLightComponent::StaticType().ID))
        {
            const auto* Lights = static_cast<const DirectionalLightComponent*>(Pool->GetDenseData());
            bool bBakedSun = false, bLiveSun = false;
            /* Inset shadows' light: the live sun, else the first baked one. */
            const auto InsetFrom = [&Lighting](const DirectionalLightComponent& InLight, const bool bInBaked)
            {
                Lighting.bInsetLight           = InLight.bCastShadows && InLight.Intensity > 0.0f ? 1 : 0;
                Lighting.bInsetLightBaked      = bInBaked ? 1 : 0;
                Lighting.InsetLightDirection   = LightForward(PoseOf(InLight.Owner).Rotation);
                Lighting.InsetShadowTint       = InLight.ShadowTint;
                Lighting.InsetShadowBias       = InLight.ShadowBias;
                Lighting.InsetShadowNormalBias = InLight.ShadowNormalBias;
            };
            for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
            {
                const DirectionalLightComponent& Light = Lights[Index];
                if(!Light.bActive) { continue; }
                if(Baked(Light.Owner))
                {
                    if(!bBakedSun) { InsetFrom(Light, true); }
                    bBakedSun = true;
                    continue;
                }
                bLiveSun = true;
                InsetFrom(Light, false);
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
            if(bBakedSun && !bLiveSun)
            {
                Lighting.LightIntensity    = 0.0f;
                Lighting.bLightCastShadows = 0;
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
                std::memcpy(Lighting.SkyEnvironment, Skies[Index].Environment, sizeof(Lighting.SkyEnvironment));
                Lighting.SkyEnvironment[sizeof(Lighting.SkyEnvironment) - 1] = '\0';
                Lighting.SkyEnvironmentIntensity = std::max(Skies[Index].EnvironmentIntensity, 0.0f);
                Lighting.SkyEnvironmentRotation  = Skies[Index].EnvironmentRotation;
                Lighting.bSkyBackground          = Skies[Index].bDrawBackground;
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
                if(!Light.bActive || Light.Intensity <= 0.0f || Light.Radius <= 0.0f || Baked(Light.Owner)) { continue; }
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
                if(!Light.bActive || Light.Intensity <= 0.0f || Light.Range <= 0.0f || Baked(Light.Owner)) { continue; }
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

        /* Indices are sorted, not the lights: RenderLocalLight's SIMD math is
         * over-aligned, which std::stable_sort's buffer refuses on MSVC. */
        const bool bShadows = Lighting.Shadows.bEnabled != 0;
        std::vector<uint32_t> Order(Found.size());
        for(uint32_t Index = 0; Index < uint32_t(Order.size()); ++Index) { Order[Index] = Index; }
        std::stable_sort(Order.begin(), Order.end(), [&Found, &InEye, bShadows](const uint32_t InIndexA, const uint32_t InIndexB)
        {
            const RenderLocalLight& InA = Found[InIndexA];
            const RenderLocalLight& InB = Found[InIndexB];
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
            Lighting.Locals[Index] = Found[Order[Index]];
        }
        return Lighting;
    }

    void WorldBridge::UpdateBake()
    {
        const WorldLightingSettings& Settings = BoundWorld->GetSettings().Lighting;
        const std::string Path = Settings.bBakedLighting ? Settings.BakedData : std::string();
        const uint64_t Generation = BakedLightingCache::GetGeneration();
        if(Path != BakePath || Generation != BakeGeneration)
        {
            BakePath       = Path;
            BakeGeneration = Generation;
            Bake           = Path.empty() ? nullptr : BakedLightingCache::Find(Path);
            BakeEntries.clear();
            if(Bake)
            {
                for(const BakedLightmapEntry& Each : Bake->Entries)
                {
                    EntityGuid Guid;
                    Guid.Hi = Each.GuidHi;
                    Guid.Lo = Each.GuidLo;
                    if(Each.Atlas < RENDER_BAKED_ATLASES_MAX) { BakeEntries[Guid] = &Each; }
                }
                if(Bake->Atlases.size() > RENDER_BAKED_ATLASES_MAX)
                {
                    RENDERCORE_LOG_WARN("baked lighting '" + Path + "' has " + std::to_string(Bake->Atlases.size())
                                        + " lightmaps; only " + std::to_string(RENDER_BAKED_ATLASES_MAX) + " are drawn")
                }
            }
        }
        bBakeActive = Bake != nullptr && GBakedLighting.GetBool();
    }

    RenderBakedDraw WorldBridge::BakedDrawFor(const Entity& InEntity, const EngineCore::Vector3& InPosition) const
    {
        RenderBakedDraw Out;
        std::memset(static_cast<void*>(&Out), 0, sizeof(Out));   // compared bytewise in static records
        if(!bBakeActive) { return Out; }

        if(InEntity.IsStatic())
        {
            const auto Found = BakeEntries.find(InEntity.GetGuid());
            if(Found != BakeEntries.end())
            {
                Out.Mode  = uint8_t(EMaterialBakedLighting::Lightmap);
                Out.Atlas = Found->second->Atlas;
                std::memcpy(Out.ScaleOffset, Found->second->ScaleOffset, sizeof(Out.ScaleOffset));
                return Out;
            }
        }

        BakedAmbientCube Cube;
        const float Point[3] = { float(InPosition.GetX()), float(InPosition.GetY()), float(InPosition.GetZ()) };
        if(Bake->Probes.Sample(Point, Cube))
        {
            Out.Mode = uint8_t(EMaterialBakedLighting::AmbientCube);
            std::memcpy(Out.Cube, Cube.Faces, sizeof(Out.Cube));
        }
        return Out;
    }

    RenderPostProcess WorldBridge::BuildPostProcess(const RenderView& InView) const
    {
        RenderPostProcess Result;
        ComponentPool* Pool = BoundWorld ? BoundWorld->FindPool(PostProcessComponent::StaticType().ID) : nullptr;
        if(!Pool)
        {
            /* No volumes: a sequence's override alone. */
            if(BoundWorld) { ApplyPostProcessOverride(BoundWorld, Result.Settings); }
            return Result;
        }

        /* Where the camera is: the eye, or an orthographic view's centre (its
         * Origin is the bottom-left corner). */
        EngineCore::Vector3 Eye = InView.Origin;
        if(InView.Projection.Data[3][2] == 0.0f)
        {
            Eye = Eye + EngineCore::Vector3(InView.ViewportPixels.Width * 0.5, InView.ViewportPixels.Height * 0.5, 0.0);
        }

        /* Every enabled volume the camera is near enough to, by weight. */
        struct Volume { const PostProcessComponent* Component; float Weight; };
        std::vector<Volume> Volumes;
        const auto* Components = static_cast<const PostProcessComponent*>(Pool->GetDenseData());
        for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
        {
            const PostProcessComponent& Each = Components[Index];
            if(!Each.bEnabled || Each.Stack[0] == '\0' || !(Each.Weight > 0.0f))
            {
                continue;
            }
            float Weight = std::min(Each.Weight, 1.0f);
            if(const BoundsComponent* Bounds = Each.Owner.GetSibling<BoundsComponent>())
            {
                if(Bounds->World.IsEmpty())
                {
                    continue;   // not refit yet: nowhere
                }
                /* How far outside the box the camera is; 0 inside. */
                double Outside = 0.0;
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    const double P   = Axis == 0 ? Eye.GetX() : Axis == 1 ? Eye.GetY() : Eye.GetZ();
                    const double Min = Axis == 0 ? Bounds->World.Min.GetX() : Axis == 1 ? Bounds->World.Min.GetY() : Bounds->World.Min.GetZ();
                    const double Max = Axis == 0 ? Bounds->World.Max.GetX() : Axis == 1 ? Bounds->World.Max.GetY() : Bounds->World.Max.GetZ();
                    const double Gap = P < Min ? Min - P : P > Max ? P - Max : 0.0;
                    Outside += Gap * Gap;
                }
                Weight *= GetPostProcessVolumeWeight(float(std::sqrt(Outside)), Each.BlendDistance);
            }
            if(Weight > 0.0f)
            {
                Volumes.push_back({ &Each, Weight });
            }
        }
        std::stable_sort(Volumes.begin(), Volumes.end(), [](const Volume& InA, const Volume& InB)
        {
            return InA.Component->Priority < InB.Component->Priority;
        });

        for(const Volume& Each : Volumes)
        {
            const std::string Path(Each.Component->Stack, strnlen(Each.Component->Stack, sizeof(Each.Component->Stack)));
            if(const std::shared_ptr<const PostProcessSettings> Stack = PostProcessStackCache::Find(Path))
            {
                BlendPostProcess(Result.Settings, *Stack, Each.Weight);
            }
        }

        /* A sequence's stacks and settings, over the volumes (PostProcessOverride). */
        ApplyPostProcessOverride(BoundWorld, Result.Settings);

        /* Materials run by stage, then order; volumes' order breaks ties. */
        PostProcessSettings& S = Result.Settings;
        std::stable_sort(S.Materials, S.Materials + S.MaterialCount, [](const PostProcessMaterial& InA, const PostProcessMaterial& InB)
        {
            return InA.Stage != InB.Stage ? InA.Stage < InB.Stage : InA.Order < InB.Order;
        });
        return Result;
    }

    void WorldBridge::ExtractAndRender(const FrameInfo& InFrame, const RenderView* InViewOverride,
                                       const RenderViewRect* InRects, const uint32_t InCount, const RenderViewSites* InSites)
    {
        if(!BoundWorld)
        {
            return;
        }

        /* A reimported mesh may change a static proxy's bounds: send every
         * static record once more. */
        if(MeshAssetCache::GetGeneration() != StaticMeshGeneration)
        {
            StaticMeshGeneration = MeshAssetCache::GetGeneration();
            StaticRecords.clear();
        }

        /* Before the records, which carry what it says of each entity. */
        UpdateBake();
        Procedural.Update(*BoundWorld);
        const ComponentTypeID RenderType = RenderComponent::StaticType().ID;

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
            size_t   Written = 0;
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
                std::memset(static_cast<void*>(&Record), 0, sizeof(Record));   // padding too: static records are compared bytewise
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
                Record.bStatic = OwnerEntity.IsStatic() ? 1 : 0;
                if(Type == RenderType)
                {
                    Record.Baked = BakedDrawFor(OwnerEntity, Record.Transform.Position);
                }

                std::memcpy(Write, &Record, sizeof(ExtractHeader));
                std::memcpy(Write + sizeof(ExtractHeader), Component, TypeInfo.SizeBytes);

                /* STATIC: sent only when something differs from the last
                 * record -- an edit, a move, the mobility itself. */
                if(Record.bStatic && Handle.Ptr)
                {
                    std::vector<uint8_t>& Last = StaticRecords[Handle.Ptr];
                    const size_t Bytes = sizeof(ExtractHeader) + TypeInfo.SizeBytes;
                    if(Last.size() == Bytes && std::memcmp(Last.data(), Write, Bytes) == 0)
                    {
                        continue;
                    }
                    Last.assign(Write, Write + Bytes);
                }
                else if(Handle.Ptr)
                {
                    StaticRecords.erase(Handle.Ptr);
                }

                Write += Stride;
                ++Written;
            }
            if(Written == 0)
            {
                EngineCore::Memory::Free(Blob);
                continue;
            }

            const size_t   BlobStride = Stride;
            const size_t   BlobCount  = Written;
            void* const    BlobPtr    = Blob;

            ENQUEUE_RENDER_COMMAND(ApplyExtraction)([BlobPtr, BlobStride, BlobCount](RenderContext&)
            {
                const uint8_t* Read = static_cast<const uint8_t*>(BlobPtr);
                for(size_t Index = 0; Index < BlobCount; ++Index)
                {
                    const auto* Record = reinterpret_cast<const ExtractHeader*>(Read);
                    if(RenderProxy* Proxy = Record->Handle.Ptr)
                    {
                        Proxy->SetStatic(Record->bStatic != 0);
                        Proxy->SetTransform(Record->Transform);
                        Proxy->SetBakedDraw(Record->Baked);
                        Proxy->ApplyComponent(Read + sizeof(ExtractHeader));
                        Proxy->MarkChanged();
                    }
                    Read += BlobStride;
                }
                EngineCore::Memory::Free(BlobPtr);
            });
        }

        ExtractPoses(InFrame);

        /* The views (SPLITSCREEN: one per rectangle), each with the light
         * nearest its eye and the post process volumes it is in. Trivially
         * copyable, so the command carries them as a value. */
        struct FrameViews
        {
            RenderView        Views[RENDER_VIEWS_MAX];
            RenderLighting    Lightings[RENDER_VIEWS_MAX];
            RenderPostProcess PostProcesses[RENDER_VIEWS_MAX];
            uint32_t          Count = 1;
            EngineCore::Size  Window;
            RenderViewSites   Sites;
        };
        FrameViews Frame;
        if(InSites) { Frame.Sites = *InSites; }
        Frame.Count  = InRects && InCount > 1 ? std::min<uint32_t>(InCount, RENDER_VIEWS_MAX) : 1;
        Frame.Window = InFrame.ViewportPixels;
        for(uint32_t Index = 0; Index < Frame.Count; ++Index)
        {
            FrameInfo ViewFrame = InFrame;
            int32_t   X = 0, Y = 0;
            if(Frame.Count > 1)
            {
                const RenderViewRect& Rect = InRects[Index];
                ViewFrame.ViewportPixels = EngineCore::Size(std::max(Rect.Width, 0), std::max(Rect.Height, 0));
                X = Rect.X;
                Y = Rect.Y;
            }
            RenderView& View = Frame.Views[Index];
            View = Index == 0 && InViewOverride ? *InViewOverride : BuildView(ViewFrame, Index);
            View.ViewportPixels = ViewFrame.ViewportPixels;   // the render targets are sized from this
            View.ViewportX      = X;
            View.ViewportY      = Y;
            View.ViewIndex      = Index;
            View.RevealCount    = RevealCount;
            View.RevealRadius   = RevealRadius;
            for(uint32_t Point = 0; Point < RevealCount; ++Point) { View.RevealPoints[Point] = RevealPoints[Point]; }
            if(Animation) { Animation->AddView(InFrame.FrameNumber, View.Origin, View.Projection * View.View); }
            Frame.Lightings[Index]     = BuildLighting(View.Origin);
            Frame.PostProcesses[Index] = BuildPostProcess(View);
        }
        /* Captures and reflection probes once a frame, for the main view. */
        const SceneCaptureList Captures = BuildCaptures(Frame.Views[0], InFrame.DeltaSeconds);
        RenderScene* SceneCapture = Scene;

        /* Its custom render passes' values for this frame (CustomRenderPass.h);
         * the render thread finds the passes by the World's address. */
        ExtractCustomRenderPasses(BoundWorld);
        const void* PassWorld = BoundWorld;

        ENQUEUE_RENDER_COMMAND(SetViewAndRender)([SceneCapture, Frame, Captures, PassWorld](RenderContext& Ctx)
        {
            SceneCapture->SetViews(Frame.Views, Frame.Lightings, Frame.PostProcesses, Frame.Count, Frame.Window, &Frame.Sites);
            SceneCapture->SetCaptures(Captures);
            SceneCapture->SetCustomPassWorld(PassWorld);
            SceneCapture->Render(Ctx);
        });
    }

    namespace
    {
        /* One proxy's pose in ExtractPoses' blob: this header, then Count
         * AnimMatrix. Trivially copyable, freed whole, as the extraction is. */
        struct PoseHeader
        {
            RenderProxy* Proxy = nullptr;
            uint32_t     Count = 0;
            uint32_t     Pad   = 0;
        };
    }

    void WorldBridge::ExtractPoses(const FrameInfo& InFrame)
    {
        if(!Animation)
        {
            return;
        }

        /* A world that does not tick (the editor's) still shows each
         * animation -- playing, or where it starts -- and follows edits to it. */
        Animation->PrepareFrame(InFrame.FrameNumber, float(InFrame.DeltaSeconds), GAnimationPreview.GetBool());

        struct Posed { RenderProxy* Proxy; const std::vector<AnimMatrix>* Bones; };
        std::vector<Posed> Now;
        BoundWorld->Each<AnimationComponent>([&](const Entity& InEntity, AnimationComponent&)
        {
            const RenderComponent* Render = InEntity.GetComponent<RenderComponent>();
            const AnimInstance*    Instance = Animation->FindInstance(InEntity.GetID());
            if(Render && Render->Proxy.Ptr && Instance && Instance->HasPose())
            {
                Now.push_back({ Render->Proxy.Ptr, &Instance->GetModelPose() });
            }
        });

        /* Posed last frame and not now: back to the bind pose (Count 0). */
        std::vector<RenderProxy*> Released;
        for(const void* Each : PosedProxies)
        {
            const bool bStill = std::any_of(Now.begin(), Now.end(), [Each](const Posed& InPosed) { return InPosed.Proxy == Each; });
            if(!bStill) { Released.push_back(static_cast<RenderProxy*>(const_cast<void*>(Each))); }
        }
        PosedProxies.clear();
        for(const Posed& Each : Now) { PosedProxies.insert(Each.Proxy); }
        if(Now.empty() && Released.empty())
        {
            return;
        }

        size_t Bytes = 0;
        for(const Posed& Each : Now) { Bytes += sizeof(PoseHeader) + Each.Bones->size() * sizeof(AnimMatrix); }
        Bytes += Released.size() * sizeof(PoseHeader);
        void* Blob = EngineCore::Memory::Allocate(Bytes, alignof(PoseHeader), EngineCore::POOL_Default, nullptr,
                                                  EngineCore::AF_None, EngineCore::GImageID);
        if(!Blob)
        {
            RENDERCORE_LOG_ERROR("WorldBridge::ExtractPoses: allocation failed; poses skipped this frame")
            return;
        }
        uint8_t* Write = static_cast<uint8_t*>(Blob);
        const auto Put = [&Write](RenderProxy* InProxy, const std::vector<AnimMatrix>* InBones)
        {
            PoseHeader Header;
            Header.Proxy = InProxy;
            Header.Count = InBones ? uint32_t(InBones->size()) : 0u;
            std::memcpy(Write, &Header, sizeof(Header));
            Write += sizeof(Header);
            if(Header.Count > 0)
            {
                std::memcpy(Write, InBones->data(), Header.Count * sizeof(AnimMatrix));
                Write += Header.Count * sizeof(AnimMatrix);
            }
        };
        for(const Posed& Each : Now)          { Put(Each.Proxy, Each.Bones); }
        for(RenderProxy* Each : Released)     { Put(Each, nullptr); }

        /* After this frame's ApplyExtraction commands: the component's mesh
         * is the proxy's before its pose is. Only Render components' proxies
         * are posed, so the cast is to theirs. */
        const size_t BlobBytes = Bytes;
        ENQUEUE_RENDER_COMMAND(ApplyPoses)([Blob, BlobBytes](RenderContext&)
        {
            const uint8_t* Read = static_cast<const uint8_t*>(Blob);
            const uint8_t* End  = Read + BlobBytes;
            while(Read + sizeof(PoseHeader) <= End)
            {
                PoseHeader Header;
                std::memcpy(&Header, Read, sizeof(Header));
                Read += sizeof(Header);
                /* The bones follow the 16-byte header: aligned for floats. */
                static_cast<RenderComponentProxy*>(Header.Proxy)->SetSkinPose(reinterpret_cast<const AnimMatrix*>(Read), Header.Count);
                Read += Header.Count * sizeof(AnimMatrix);
            }
            EngineCore::Memory::Free(Blob);
        });
    }

    SceneCaptureList WorldBridge::BuildCaptures(const RenderView& InView, const double InDeltaSeconds)
    {
        SceneCaptureList List;
        CaptureClock += std::max(InDeltaSeconds, 0.0);
        ComponentPool* Pool = BoundWorld ? BoundWorld->FindPool(SceneCaptureProbeComponent::StaticType().ID) : nullptr;
        auto* Probes = Pool ? static_cast<SceneCaptureProbeComponent*>(Pool->GetDenseData()) : nullptr;
        for(size_t Index = 0; Pool && Index < Pool->GetCount() && List.Count < RENDER_CAPTURES_MAX; ++Index)
        {
            SceneCaptureProbeComponent& Probe = Probes[Index];
            if(!Probe.bEnabled || Probe.Target[0] == '\0')
            {
                continue;
            }
            const GameCore::Entity Owner = Probe.Owner.GetEntity();
            ProbeState& State = ProbeStates[Owner.GetID().Pack()];
            bool bDue = Probe.bCaptureNow != 0;
            switch(Probe.Frequency)
            {
                case SCF_EveryFrame: bDue = true; break;
                case SCF_Interval:   bDue = bDue || CaptureClock - State.LastCapture >= double(std::max(Probe.Interval, 0.0f)); break;
                case SCF_Once:       bDue = bDue || !State.bCaptured; break;
                default:             break;
            }
            if(!bDue)
            {
                continue;
            }

            const GameCore::TransformPose Pose = PoseOf(Probe.Owner);
            const GameCore::Rotation3     Axes = GameCore::TransformMath::EulerToRotation(Pose.Rotation);
            SceneCaptureRequest& Request = List.Requests[List.Count];
            Request = SceneCaptureRequest();
            std::memcpy(Request.Target, Probe.Target, sizeof(Request.Target));
            Request.Target[sizeof(Request.Target) - 1] = '\0';
            Request.Mode       = Probe.Mode;
            Request.Resolution = uint32_t(std::clamp(Probe.Resolution, 0, int32_t(RENDER_TARGET_MAX_SIZE)));
            Request.Position   = Pose.Position;
            Request.Normal     = Axes.GetAxis(1);
            Request.NearClip   = Probe.NearClip;
            Request.FarClip    = Probe.FarClip;
            Request.bShadows   = Probe.bCaptureShadows;

            /* A mirror the camera cannot see is not worth a capture (unless asked for). */
            if(Probe.Mode == SCM_PlanarMirror && !Probe.bCaptureNow && !InView.Bounds.IsEmpty())
            {
                const EngineCore::Vector3 Half = Axes.GetAxis(0) * double(Probe.ExtentX) + Axes.GetAxis(2) * double(Probe.ExtentZ);
                const EngineCore::Vector3 Other = Axes.GetAxis(0) * double(Probe.ExtentX) - Axes.GetAxis(2) * double(Probe.ExtentZ);
                GameCore::AABB Mirror;
                for(const EngineCore::Vector3& Corner : { Pose.Position + Half, Pose.Position - Half, Pose.Position + Other, Pose.Position - Other })
                {
                    Mirror = GameCore::AABB::Union(Mirror, GameCore::AABB(Corner, Corner));
                }
                if(!Mirror.Overlaps(InView.Bounds))
                {
                    continue;
                }
            }

            /* Its own subtree, left out (a mirror, a chrome ball). */
            if(Probe.bHideOwner)
            {
                std::vector<GameCore::EntityID> Pending{ Owner.GetID() };
                while(!Pending.empty() && Request.ExcludedCount < RENDER_CAPTURE_EXCLUDED_MAX)
                {
                    const GameCore::EntityID Each = Pending.back();
                    Pending.pop_back();
                    Request.Excluded[Request.ExcludedCount++] = Each;
                    for(GameCore::EntityID Child = BoundWorld->GetEntity(Each).GetFirstChild(); Child.IsSet();
                        Child = BoundWorld->GetEntity(Child).GetNextSibling())
                    {
                        Pending.push_back(Child);
                    }
                }
            }

            ++List.Count;
            State.LastCapture = CaptureClock;
            State.bCaptured   = true;
            Probe.bCaptureNow = 0;
        }
        BuildReflectionProbes(InView, List);
        return List;
    }

    void WorldBridge::BuildReflectionProbes(const RenderView& InView, SceneCaptureList& OutList)
    {
        OutList.ProbeCount = 0;
        ComponentPool* Pool = BoundWorld ? BoundWorld->FindPool(ReflectionProbeComponent::StaticType().ID) : nullptr;
        if(!Pool)
        {
            return;
        }
        /* Every enabled probe whose influence the view reaches, nearest the camera first. */
        struct Candidate { ReflectionProbeComponent* Probe; ReflectionProbeRequest Request; double Distance; };
        std::vector<Candidate> Found;
        auto* Probes = static_cast<ReflectionProbeComponent*>(Pool->GetDenseData());
        for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
        {
            ReflectionProbeComponent& Probe = Probes[Index];
            if(!Probe.bEnabled)
            {
                continue;
            }
            const GameCore::Entity Owner = Probe.Owner.GetEntity();
            const GameCore::TransformPose Pose = PoseOf(Probe.Owner);
            ReflectionProbeRequest Request;
            Request.Key        = Owner.GetID().Pack();
            Request.Position   = Pose.Position;
            Request.Radius     = std::max(Probe.Radius, 1.0f);
            Request.Extent[0]  = float(std::max(double(Probe.BoxExtent.GetX()), 0.0));
            Request.Extent[1]  = float(std::max(double(Probe.BoxExtent.GetY()), 0.0));
            Request.Extent[2]  = float(std::max(double(Probe.BoxExtent.GetZ()), 0.0));
            Request.Intensity  = std::max(Probe.Intensity, 0.0f);
            Request.NearClip   = std::max(Probe.NearClip, 0.01f);
            Request.FarClip    = std::max(Probe.FarClip, Request.NearClip + 1.0f);
            Request.Resolution = uint32_t(std::clamp(Probe.Resolution, 16, 512));
            Request.bShadows   = Probe.bCaptureShadows;

            const bool bBox = Request.Extent[0] > 0.0f && Request.Extent[1] > 0.0f && Request.Extent[2] > 0.0f;
            const EngineCore::Vector3 Reach = bBox ? EngineCore::Vector3(Request.Extent[0], Request.Extent[1], Request.Extent[2])
                                                   : EngineCore::Vector3(Request.Radius, Request.Radius, Request.Radius);
            const GameCore::AABB Influence(Pose.Position - Reach, Pose.Position + Reach);
            if(!InView.Bounds.IsEmpty() && !Influence.Overlaps(InView.Bounds))
            {
                continue;
            }

            ProbeState& State = ProbeStates[Request.Key ^ 0x5265666cull];   // apart from a SceneCaptureProbe on the same entity
            bool bDue = Probe.bCaptureNow != 0;
            switch(Probe.Frequency)
            {
                case SCF_EveryFrame: bDue = true; break;
                case SCF_Interval:   bDue = bDue || CaptureClock - State.LastCapture >= double(std::max(Probe.Interval, 0.0f)); break;
                case SCF_Once:       bDue = bDue || !State.bCaptured; break;
                default:             break;
            }
            Request.bDue = bDue ? 1 : 0;
            Found.push_back({ &Probe, Request, (Pose.Position - InView.Origin).GetLength() });
        }
        /* Indices are sorted, not the candidates: a ReflectionProbeRequest's
         * SIMD math is over-aligned, which std::stable_sort's buffer refuses on MSVC. */
        std::vector<uint32_t> Order(Found.size());
        for(uint32_t Index = 0; Index < uint32_t(Order.size()); ++Index) { Order[Index] = Index; }
        std::stable_sort(Order.begin(), Order.end(), [&Found](const uint32_t InA, const uint32_t InB)
        {
            return Found[InA].Distance < Found[InB].Distance;
        });
        for(const uint32_t Sorted : Order)
        {
            Candidate& Each = Found[Sorted];
            if(OutList.ProbeCount >= RENDER_REFLECTION_PROBES_MAX)
            {
                break;
            }
            OutList.Probes[OutList.ProbeCount++] = Each.Request;
            if(Each.Request.bDue)
            {
                ProbeState& State = ProbeStates[Each.Request.Key ^ 0x5265666cull];
                State.LastCapture = CaptureClock;
                State.bCaptured   = true;
                Each.Probe->bCaptureNow = 0;
            }
        }
    }
}
