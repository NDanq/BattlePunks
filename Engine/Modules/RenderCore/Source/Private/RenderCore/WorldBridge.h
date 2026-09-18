#ifndef RENDERCORE_WORLDBRIDGE_H
#define RENDERCORE_WORLDBRIDGE_H

#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"

/*
 * The game-thread half of the bridge between a GameCore::World and the
 * render thread's RenderScene: proxy lifecycle (bound to World's own
 * OnComponentAdded/OnComponentRemoved delegates) and the per-frame
 * extraction + view build Renderer::EndFrame drives. See DESIGN.md's
 * "Extraction", "Proxy lifecycle" and "View" sections for the full
 * reasoning; this header is the shape, not the rationale.
 *
 * GAME THREAD ONLY -- every method here runs on the thread that calls
 * World's own methods, the same contract World.h states for itself.
 */
namespace RenderCore
{
    class RenderScene;

    class WorldBridge
    {
    public:
        explicit WorldBridge(RenderScene& InScene);
        ~WorldBridge();

        WorldBridge(const WorldBridge&)            = delete;
        WorldBridge& operator=(const WorldBridge&) = delete;

        /* Binds OnComponentAdded/OnComponentRemoved (user data = this), then
         * sweeps every already-registered proxy type's pool for components
         * that predate the binding -- a component added before BindWorld()
         * ran gets its proxy created here, exactly as if OnComponentAdded
         * had fired for it. */
        void BindWorld(GameCore::World& InWorld);

        /* RemoveProxy for every live proxy this bridge created, then a
         * Flush() so those removals have actually happened before this
         * returns, then RemoveAll(this) on both delegates. Safe to call with
         * no World bound. */
        void UnbindWorld();

        bool IsBound() const;

        /* Builds this frame's RenderView (see DESIGN.md's "View"), enqueues
         * one extraction command per registered proxy type, then enqueues
         * [Scene](Ctx){ Scene->SetView(View); Scene->Render(Ctx); } against
         * the RenderScene this bridge was constructed with. */
        void ExtractAndRender(const FrameInfo& InFrame);

    private:
        static void OnComponentAdded(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);
        static void OnComponentRemoved(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

        RenderView BuildView(const FrameInfo& InFrame) const;

        RenderScene*    Scene = nullptr;
        GameCore::World* BoundWorld = nullptr;

        GameCore::DelegateHandle AddedHandle;
        GameCore::DelegateHandle RemovedHandle;
    };
}

#endif //RENDERCORE_WORLDBRIDGE_H
