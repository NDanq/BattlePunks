#include "Public/RenderCore/RenderScene.h"

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderProxy.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/RenderCore/CaptureScheduler.h"
#include "Private/RenderCore/DrawPolicy.h"
#include "Private/RenderCore/MaterialPreview.h"
#include "Private/RenderCore/SceneCapture.h"
#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/RenderThreadInternal.h"
#include "Private/RenderCore/SceneTargets.h"
#include "Private/RenderCore/UIRenderer.h"

/*
 * A plain dense vector of proxies, swap-removed by the SceneIndex each proxy
 * carries -- the same sparse-set-adjacent shape GameCore::ComponentPool uses
 * for the same reason: O(1) remove, contiguous iteration for Render(). No
 * sparse lookup table here (unlike ComponentPool), because nothing ever asks
 * "does entity E have a proxy" through this class -- WorldBridge already
 * knows through the component's own RenderProxyHandle.
 */

/* [Rendering] OcclusionCulling: a perspective view's proxies hidden behind its
 * occluders (RenderComponent::bOccluder) are skipped (RENDERPIPELINE.md,
 * "Occlusion culling"), where the backend has occlusion queries. */
NIMBLE_CONFIG_BOOL(GRenderOcclusionCulling, "Rendering", "OcclusionCulling", true, nullptr);
/* A reflection probe's capture spread over frames (GLOBAL_ILLUMINATION.md, Phase 4): cube faces
 * rendered a frame, 1-6; 6 captures a probe whole in one frame, as before. */
NIMBLE_CONFIG_INT(GRenderProbeFacesPerFrame, "Rendering", "ProbeFacesPerFrame", 1, nullptr);
/* A GI Volume's probes (GLOBAL_ILLUMINATION.md, Phase 6): whether they light, and how many update a frame
 * (each six 16 x 16 views of the scene); 0 holds them as they are. */
NIMBLE_CONFIG_BOOL(GRenderDynamicGI, "Rendering", "DynamicGI", true, nullptr);
NIMBLE_CONFIG_INT(GRenderGIProbesPerFrame, "Rendering", "GIProbesPerFrame", 1, nullptr);
/* A sky from custom passes at ERenderHook::Sky (RENDERPIPELINE.md, "Image-based lighting"): at most one
 * capture of it every SkyCaptureFrames frames, however often its version changes; and whether a new one
 * has every reflection probe captured again (one at a time, ProbeFacesPerFrame), so they see it too. */
NIMBLE_CONFIG_INT(GRenderSkyCaptureFrames, "Rendering", "SkyCaptureFrames", 8, nullptr);
NIMBLE_CONFIG_BOOL(GRenderProbesFollowSky, "Rendering", "ProbesFollowSky", true, nullptr);
/* Off: the Sky Light's sky lights, whatever Sky passes there are (an A/B switch). */
NIMBLE_CONFIG_BOOL(GRenderSkyPassLighting, "Rendering", "SkyPassLighting", true, nullptr);
/* THE CAPTURE BUDGET (GLOBAL_ILLUMINATION.md, "Capture budget"): the GPU milliseconds a frame's
 * reflection probe and GI Volume faces may take, shared, face by face (0: ProbeFacesPerFrame and
 * GIProbesPerFrame, as before); one set of shadow maps for all of a frame's capture views; capture
 * views drawn simply (no custom passes or translucency, nothing under CaptureCullPixels pixels, a GI
 * Volume's static geometry alone out to twice its diagonal); and captures that stop once converged,
 * starting again when what they see changes -- the lighting (the sun by CaptureSunStepDegrees) or the
 * scene -- a GI Volume after GIConvergeRounds rounds. Scalability tiers set the budget. */
NIMBLE_CONFIG_FLOAT(GRenderCaptureBudgetMs, "Rendering", "CaptureBudgetMs", 2.0f, nullptr);
NIMBLE_CONFIG_BOOL(GRenderSharedCaptureShadows, "Rendering", "SharedCaptureShadows", true, nullptr);
NIMBLE_CONFIG_BOOL(GRenderSimpleCaptures, "Rendering", "SimpleCaptures", true, nullptr);
NIMBLE_CONFIG_FLOAT(GRenderCaptureCullPixels, "Rendering", "CaptureCullPixels", 2.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GRenderCaptureLodScale, "Rendering", "CaptureLodScale", 4.0f, nullptr);
NIMBLE_CONFIG_BOOL(GRenderGICaptureStaticOnly, "Rendering", "GICaptureStaticOnly", true, nullptr);
NIMBLE_CONFIG_FLOAT(GRenderGICaptureSpacings, "Rendering", "GICaptureSpacings", 8.0f, nullptr);
NIMBLE_CONFIG_BOOL(GRenderStaticCaptures, "Rendering", "StaticCaptures", true, nullptr);
NIMBLE_CONFIG_FLOAT(GRenderCaptureSunStep, "Rendering", "CaptureSunStepDegrees", 1.0f, nullptr);
NIMBLE_CONFIG_INT(GRenderGIConvergeRounds, "Rendering", "GIConvergeRounds", 2, nullptr);
/* RELIGHTING (GLOBAL_ILLUMINATION.md, "Relighting"): a GI Volume keeps what its probes' faces saw, and
 * once every probe's is kept and its static geometry has not changed since, a lighting change lights the
 * probes again from it (DDGIRelight.fsh) -- many a frame -- instead of drawing the scene again. */
NIMBLE_CONFIG_BOOL(GRenderGIRelight, "Rendering", "GIRelight", true, nullptr);
/* Volumes of more probes than this are not cached (32 bytes a texel: about 50 MB for 1024 probes). */
NIMBLE_CONFIG_INT(GRenderGIRelightMaxProbes, "Rendering", "GIRelightMaxProbes", 2048, nullptr);
/* Log, about once a second, what the captures did: faces, the budget's unit costs, a GI Volume's progress. */
NIMBLE_CONFIG_BOOL(GRenderCaptureLog, "Rendering", "CaptureLog", false, nullptr);

namespace RenderCore
{
    namespace
    {
        /* Every live scene, so a GDI block's GPU times reach the scene that
         * wrote them a block later ("stat frame"). Render thread only. */
        std::vector<RenderScene*> GLiveScenes;

        /* The fewest queries a scene's set holds; it doubles past that as a
         * frame asks for more. */
        constexpr uint32_t OCCLUSION_QUERIES_MIN = 256;
        constexpr uint32_t OCCLUSION_QUERIES_MAX = 65536;

        /* RenderProxy::OCCLUSION_SETS_MAX, which only the scene may see (Impl asserts they match). */
        constexpr uint32_t OCCLUSION_SETS_MAX = 4;

        bool IsPerspective(const RenderView& InView)
        {
            return InView.Projection.Data[3][2] != 0.0f && InView.ZNear > 0.0f;
        }

        /* How far from the eye the near plane's corners are: a box nearer than
         * that may be cut by the near plane, which can leave a query nothing
         * of it to draw where it covers what is behind. */
        double NearPlaneReach(const RenderView& InView)
        {
            const double FocalX = std::max(std::abs(double(InView.Projection.Data[0][0])), 1e-6);
            const double FocalY = std::max(std::abs(double(InView.Projection.Data[1][1])), 1e-6);
            return double(InView.ZNear) * std::sqrt(1.0 + 1.0 / (FocalX * FocalX) + 1.0 / (FocalY * FocalY));
        }

        /* The distance from InPoint to the nearest point of InBox (0 inside it). */
        double DistanceToBox(const GameCore::AABB& InBox, const EngineCore::Vector3& InPoint)
        {
            const double Dx = std::max({ double(InBox.Min.GetX()) - double(InPoint.GetX()), 0.0, double(InPoint.GetX()) - double(InBox.Max.GetX()) });
            const double Dy = std::max({ double(InBox.Min.GetY()) - double(InPoint.GetY()), 0.0, double(InPoint.GetY()) - double(InBox.Max.GetY()) });
            const double Dz = std::max({ double(InBox.Min.GetZ()) - double(InPoint.GetZ()), 0.0, double(InPoint.GetZ()) - double(InBox.Max.GetZ()) });
            return std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
        }

        /*
         * OCCLUSION CULLING (RENDERPIPELINE.md): a view's query sets, one per
         * frame the GPU may run behind (GDIGetFramesInFlight), taken in turn:
         * a set's answers are read when it comes round again, its round
         * finished by then without waiting for it. Each is made on first use
         * and remade larger when a frame wanted more queries than it holds.
         * Then the answers read, and what the last frame did, for RenderStats.
         */
        struct ViewOcclusion
        {
            GDIOcclusionQueries* Sets[OCCLUSION_SETS_MAX] = {};
            uint32_t             SetCount = 1;
            uint32_t             At       = 0;         // this frame's set
            GDIOcclusionQueries* Current  = nullptr;   // Sets[At], or null: this frame queries nothing (Prepare)
            uint32_t             Wanted   = 0;
            std::vector<uint8_t> Answers;
            uint64_t             LastOccluded = 0;
            uint64_t             LastQueries  = 0;

            /* The set the view's queries go in this frame (Current); null if
             * occlusion culling is off, the backend has no queries, or InView
             * is not a perspective one (an orthographic view has no near plane
             * to judge nearness by, and nothing it draws is usually hidden). */
            GDIOcclusionQueries* Prepare(const RenderView& InView)
            {
                if(!GRenderOcclusionCulling.GetBool() || !GDISupportsOcclusionQueries() || !IsPerspective(InView))
                {
                    Release();
                    return Current = nullptr;
                }
                const uint32_t Count = std::clamp<uint32_t>(GDIGetFramesInFlight(), 1, OCCLUSION_SETS_MAX);
                if(Count != SetCount)
                {
                    Release();
                    SetCount = Count;
                }
                At = (At + 1) % SetCount;
                GDIOcclusionQueries*& Set = Sets[At];
                if(!Set || Wanted > Set->GetCapacity())
                {
                    uint32_t Capacity = Set ? Set->GetCapacity() : OCCLUSION_QUERIES_MIN;
                    while(Capacity < Wanted && Capacity < OCCLUSION_QUERIES_MAX) { Capacity *= 2; }
                    if(Set) { Set->Release(); }   // its answers with it: a frame without CPU culling
                    Set = GDICreateOcclusionQueries(Capacity);
                }
                return Current = Set;
            }

            void Release()
            {
                for(GDIOcclusionQueries*& Set : Sets)
                {
                    if(Set) { Set->Release(); }
                    Set = nullptr;
                }
                At = 0;
                Current = nullptr;
                LastOccluded = LastQueries = 0;
            }
        };

        /*
         * ONE VIEW OF THE SCENE (SPLITSCREEN: one per player view): what it
         * looks through, and everything that carries from one of its frames
         * to the next -- the targets (TAA history, exposure, SSR's last copy,
         * velocity), last frame's view-projection for motion blur and the
         * reprojection, TAA's jitter cycle, and its occlusion queries. What
         * happens once a frame whatever the views (Simulate, captures, image
         * based lighting, EndFrameMotion, the stats) stays on the scene.
         */
        struct SceneViewState
        {
            SceneViewState(const char* InName) : Targets(InName) {}

            RenderView        View;
            RenderLighting    Lighting;      // nearest lights to its eye, its shadows (WorldBridge::BuildLighting)
            RenderPostProcess PostProcess;   // the volumes its camera is in (WorldBridge::BuildPostProcess)

            /* Last frame's Projection * View and viewport, for motion blur;
             * none on the first frame or after a resize (a jump, not motion). */
            EngineCore::Matrix4x4 PrevViewProjection;
            EngineCore::Size      PrevViewport;
            bool                  bHasPrevView = false;

            /* TAA's jitter: where in its 8-frame Halton(2, 3) cycle the view is. */
            uint32_t JitterIndex = 0;

            ViewOcclusion Occlusion;

            /*
             * THE VIEW'S RENDER TARGETS, and the whole reason anything reaches
             * the screen. GDIDynamicDriver::PresentToWindow presents
             * CurrentStateData->ActiveRenderTarget -- the most recently bound RT --
             * which RenderContext::RenderSceneStages leaves as the scene's final
             * colour (after PostProcess, if any). With no RT bound at all,
             * ActiveRenderTarget stays null and every present silently returns.
             *
             * Deliberately NOT GDIRenderTargetManager, whose Main/Buffer1/
             * PostBuffer1 and Resolve/Restore pair are paint-application concepts
             * (canvas stroke buffering). See SceneTargets.h.
             *
             * Resized every frame the viewport changes, and cheap otherwise:
             * recreating a GPU image per frame would destroy the target the UI
             * thread is presenting from while it may still be sampling it. A
             * zero-sized viewport (a minimised window) keeps what there was.
             */
            SceneTargets Targets;

            /* Whether it was drawn last frame: one appearing in the dynamic split starts from view 0's history. */
            bool bDrawnLastFrame = false;

            /* What drawing the view counted, for RenderStats. */
            uint64_t VisibleCount = 0;
            uint64_t CulledCount  = 0;
            uint64_t StaticCount  = 0;
            float    QueueMs      = 0.0f;

            void Release()
            {
                Targets.Release();
                Occlusion.Release();
            }
        };
    }

    struct RenderScene::Impl
    {
        static_assert(OCCLUSION_SETS_MAX == RenderProxy::OCCLUSION_SETS_MAX, "a view's query sets index RenderProxy::OcclusionQueries");

        std::vector<RenderProxy*> Proxies;

        /* SetUIDrawList's, drawn over the final colour each Render(). */
        UISceneState UI;

        /* The view mode, and the targets a frame could show: names under
         * TargetsLock, for the game thread. */
        ERenderViewMode           ViewMode = ERenderViewMode::Lit;
        std::string               ViewTarget;
        mutable std::mutex        TargetsLock;
        std::vector<std::string>  TargetNames;

        /*
         * STATIC DRAW LISTS (RENDERPIPELINE.md): a static proxy's draws, shadow
         * casters and cull padding, captured the first frame each is needed
         * and replayed after -- until the proxy changes (its change serial) or
         * something they point at goes (the context's content generation).
         */
        struct StaticDrawList
        {
            uint64_t                Serial     = ~0ull;
            uint64_t                Generation = ~0ull;
            bool                    bDraws     = false;
            bool                    bShadows   = false;
            bool                    bPadding   = false;
            float                   Padding    = 0.0f;
            std::vector<StagedDraw> Draws;
            std::vector<StagedDraw> Shadows;
        };
        std::unordered_map<const RenderProxy*, StaticDrawList> StaticLists;
        uint64_t                                               StaticBuilds = 0;   // lifetime captures

        /* The views (SceneViewState): ViewCount of them this frame, [0] the
         * main one -- what captures, image based lighting and the editor
         * see from. A view past ViewCount keeps its targets and history for
         * when it comes back. */
        SceneViewState Views[RENDER_VIEWS_MAX] = { "RenderCore.Scene", "RenderCore.Scene.View1",
                                                   "RenderCore.Scene.View2", "RenderCore.Scene.View3" };
        uint32_t       ViewCount = 1;

        /*
         * SPLITSCREEN: with more than one view, each view's final colour placed
         * at its rectangle in this window-sized target (RenderContext::
         * CompositeViews), the UI and the overlays over it, and it presented.
         * Made on first use, resized with the window, never replaced while the
         * scene lives (PresentTarget's rule).
         */
        EngineCore::Size WindowPixels;
        GDIRenderTarget* Composite = nullptr;
        RenderViewSites  Sites;   // SetViews'; Count 2 or more: the dynamic split's composite

