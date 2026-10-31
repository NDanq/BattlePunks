#ifndef RENDERCORE_RENDERPROXY_H
#define RENDERCORE_RENDERPROXY_H

#include <cstring>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Matrix.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderTypes.h"

/*
 * The render-thread mirror of a GameCore entity that draws -- Unreal's
 * FPrimitiveSceneProxy contract, restated over this engine's own pieces.
 *
 * LIFECYCLE. Constructed on the GAME THREAD (so it can read the component
 * and entity it mirrors), then handed to RenderScene::AddProxy through an
 * AddProxy command -- from that instant, the game thread never dereferences
 * it again, only the opaque RenderProxyHandle below. InitRenderResources
 * runs on the RENDER THREAD, once, right after AddProxy: GPU resources
 * belong there, never in the constructor. ApplyComponent runs every frame,
 * fed the mirrored component's current bytes by WorldBridge's extraction
 * pass (a later build-order step). ReleaseRenderResources runs on the render
 * thread, once, right before RemoveProxy frees the object.
 *
 * PLAIN POD MEMBERS, NO PIMPL -- a hot module may subclass this (its own
 * proxy type, registered through RegisterProxyType below), so the layout is
 * stated here as frozen, the same FROZEN LAYOUT discipline
 * GameCore::ComponentTypeInfo and RenderCommandHeader both state for the
 * same cross-image reason.
 */
namespace RenderCore
{
    class RenderContext;

    class RENDERCORE_API RenderProxy
    {
    public:
        explicit RenderProxy(const GameCore::EntityID InOwner) : Owner(InOwner) {}
        virtual ~RenderProxy() = default;

        RenderProxy(const RenderProxy&)            = delete;
        RenderProxy& operator=(const RenderProxy&) = delete;

        /* RENDER THREAD, called by RenderScene::AddProxy right after this
         * proxy is stored -- create GDI resources HERE, never in the
         * constructor, which runs on the game thread and has no render
         * context to create them with. */
        virtual void InitRenderResources(RenderContext& InContext) { (void)InContext; }

        /* RENDER THREAD, called by RenderScene::RemoveProxy right before the
         * object is freed. */
        virtual void ReleaseRenderResources(RenderContext& InContext) { (void)InContext; }

        /* RENDER THREAD, once per frame, fed the mirrored component's
         * current POD bytes by WorldBridge's extraction pass -- see
         * RenderComponents.h and DESIGN.md's "Extraction" section. Every
         * proxy type gets transform and parameter updates for free by
         * implementing this alone; SetTransform is called separately, just
         * before this, from the same extraction record. */
        virtual void ApplyComponent(const void* InComponentBytes) = 0;

        /* RENDER THREAD, once per frame, only for a proxy RenderScene::Render
         * decided is visible (IsVisible() and WorldBounds overlaps the
         * view). */
        virtual void Draw(RenderContext& InContext) = 0;

        /* RENDER THREAD, once per shadow view a frame, for a visible proxy
         * whose WorldBounds that view can see -- including proxies the
         * camera culled, whose shadows may still reach the screen. Queue
         * casters with RenderContext::DrawMeshShadow. Nothing by default: a
         * proxy type casts no shadow unless it says so. */
        virtual void DrawShadow(RenderContext& InContext) { (void)InContext; }

        /* OCCLUSION CULLING (RENDERPIPELINE.md, "Occlusion culling"). An
         * occluder is never culled by occlusion itself: RENDER THREAD, once a
         * frame while it is in view, DrawOccluder queues its depth into the
         * PrePass (RenderContext::DrawMeshOccluder), which every other proxy's
         * box is tested against. Neither by default: a proxy type occludes
         * nothing unless it says so (RenderComponent::bOccluder). */
        virtual bool IsOccluder() const { return false; }

