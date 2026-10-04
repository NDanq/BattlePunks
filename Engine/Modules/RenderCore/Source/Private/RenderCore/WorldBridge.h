#ifndef RENDERCORE_WORLDBRIDGE_H
#define RENDERCORE_WORLDBRIDGE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/BakedLightingAsset.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/RenderView.h"
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