        void ReleaseViews()
        {
            for(SceneViewState& Each : Views) { Each.Release(); }
            if(Composite)
            {
                GDIReleaseRenderTarget(Composite);
                delete Composite;
                Composite = nullptr;
            }
        }

        /* InProxy's bounding sphere, world space, from its local box: the
         * same size however it is turned (its world box grows as it turns). */
        static InsetShadowCaster InsetSphere(const RenderProxy& InProxy)
        {
            const GameCore::AABB& Local = InProxy.GetLocalBounds();
            const EngineCore::Vector3 Centre = (Local.Min + Local.Max) * 0.5;
            const EngineCore::Vector3 Scale  = InProxy.GetTransform().Scale;
            const double Largest = std::max({ std::abs(double(Scale.GetX())), std::abs(double(Scale.GetY())), std::abs(double(Scale.GetZ())) });
            const EngineCore::Vector4 World = InProxy.GetLocalToWorld() * EngineCore::Vector4(Centre.GetX(), Centre.GetY(), Centre.GetZ(), 1.0);

            InsetShadowCaster Out;
            Out.Centre = EngineCore::Vector3(World.GetX(), World.GetY(), World.GetZ());
            Out.Radius = float(((Local.Max - Local.Min) * 0.5).GetLength() * Largest);
            return Out;
        }

        StaticDrawList& ListFor(const RenderProxy* InProxy, const uint64_t InGeneration)
        {
            StaticDrawList& List = StaticLists[InProxy];
            if(List.Serial != InProxy->GetChangeSerial() || List.Generation != InGeneration)
            {
                List = StaticDrawList();
                List.Serial     = InProxy->GetChangeSerial();
                List.Generation = InGeneration;
            }
            return List;
        }

        /*
         * PrePass for one view: every visible proxy's draws queued in their
         * stages (a static one's from its kept list), then each shadow view's
         * casters. The main view, or a scene capture's (InCapture: its owner's
         * subtree left out). Counts what it drew and culled.
         *
         * With InOcclusion (a player view's, Prepared; RENDERPIPELINE.md, "Occlusion
         * culling"): each visible proxy but an occluder has its box queried
         * against the occluders' depth -- unless it is so near the eye the
         * near plane may cut it -- and its draws predicated on the answer,
         * so the GPU skips them if it is hidden this frame. One the last
         * answer read found hidden is not drawn at all; its query alone
         * says when it shows again, a frame late. Occluders queue their
         * depth for the PrePass.
         */
        void QueueDraws(RenderContext& InContext, const RenderView& InView, const RenderLighting& InLighting,
                        const SceneCaptureRequest* InCapture, uint64_t& OutVisible, uint64_t& OutCulled, uint64_t& OutStatic,
                        ViewOcclusion* InOcclusion = nullptr)
        {
            InContext.BeginSceneDraws();

            /* This set's last round's answers -- complete, as its block has
             * finished (FramesInFlight blocks back) -- then this round's queries. Without answers nothing is hidden on
             * the CPU; the GPU still skips what this frame's find hidden. */
            GDIOcclusionQueries* const Set = InOcclusion ? InOcclusion->Current : nullptr;
            const bool bOcclusion = Set != nullptr;
            const bool bAnswers   = bOcclusion && GDIReadOcclusionResults(Set, InOcclusion->Answers);
            const bool bPredicate = bOcclusion && InContext.CanPredicateOcclusion();
            const double NearReach = bOcclusion ? NearPlaneReach(InView) * 1.5 : 0.0;
            if(bOcclusion) { InContext.SetOcclusionQueries(Set); }
            /* Nothing to test against without an occluder in view: no queries. */
            const bool bQueries = bOcclusion && std::any_of(Proxies.begin(), Proxies.end(), [&InView](const RenderProxy* InProxy)
            {
                return InProxy->IsRenderable() && InProxy->IsOccluder()
                    && (InView.Bounds.IsEmpty() || InProxy->GetWorldBounds().IsEmpty() || InProxy->GetWorldBounds().Overlaps(InView.Bounds));
            });
            uint32_t OcclusionWantedNow = 0;
            uint64_t OccludedCount      = 0;

            /* TEXTURE STREAMING: a world unit at distance d spans K / d pixels
             * (K: half the viewport's height times the projection's y scale);
             * an orthographic view's span K everywhere. */
            const double StreamK = double(std::max(InView.ViewportPixels.Height, 0)) * 0.5 * std::abs(double(InView.Projection.Data[1][1]));
            const bool   bStreamPerspective = IsPerspective(InView);
            const double StreamNear = std::max(double(InView.ZNear), 0.01);

            uint64_t VisibleCount = 0;
            uint64_t CulledCount  = 0;
            const auto Excluded = [InCapture](const RenderProxy* InProxy)
            {
                for(uint32_t Index = 0; InCapture && Index < InCapture->ExcludedCount; ++Index)
                {
                    if(InCapture->Excluded[Index] == InProxy->GetOwner()) { return true; }
                }
                return false;
            };

            /* A static proxy's padding comes from its list; a dynamic one's is asked. */
            const uint64_t Generation = InContext.GetContentGeneration();
            const auto PaddingOf = [&](RenderProxy* InProxy)
            {
                if(!InProxy->IsStatic())
                {
                    return InProxy->GetBoundsPadding(InContext);
                }
                StaticDrawList& List = ListFor(InProxy, Generation);
                if(!List.bPadding)
                {
                    List.Padding  = InProxy->GetBoundsPadding(InContext);
                    List.bPadding = true;
                }
                return List.Padding;
            };
            uint64_t StaticCount = 0;

            /* Proxies asking for an inset shadow -- visible, or culled but with
             * a shadow that can reach the view -- and how much of the view
             * each fills (radius over distance): the largest get one. */
            struct InsetCandidate
            {
                RenderProxy*      Proxy;
                InsetShadowCaster Caster;
                double            Coverage;
            };
            std::vector<InsetCandidate> InsetCandidates;
            const auto OfferInset = [&](RenderProxy* InProxy)
            {
                InsetCandidates.push_back({ InProxy, InsetSphere(*InProxy), 0.0 });
                InsetCandidate& Candidate = InsetCandidates.back();
                const double Distance = (Candidate.Caster.Centre - InView.Origin).GetLength();
                Candidate.Coverage = double(Candidate.Caster.Radius) / std::max(Distance, 1.0);
            };

            /* A culled caster's shadow reaches the view if its sphere's box,
             * swept along the inset light across the view's whole extent,
             * overlaps it. Conservative: a box, not the prism. */
            const double LightLength = InLighting.InsetLightDirection.GetLength();
            const bool   bInsetLight = InLighting.bInsetLight && LightLength > 1e-9 && !InView.Bounds.IsEmpty();
            const EngineCore::Vector3 InsetSweep = bInsetLight
                ? InLighting.InsetLightDirection * ((InView.Bounds.Max - InView.Bounds.Min).GetLength() / LightLength)
                : EngineCore::Vector3(0.0, 0.0, 0.0);
            const auto ShadowReachesView = [&](const RenderProxy& InProxy)
            {
                const InsetShadowCaster Sphere = InsetSphere(InProxy);
                const EngineCore::Vector3 Half(Sphere.Radius, Sphere.Radius, Sphere.Radius);
                const GameCore::AABB Near(Sphere.Centre - Half, Sphere.Centre + Half);
                const GameCore::AABB Far(Near.Min + InsetSweep, Near.Max + InsetSweep);
                return GameCore::AABB::Union(Near, Far).Overlaps(InView.Bounds);
            };

            /* What the view draws, so where an inset's shadow can land: unknown
             * (unbounded) if the view or any drawn proxy has no bounds. */
            GameCore::AABB Receivers;
            bool           bReceiversKnown = !InView.Bounds.IsEmpty();

            for(RenderProxy* Proxy : Proxies)
            {
                /* Its query in this set's last round, read before anything can
                 * skip it: an index is only good for the set and round it was
                 * given in. */
                int32_t  LastQuery = -1;
                int32_t* Query     = bOcclusion ? &Proxy->OcclusionQueries[std::min<uint32_t>(InView.ViewIndex, RENDER_VIEWS_MAX - 1)][InOcclusion->At] : nullptr;
                if(bOcclusion)
                {
                    LastQuery        = *Query;
                    *Query           = -1;
                    Proxy->bOccluded = false;
                }
                if(!Proxy->IsRenderable() || Excluded(Proxy))
                {
                    continue;
                }
                /* A capture's own culls (SimpleCaptures): static geometry alone, nothing too small to see. */
                if(InCapture && ((InCapture->bStaticOnly && !Proxy->IsStatic())
                                 || (InCapture->CullRatio > 0.0f && SizeFrom(*Proxy, InCapture->Position) >= 0.0
                                     && SizeFrom(*Proxy, InCapture->Position) < double(InCapture->CullRatio))))
                {
                    ++CulledCount;
                    continue;
                }
                if(!Proxy->IsStatic() && !StaticLists.empty())
                {
                    StaticLists.erase(Proxy);   // it was static: its list is stale for good
                }

                /*
                 * EMPTY MEANS "NO BASIS TO CULL", on EITHER side -- not "never
                 * overlaps", which is what GameCore::AABB::Overlaps gives (it
                 * returns false the instant either box IsEmpty()) and is exactly
                 * backwards here. A caller with no real view still wants
                 * everything drawn, and a proxy that never called SetLocalBounds
                 * has given RenderScene nothing to cull it BY: undeclared, not
                 * "everywhere and nowhere". Undeclared must default to visible or
                 * a proxy type that never opts into bounds could never be drawn --
                 * this file's smoke test has one, deliberately, to prove the path.
                 * Explicitly overrides AABB::Overlaps' own empty-handling.
                 */
                const bool bViewHasBounds  = !InView.Bounds.IsEmpty();
                const bool bProxyHasBounds = !Proxy->GetWorldBounds().IsEmpty();

                /* World Position Offset can move geometry past its bounds: pad
                 * them by the largest Max Offset among its materials. */
                GameCore::AABB Bounds = Proxy->GetWorldBounds();
                const float Padding = bViewHasBounds && bProxyHasBounds ? PaddingOf(Proxy) : 0.0f;
                if(Padding > 0.0f)
                {
                    const EngineCore::Vector3 Pad(Padding, Padding, Padding);
                    Bounds = GameCore::AABB(Bounds.Min - Pad, Bounds.Max + Pad);
                }
                const bool bVisible = !bViewHasBounds || !bProxyHasBounds || Bounds.Overlaps(InView.Bounds);

                if(!bVisible)
                {
                    ++CulledCount;
                    if(bInsetLight && Proxy->CastsInsetShadow() && bProxyHasBounds && ShadowReachesView(*Proxy))
                    {
                        OfferInset(Proxy);
                    }
                    continue;
                }

                /* Occlusion: this round's query on its box, a little larger so
                 * coplanar faces pass; an occluder draws its depth instead. */
                if(bQueries && !Proxy->IsOccluder() && bProxyHasBounds && DistanceToBox(Bounds, InView.Origin) > NearReach)
                {
                    const double Pad = (Bounds.Max - Bounds.Min).GetLength() * 0.01 + 1e-3;
                    const EngineCore::Vector3 Grow(Pad, Pad, Pad);
                    ++OcclusionWantedNow;
                    *Query           = InContext.AddOcclusionQuery(GameCore::AABB(Bounds.Min - Grow, Bounds.Max + Grow));
                    Proxy->bOccluded = *Query >= 0 && bAnswers && LastQuery >= 0
                                         && size_t(LastQuery) < InOcclusion->Answers.size() && InOcclusion->Answers[size_t(LastQuery)] == 0;
                }
                /* Only the main view's answers: a capture sees from elsewhere. */
                if(bOcclusion && Proxy->bOccluded)
                {
                    ++OccludedCount;
                }
                else
                {
                    InContext.SetDrawOcclusion(bPredicate && Query ? *Query : -1);
                    /* Its nearest point's distance decides its textures' levels; no bounds: all of them. */
                    const double StreamScale = !bProxyHasBounds || StreamK <= 0.0 ? 0.0
                                             : (bStreamPerspective ? StreamK / std::max(DistanceToBox(Bounds, InView.Origin), StreamNear) : StreamK);
                    InContext.SetDrawStreamingScale(float(StreamScale));
                    InContext.SetDrawCustomGroups(Proxy->GetCustomPassGroups());   // a custom pass's mesh groups (CustomRenderPass.h)
                    InContext.SetDrawReveal(Proxy->RevealsPlayers());              // dithered where it hides a player
                    if(Proxy->IsStatic())
                    {
                        StaticDrawList& List = ListFor(Proxy, Generation);
                        if(!List.bDraws)
                        {
                            InContext.BeginCapture(&List.Draws);
                            Proxy->Draw(InContext);
                            InContext.EndCapture();
                            List.bDraws = true;
                            ++StaticBuilds;
                        }
                        InContext.SubmitCaptured(List.Draws);
                        ++StaticCount;
                    }
                    else
                    {
                        /* Its velocity: how it moved since the frame before (a static list's never does). */
                        EngineCore::Matrix4x4 Motion;
                        const bool bMoved = Proxy->GetMotion(Motion);
                        InContext.SetDrawMotion(bMoved ? &Motion : nullptr);
                        Proxy->Draw(InContext);
                        InContext.SetDrawMotion(nullptr);
                    }
                    InContext.SetDrawCustomGroups(0);
                    InContext.SetDrawReveal(false);
                    InContext.SetDrawOcclusion(-1);
                    InContext.SetDrawStreamingScale(0.0f);
                    ++VisibleCount;
                }
                /* An occluder that reveals players draws no depth while this view reveals any:
                 * it would fill in the hole its Base Pass dithers. */
                if(bQueries && Proxy->IsOccluder() && !(InView.RevealCount > 0 && Proxy->RevealsPlayers()))
                {
                    InContext.SetGatheringOccluders(true);
                    Proxy->DrawOccluder(InContext);
                    InContext.SetGatheringOccluders(false);
                }

                if(Proxy->CastsInsetShadow() && !Proxy->GetLocalBounds().IsEmpty())
                {
                    OfferInset(Proxy);
                }
                if(bProxyHasBounds) { Receivers = GameCore::AABB::Union(Receivers, Bounds); }
                else                { bReceiversKnown = false; }
            }
            std::stable_sort(InsetCandidates.begin(), InsetCandidates.end(),
                             [](const InsetCandidate& InA, const InsetCandidate& InB) { return InA.Coverage > InB.Coverage; });
            InsetCandidates.resize(std::min<size_t>(InsetCandidates.size(), RENDER_INSET_SHADOW_MAX));
            InsetShadowCaster InsetCasters[RENDER_INSET_SHADOW_MAX];
            for(size_t Index = 0; Index < InsetCandidates.size(); ++Index) { InsetCasters[Index] = InsetCandidates[Index].Caster; }

            /*
             * Shadow casters: per shadow view, every visible proxy it can see,
             * camera-culled or not -- a caster off screen can shade what is on
             * it. Padded like the camera's cull, for World Position Offset.
             * A capture without CaptureShadows prepares none: SetView left the
             * scene's shadows off, so it lights unshadowed.
             */
            const bool     bShadows    = !InCapture || InCapture->bShadows;
            const uint32_t ShadowViews = bShadows ? InContext.PrepareShadows(InsetCasters, uint32_t(InsetCandidates.size()),
                                                                             bReceiversKnown ? &Receivers : nullptr) : 0;

            /* The casters that got an inset view: theirs replaces the sun's. */
            std::vector<const RenderProxy*> Inset;
            for(uint32_t ViewIndex = 0; ViewIndex < ShadowViews; ++ViewIndex)
            {
                const int32_t Caster = InContext.GetShadowViewInset(ViewIndex);
                if(Caster >= 0 && size_t(Caster) < InsetCandidates.size()) { Inset.push_back(InsetCandidates[size_t(Caster)].Proxy); }
            }
            const auto HasInset = [&Inset](const RenderProxy* InProxy)
            {
                return std::find(Inset.begin(), Inset.end(), InProxy) != Inset.end();
            };

            /* Queues InProxy's casters for the current shadow view. */
            const auto DrawCaster = [&](RenderProxy* InProxy, const uint32_t InViewIndex)
            {
                if(InProxy->IsStatic())
                {
                    StaticDrawList& List = ListFor(InProxy, Generation);
                    if(!List.bShadows)
                    {
                        InContext.BeginCapture(&List.Shadows);
                        InProxy->DrawShadow(InContext);
                        InContext.EndCapture();
                        List.bShadows = true;
                        ++StaticBuilds;
                    }
                    InContext.SubmitCaptured(List.Shadows, int32_t(InViewIndex));
                }
                else
                {
                    InProxy->DrawShadow(InContext);
                }
            };

            for(uint32_t ViewIndex = 0; ViewIndex < ShadowViews; ++ViewIndex)
            {
                const GameCore::AABB& ViewBounds = InContext.GetShadowViewBounds(ViewIndex);
                InContext.SetShadowCasterView(int32_t(ViewIndex));

                /* An inset view draws its own caster and nothing else. */
                const int32_t InsetCaster = InContext.GetShadowViewInset(ViewIndex);
                if(InsetCaster >= 0)
                {
                    if(size_t(InsetCaster) < InsetCandidates.size())
                    {
                        DrawCaster(InsetCandidates[size_t(InsetCaster)].Proxy, ViewIndex);
                    }
                    continue;
                }
                const bool bSunView = InContext.IsSunShadowView(ViewIndex);

                for(RenderProxy* Proxy : Proxies)
                {
                    if(!Proxy->IsRenderable() || Excluded(Proxy) || (bSunView && !Inset.empty() && HasInset(Proxy)))
                    {
                        continue;
                    }
                    GameCore::AABB Bounds = Proxy->GetWorldBounds();
                    if(!Bounds.IsEmpty() && !ViewBounds.IsEmpty())
                    {
                        const float Padding = PaddingOf(Proxy);
                        const EngineCore::Vector3 Pad(Padding, Padding, Padding);
                        Bounds = GameCore::AABB(Bounds.Min - Pad, Bounds.Max + Pad);
                        if(!Bounds.Overlaps(ViewBounds))
                        {
                            continue;
                        }
                    }
                    DrawCaster(Proxy, ViewIndex);
                }
            }
            InContext.SetShadowCasterView(-1);
            OutVisible = VisibleCount;
            OutCulled  = CulledCount;
            OutStatic  = StaticCount;
            if(bOcclusion)
            {
                InOcclusion->Wanted       = OcclusionWantedNow;
                InOcclusion->LastOccluded = OccludedCount;
                InOcclusion->LastQueries  = std::min<uint64_t>(OcclusionWantedNow, Set->GetCapacity());
            }
        }

#if WITH_EDITOR
        std::vector<DebugLine>    DebugLines;   // consumed by the next Render()
        std::vector<MaterialPreviewRequest> Previews;   // likewise
        std::vector<std::pair<uint64_t, MaterialPreviewMailbox*>> Captures;   // likewise
        std::vector<std::pair<uint64_t, MaterialPreviewMailbox*>> CapturesWithLines;   // likewise, read after the debug lines
        struct TargetReadback { uint64_t Request; std::string Path; MaterialPreviewMailbox* Mailbox; };
        std::vector<TargetReadback> TargetReadbacks;                           // likewise
#endif
        SceneCaptureList          CaptureList;                         // SetCaptures; consumed by Render
        const void*               PassWorld = nullptr;                 // SetCustomPassWorld; consumed by Render
        EngineCore::Color         ClearColor{24, 26, 32, 255};        // the top, with a gradient
        EngineCore::Color         ClearBottom{24, 26, 32, 255};
        bool                      bClearGradient = false;