        /* OCCLUSION REVEAL: whether this proxy's draws are dithered away where
         * they hide a view's players (RenderComponent::bRevealPlayers); while
         * a view reveals any, such an occluder draws no PrePass depth. */
        virtual bool RevealsPlayers() const { return false; }
        virtual void DrawOccluder(RenderContext& InContext) { (void)InContext; }

        /* RENDER THREAD, before culling: how far past WorldBounds this
         * proxy's draw can reach -- a material's World Position Offset (Max
         * Offset, RenderContext::GetMaterialMaxOffset). 0 by default. */
        virtual float GetBoundsPadding(RenderContext& InContext) { (void)InContext; return 0.0f; }

        /* RENDER THREAD, once per frame for every proxy, visible or not,
         * before any view is drawn or target bound -- GPU work a frame needs
         * once, however many views then draw it: a particle system's
         * simulation (RenderContext::SimulateParticles). Nothing by default. */
        virtual void Simulate(RenderContext& InContext) { (void)InContext; }

        /* Whether this proxy's draws may be captured once and replayed while
         * its owner is static (SetStatic). False for one whose draws change
         * every frame on their own, as particles do. */
        virtual bool AllowsStaticDraws() const { return true; }

        /* Whether this proxy asks for an inset shadow -- a sun shadow map of
         * its own (RenderComponent::bCastInsetShadow). RenderScene offers
         * the visible ones to the frame's shadow views; one granted draws
         * into its own view alone, and no longer into the sun's. */
        virtual bool CastsInsetShadow() const { return false; }

        /* Recomputes LocalToWorld = InTransform.ToMatrix() and
         * WorldBounds = AABB::Transformed(LocalBounds, LocalToWorld) --
         * exactly World::RefreshSpatialIndex's own refit math (World.cpp),
         * restated here since a proxy has no World to ask. */
        void SetTransform(const RenderTransform& InTransform)
        {
            Transform    = InTransform;
            LocalToWorld = InTransform.ToMatrix();
            WorldBounds  = GameCore::AABB::Transformed(LocalBounds, LocalToWorld);
        }

        /* VELOCITY (RENDERPIPELINE.md): RenderScene calls this once a frame,
         * after drawing it: what it is now becomes what it was. A proxy with
         * more that moves (a skinned mesh's pose) keeps that too. */
        virtual void EndFrameMotion()
        {
            PrevLocalToWorld  = LocalToWorld;
            bHasPrevTransform = true;
        }

        /* How it moved since the frame before: OutMotion times LocalToWorld
         * is LocalToWorld then. False if it did not, or is new. */
        bool GetMotion(EngineCore::Matrix4x4& OutMotion) const
        {
            if(!bHasPrevTransform || std::memcmp(&PrevLocalToWorld, &LocalToWorld, sizeof(LocalToWorld)) == 0)
            {
                return false;
            }
            OutMotion = PrevLocalToWorld * EngineCore::Matrix4x4::Inverse(LocalToWorld);
            return true;
        }

        void SetLocalBounds(const GameCore::AABB& InBounds)
        {
            LocalBounds = InBounds;
            WorldBounds = GameCore::AABB::Transformed(LocalBounds, LocalToWorld);
        }

        void SetVisible(const bool bInVisible) { bVisible = bInVisible; }

        /* Its entity's baked lighting this frame (RenderLighting.h), set by
         * extraction; what a proxy that draws lit meshes passes on. */
        void SetBakedDraw(const RenderBakedDraw& InBaked) { Baked = InBaked; }
        const RenderBakedDraw& GetBakedDraw() const { return Baked; }

        /* The owner's mobility (GameCore's Mobility.h), set by extraction: a
         * static proxy's draws are captured once and replayed (RenderScene's
         * static draw lists) until its change serial moves. */
        void SetStatic(bool bInStatic)
        {
            bInStatic = bInStatic && AllowsStaticDraws();
            if(bStatic != bInStatic) { bStatic = bInStatic; ++ChangeSerial; }
        }
        bool IsStatic() const { return bStatic; }

