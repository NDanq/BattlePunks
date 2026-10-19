#ifndef RENDERCORE_WORLDBRIDGE_H
#define RENDERCORE_WORLDBRIDGE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/BakedLightingAsset.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"
#include "Private/RenderCore/ProceduralSystem.h"
#include "Private/RenderCore/SceneCapture.h"

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
    struct CameraComponent;

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

        /* The camera view InViewIndex (RENDER_VIEWS_MAX) looks through,
         * whatever its Active flag says, while InCamera is alive with a
         * CameraComponent; unset (the default) restores the rule: view 0's
         * camera for any other view, else the first active camera. Cleared
         * by UnbindWorld. */
        void SetViewCamera(GameCore::EntityID InCamera, uint32_t InViewIndex = 0)
        {
            if(InViewIndex < RENDER_VIEWS_MAX) { ViewCameras[InViewIndex] = InCamera; }
        }
        GameCore::EntityID GetViewCamera(const uint32_t InViewIndex = 0) const
        {
            return InViewIndex < RENDER_VIEWS_MAX ? ViewCameras[InViewIndex] : GameCore::EntityID();
        }

        /* Builds the RenderViews (DESIGN.md's "View"), enqueues one extraction
         * per proxy type, then the scene render. InViewOverride replaces view
         * 0's camera. SPLITSCREEN: InRects, InCount > 1 of them, are the
         * views' rectangles of InFrame's viewport -- each view sized to its
         * own, with its own light and post process; otherwise one view, the
         * whole of InFrame.ViewportPixels. */
        void ExtractAndRender(const FrameInfo& InFrame, const RenderView* InViewOverride = nullptr,
                              const RenderViewRect* InRects = nullptr, uint32_t InCount = 1);

    private:
        static void OnComponentAdded(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);
        static void OnComponentRemoved(GameCore::EntityID InEntity, GameCore::ComponentTypeID InType, void* InUserData);

        RenderView BuildView(const FrameInfo& InFrame, uint32_t InViewIndex = 0) const;
        RenderView ViewThrough(const CameraComponent& InCamera, const FrameInfo& InFrame,
                               const EngineCore::Vector3& InOffset = EngineCore::Vector3(0.0, 0.0, 0.0)) const;

        /* Each animated Render component's pose, to its proxy, as one batch
         * after the frame's extraction (ANIMATION.md); a proxy posed before
         * and not now is told so, and goes back to its bind pose. */
        void ExtractPoses(const FrameInfo& InFrame);

        /* The first active DirectionalLightComponent and SkyLightComponent,
         * over RenderLighting's defaults; the point and spot lights nearest
         * InEye (casters first); the [Shadows] settings. Game thread, like
         * BuildView. */
        RenderLighting    BuildLighting(const EngineCore::Vector3& InEye) const;

        /* The bound world's bake, as its WorldLightingSettings name it, loaded
         * (again when BakedLightingCache moves on); whether this frame draws
         * with it: baked lighting on in the world and [Rendering]
         * BakedLighting on. RENDERPIPELINE.md, "Baked lighting". */
        void UpdateBake();

        /* InEntity's baked lighting this frame: a static entity's lightmap
         * square if the bake has one for it, else the probes' blend at
         * InPosition; nothing while no bake is drawn with. */
        RenderBakedDraw BakedDrawFor(const GameCore::Entity& InEntity, const EngineCore::Vector3& InPosition) const;

        /* Every enabled Post Process volume InView's camera is in or near,
         * blended Priority low first, weighted by how far inside it is. */
        RenderPostProcess BuildPostProcess(const RenderView& InView) const;

        /* The SceneCaptureProbes due this frame (Frequency, CaptureNow --
         * cleared once taken), as capture requests for InView; a mirror out of
         * InView's sight is not due. Game thread. */
        SceneCaptureList BuildCaptures(const RenderView& InView, double InDeltaSeconds);

        /* The Reflection Probes lighting InView into OutList (Probes): up to
         * RENDER_REFLECTION_PROBES_MAX, nearest the camera first, each marked
         * due by its Frequency. Called by BuildCaptures. */
        void BuildReflectionProbes(const RenderView& InView, SceneCaptureList& OutList);

        /* BuildCaptures' clock and each probe's last capture, by entity. */
        struct ProbeState { double LastCapture = -1.0e30; bool bCaptured = false; };
        std::unordered_map<uint64_t, ProbeState> ProbeStates;
        double                                   CaptureClock = 0.0;

        RenderScene*    Scene = nullptr;
        GameCore::World* BoundWorld = nullptr;
        GameCore::EntityID ViewCameras[RENDER_VIEWS_MAX];   // SetViewCamera

        /* The bound world's animation (attached while bound), and the
         * proxies ExtractPoses posed last frame. */
        class AnimationSystem*               Animation = nullptr;

        /* The bound world's Procedural Spawners: what they read, and their
         * generations on worker threads (ProceduralSystem.h). */
        ProceduralSystem                     Procedural;
        std::unordered_set<const void*>      PosedProxies;

        /* A static component's last extraction record (header and bytes), by
         * its proxy: unchanged, it is not sent again. Cleared when a mesh is
         * reimported (MeshAssetCache's generation), so each proxy looks again. */
        std::unordered_map<const void*, std::vector<uint8_t>> StaticRecords;
        uint64_t                                             StaticMeshGeneration = 0;

        std::shared_ptr<const BakedLightingData> Bake;
        std::string                              BakePath;
        uint64_t                                 BakeGeneration = ~0ull;
        bool                                     bBakeActive    = false;
        std::unordered_map<GameCore::EntityGuid, const BakedLightmapEntry*, GameCore::EntityGuidHash> BakeEntries;

        GameCore::DelegateHandle AddedHandle;

        /* "Too many point and spot lights", once per bridge. */
        mutable bool bWarnedLocalLights = false;
        GameCore::DelegateHandle RemovedHandle;
    };
}

#endif //RENDERCORE_WORLDBRIDGE_H