        /* "stat frame": GPU times per ERenderGpuTimer, summed over the current
         * half second, and the last half second's averages (-1: not timed). */
        double  GpuSums[RGT_Count]   = {};
        int     GpuSamples[RGT_Count] = {};
        int64_t GpuWindowStartNs     = 0;
        float   GpuMs[RGT_Count]     = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };

        void AddGpuTimes(const float (&InGpuMs)[RGT_Count])
        {
            const int64_t Now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if(GpuWindowStartNs == 0) { GpuWindowStartNs = Now; }
            for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
            {
                if(InGpuMs[Timer] >= 0.0f) { GpuSums[Timer] += InGpuMs[Timer]; ++GpuSamples[Timer]; }
            }
            if(Now - GpuWindowStartNs >= 500'000'000)
            {
                for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
                {
                    GpuMs[Timer] = GpuSamples[Timer] > 0 ? float(GpuSums[Timer] / GpuSamples[Timer]) : -1.0f;
                    GpuSums[Timer]    = 0.0;
                    GpuSamples[Timer] = 0;
                }
                GpuWindowStartNs = Now;
                if(bReportsStats) { SetReportedGpuTimes(GpuMs); }
            }
        }

        /* The render thread's side (RenderStats::CpuMs, StageDraws, QueueCpuMs):
         * summed every frame, always, and averaged over the same half second. */
        double  CpuSums[RGT_Count]    = {};
        int     CpuSamples[RGT_Count] = {};
        double  DrawSums[RGT_Count]   = {};
        int     DrawSamples[RGT_Count] = {};
        double  QueueSum              = 0.0;
        int     QueueSamples          = 0;
        int64_t CpuWindowStartNs      = 0;
        float   CpuMs[RGT_Count]      = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
        int32_t StageDraws[RGT_Count] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        float   QueueMs               = -1.0f;