        /* Its owner's scalability bucket is above the world's tier
         * (GameCore's DetailMode.h), set by extraction. Apart from
         * SetVisible, which a component's own bVisible rewrites every
         * extraction. A detail-hidden proxy is culled from every view, shadow
         * and occluder pass; it keeps its resources, so a tier switch back is
         * immediate. */
        void SetDetailHidden(bool bInHidden)
        {
            if(bDetailHidden != bInHidden) { bDetailHidden = bInHidden; ++ChangeSerial; }
        }
        bool IsDetailHidden() const { return bDetailHidden; }

        /* Which custom render pass mesh groups its draws join (CustomRenderPass.h,
         * DrawMeshGroup): RenderComponent::CustomPassGroups' bits, set by its
         * ApplyComponent; RenderScene passes them on while it draws. */
        void    SetCustomPassGroups(const uint8_t InGroups) { CustomPassGroups = InGroups; }
        uint8_t GetCustomPassGroups() const { return CustomPassGroups; }

        /* Bumped by every extraction record applied to it: whatever was
         * cached from its draws is stale once this moves. */
        void     MarkChanged() { ++ChangeSerial; }
        uint64_t GetChangeSerial() const { return ChangeSerial; }

        const RenderTransform&        GetTransform()    const { return Transform; }
        const EngineCore::Matrix4x4&  GetLocalToWorld() const { return LocalToWorld; }
        const GameCore::AABB&         GetWorldBounds()  const { return WorldBounds; }
        const GameCore::AABB&         GetLocalBounds()  const { return LocalBounds; }
        bool                          IsVisible()       const { return bVisible; }
        /* What the scene's culling reads: visible, and not bucketed out. */
        bool                          IsRenderable()    const { return bVisible && !bDetailHidden; }
        GameCore::EntityID            GetOwner()        const { return Owner; }

    private:
        friend class RenderScene;

        GameCore::EntityID     Owner;
        RenderTransform        Transform;
        EngineCore::Matrix4x4  LocalToWorld;
        EngineCore::Matrix4x4  PrevLocalToWorld;            // as the frame before was drawn (EndFrameMotion)
        bool                   bHasPrevTransform = false;
        GameCore::AABB         LocalBounds;
        GameCore::AABB         WorldBounds;

        /* RenderScene's own bookkeeping -- the dense-array slot this proxy
         * currently occupies, for O(1) swap-remove. -1 until AddProxy. */
        int32_t SceneIndex = -1;

        bool     bVisible     = true;
        bool     bStatic      = false;
        bool     bDetailHidden = false;
        uint8_t  CustomPassGroups = 0;
        uint64_t ChangeSerial = 0;
        RenderBakedDraw Baked;

        /* RenderScene's occlusion bookkeeping: per view (RENDER_VIEWS_MAX), its
         * query in the last round of each of that view's query sets (one per
         * frame in flight; -1: none), and whether the answer read for the view
         * being drawn found it hidden. */
        static constexpr uint32_t OCCLUSION_SETS_MAX = 4;
        int32_t  OcclusionQueries[RENDER_VIEWS_MAX][OCCLUSION_SETS_MAX] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 },
                                                                            { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
        bool     bOccluded      = false;
    };

    /*
     * The game thread's opaque handle to a proxy it no longer owns. Ptr is
     * ONLY meaningful on the render thread, or inside a render command's own
     * lambda (which runs there) -- Resolve() CORE_CHECKs IsInRenderThread()
     * for exactly that reason, the same guard GetRenderThreadContext() uses.
     *
     * Trivially copyable (one pointer), so it is safe to capture into a
     * render command by value -- see RenderCommand.h's capture rule.
     */
    struct RenderProxyHandle
    {
        RenderProxy* Ptr = nullptr;

        bool IsValid() const { return Ptr != nullptr; }

        template<typename T>
        T* Resolve() const
        {
            CORE_CHECK(IsInRenderThread(), "RenderProxyHandle::Resolve() called from outside the render thread")
            return static_cast<T*>(Ptr);
        }
    };

