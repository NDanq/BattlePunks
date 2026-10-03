#ifndef RENDERCORE_WORLDBRIDGE_H
#define RENDERCORE_WORLDBRIDGE_H

#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderLighting.h"
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

        /* Builds the RenderView (DESIGN.md's "View"), enqueues one extraction
         * per proxy type, then the scene render. InViewOverride replaces the
         * camera; ViewportPixels always comes from InFrame. */
        void ExtractAndRender(const FrameInfo& InFrame, const RenderView* InViewOverride = nullptr);

    private:
        static void OnComponentAdded(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);
        static void OnComponentRemoved(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

        RenderView BuildView(const FrameInfo& InFrame) const;

        /* The first active DirectionalLightComponent and SkyLightComponent,
         * over RenderLighting's defaults; the point and spot lights nearest
         * InEye (casters first); the [Shadows] settings. Game thread, like
         * BuildView. */
        RenderLighting    BuildLighting(const EngineCore::Vector3& InEye) const;

        /* Every enabled Post Process volume InView's camera is in or near,
         * blended Priority low first, weighted by how far inside it is. */
        RenderPostProcess BuildPostProcess(const RenderView& InView) const;

        RenderScene*    Scene = nullptr;
        GameCore::World* BoundWorld = nullptr;

        GameCore::DelegateHandle AddedHandle;

        /* "Too many point and spot lights", once per bridge. */
        mutable bool bWarnedLocalLights = false;
        GameCore::DelegateHandle RemovedHandle;
    };
}

#endif //RENDERCORE_WORLDBRIDGE_H