        void AddCpuTimes(const float (&InCpuMs)[RGT_Count], const int32_t (&InDraws)[RGT_Count], const float InQueueMs)
        {
            const int64_t Now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if(CpuWindowStartNs == 0) { CpuWindowStartNs = Now; }
            for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
            {
                if(InCpuMs[Timer] >= 0.0f) { CpuSums[Timer] += InCpuMs[Timer]; ++CpuSamples[Timer]; }
                if(InDraws[Timer] >= 0)    { DrawSums[Timer] += InDraws[Timer]; ++DrawSamples[Timer]; }
            }
            QueueSum += InQueueMs;
            ++QueueSamples;
            if(Now - CpuWindowStartNs >= 500'000'000)
            {
                for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer)
                {
                    CpuMs[Timer]      = CpuSamples[Timer] > 0 ? float(CpuSums[Timer] / CpuSamples[Timer]) : -1.0f;
                    StageDraws[Timer] = DrawSamples[Timer] > 0 ? int32_t(DrawSums[Timer] / DrawSamples[Timer] + 0.5) : -1;
                    CpuSums[Timer] = DrawSums[Timer] = 0.0;
                    CpuSamples[Timer] = DrawSamples[Timer] = 0;
                }
                QueueMs = QueueSamples > 0 ? float(QueueSum / QueueSamples) : -1.0f;
                QueueSum = 0.0;
                QueueSamples = 0;
                CpuWindowStartNs = Now;
                if(bReportsStats) { SetReportedStageTimes(CpuMs, StageDraws, QueueMs); }
            }
        }

        void ClearGpuTimes()
        {
            for(uint32_t Timer = 0; Timer < RGT_Count; ++Timer) { GpuSums[Timer] = 0.0; GpuSamples[Timer] = 0; GpuMs[Timer] = -1.0f; }
            GpuWindowStartNs = 0;
            if(bReportsStats) { SetReportedGpuTimes(GpuMs); }
        }
        /*
         * One view's pass (SceneViewState), after the frame's captures and
         * image based lighting: TAA's jitter, its occlusion, its draws
         * queued, the post process stack, then Base Pass through PostProcess
         * into its targets -- leaving its final colour bound and the context
         * on its jittered view.
         */
        void RenderSceneView(RenderContext& InContext, SceneViewState& InView)
        {
            InContext.SetLighting(InView.Lighting);

            /* TAA's jitter (RENDERPIPELINE.md, "Temporal antialiasing"): the view drawn a
             * sub-pixel off, a different way each frame (Halton 2, 3), so the resolve averages
             * many samples a pixel. InView.View stays unjittered: the captures, the next
             * frame's PrevViewProjection the resolve reprojects with, what draws after. */
            RenderView Drawn = InView.View;
            InContext.SetTemporalJitter(0.0f, 0.0f);
            const PostProcessSettings& Post = InView.PostProcess.Settings;
            if(Post.bAntialiasing && Post.AaMethod == 1 && InView.Targets.bHDR && InView.Targets.Width > 0 && InView.Targets.Height > 0)
            {
                const auto Halton = [](uint32_t InIndex, const uint32_t InBase)
                {
                    double Fraction = 1.0, Result = 0.0;
                    while(InIndex > 0)
                    {
                        Fraction /= double(InBase);
                        Result   += Fraction * double(InIndex % InBase);
                        InIndex  /= InBase;
                    }
                    return Result;
                };
                InView.JitterIndex = (InView.JitterIndex + 1) % 8;
                const double X = (Halton(InView.JitterIndex + 1, 2) - 0.5) * 2.0 / double(InView.Targets.Width);    // NDC
                const double Y = (Halton(InView.JitterIndex + 1, 3) - 0.5) * 2.0 / double(InView.Targets.Height);
                InContext.SetTemporalJitter(float(X), float(Y));
                using Element = std::remove_reference_t<decltype(Drawn.Projection.Data[0][0])>;
                for(int Column = 0; Column < 4; ++Column)
                {
                    // clip.xy += offset * clip.w: whatever the projection, perspective or not.
                    Drawn.Projection.Data[0][Column] += Element(X) * Drawn.Projection.Data[3][Column];
                    Drawn.Projection.Data[1][Column] += Element(Y) * Drawn.Projection.Data[3][Column];
                }
            }
            InContext.SetView(Drawn);

            InView.Occlusion.Prepare(Drawn);
            const auto QueueStart = std::chrono::steady_clock::now();
            QueueDraws(InContext, Drawn, InView.Lighting, nullptr, InView.VisibleCount, InView.CulledCount, InView.StaticCount, &InView.Occlusion);
            InView.QueueMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - QueueStart).count());

            /* The post process stack: the volumes' blend (WorldBridge), its
             * materials by stage and order, then the effects' settings. */
            for(uint32_t Index = 0; Index < Post.MaterialCount && Index < RENDER_POST_MATERIAL_MAX; ++Index)
            {
                InContext.SubmitPostProcess(Post.Materials[Index].Path, Post.Materials[Index].Stage);
            }
            const bool bSameViewport = InView.bHasPrevView && InView.PrevViewport.Width == InView.View.ViewportPixels.Width
                                    && InView.PrevViewport.Height == InView.View.ViewportPixels.Height;
            InContext.SetPostProcess(Post, bSameViewport ? &InView.PrevViewProjection : nullptr);
            InView.PrevViewProjection = InView.View.Projection * InView.View.View;
            InView.PrevViewport       = InView.View.ViewportPixels;
            InView.bHasPrevView       = true;

            /* Base Pass, Lighting, Unlit, the translucent stages, PostProcess;
             * leaves the final colour bound, for the present and the overlay. */
            InContext.SetUnlit(ViewMode == ERenderViewMode::Unlit);
            InContext.RenderSceneStages(InView.Targets, ClearColor, /*bInTimed*/ true, bClearGradient ? &ClearBottom : nullptr);
            InContext.SetUnlit(false);
        }

        /*
         * IMAGE-BASED LIGHTING (RENDERPIPELINE.md, "Image-based lighting"): the
         * probe atlas and cube strip; the stage targets a reflection probe's
         * faces render through; which probe each of the atlas's sixteen probe
         * blocks holds (by entity key) and whether its capture is in it; the
         * one capture in progress, its faces in the strip so far (a probe is
         * captured a few faces a frame, ProbeFacesPerFrame); and
         * whether the sky block is, for which sky. UpdateImageBasedLighting
         * keeps them; ApplyImageBasedLighting hands the context what the next
         * stages light with.
         */
        IBLResources IBL;
        SceneTargets ProbeStages{"RenderCore.ProbeStages"};
        SceneTargets SkyStages{"RenderCore.SkyStages"};   // a Sky pass's faces (ERenderHook::Sky)
        uint32_t     SkyCaptureWait = 0;                  // frames before a Sky pass's sky may be captured again
        /* A block's probe; whether its capture is in and whether another is wanted; and, with
         * [Rendering] StaticCaptures, what its last capture saw (the lighting's and the scene's
         * signatures as the capture began), so a due probe that would see the same is skipped. */
        struct ProbeSlot
        {
            uint64_t Key = 0;
            bool     bValid = false;
            bool     bPending = false;
            bool     bSigned = false;
            uint64_t LightSignature = 0;
            uint64_t SceneSignature = 0;
        };
        ProbeSlot    Slots[RENDER_REFLECTION_PROBES_MAX];
        int32_t      Capturing = -1;      // the block whose probe is mid-capture, or -1
        uint32_t     CapturedFaces = 0;   // its faces in the strip
        uint32_t     SearchFrom = 0;      // where the next search for a probe to capture starts: round the list, so none starves
        bool         bSkyValid = false;
        uint64_t     SkyHash   = 0;
        float        ProbeData[RENDER_REFLECTION_PROBES_MAX * 3][4] = {};
        uint32_t     ProbeDataCount = 0;

        /*
         * THE CAPTURE BUDGET (GLOBAL_ILLUMINATION.md, "Capture budget"; CaptureScheduler.h): the
         * units this frame's reflection probe and GI Volume faces may spend, [Rendering]
         * CaptureBudgetMs over what a unit costs; what they have spent; the lighting's signature;
         * the region the frame's capture shadow maps cover and whether they are drawn (each frame's
         * captures share one set, [Rendering] SharedCaptureShadows). BudgetSerial: this frame's GDI
         * block, whose Captures span comes back frames in flight later.
         */
        CaptureBudget  Budget;
        bool           bBudgeted        = false;
        float          FrameAllowance   = 0.0f;
        float          FrameUnits       = 0.0f;
        uint32_t       FrameRelit       = 0;      // GI probes relit this frame (RelightBudget's units)
        double         FrameCaptureCpuMs = 0.0;
        uint64_t       BudgetSerial     = 0;
        uint64_t       FrameLighting    = 0;
        GameCore::AABB CaptureRegion;
        bool           bCaptureShadowsTried = false;
        bool           bCaptureShadowsDrawn = false;
        bool           bCaptureShadowsRedrawn = false;   // drawn this frame, not kept from an earlier one
        bool           bCaptureDynamicCasters = false;
        /* [Rendering] CaptureLog's counts since its last line. */
        struct CaptureLogCounts
        {
            int64_t  StartNs = 0;
            uint32_t Frames = 0, ProbeFaces = 0, GIFaces = 0, ShadowPasses = 0, GIProbes = 0, Faces = 0, Relit = 0;
            uint64_t Visible = 0;
            float    Units = 0.0f;
            double   CpuMs = 0.0;
        } CaptureCounts;

        void LogCaptures()
        {
            CaptureLogCounts& C = CaptureCounts;
            ++C.Frames;
            C.Units += FrameUnits;
            C.Relit += FrameRelit;
            C.CpuMs += FrameCaptureCpuMs;
            C.ShadowPasses += bCaptureShadowsRedrawn ? 1u : 0u;
            const int64_t Now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if(C.StartNs == 0) { C.StartNs = Now; }
            if(Now - C.StartNs < 1'000'000'000) { return; }
            const double Frames = double(std::max(C.Frames, 1u));
            char Line[512];
            std::snprintf(Line, sizeof(Line),
                          "captures: %u frames, a frame %.1f units of %.1f allowed (unit %.3f ms GPU, %.3f ms CPU; CPU %.2f ms), "
                          "%.1f probe faces, %.1f GI faces (%.0f proxies a face), %.1f GI probes relit (%.4f ms each), %.2f shadow passes; "
                          "GI volume %s %u/%u probes (%u kept), %llu since change, %s",
                          C.Frames, double(C.Units) / Frames, double(FrameAllowance), double(Budget.GetUnitMs()), double(Budget.GetUnitCpuMs()),
                          C.CpuMs / Frames, double(C.ProbeFaces) / Frames, double(C.GIFaces) / Frames,
                          double(C.Visible) / double(std::max(C.Faces, 1u)), double(C.Relit) / Frames, double(RelightBudget.GetUnitMs()),
                          double(C.ShadowPasses) / Frames,
                          GIActive ? (GIActive->bReady ? "ready" : "filling") : "none", GIActive ? GIActive->Updated : 0u, GITotal,
                          GIActive ? GIActive->Stored : 0u,
                          static_cast<unsigned long long>(GIActive ? (GIActive->Relit.bKnown ? GIActive->Relit.SinceChange : GIActive->Convergence.SinceChange) : 0),
                          bGIWanted ? (bGIRelight ? "relighting" : "updating") : "idle");
            RENDERCORE_LOG(Line);
            C = CaptureLogCounts();
            C.StartNs = Now;
        }

        void ApplyImageBasedLighting(RenderContext& InContext) const
        {
            InContext.SetImageBasedLighting(IBL.Atlas ? &IBL : nullptr, ProbeData, ProbeDataCount, bSkyValid);
        }

        /* What the sky block depends on: the colours, the HDRI (and the loaded file, so a reimport counts). */
        uint64_t SkyHashOf() const
        {
            uint64_t Hash = 1469598103934665603ull;
            const auto Mix = [&Hash](const void* InData, const size_t InSize)
            {
                const auto* Bytes = static_cast<const uint8_t*>(InData);
                for(size_t Index = 0; Index < InSize; ++Index) { Hash = (Hash ^ Bytes[Index]) * 1099511628211ull; }
            };
            const RenderLighting& Lighting = Views[0].Lighting;   // the sky is the scene's: every view's is the same
            const EngineCore::Color Colors[2] = { Lighting.SkyColor, Lighting.GroundColor };
            for(const EngineCore::Color& Each : Colors) { const uint8_t Rgb[3] = { Each.R, Each.G, Each.B }; Mix(Rgb, 3); }
            Mix(&Lighting.SkyIntensity, sizeof(float));
            Mix(Lighting.SkyEnvironment, std::strlen(Lighting.SkyEnvironment));
            Mix(&Lighting.SkyEnvironmentIntensity, sizeof(float));
            Mix(&Lighting.SkyEnvironmentRotation, sizeof(float));
            if(Lighting.SkyEnvironment[0] != '\0')
            {
                const void* Loaded = TextureAssetCache::Find(Lighting.SkyEnvironment).get();
                Mix(&Loaded, sizeof(Loaded));
            }
            return Hash;
        }

        /* What capture views multiply their LOD distances by (SimpleCaptures). */
        static float CaptureLodScale()
        {
            return GRenderSimpleCaptures.GetBool() ? std::max(GRenderCaptureLodScale.GetFloat(), 1.0f) : 1.0f;
        }

        /* A proxy's size against its distance from InEye (radius over distance), or -1 without bounds. */
        static double SizeFrom(const RenderProxy& InProxy, const EngineCore::Vector3& InEye)
        {
            const GameCore::AABB& Bounds = InProxy.GetWorldBounds();
            if(Bounds.IsEmpty()) { return -1.0; }
            const double Radius   = (Bounds.Max - Bounds.Min).GetLength() * 0.5;
            const double Distance = (((Bounds.Min + Bounds.Max) * 0.5) - InEye).GetLength();
            return Radius / std::max(Distance, 1.0);
        }

        /* What a capture from InEye sees of the scene, as a hash: each renderable proxy within InReach
         * of it (static ones alone, with bInStaticOnly) not too small to draw (InCullRatio, as
         * QueueDraws culls a capture's), by its change serial (a dynamic one by its bounds and transform: it is extracted every frame), summed so their order does not count;
         * and the content generation (materials and meshes reloaded). */
        uint64_t SceneSignature(RenderContext& InContext, const EngineCore::Vector3& InEye, const double InReach, const double InCullRatio,
                                const bool bInStaticOnly) const
        {
            const EngineCore::Vector3 Half(InReach, InReach, InReach);
            const GameCore::AABB Reach(InEye - Half, InEye + Half);
            uint64_t Sum = 0, Count = 0;
            for(const RenderProxy* Proxy : Proxies)
            {
                if(!Proxy->IsRenderable() || (bInStaticOnly && !Proxy->IsStatic())) { continue; }
                const GameCore::AABB& Bounds = Proxy->GetWorldBounds();
                if(!Bounds.IsEmpty() && (!Bounds.Overlaps(Reach) || SizeFrom(*Proxy, InEye) < InCullRatio)) { continue; }
                /* A static proxy's serial moves only when it is edited; a dynamic one is extracted every
                 * frame, so where it is and how big stand for it instead. */
                uint64_t State = Proxy->GetChangeSerial();
                if(!Proxy->IsStatic())
                {
                    State = 1469598103934665603ull;
                    const double Box[6] = { Bounds.Min.GetX(), Bounds.Min.GetY(), Bounds.Min.GetZ(), Bounds.Max.GetX(), Bounds.Max.GetY(), Bounds.Max.GetZ() };
                    for(const double Value : Box) { State = (State ^ uint64_t(int64_t(std::llround(Value * 16.0)))) * 1099511628211ull; }
                    const EngineCore::Matrix4x4& World = Proxy->GetLocalToWorld();
                    for(int Row = 0; Row < 3; ++Row)
                    {
                        for(int Col = 0; Col < 4; ++Col) { State = (State ^ uint64_t(int64_t(std::llround(double(World.Data[Row][Col]) * 1024.0)))) * 1099511628211ull; }
                    }
                }
                uint64_t Each = (uint64_t(reinterpret_cast<uintptr_t>(Proxy)) ^ (State * 0x9E3779B97F4A7C15ull)) * 1099511628211ull;
                Each ^= Each >> 29;
                Sum += Each;
                ++Count;
            }
            return (Sum ^ (Count * 0xC2B2AE3D27D4EB4Full)) + InContext.GetContentGeneration() * 0x165667B19E3779F9ull;
        }

        /* What a unit (a face) and a relit GI probe are taken to cost, the dearer of GPU and CPU. */
        float FaceUnitMs() const { return std::max(Budget.GetUnitMs(), Budget.GetUnitCpuMs()); }
        float RelitProbeMs() const { return std::max(RelightBudget.GetUnitMs(), RelightBudget.GetUnitCpuMs()); }

        /* The milliseconds this frame's captures have spent so far, as those costs reckon them. */
        float FrameSpentMs() const { return FrameUnits * FaceUnitMs() + float(FrameRelit) * RelitProbeMs(); }

        /* Whether InUnits more of faces fit InShareMs of the frame ([Rendering] CaptureBudgetMs) after
         * what it has spent; the frame's first work always does, so captures move however dear. */
        bool FaceFits(const float InUnits, const float InShareMs, const float InPending = 0.0f) const
        {
            if(!bBudgeted || (FrameUnits <= 0.0f && FrameRelit == 0 && InPending <= 0.0f)) { return true; }
            return FrameSpentMs() + (InPending + InUnits) * FaceUnitMs() <= InShareMs + 1e-4f;
        }

        /* The frame's capture shadow maps (RenderContext::BeginCaptureShadows), drawn the first time
         * a capture face wants shadows: CaptureRegion's casters, those smaller than a texel left out;
         * Dynamic ones only when a reflection probe captures (a GI Volume sees static ones alone). */
        void EnsureCaptureShadows(RenderContext& InContext)
        {
            if(bCaptureShadowsTried || !GRenderSharedCaptureShadows.GetBool())
            {
                return;
            }
            bCaptureShadowsTried = true;
            if(CaptureRegion.IsEmpty())
            {
                return;
            }
            /* What the maps would hold: the lighting, the region, its casters (as SceneSignature sees
             * them); the same as the kept maps', they are used again. */
            const EngineCore::Vector3 Centre = (CaptureRegion.Min + CaptureRegion.Max) * 0.5;
            const double HalfDiagonal = (CaptureRegion.Max - CaptureRegion.Min).GetLength() * 0.5;
            uint64_t Key = FrameLighting * 0x9E3779B97F4A7C15ull + SceneSignature(InContext, Centre, HalfDiagonal * 2.0, 0.0, !bCaptureDynamicCasters);
            const double Box[6] = { CaptureRegion.Min.GetX(), CaptureRegion.Min.GetY(), CaptureRegion.Min.GetZ(),
                                    CaptureRegion.Max.GetX(), CaptureRegion.Max.GetY(), CaptureRegion.Max.GetZ() };
            for(const double Each : Box) { Key = (Key ^ uint64_t(int64_t(std::llround(Each)))) * 1099511628211ull; }
            Key = Key == 0 ? 1 : Key;

            InContext.SetView(Views[0].View);
            InContext.BeginSceneDraws();
            const uint32_t ShadowViews = InContext.BeginCaptureShadows(CaptureRegion, Key);
            bCaptureShadowsRedrawn = ShadowViews > 0;
            InContext.SetLodDistanceScale(CaptureLodScale());
            const uint64_t Generation  = InContext.GetContentGeneration();
            const double   Texel       = (CaptureRegion.Max - CaptureRegion.Min).GetLength()
                                       / double(std::max(Views[0].Lighting.Shadows.Resolution, 256));
            for(uint32_t ViewIndex = 0; ViewIndex < ShadowViews; ++ViewIndex)
            {
                const GameCore::AABB& ViewBounds = InContext.GetShadowViewBounds(ViewIndex);
                InContext.SetShadowCasterView(int32_t(ViewIndex));
                const bool bSun = InContext.IsSunShadowView(ViewIndex);
                for(RenderProxy* Proxy : Proxies)
                {
                    if(!Proxy->IsRenderable() || (!bCaptureDynamicCasters && !Proxy->IsStatic())) { continue; }
                    GameCore::AABB Bounds = Proxy->GetWorldBounds();
                    if(!Bounds.IsEmpty())
                    {
                        /* Smaller than half a texel of the sun's map: no shadow worth drawing. */
                        if(bSun && (Bounds.Max - Bounds.Min).GetLength() < Texel) { continue; }
                        if(!ViewBounds.IsEmpty())
                        {
                            const float Padding = Proxy->IsStatic() ? 0.0f : Proxy->GetBoundsPadding(InContext);
                            const EngineCore::Vector3 Pad(Padding, Padding, Padding);
                            Bounds = GameCore::AABB(Bounds.Min - Pad, Bounds.Max + Pad);
                            if(!Bounds.Overlaps(ViewBounds)) { continue; }
                        }
                    }
                    if(Proxy->IsStatic())
                    {
                        StaticDrawList& List = ListFor(Proxy, Generation);
                        if(!List.bShadows)
                        {
                            InContext.BeginCapture(&List.Shadows);
                            Proxy->DrawShadow(InContext);
                            InContext.EndCapture();
                            List.bShadows = true;
                            ++StaticBuilds;
                        }
                        InContext.SubmitCaptured(List.Shadows, int32_t(ViewIndex));
                    }
                    else
                    {
                        Proxy->DrawShadow(InContext);
                    }
                }
            }
            InContext.SetShadowCasterView(-1);
            InContext.SetLodDistanceScale(1.0f);
            bCaptureShadowsDrawn = InContext.DrawCaptureShadows();
        }

        /* One cube face of InCube (InFace of Views, at InCube's capture settings) into InStages:
         * the frame's capture shadow maps when it wants shadows and they are shared. */
        void RenderCaptureFace(RenderContext& InContext, const SceneCaptureRequest& InCube, const RenderView& InView, SceneTargets& InStages)
        {
            const bool bShadows = InCube.bShadows != 0;
            if(bShadows) { EnsureCaptureShadows(InContext); }
            SceneCaptureRequest Face = InCube;
            if(bShadows && bCaptureShadowsDrawn)
            {
                Face.bShadows = 1;   // PrepareShadows hands it the shared maps: nothing to queue
            }
            const auto Start = std::chrono::steady_clock::now();
            InContext.SetView(InView);
            InContext.SetLodDistanceScale(CaptureLodScale());
            uint64_t Visible = 0, Culled = 0, Static = 0;
            QueueDraws(InContext, InView, this->Views[0].Lighting, &Face, Visible, Culled, Static);
            InContext.SetLodDistanceScale(1.0f);
            CaptureCounts.Visible += Visible;
            ++CaptureCounts.Faces;
            InContext.SetCaptureRendering(GRenderSimpleCaptures.GetBool());
            InContext.RenderSceneStages(InStages, ClearColor);
            InContext.SetCaptureRendering(false);
            FrameCaptureCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
        }

        /* A reflection probe's faces InFirst to InFirst + InCount - 1, HDR, into the strip; after its
         * sixth, the strip prefiltered into atlas block InBlock. False if one failed. */
        bool CaptureReflectionProbe(RenderContext& InContext, const ReflectionProbeRequest& InProbe, const uint32_t InBlock,
                                    const uint32_t InFirst, const uint32_t InCount)
        {
            const uint32_t Face = InProbe.Resolution;
            if(!InContext.EnsureIBLResources(IBL, Face) || !ProbeStages.Allocate(Face, Face))
            {
                return false;
            }
            SceneCaptureRequest Cube;
            Cube.Mode     = SCM_Omnidirectional;
            Cube.Position = InProbe.Position;
            Cube.NearClip = InProbe.NearClip;
            Cube.FarClip  = InProbe.FarClip;
            Cube.bShadows = InProbe.bShadows;
            if(GRenderSimpleCaptures.GetBool())
            {
                Cube.CullRatio = float(std::max(GRenderCaptureCullPixels.GetFloat(), 0.0f) / float(std::max(Face, 1u)));
            }
            RenderView Views[6];
            const uint32_t ViewCount = BuildCaptureViews(Cube, this->Views[0].View, Face, Face, Views);

            /* Lit by the sky alone (no probe sees another, or itself), the sky behind. */
            InContext.SetImageBasedLighting(&IBL, nullptr, 0, bSkyValid);
            InContext.SetSkyBackground(true);
            bool bCopied = ViewCount == 6;
            for(uint32_t Index = InFirst; Index < std::min(InFirst + InCount, ViewCount); ++Index)
            {
                SCOPED_DRAW_EVENT(GetCubeFaceName(Index));
                RenderCaptureFace(InContext, Cube, Views[Index], ProbeStages);
                bCopied = InContext.CopyCaptureToStrip(ProbeStages, IBL, Index) && bCopied;
            }
            InContext.SetSkyBackground(false);
            return bCopied && (InFirst + InCount < 6 || InContext.PrefilterIBLBlock(IBL, InBlock));
        }

        /* A probe's reach for the capture shadow maps: its influence, half as much again round it
         * (what its reflections show beyond is left unshadowed). */
        static GameCore::AABB ProbeShadowRegion(const ReflectionProbeRequest& InProbe)
        {
            const bool bBox = InProbe.Extent[0] > 0.0f && InProbe.Extent[1] > 0.0f && InProbe.Extent[2] > 0.0f;
            const EngineCore::Vector3 Reach = bBox ? EngineCore::Vector3(InProbe.Extent[0], InProbe.Extent[1], InProbe.Extent[2]) * 1.5
                                                   : EngineCore::Vector3(InProbe.Radius, InProbe.Radius, InProbe.Radius) * 1.5;
            return GameCore::AABB(InProbe.Position - Reach, InProbe.Position + Reach);
        }

        /*
         * The frame's captures, inside the Captures span: the sky block when the sky changed; then
         * the probes in view and the GI Volume nearest the camera, their faces within this frame's
         * share of [Rendering] CaptureBudgetMs (or, at 0, ProbeFacesPerFrame faces of one probe and
         * GIProbesPerFrame whole GI probes, as before the budget). Leaves the context on the main
         * view, lit by the probes and the volume.
         */
        void UpdateCaptures(RenderContext& InContext)
        {
            const float BudgetMs = GRenderCaptureBudgetMs.GetFloat();
            bBudgeted            = BudgetMs > 0.0f;
            FrameAllowance       = bBudgeted ? Budget.GetAllowance(BudgetMs) : 0.0f;
            FrameUnits           = 0.0f;
            FrameRelit           = 0;
            FrameCaptureCpuMs    = 0.0;
            FrameLighting        = CaptureLightingSignature(Views[0].Lighting, GRenderCaptureSunStep.GetFloat());
            CaptureRegion        = GameCore::AABB();
            bCaptureShadowsTried = false;
            bCaptureShadowsDrawn = false;
            bCaptureShadowsRedrawn = false;
            bCaptureDynamicCasters = false;

            const int32_t Probe = PlanImageBasedLighting(InContext);
            const bool    bGI   = PlanGlobalIllumination(InContext);

            /* The order: a GI Volume's relights first (cheap, and a lighting change shows fastest
             * so); else, both wanting faces, turn about frame by frame, the first to half the budget.
             * The second takes what is left. A GI Volume's faces are lit by this frame's probes when
             * they come second, by the last frame's when first. */
            const bool  bGIFirst = bBudgeted && bGI && (bGIRelight || Probe < 0 || (GIFrame & 1u) != 0);
            const float FirstMs  = bGI && Probe >= 0 && !bGIRelight ? BudgetMs * 0.5f : BudgetMs;
            if(bGIFirst)
            {
                CaptureGlobalIllumination(InContext, FirstMs);
                CaptureProbes(InContext, Probe, BudgetMs);
                FinishImageBasedLighting(InContext);
            }
            else
            {
                CaptureProbes(InContext, Probe, bGI ? FirstMs : BudgetMs);
                FinishImageBasedLighting(InContext);
                CaptureGlobalIllumination(InContext, BudgetMs);
            }
            /* The views light by the active volume (CaptureGlobalIllumination set it; the probes' captures may not have). */
            if(GIActive && GIActive->bReady)
            {
                float Header[4][4];
                MakeHeader(GIActiveGrid, GIActive->Resources, true, Header);
                InContext.SetDDGI(&GIActive->Resources, Header);
            }
            else
            {
                InContext.SetDDGI(nullptr, nullptr);
            }

            InContext.EndCaptureShadows();
            InContext.SetCaptureRendering(false);
            InContext.SetView(Views[0].View);
            if(bBudgeted)
            {
                /* A frame of relights alone teaches RelightBudget; one of faces, Budget. */
                Budget.Record(BudgetSerial, FrameUnits);
                RelightBudget.Record(BudgetSerial, FrameUnits < 0.5f ? float(FrameRelit) : 0.0f);
                if(FrameUnits >= 0.5f)      { Budget.MeasuredCpu(FrameUnits, float(FrameCaptureCpuMs)); }
                else if(FrameRelit > 0)     { RelightBudget.MeasuredCpu(float(FrameRelit), float(FrameCaptureCpuMs)); }
            }
            if(GRenderCaptureLog.GetBool()) { LogCaptures(); }
        }

        /* The sky block when the sky changed, then which block holds each probe in view and which
         * want a capture: due, or new to it -- with StaticCaptures, a due probe only if what it
         * would see changed since its last, and any probe (but a Manual one) whose lighting did.
         * Returns the probe (CaptureList index) to capture now, or -1. */
        int32_t PlanImageBasedLighting(RenderContext& InContext)
        {
            if(!InContext.CanImageBasedLight())
            {
                bSkyValid = false;
                return -1;
            }
            const bool bStatic = GRenderStaticCaptures.GetBool();

            /* A custom pass's sky (ERenderHook::Sky) in place of the Sky Light's when there is one:
             * captured again when its version changes, but no more often than SkyCaptureFrames. */
            uint64_t   SkyVersion = 0;
            const bool bSkyPass   = GRenderSkyPassLighting.GetBool() && InContext.GetSkyPassVersion(SkyVersion);
            const uint64_t Hash   = bSkyPass ? (SkyVersion ^ 0x5ca1ab1e0ddba11ull) * 1099511628211ull : SkyHashOf();
            if(SkyCaptureWait > 0) { --SkyCaptureWait; }
            if(!bSkyValid || (Hash != SkyHash && (!bSkyPass || SkyCaptureWait == 0)))
            {
                const bool bWasValid = bSkyValid;
                if(bSkyPass)
                {
                    SCOPED_DRAW_EVENT("SkyLight (Sky passes)");
                    constexpr uint32_t Face = 128;
                    SceneCaptureRequest Cube;
                    Cube.Mode     = SCM_Omnidirectional;
                    Cube.Position = Views[0].View.Origin;
                    Cube.NearClip = 1.0f;
                    Cube.FarClip  = 1000.0f;
                    RenderView Faces[6];
                    bSkyValid = InContext.EnsureIBLResources(IBL, Face) && SkyStages.Allocate(Face, Face)
                             && BuildCaptureViews(Cube, Views[0].View, Face, Face, Faces) == 6
                             && InContext.RenderSkyPassesToStrip(IBL, SkyStages, Faces)
                             && InContext.PrefilterIBLBlock(IBL, IBL_SKY_BLOCK);
                    InContext.SetView(Views[0].View);
                    SkyCaptureWait = uint32_t(std::max(0, GRenderSkyCaptureFrames.GetInt()));
                }
                else
                {
                    SCOPED_DRAW_EVENT("SkyLight");
                    bSkyValid = InContext.EnsureIBLResources(IBL, 128) && InContext.RenderSkyToStrip(IBL)
                             && InContext.PrefilterIBLBlock(IBL, IBL_SKY_BLOCK);
                }
                /* The probes saw the old sky behind their scene, and were lit by it -- with
                 * StaticCaptures, only those whose lighting moved since (below). */
                if(bSkyValid && bWasValid && bSkyPass && GRenderProbesFollowSky.GetBool() && !bStatic)
                {
                    for(ProbeSlot& Slot : Slots) { if(Slot.Key != 0 && Slot.bValid) { Slot.bPending = true; } }
                }
                SkyHash = Hash;
                CapturedFaces = 0;   // the sky took the strip: a capture in progress starts over
            }

            const uint32_t Count = std::min<uint32_t>(CaptureList.ProbeCount, RENDER_REFLECTION_PROBES_MAX);
            const auto Listed = [&](const uint64_t InKey)
            {
                for(uint32_t Index = 0; Index < Count; ++Index) { if(CaptureList.Probes[Index].Key == InKey) { return true; } }
                return false;
            };
            for(uint32_t Index = 0; Index < Count; ++Index)
            {
                const ReflectionProbeRequest& Probe = CaptureList.Probes[Index];
                BlockOf[Index] = -1;
                for(uint32_t Block = 0; Block < RENDER_REFLECTION_PROBES_MAX; ++Block)
                {
                    if(Slots[Block].Key == Probe.Key && Slots[Block].Key != 0) { BlockOf[Index] = int32_t(Block); break; }
                }
                if(BlockOf[Index] < 0)
                {
                    for(uint32_t Block = 0; Block < RENDER_REFLECTION_PROBES_MAX; ++Block)
                    {
                        if(Slots[Block].Key == 0 || !Listed(Slots[Block].Key))
                        {
                            if(Capturing == int32_t(Block)) { Capturing = -1; }
                            Slots[Block] = ProbeSlot{};
                            Slots[Block].Key = Probe.Key;
                            Slots[Block].bPending = true;
                            BlockOf[Index] = int32_t(Block);
                            break;
                        }
                    }
                }
                if(BlockOf[Index] < 0) { continue; }
                ProbeSlot& Slot = Slots[BlockOf[Index]];
                if(!bStatic || Probe.bForce || !Slot.bValid || !Slot.bSigned)
                {
                    if(Probe.bDue) { Slot.bPending = true; }
                    continue;
                }
                /* StaticCaptures: the lighting moved (a sky pass's sky with it), or, due, what it sees did. */
                if(Probe.bFollowsLight && Slot.LightSignature != FrameLighting)
                {
                    Slot.bPending = true;
                }
                else if(Probe.bDue && !Slot.bPending
                        && Slot.SceneSignature != SceneSignature(InContext, Probe.Position, Probe.FarClip, ProbeCullRatio(Probe), false))
                {
                    Slot.bPending = true;
                }
            }

            /* The capture in progress, else the next probe wanting one. */
            int32_t Current = -1;
            for(uint32_t Index = 0; Index < Count; ++Index)
            {
                if(BlockOf[Index] >= 0 && BlockOf[Index] == Capturing) { Current = int32_t(Index); break; }
            }
            if(Current < 0)
            {
                Capturing = -1;
                Current   = NextProbe(Count);
            }
            /* The shadow maps' region: every listed probe that captures shadows (the same from frame
             * to frame while the view's probes are, so the maps are kept); Dynamic casters too while
             * one of them is capturing. */
            bool bAnyWanted = Current >= 0;
            for(uint32_t Index = 0; Index < Count; ++Index)
            {
                if(BlockOf[Index] < 0 || !CaptureList.Probes[Index].bShadows) { continue; }
                const ProbeSlot& Slot = Slots[BlockOf[Index]];
                CaptureRegion = GameCore::AABB::Union(CaptureRegion, ProbeShadowRegion(CaptureList.Probes[Index]));
                bAnyWanted = bAnyWanted || Slot.bPending || !Slot.bValid;
            }
            bCaptureDynamicCasters = bAnyWanted && !CaptureRegion.IsEmpty();
            return Current;
        }

        int32_t BlockOf[RENDER_REFLECTION_PROBES_MAX] = {};   // PlanImageBasedLighting's: each listed probe's block, or -1

        /* A probe's smallest drawn size, radius over distance (QueueDraws' capture cull). */
        static double ProbeCullRatio(const ReflectionProbeRequest& InProbe)
        {
            return GRenderSimpleCaptures.GetBool()
                 ? double(std::max(GRenderCaptureCullPixels.GetFloat(), 0.0f)) / double(std::max(InProbe.Resolution, 1u)) : 0.0;
        }

        /* The next listed probe wanting a capture, round the list from SearchFrom; starts it. */
        int32_t NextProbe(const uint32_t InCount)
        {
            for(uint32_t Step = 0; Step < InCount; ++Step)
            {
                const uint32_t Index = (SearchFrom + Step) % InCount;
                if(BlockOf[Index] < 0) { continue; }
                const ProbeSlot& Slot = Slots[BlockOf[Index]];
                if(Slot.bPending || !Slot.bValid)
                {
                    Capturing     = BlockOf[Index];
                    CapturedFaces = 0;
                    SearchFrom    = Index + 1;
                    return int32_t(Index);
                }
            }
            return -1;
        }

        /* Probe InCurrent's faces, then (budgeted) the next probe's, while InShare lasts: one at a
         * time, the probe's old capture (if any) lighting until its last face is in. */
        void CaptureProbes(RenderContext& InContext, int32_t InCurrent, const float InShareMs)
        {
            const uint32_t Count = std::min<uint32_t>(CaptureList.ProbeCount, RENDER_REFLECTION_PROBES_MAX);
            while(InCurrent >= 0)
            {
                const ReflectionProbeRequest& Probe = CaptureList.Probes[InCurrent];
                const float Units = CaptureFaceUnits(Probe.Resolution);
                if(!FaceFits(Units, InShareMs)) { break; }

                SCOPED_DRAW_EVENT("ReflectionProbe " + std::to_string(Capturing));
                ProbeSlot& Slot = Slots[Capturing];
                if(CapturedFaces == 0)
                {
                    /* What it sees as it starts: a change after this captures it again. */
                    Slot.bPending       = false;   // a request while it captures starts the next
                    Slot.LightSignature = FrameLighting;
                    Slot.SceneSignature = GRenderStaticCaptures.GetBool()
                                        ? SceneSignature(InContext, Probe.Position, Probe.FarClip, ProbeCullRatio(Probe), false) : 0;
                    Slot.bSigned        = true;
                }
                uint32_t Now = uint32_t(std::clamp<int>(GRenderProbeFacesPerFrame.GetInt(), 1, 6));
                if(bBudgeted)
                {
                    Now = 1;
                    while(Now < 6u - CapturedFaces && FaceFits(Units, InShareMs, Units * float(Now))) { ++Now; }
                }
                Now = std::min(Now, 6u - CapturedFaces);
                const bool bOk = CaptureReflectionProbe(InContext, Probe, uint32_t(Capturing), CapturedFaces, Now);
                FrameUnits += Units * float(Now);
                CaptureCounts.ProbeFaces += Now;
                bool bFinished = false;
                if(!bOk)
                {
                    Slot.bValid = false;
                    Capturing   = -1;
                    bFinished   = true;
                }
                else if((CapturedFaces += Now) >= 6)
                {
                    Slot.bValid = true;
                    Capturing   = -1;
                    bFinished   = true;
                }
                /* Budgeted, the next probe wanting one may start in what is left; else one a frame. */
                InCurrent = bFinished && bBudgeted && bOk ? NextProbe(Count) : (bFinished ? -1 : InCurrent);
                if(!bBudgeted) { break; }
            }
            InContext.SetView(Views[0].View);
        }

        /* The probes' data for the views (LightList_Probes), then handed to the context. */
        void FinishImageBasedLighting(RenderContext& InContext)
        {
            const uint32_t Count = InContext.CanImageBasedLight() ? std::min<uint32_t>(CaptureList.ProbeCount, RENDER_REFLECTION_PROBES_MAX) : 0;
            ProbeDataCount = 0;
            for(uint32_t Index = 0; Index < Count; ++Index)
            {
                if(BlockOf[Index] < 0 || !Slots[BlockOf[Index]].bValid) { continue; }
                const ReflectionProbeRequest& Probe = CaptureList.Probes[Index];
                float (*Out)[4] = &ProbeData[ProbeDataCount * 3];
                Out[0][0] = float(Probe.Position.GetX()); Out[0][1] = float(Probe.Position.GetY());
                Out[0][2] = float(Probe.Position.GetZ()); Out[0][3] = Probe.Radius;
                Out[1][0] = Probe.Extent[0]; Out[1][1] = Probe.Extent[1]; Out[1][2] = Probe.Extent[2]; Out[1][3] = Probe.Intensity;
                Out[2][0] = float(BlockOf[Index]); Out[2][1] = 0.0f; Out[2][2] = 0.0f; Out[2][3] = 0.0f;
                ++ProbeDataCount;
            }
            ApplyImageBasedLighting(InContext);
        }

        /*
         * DYNAMIC GI (GLOBAL_ILLUMINATION.md, Phase 6): the GI Volumes' probes and the stage targets
         * their faces render through. A few volumes are kept (DDGI_VOLUMES_KEPT, the least recently
         * lit dropped), so going back to one needs no new round; each has its grid (a hash: a change
         * starts it over), the next probe and face to update, how many probes have updated, whether
         * every one has -- it lights only then, a probe never seen being black -- and how many since
         * what it sees last changed (StaticCaptures: GIConvergeRounds rounds after, it stops).
         */
        static constexpr uint32_t DDGI_VOLUMES_KEPT = 4;
        struct GIVolumeState
        {
            uint64_t           Grid    = 0;
            DDGIResources      Resources;
            uint32_t           Cursor  = 0;
            uint32_t           Face    = 0;
            uint32_t           Updated = 0;
            bool               bReady  = false;
            uint64_t           LastLit = 0;
            CaptureConvergence Convergence;
            int64_t            ChangedNs   = 0;       // CaptureLog: when what it sees last changed
            bool               bLoggedDone = false;   // and whether its convergence since is logged

            /* Relighting: the static geometry's signature its cache holds, how many probes' six faces
             * are in the cache since that last changed, whether it has a cache at all, the next probe
             * to light again, and the lighting's convergence by relit probes. */
            uint64_t           Geometry      = 0;
            bool               bGeometry     = false;
            uint32_t           Stored        = 0;
            bool               bCache        = false;
            uint32_t           RelightCursor = 0;
            CaptureConvergence Relit;
        };
        GIVolumeState GIVolumes[DDGI_VOLUMES_KEPT];
        SceneTargets  DDGIStages{"RenderCore.DDGIStages"};
        uint64_t      GIFrame = 0;

        /* A volume's grid: probes along each axis, their spacing, the first one's place, how many,
         * the longest distance a probe keeps, how far its faces see, and a hash of it all (a change
         * starts the volume over); its box and intensity, from the request. */
        struct GIGrid
        {
            uint32_t            Count[3]   = {};
            float               Spacing[3] = {};
            float               First[3]   = {};
            uint32_t            Total      = 0;
            float               Longest    = 0.0f;
            float               FarClip    = 20000.0f;
            float               Diagonal   = 0.0f;
            uint64_t            Hash       = 0;
            float               Intensity  = 1.0f;
            EngineCore::Vector3 Position;
            float               Extent[3]  = {};
        };

        static GIGrid MakeGrid(const GIVolumeRequest& InVolume)
        {
            GIGrid Out;
            const uint32_t Most[3] = { 32, 8, 32 };
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                Out.Count[Axis]   = std::clamp<uint32_t>(uint32_t(2.0f * InVolume.Extent[Axis] / InVolume.Spacing) + 1u, 2u, Most[Axis]);
                Out.Spacing[Axis] = 2.0f * InVolume.Extent[Axis] / float(Out.Count[Axis] - 1);
                Out.Extent[Axis]  = InVolume.Extent[Axis];
            }
            Out.First[0]  = float(InVolume.Position.GetX()) - InVolume.Extent[0];
            Out.First[1]  = float(InVolume.Position.GetY()) - InVolume.Extent[1];
            Out.First[2]  = float(InVolume.Position.GetZ()) - InVolume.Extent[2];
            Out.Total     = Out.Count[0] * Out.Count[1] * Out.Count[2];
            Out.Longest   = 1.5f * std::sqrt(Out.Spacing[0] * Out.Spacing[0] + Out.Spacing[1] * Out.Spacing[1] + Out.Spacing[2] * Out.Spacing[2]);
            Out.Intensity = InVolume.Intensity;
            Out.Position  = InVolume.Position;

            uint64_t Hash = 1469598103934665603ull;
            const auto Mix = [&Hash](const void* InData, const size_t InSize)
            {
                const auto* Bytes = static_cast<const uint8_t*>(InData);
                for(size_t Index = 0; Index < InSize; ++Index) { Hash = (Hash ^ Bytes[Index]) * 1099511628211ull; }
            };
            Mix(Out.First, sizeof(Out.First)); Mix(Out.Spacing, sizeof(Out.Spacing)); Mix(Out.Count, sizeof(Out.Count));
            Out.Hash = Hash;

            /* A probe sees the scene out to GICaptureSpacings probe spacings (at least 2500, at most
             * 20000 and twice the volume's diagonal): its neighbourhood, so a probe costs the same
             * however large the level; the sky stands behind. */
            Out.Diagonal = 2.0f * std::sqrt(InVolume.Extent[0] * InVolume.Extent[0] + InVolume.Extent[1] * InVolume.Extent[1]
                                            + InVolume.Extent[2] * InVolume.Extent[2]);
            const float Reach = std::max(GRenderGICaptureSpacings.GetFloat(), 1.0f) * std::max({ Out.Spacing[0], Out.Spacing[1], Out.Spacing[2] });
            Out.FarClip = GRenderSimpleCaptures.GetBool() ? std::clamp(std::min(Reach, 2.0f * Out.Diagonal), 2500.0f, 20000.0f) : 20000.0f;
            return Out;
        }

        /* The volume's header for the light block (LightList_DDGI): lit only once ready. */
        static void MakeHeader(const GIGrid& InGrid, const DDGIResources& InResources, const bool bInReady, float (&OutHeader)[4][4])
        {
            const float Header[4][4] = {
                { InGrid.First[0], InGrid.First[1], InGrid.First[2], bInReady ? InGrid.Intensity : 0.0f },
                { InGrid.Spacing[0], InGrid.Spacing[1], InGrid.Spacing[2], InGrid.Longest },
                { float(InGrid.Count[0]), float(InGrid.Count[1]), float(InGrid.Count[2]), float(DDGI_TILES_PER_ROW) },
                { float(InResources.Irradiance->GetWidth()), float(InResources.Irradiance->GetHeight()),
                  float(InResources.Distance->GetWidth()), float(InResources.Distance->GetHeight()) } };
            std::memcpy(OutHeader, Header, sizeof(Header));
        }

        /* PlanGlobalIllumination's: the volume lighting this frame and its grid, and what it wants
         * (faces, relights, or nothing); and the volume after it whose probes fill in the background
         * meanwhile (BACKGROUND FILL: the next nearest, so it is ready when the camera gets there). */
        GIVolumeState* GIActive     = nullptr;
        GIGrid         GIActiveGrid;
        GIVolumeState* GIBackground = nullptr;
        GIGrid         GIBackgroundGrid;
        bool           bGIWanted    = false;
        bool           bGIRelight   = false;   // this frame's updates are relights, not draws
        CaptureBudget  RelightBudget;          // its units: probes relit
        uint32_t       GITotal      = 0;       // the active volume's probes (CaptureLog)

        void ReleaseGIVolumes(RenderContext& InContext)
        {
            for(GIVolumeState& Each : GIVolumes)
            {
                InContext.ReleaseDDGIResources(Each.Resources);
                Each = GIVolumeState();
            }
            GIActive     = nullptr;
            GIBackground = nullptr;
        }

        /* InGrid's kept state, else a new one in the least recently used's place -- never one of
         * InKeep's (this frame's). Null if its targets cannot be made. */
        GIVolumeState* StateFor(RenderContext& InContext, const GIGrid& InGrid, const GIVolumeState* InKeep, const uint64_t InUse)
        {
            GIVolumeState* State = nullptr;
            for(GIVolumeState& Each : GIVolumes) { if(Each.Grid == InGrid.Hash && Each.Grid != 0) { State = &Each; break; } }
            if(!State)
            {
                for(GIVolumeState& Each : GIVolumes)
                {
                    if(&Each == InKeep) { continue; }
                    if(!State || Each.LastLit < State->LastLit) { State = &Each; }
                }
                if(!State) { return nullptr; }
                DDGIResources Kept = State->Resources;   // reused, resized for this grid
                *State = GIVolumeState();
                State->Resources = Kept;
                State->Grid      = InGrid.Hash;
            }
            if(!InContext.EnsureDDGIResources(State->Resources, InGrid.Total))
            {
                return nullptr;
            }
            State->LastLit = std::max(State->LastLit, InUse);
            return State;
        }

        enum class EGIWork : uint8_t { None, Faces, Relight };

        /* What volume InState (InGrid) wants this frame: faces until every probe has been drawn
         * (and, with StaticCaptures, until it converges or its cache is full), then relights
         * while its lighting's rounds are not all in, else nothing. */
        EGIWork PlanVolume(RenderContext& InContext, GIVolumeState& InState, const GIGrid& InGrid, const bool bInLog)
        {
            const uint32_t PerFrame = uint32_t(std::clamp<int>(GRenderGIProbesPerFrame.GetInt(), 0, 64));
            if(!bBudgeted && PerFrame == 0)
            {
                return EGIWork::None;
            }
            if(!GRenderStaticCaptures.GetBool())
            {
                return EGIWork::Faces;
            }
            const uint64_t Geometry  = SceneSignature(InContext, InGrid.Position, double(InGrid.FarClip) + double(InGrid.Diagonal) * 0.5,
                                                      GICullRatio(), GRenderGICaptureStaticOnly.GetBool());
            const uint64_t Signature = FrameLighting * 31u + Geometry;
            const int64_t  Now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if(InState.Convergence.Observe(Signature))
            {
                InState.ChangedNs   = Now;
                InState.bLoggedDone = false;
            }
            const uint32_t Rounds = uint32_t(std::max(GRenderGIConvergeRounds.GetInt(), 1));

            /* Relighting: the geometry the cache holds; once every probe's faces are in it, the
             * lighting's rounds are relights. */
            const bool bRelight = GRenderGIRelight.GetBool() && GRenderSimpleCaptures.GetBool() && GRenderGICaptureStaticOnly.GetBool()
                               && InGrid.Total <= uint32_t(std::max(GRenderGIRelightMaxProbes.GetInt(), 0));
            InState.bCache = bRelight && InContext.EnsureDDGICache(InState.Resources, InGrid.Total);
            if(!InState.bGeometry || InState.Geometry != Geometry)
            {
                InState.Geometry  = Geometry;
                InState.bGeometry = true;
                InState.Stored    = 0;
            }
            if(InState.bCache && InState.bReady && InState.Stored >= InGrid.Total)
            {
                if(!InState.Relit.bKnown)
                {
                    InState.Relit.Observe(FrameLighting);
                    InState.Relit.Updated(InGrid.Total);   // the round that filled the cache lit them once
                }
                else if(InState.Relit.Observe(FrameLighting))
                {
                    InState.ChangedNs   = Now;
                    InState.bLoggedDone = false;
                }
                if(!InState.Relit.IsConverged(InGrid.Total, Rounds))
                {
                    return EGIWork::Relight;
                }
                if(bInLog && !InState.bLoggedDone && GRenderCaptureLog.GetBool())
                {
                    RENDERCORE_LOG("captures: GI volume of " + std::to_string(InGrid.Total) + " probes relit, converged "
                                   + std::to_string(double(Now - InState.ChangedNs) * 1e-9) + " s after its lighting last changed");
                }
                InState.bLoggedDone = bInLog || InState.bLoggedDone;
                return EGIWork::None;
            }
            InState.Relit.Reset();
            if(InState.bReady && InState.Convergence.IsConverged(InGrid.Total, Rounds)
               && (!InState.bCache || InState.Stored >= InGrid.Total))
            {
                if(bInLog && !InState.bLoggedDone && GRenderCaptureLog.GetBool())
                {
                    RENDERCORE_LOG("captures: GI volume of " + std::to_string(InGrid.Total) + " probes converged "
                                   + std::to_string(double(Now - InState.ChangedNs) * 1e-9) + " s after what it sees last changed");
                }
                InState.bLoggedDone = bInLog || InState.bLoggedDone;
                return EGIWork::None;
            }
            return EGIWork::Faces;
        }

        /* The volume nearest the camera: its grid and kept state, and what it wants; when it wants
         * nothing, the next volume that wants faces fills in the background. False if nothing does. */
        bool PlanGlobalIllumination(RenderContext& InContext)
        {
            GIActive     = nullptr;
            GIBackground = nullptr;
            bGIWanted    = false;
            bGIRelight   = false;
            GITotal      = 0;
            ++GIFrame;
            if(!CaptureList.bGIVolume || !GRenderDynamicGI.GetBool() || !InContext.CanImageBasedLight()
               || !DDGIStages.Allocate(DDGI_FACE, DDGI_FACE))
            {
                return false;
            }
            GIActiveGrid = MakeGrid(CaptureList.GIVolume);
            GIActive     = StateFor(InContext, GIActiveGrid, nullptr, GIFrame);
            if(!GIActive)
            {
                return false;
            }
            GITotal = GIActiveGrid.Total;
            const auto AddRegion = [this](const GIGrid& InGrid)
            {
                /* The capture shadow maps hold the volume, a spacing round it. */
                const EngineCore::Vector3 Half(InGrid.Extent[0] + InGrid.Spacing[0], InGrid.Extent[1] + InGrid.Spacing[1],
                                               InGrid.Extent[2] + InGrid.Spacing[2]);
                CaptureRegion = GameCore::AABB::Union(CaptureRegion, GameCore::AABB(InGrid.Position - Half, InGrid.Position + Half));
            };
            AddRegion(GIActiveGrid);

            const EGIWork Work = PlanVolume(InContext, *GIActive, GIActiveGrid, true);
            bGIWanted  = Work != EGIWork::None;
            bGIRelight = Work == EGIWork::Relight;
            if(bGIWanted || !bBudgeted || !GRenderStaticCaptures.GetBool())
            {
                return bGIWanted;
            }

            /* Nothing to do here: the next volume wanting faces, in the background. */
            for(uint32_t Index = 0; Index < CaptureList.GIOtherCount && Index < RENDER_GI_BACKGROUND_MAX; ++Index)
            {
                const GIGrid Grid = MakeGrid(CaptureList.GIOthers[Index]);
                GIVolumeState* State = StateFor(InContext, Grid, GIActive, GIFrame - 1);
                if(State && PlanVolume(InContext, *State, Grid, false) == EGIWork::Faces)
                {
                    GIBackground     = State;
                    GIBackgroundGrid = Grid;
                    AddRegion(Grid);
                    return true;
                }
            }
            return false;
        }

        /* A GI probe face's smallest drawn size, radius over distance. */
        static double GICullRatio()
        {
            return GRenderSimpleCaptures.GetBool() ? double(std::max(GRenderCaptureCullPixels.GetFloat(), 0.0f)) / double(DDGI_FACE) : 0.0;
        }

        /* Volume InState's probes, round the grid -- face by face within InShare (budgeted), else
         * GIProbesPerFrame whole probes -- each face lit by the live lights, the sky and (once every
         * probe has been seen) the volume itself, so light bounces again each round. */
        void CaptureVolumeFaces(RenderContext& InContext, GIVolumeState& InState, const GIGrid& InGrid, const float InShareMs)
        {
            DDGIResources& DDGI = InState.Resources;
            float Header[4][4];
            MakeHeader(InGrid, DDGI, InState.bReady, Header);
            ApplyImageBasedLighting(InContext);
            InContext.SetDDGI(InState.bReady ? &DDGI : nullptr, Header);
            InContext.SetSkyBackground(true);
            const float    Units      = CaptureFaceUnits(DDGI_FACE);
            const uint32_t PerFrame   = uint32_t(std::clamp<int>(GRenderGIProbesPerFrame.GetInt(), 0, 64));
            const uint32_t FaceBudget = bBudgeted ? 6u * InGrid.Total : std::min(PerFrame, InGrid.Total) * 6u;
            for(uint32_t Done = 0; Done < FaceBudget; ++Done)
            {
                if(bBudgeted && !FaceFits(Units, InShareMs)) { break; }
                const uint32_t Probe = InState.Cursor;
                const uint32_t Cell[3] = { Probe % InGrid.Count[0], (Probe / InGrid.Count[0]) % InGrid.Count[1],
                                           Probe / (InGrid.Count[0] * InGrid.Count[1]) };
                SceneCaptureRequest Cube;
                Cube.Mode        = SCM_Omnidirectional;
                Cube.Position    = EngineCore::Vector3(InGrid.First[0] + float(Cell[0]) * InGrid.Spacing[0],
                                                       InGrid.First[1] + float(Cell[1]) * InGrid.Spacing[1],
                                                       InGrid.First[2] + float(Cell[2]) * InGrid.Spacing[2]);
                Cube.NearClip    = 1.0f;
                Cube.FarClip     = InGrid.FarClip;
                Cube.bShadows    = 1;
                Cube.CullRatio   = float(GICullRatio());
                Cube.bStaticOnly = GRenderSimpleCaptures.GetBool() && GRenderGICaptureStaticOnly.GetBool() ? 1 : 0;
                RenderView Faces[6];
                if(BuildCaptureViews(Cube, Views[0].View, DDGI_FACE, DDGI_FACE, Faces) != 6)
                {
                    break;
                }
                const uint32_t Face = InState.Face;
                SCOPED_DRAW_EVENT(GetCubeFaceName(Face));
                RenderCaptureFace(InContext, Cube, Faces[Face], DDGIStages);
                InContext.CopyDDGIFace(DDGIStages, DDGI, Face, InGrid.Longest);
                if(InState.bCache) { InContext.StoreDDGIFace(DDGIStages, DDGI, Probe, Face); }
                FrameUnits += Units;
                ++CaptureCounts.GIFaces;
                if(++InState.Face < 6) { continue; }

                /* Its sixth face: the strip into its tiles, and on round the grid. */
                InState.Face = 0;
                InContext.UpdateDDGIProbe(DDGI, Probe, InGrid.Longest);
                InState.Cursor = (InState.Cursor + 1) % InGrid.Total;
                InState.Convergence.Updated();
                if(InState.bCache) { InState.Stored = std::min(InState.Stored + 1, InGrid.Total); }
                if(++InState.Updated >= InGrid.Total) { InState.bReady = true; }
            }
            InContext.SetSkyBackground(false);
            InContext.SetView(Views[0].View);
        }

        /* The active volume's updates -- faces, or relights from its cache -- or the background
         * volume's faces; then the active volume lights the views. Leaves the context on the main view. */
        void CaptureGlobalIllumination(RenderContext& InContext, const float InShareMs)
        {
            if(!GIActive)
            {
                InContext.SetDDGI(nullptr, nullptr);
                return;
            }
            GIVolumeState& State = *GIActive;
            DDGIResources& DDGI  = State.Resources;
            const GIGrid&  Grid  = GIActiveGrid;
            float Header[4][4];

            if(bGIWanted && bGIRelight)
            {
                /* Relit from the cache: as many probes as fit, round the grid, one column of the cache at a time. */
                SCOPED_DRAW_EVENT("GIVolume");
                EnsureCaptureShadows(InContext);
                InContext.SetView(Views[0].View);
                ApplyImageBasedLighting(InContext);
                MakeHeader(Grid, DDGI, true, Header);
                InContext.SetDDGI(&DDGI, Header);
                /* What is left of the frame's milliseconds after its faces, at what a relit probe costs. */
                const float    Left   = bBudgeted ? std::max(InShareMs - FrameSpentMs(), 0.0f) : 0.0f;
                uint32_t       Count  = bBudgeted ? uint32_t(RelightBudget.GetAllowance(Left, 1.0f)) : Grid.Total;
                const uint32_t PerCol = std::max(DDGI.CachePerColumn, 1u);
                Count = std::min({ Count, Grid.Total - State.RelightCursor, PerCol - State.RelightCursor % PerCol });
                Count = std::max(Count, 1u);
                const auto Start = std::chrono::steady_clock::now();
                if(InContext.RelightDDGI(DDGI, State.RelightCursor, Count, Grid.Longest))
                {
                    State.Relit.Updated(Count);
                    FrameRelit += Count;
                }
                FrameCaptureCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
                State.RelightCursor = (State.RelightCursor + Count) % Grid.Total;
                InContext.SetView(Views[0].View);
            }
            else if(bGIWanted)
            {
                SCOPED_DRAW_EVENT("GIVolume");
                CaptureVolumeFaces(InContext, State, Grid, InShareMs);
            }
            else if(GIBackground)
            {
                SCOPED_DRAW_EVENT("GIVolume (background)");
                CaptureVolumeFaces(InContext, *GIBackground, GIBackgroundGrid, InShareMs);
            }
            MakeHeader(Grid, DDGI, State.bReady, Header);
            InContext.SetDDGI(State.bReady ? &DDGI : nullptr, Header);
        }

        /* Targets.Final as of the last Render(), for PresentToWindow on the UI
         * thread. A target is resized in place, never replaced, while the
         * scene lives, so the pointer stays good; GDI skips a present while a
         * resize is in progress. */
        std::atomic<GDIRenderTarget*> PresentTarget{nullptr};

        bool bReportsStats = true;
    };

    RenderScene::RenderScene() : P(new Impl())
    {
        GLiveScenes.push_back(this);
    }

    RenderScene::~RenderScene()
    {
        GLiveScenes.erase(std::remove(GLiveScenes.begin(), GLiveScenes.end(), this), GLiveScenes.end());
        if(!P->Proxies.empty())
        {
            RENDERCORE_LOG_WARN("RenderScene destroyed with proxies still present; "
                                 "somebody skipped RemoveProxy -- see ReleaseAll")
        }

        /* Belt and braces: ReleaseAll() is the ordinary path and already
         * did this, but a scene destroyed without it (a test, an early
         * failure) must still not leak a GPU image. Idempotent. */
        P->ReleaseViews();
        P->ProbeStages.Release();
        P->SkyStages.Release();
        P->DDGIStages.Release();
        for(GDIRenderTarget** Each : { &P->IBL.Atlas, &P->IBL.Strip })
        {
            if(*Each) { GDIReleaseRenderTarget(*Each); delete *Each; *Each = nullptr; }
        }
        for(Impl::GIVolumeState& Volume : P->GIVolumes)
        {
            for(GDIRenderTarget** Each : { &Volume.Resources.Irradiance, &Volume.Resources.Distance, &Volume.Resources.Strip,
                                           &Volume.Resources.Cache[0], &Volume.Resources.Cache[1], &Volume.Resources.Radiance })
            {
                if(*Each) { GDIReleaseRenderTarget(*Each); delete *Each; *Each = nullptr; }
            }
        }

        delete P;
    }

    void RenderScene::AddProxy(RenderProxy* InProxy, RenderContext& InContext)
    {
        if(!InProxy)
        {
            return;
        }

        InProxy->SceneIndex = int32_t(P->Proxies.size());
        P->Proxies.push_back(InProxy);

        InProxy->InitRenderResources(InContext);
    }

    void RenderScene::RemoveProxy(RenderProxy* InProxy, RenderContext& InContext)
    {
        if(!InProxy || InProxy->SceneIndex < 0)
        {
            return;
        }

        InProxy->ReleaseRenderResources(InContext);
        P->StaticLists.erase(InProxy);

        const size_t Index    = size_t(InProxy->SceneIndex);
        const size_t LastSlot = P->Proxies.size() - 1;

        if(Index != LastSlot)
        {
            /* Swap-and-pop: move the last live proxy into the hole, exactly
             * ComponentPool::Remove's own POD swap-remove -- except here it
             * is a pointer swap, not a memcpy, since a RenderProxy is a
             * polymorphic object rather than trivially copyable bytes. */
            RenderProxy* Moved = P->Proxies[LastSlot];
            P->Proxies[Index]  = Moved;
            Moved->SceneIndex  = int32_t(Index);
        }
        P->Proxies.pop_back();

        InProxy->SceneIndex = -1;

        EngineCore::Memory::DestroyObject(InProxy);
        EngineCore::Memory::Free(InProxy);
    }

    void RenderScene::SetView(const RenderView& InView)
    {
        P->Views[0].View = InView;
        P->Views[0].View.ViewIndex = 0;
        P->ViewCount   = 1;
        P->Sites.Count = 0;
    }

    void RenderScene::SetViews(const RenderView* InViews, const RenderLighting* InLightings, const RenderPostProcess* InPostProcesses,
                               const uint32_t InCount, const EngineCore::Size InWindowPixels, const RenderViewSites* InSites)
    {
        P->ViewCount    = std::clamp<uint32_t>(InCount, 1, RENDER_VIEWS_MAX);
        P->WindowPixels = InWindowPixels;
        P->Sites        = InSites ? *InSites : RenderViewSites();
        if(P->Sites.Count < 2 || P->ViewCount < 2) { P->Sites.Count = 0; }
        P->Sites.Count  = std::min(P->Sites.Count, P->ViewCount);
        for(uint32_t Index = 0; Index < P->ViewCount; ++Index)
        {
            SceneViewState& View = P->Views[Index];
            View.View           = InViews[Index];
            View.View.ViewIndex = Index;
            View.Lighting       = InLightings[Index];
            View.PostProcess    = InPostProcesses[Index];
        }
    }

    void RenderScene::SetClearColor(const EngineCore::Color InColor)
    {
        P->ClearColor     = InColor;
        P->ClearBottom    = InColor;
        P->bClearGradient = false;
    }

    void RenderScene::SetCaptures(const SceneCaptureList& InCaptures)
    {
        P->CaptureList = InCaptures;
    }

    void RenderScene::SetClearGradient(const EngineCore::Color InTop, const EngineCore::Color InBottom)
    {
        P->ClearColor     = InTop;
        P->ClearBottom    = InBottom;
        P->bClearGradient = true;
    }

    void RenderScene::SetLighting(const RenderLighting& InLighting)
    {
        P->Views[0].Lighting = InLighting;
    }

    void RenderScene::SetViewMode(const ERenderViewMode InMode, const std::string& InTarget)
    {
        P->ViewMode   = InMode;
        P->ViewTarget = InTarget;
    }

    std::vector<std::string> RenderScene::GetViewableTargets() const
    {
        std::lock_guard<std::mutex> Guard(P->TargetsLock);
        return P->TargetNames;
    }

    void RenderScene::SetPostProcess(const RenderPostProcess& InPostProcess)
    {
        P->Views[0].PostProcess = InPostProcess;
    }

    void RenderScene::SetUIDrawList(void* InBlock, const size_t InBytes)
    {
        P->UI.Set(InBlock, InBytes);
    }