    /*
     * Second thing in a render component, right after
     * GAMECORE_COMPONENT_BODY() -- the handle WorldBridge writes at a fixed
     * offset without knowing the component's type, ComponentOwner's own
     * trick (GameCore::World::AddComponentRaw writes ComponentOwner at
     * offset 0 the same way).
     */
    #define RENDERCORE_PROXY_BODY() ::RenderCore::RenderProxyHandle Proxy;

    using CreateProxyFn = RenderProxy* (*)(GameCore::EntityID InEntity, const void* InComponentBytes);

    /*
     * GAMECORE_DEFINE_COMPONENT plus the proxy factory. TypeName must already
     * have GAMECORE_COMPONENT_BODY() then RENDERCORE_PROXY_BODY() as its
     * first two members, in that order -- the static_assert below is what
     * makes "Proxy sits at sizeof(ComponentOwner)" a build error rather than
     * a silent wrong-offset write if that order is ever violated.
     *
     * ProxyType's constructor must be (GameCore::EntityID, const TypeName&):
     * the factory below is what RegisterProxyType's caller hands to it, and
     * it is defined HERE, out-of-line, in the DEFINING image, so the
     * allocation it performs is tagged with THAT image's GImageID -- the
     * same reasoning GameCore::ComponentType.h gives for why StaticType()
     * must be out-of-line and defined exactly once.
     */
    #define RENDERCORE_DEFINE_PROXY_COMPONENT(TypeName, NameLiteral, ProxyType)                                    \
        GAMECORE_DEFINE_COMPONENT(TypeName, NameLiteral)                                                           \
        static_assert(offsetof(TypeName, Proxy) == sizeof(::GameCore::ComponentOwner),                             \
                      "RENDERCORE_PROXY_BODY() must be the second thing declared in the struct, "                  \
                      "right after GAMECORE_COMPONENT_BODY()");                                                    \
        static ::RenderCore::RenderProxy* TypeName##_CreateProxy(::GameCore::EntityID InEntity, const void* InBytes) \
        {                                                                                                           \
            return ::EngineCore::Memory::Alloc<ProxyType>(InEntity, *static_cast<const TypeName*>(InBytes));       \
        }

    /* GAME THREAD, before any InType component exists (before BindWorld).
     * False on a duplicate. */
    RENDERCORE_API bool RegisterProxyType(GameCore::ComponentTypeID InType, CreateProxyFn InFn,
                                          EngineCore::ImageID InOwningImage = EngineCore::GImageID);

    /*
     * A PROXY IS A PROMISE TO REMOVE -- Delegate.h's "a binding is a promise
     * to unbind" rule, restated: a proxy this factory allocated carries
     * InOwningImage's tag, and Memory::OnImageUnloading would destroy it out
     * from under the render thread the moment that image unloads. A module
     * that registers a proxy type unregisters it before it unloads.
     *
     * Clears the table slot only -- this registry has no reach into whatever
     * RenderScene a Renderer happens to own (there may be more than one
     * bound World, or none), so sweeping every live proxy of InType is
     * WorldBridge's job: a caller removes every component of InType from
     * every World it bound before calling this, which drives RemoveProxy
     * through WorldBridge's own OnComponentRemoved handler the ordinary way.
     */
    RENDERCORE_API void UnregisterProxyType(GameCore::ComponentTypeID InType);

    /* nullptr if InType was never registered (or was already unregistered).
     * GAME THREAD -- WorldBridge's OnComponentAdded handler calls the
     * factory this returns synchronously, from inside World::AddComponentRaw
     * (see GameCore::World.cpp), which runs on the game thread like every
     * other World call. */
    RENDERCORE_API CreateProxyFn FindProxyFactory(GameCore::ComponentTypeID InType);
}

#endif //RENDERCORE_RENDERPROXY_H
