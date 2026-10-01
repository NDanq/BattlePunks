#ifndef RENDERCORE_RENDERPROXY_H
#define RENDERCORE_RENDERPROXY_H

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

        void SetLocalBounds(const GameCore::AABB& InBounds)
        {
            LocalBounds = InBounds;
            WorldBounds = GameCore::AABB::Transformed(LocalBounds, LocalToWorld);
        }

        void SetVisible(const bool bInVisible) { bVisible = bInVisible; }

        const RenderTransform&        GetTransform()    const { return Transform; }
        const EngineCore::Matrix4x4&  GetLocalToWorld() const { return LocalToWorld; }
        const GameCore::AABB&         GetWorldBounds()  const { return WorldBounds; }
        bool                          IsVisible()       const { return bVisible; }
        GameCore::EntityID            GetOwner()        const { return Owner; }

    private:
        friend class RenderScene;

        GameCore::EntityID     Owner;
        RenderTransform        Transform;
        EngineCore::Matrix4x4  LocalToWorld;
        GameCore::AABB         LocalBounds;
        GameCore::AABB         WorldBounds;

        /* RenderScene's own bookkeeping -- the dense-array slot this proxy
         * currently occupies, for O(1) swap-remove. -1 until AddProxy. */
        int32_t SceneIndex = -1;

        bool bVisible = true;
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