#if WITH_EDITOR
    void RenderScene::SetDebugLines(const DebugLine* InLines, const size_t InCount)
    {
        P->DebugLines.assign(InLines, InLines + InCount);
    }

    void RenderScene::AddMaterialPreview(const MaterialPreviewRequest& InRequest)
    {
        P->Previews.push_back(InRequest);
    }

    void RenderScene::AddCapture(const uint64_t InRequest, MaterialPreviewMailbox* InMailbox, const bool bInWithDebugLines)
    {
        (bInWithDebugLines ? P->CapturesWithLines : P->Captures).emplace_back(InRequest, InMailbox);
    }

    void RenderScene::AddTargetReadback(const uint64_t InRequest, const std::string& InPath, MaterialPreviewMailbox* InMailbox)
    {
        P->TargetReadbacks.push_back({ InRequest, InPath, InMailbox });
    }
#endif

    void RenderScene::SetCustomPassWorld(const void* InWorld)
    {
        P->PassWorld = InWorld;
    }

    void RenderScene::Render(RenderContext& InContext)
    {
        /* Pushed here, not in SetView(), so GetView() is always in step with
         * what Render() is about to draw against -- SetView() merely records
         * what the NEXT Render() will use; a caller changing its mind
         * between SetView() and Render() (unlikely, but not forbidden) never
         * leaves GetView() reporting a view nothing has drawn against yet. */
        SceneViewState& Main = P->Views[0];
        const uint32_t  ViewCount = P->ViewCount;
        const bool      bSplit    = ViewCount > 1;
        InContext.SetView(Main.View);
        InContext.SetLighting(Main.Lighting);
        InContext.UpdateTextureStreaming();   // the frame's first scene: last frame's demand to loads and commits
        P->ApplyImageBasedLighting(InContext);   // last frame's, for the captures below; brought up to date before the scene

        /* Created on first use and resized with the viewport -- see Impl's
         * own comment on why a scene owns these at all. Early-outs on every
         * frame whose viewport has not changed, which is nearly all of them. */
        for(uint32_t Index = 0; Index < ViewCount; ++Index)
        {
            SceneViewState& View = P->Views[Index];
            if(View.View.ViewportPixels.Width > 0 && View.View.ViewportPixels.Height > 0)
            {
                View.Targets.Allocate(uint(View.View.ViewportPixels.Width), uint(View.View.ViewportPixels.Height));
            }
        }
        if(bSplit && P->WindowPixels.Width > 0 && P->WindowPixels.Height > 0)
        {
            const uint Width = uint(P->WindowPixels.Width), Height = uint(P->WindowPixels.Height);
            if(!P->Composite)
            {
                P->Composite = GDICreateRenderTarget("RenderCore.Scene.Composite", Width, Height);
            }
            else if(P->Composite->GetWidth() != Width || P->Composite->GetHeight() != Height)
            {
                P->Composite->Resize(Width, Height);
            }
        }

        GDIBeginRendering();

        /* The frame's root draw event, ended just before EndRendering submits:
         * everything below nests under it (RENDERPIPELINE.md, "Draw events"). */
        InContext.BeginDrawEvent("Scene");

        /* "stat frame": the GPU times of the GDI block the frames in flight
         * back, to the scene that wrote them (they are complete only now),
         * and this block's own, if shown. */
        const StatUnitSnapshot Stats = GetStatUnit();
        {
            /* Timed whenever there is a capture budget, whose Captures span it learns from. */
            float GpuMs[RGT_Count];
            uint64_t Serials[2] = {};
            const bool bBudget = GRenderCaptureBudgetMs.GetFloat() > 0.0f;
            const void* Owner = InContext.BeginTimedBlock(this, Stats.bShowFrame || bBudget, GpuMs, Serials);
            P->BudgetSerial = Serials[0];
            for(RenderScene* Each : GLiveScenes)
            {
                if(Each != Owner) { continue; }
                if(Stats.bShowFrame) { Each->P->AddGpuTimes(GpuMs); }
                Each->P->Budget.Measured(Serials[1], GpuMs[RGT_SceneCapture]);
                Each->P->RelightBudget.Measured(Serials[1], GpuMs[RGT_SceneCapture]);
            }
            if(!Stats.bShowFrame && P->GpuWindowStartNs != 0)
            {
                P->ClearGpuTimes();
            }
        }

        /* GPU work each proxy needs once a frame (RenderProxy::Simulate) --
         * particles -- outside any target, against the scene's own view;
         * after the timed block opens, so "stat frame" times them. */
        {
            SCOPED_DRAW_EVENT("Simulate");
            InContext.WriteGpuTimestamp(RGT_Particles, false);   // 0 with no emitters, not "-"
            for(RenderProxy* Proxy : P->Proxies)
            {
                Proxy->Simulate(InContext);
            }
            InContext.WriteGpuTimestamp(RGT_Particles, true);
        }

#if WITH_EDITOR
        /* Previews FIRST, before the scene's own stages, so the scene's final
         * target is the active one at EndRendering and the present never
         * shows a preview. */
        InContext.DeliverMaterialPreviews();
        for(const MaterialPreviewRequest& Each : P->Previews)
        {
            SCOPED_DRAW_EVENT("MaterialPreview");
            InContext.RenderMaterialPreview(Each);
        }
        P->Previews.clear();
#endif

        /*
         * PrePass: queue each visible proxy's draws in their stages' draw
         * policies (RENDERPIPELINE.md). The stages themselves run below, AFTER
         * BeginRendering, never before: VulkanDynamicGDI::SetRenderTarget bails
         * out entirely when CurrentCmd is null (outside a BeginRendering/
         * EndRendering pair), so a bind placed above it would be a silent no-op.
         */
        /* Its World's custom render passes (CustomRenderPass.h), for the
         * captures and the view below; never the previews above. */
        InContext.SetCustomPassWorld(P->PassWorld);

        /*
         * SCENE CAPTURES (RENDERPIPELINE.md), before the scene's own pass so it
         * samples this frame's: each probe's views -- six cube faces, or the
         * camera reflected in a mirror -- through its own stage targets, each
         * copied into its render target. Shadows are drawn for each view; the
         * post process stack is the main view's alone.
         */
        bool bCaptured = false;
        InContext.WriteGpuTimestamp(RGT_SceneCapture, false);   // "stat frame": 0 with no captures, not "-"
        for(uint32_t Index = 0; Index < P->CaptureList.Count && Index < RENDER_CAPTURES_MAX; ++Index)
        {
            const SceneCaptureRequest& Request = P->CaptureList.Requests[Index];
            const bool bCube = Request.Mode == SCM_Omnidirectional;
            EngineCore::Color Clear;
            SceneTargets* Stages = InContext.BeginTargetCapture(Request.Target, Request.Resolution,
                                                                bCube ? ERenderTargetKind::Cube : ERenderTargetKind::Tex2D, Clear);
            if(!Stages)
            {
                continue;
            }
            SCOPED_DRAW_EVENT("SceneCapture " + std::string(Request.Target));
            RenderView Views[6];
            const uint32_t ViewCount = BuildCaptureViews(Request, Main.View, Stages->Width, Stages->Height, Views);
            for(uint32_t Face = 0; Face < ViewCount; ++Face)
            {
                SCOPED_DRAW_EVENT(bCube ? GetCubeFaceName(Face) : "Mirror");
                InContext.SetView(Views[Face]);
                uint64_t Visible = 0, Culled = 0, Static = 0;
                P->QueueDraws(InContext, Views[Face], Main.Lighting, &Request, Visible, Culled, Static);
                InContext.RenderSceneStages(*Stages, Clear);
                InContext.FinishTargetFace(Request.Target, Face);
                bCaptured = true;
            }
        }
        P->CaptureList.Count = 0;
        if(bCaptured)
        {
            InContext.SetView(Main.View);
        }
#if WITH_EDITOR
        for(const Impl::TargetReadback& Each : P->TargetReadbacks)
        {
            EngineCore::Image Pixels;
            const bool bRead = InContext.ReadRenderTarget(Each.Path.c_str(), Pixels);
            if(Each.Mailbox)
            {
                Each.Mailbox->Deliver(Each.Request, std::move(Pixels),
                                      bRead ? std::string() : "render target '" + Each.Path + "' has nothing captured or baked");
            }
        }
        P->TargetReadbacks.clear();
#endif

        /* IMAGE-BASED LIGHTING: the sky block and the reflection probes, before the scene lights by them.
         * Inside the Captures span: a reflection probe's capture is six views of the scene, as costly
         * as a Scene Capture Probe's, and a probe left capturing every frame should show in "stat frame". */
        {
            SCOPED_DRAW_EVENT("ImageBasedLighting");
            P->UpdateCaptures(InContext);
        }
        InContext.WriteGpuTimestamp(RGT_SceneCapture, true);

        /* SPLITSCREEN's dynamic split: a view appearing this frame starts where view 0 left off --
         * the same camera, by the split's own rule -- so its TAA history, exposure and motion carry
         * over, with no ghost or exposure pop as it splits off. */
        if(P->Sites.Count >= 2 && Main.bDrawnLastFrame)
        {
            for(uint32_t Index = 1; Index < ViewCount; ++Index)
            {
                SceneViewState& View = P->Views[Index];
                if(View.bDrawnLastFrame) { continue; }
                InContext.SeedViewHistory(View.Targets, Main.Targets);
                View.PrevViewProjection = Main.PrevViewProjection;
                View.PrevViewport       = Main.PrevViewport;
                View.bHasPrevView       = Main.bHasPrevView;
                View.JitterIndex        = Main.JitterIndex;
            }
        }
        for(uint32_t Index = 0; Index < RENDER_VIEWS_MAX; ++Index)
        {
            P->Views[Index].bDrawnLastFrame = Index < ViewCount;
        }

        /* The views (SceneViewState), each into its own targets; the Render
         * Target view mode's pick over each. Each is timed: "stat frame"'s
         * spans and the stage CPU times are the frame's, summed over them. */
        float    CpuMs[RGT_Count];
        int32_t  Draws[RGT_Count];
        uint64_t CulledCount = 0, StaticCount = 0, Occluded = 0, Queries = 0;
        float    QueueMs = 0.0f;
        for(uint32_t Index = 0; Index < ViewCount; ++Index)
        {
            SceneViewState& View = P->Views[Index];
            SCOPED_DRAW_EVENT(bSplit ? "View " + std::to_string(Index) : std::string("View"));
            P->RenderSceneView(InContext, View);
            QueueMs += View.QueueMs;
            if(P->ViewMode == ERenderViewMode::RenderTarget)
            {
                InContext.SetView(View.View);
                InContext.ShowTarget(View.Targets, P->ViewTarget);
            }
            CulledCount += View.CulledCount;
            StaticCount += View.StaticCount;
            if(View.Occlusion.Current) { Occluded += View.Occlusion.LastOccluded; Queries += View.Occlusion.LastQueries; }
        }
        InContext.EndTimedViews();
        InContext.GetStageCpuTimes(CpuMs, Draws);
        InContext.SetCustomPassWorld(nullptr);
        P->PassWorld = nullptr;   // set again by the next frame's SetCustomPassWorld, if it has a World
        P->AddCpuTimes(CpuMs, Draws, QueueMs);
        InContext.SetView(Main.View);   // unjittered again, for what draws over the picture

        /*
         * SPLITSCREEN: every view's final colour at its place in the window,
         * then the UI and overlays over that. One view: its own final colour
         * is the picture, as it always was.
         */
        GDIRenderTarget* Output = Main.Targets.Final;
        uint OutputWidth = Main.Targets.Width, OutputHeight = Main.Targets.Height;
        if(bSplit && P->Composite)
        {
            GDIRenderTarget* Sources[RENDER_VIEWS_MAX] = {};
            RenderViewRect   Rects[RENDER_VIEWS_MAX];
            for(uint32_t Index = 0; Index < ViewCount; ++Index)
            {
                const RenderView& View = P->Views[Index].View;
                Sources[Index] = P->Views[Index].Targets.Final;
                Rects[Index]   = { View.ViewportX, View.ViewportY, View.ViewportPixels.Width, View.ViewportPixels.Height };
            }
            if(P->Sites.Count >= 2)
            {
                InContext.CompositeViewSites(P->Composite, Sources, P->Sites);
            }
            else
            {
                InContext.CompositeViews(P->Composite, Sources, Rects, ViewCount);
            }
            Output       = P->Composite;
            OutputWidth  = P->Composite->GetWidth();
            OutputHeight = P->Composite->GetHeight();
        }

        /* The targets the main view drew, for the view mode's list. */
        {
            std::vector<std::string> Names;
            InContext.ListViewableTargets(Main.Targets, Names);
            std::lock_guard<std::mutex> Guard(P->TargetsLock);
            if(Names != P->TargetNames) { P->TargetNames = std::move(Names); }
        }

        /* The UI (RENDERPIPELINE.md, "UI"): over the post-processed picture,
         * before the readback so a capture shows it, and under "stat unit". */
        InContext.DrawUI(P->UI, OutputWidth, OutputHeight);

#if WITH_EDITOR
        /* Read back before the overlay; the readback flushes and resumes the frame. */
        const auto DeliverCaptures = [Output](std::vector<std::pair<uint64_t, MaterialPreviewMailbox*>>& InCaptures)
        {
            for(const auto& Each : InCaptures)
            {
                EngineCore::Image Pixels;
                if(Output) { Pixels = Output->ToImage(); }
                if(Each.second)
                {
                    Each.second->Deliver(Each.first, std::move(Pixels),
                                         Output ? std::string() : std::string("the scene has no render target"));
                }
            }
            InCaptures.clear();
        };
        DeliverCaptures(P->Captures);
#endif

        if(P->bReportsStats)
        {
            SetSceneStats(uint64_t(P->Proxies.size()), CulledCount);
            SetStaticDrawStats(StaticCount, P->StaticBuilds);
            SetOcclusionStats(Occluded, Queries);
        }

#if WITH_EDITOR
        /* Tool overlay over everything, unculled; consumed each frame. */
        if(!P->DebugLines.empty())
        {
            /* No depth test: an outline or a gizmo shows through what it marks. */
            GDISetDepthState(IDepthStateDesc());
            SCOPED_DRAW_EVENT("DebugLines");
            if(bSplit)   // drawn through the main view, so over its rectangle
            {
                GDISetViewport(uint(std::max(Main.View.ViewportX, 0)), uint(std::max(Main.View.ViewportY, 0)),
                               uint(Main.View.ViewportPixels.Width), uint(Main.View.ViewportPixels.Height));
            }
            for(const DebugLine& Line : P->DebugLines)
            {
                InContext.DrawLine(Line.Start, Line.End, Line.Color, Line.ThicknessPixels);
            }
            P->DebugLines.clear();
        }
        /* And the captures that want the overlay. */
        DeliverCaptures(P->CapturesWithLines);
#endif

        /* "stat unit" (Stats.Unit) and "stat frame" (Stats.Frame) over
         * everything, in one panel, after the readback above so a capture
         * never shows them. */
        if(Stats.bShow || Stats.bShowFrame || !Stats.Extra.empty())
        {
            std::string Text = Stats.bShow ? FormatStatUnit(Stats) : std::string();
            if(Stats.bShowFrame)
            {
                Text += (Text.empty() ? "" : "\n\n") + FormatStatFrame(P->GpuMs, P->CpuMs, P->StageDraws, P->QueueMs,
                                                                         InContext.CanTimeGpu());
            }
            if(!Stats.Extra.empty())
            {
                /* Other modules' sections (Renderer::SetStatText), below. */
                Text += (Text.empty() ? "" : "\n\n") + Stats.Extra;
            }
            InContext.DrawStatUnit(OutputWidth, OutputHeight, Text);
        }

        InContext.EndDrawEvent();   // "Scene"
        GDIEndRendering();

        /* Drawn: what each proxy is now is what it was, for the next frame's velocity. */
        for(RenderProxy* Proxy : P->Proxies)
        {
            Proxy->EndFrameMotion();
        }

        /* After EndRendering: the final colour is now in its presentable state. */
        P->PresentTarget.store(Output, std::memory_order_release);
    }

    void RenderScene::SetReportsStats(const bool bInReports)
    {
        P->bReportsStats = bInReports;
    }

    void RenderScene::PresentToWindow(const EngineCore::WindowHandle InWindow) const
    {
        GDIRenderTarget* Target = P->PresentTarget.load(std::memory_order_acquire);
        if(Target && GDynamicGDI)
        {
            GDynamicGDI->PresentRenderTargetToWindow(InWindow, Target);
        }
    }

    size_t RenderScene::GetProxyCount() const
    {
        return P->Proxies.size();
    }

    void RenderScene::ReleaseAll(RenderContext& InContext)
    {
        while(!P->Proxies.empty())
        {
            /* RemoveProxy pops from the back on the LastSlot path, so walking
             * from the back means every removal here is the cheap case --
             * matching World's own destructor style of tearing down without
             * caring which order, only that every one is actually freed. */
            RemoveProxy(P->Proxies.back(), InContext);
        }

        /* AFTER the proxies, not before: a proxy's own ReleaseRenderResources
         * may still reference the target it was drawing into. Runs on the
         * render thread, like the rest of this class, and before
         * Renderer::Shutdown stops that thread -- which is what keeps the
         * GPU image's destruction on the thread that owns the context. */
        P->PresentTarget.store(nullptr, std::memory_order_release);
        P->UI.Release();
        P->ReleaseViews();
        P->ProbeStages.Release();
        P->SkyStages.Release();
        P->DDGIStages.Release();
        InContext.ReleaseIBLResources(P->IBL);
        P->ReleaseGIVolumes(InContext);
        P->bSkyValid = false;
        for(Impl::ProbeSlot& Each : P->Slots) { Each = Impl::ProbeSlot(); }
    }
}
