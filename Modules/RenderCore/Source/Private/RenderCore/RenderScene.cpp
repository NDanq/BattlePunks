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
    }

    struct RenderScene::Impl
    {
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

        /*
         * OCCLUSION CULLING (RENDERPIPELINE.md): the main view's query sets,
         * one per frame the GPU may run behind (GDIGetFramesInFlight), taken
         * in turn: a set's answers are read when it comes round again, its
         * round finished by then without waiting for it. Each is made on
         * first use and remade larger when a frame wanted more queries than
         * it holds. Then the answers read, and what the last frame did, for
         * RenderStats.
         */
        GDIOcclusionQueries* Occlusion[RenderProxy::OCCLUSION_SETS_MAX] = {};
        uint32_t             OcclusionSets    = 1;
        uint32_t             OcclusionAt      = 0;   // this frame's set
        uint32_t             OcclusionWanted  = 0;
        std::vector<uint8_t> OcclusionAnswers;
        uint64_t             LastOccluded     = 0;
        uint64_t             LastQueries      = 0;

        /* The set the main view's queries go in this frame; null if occlusion
         * culling is off, the backend has no queries, or InView is not a
         * perspective one (an orthographic view has no near plane to judge
         * nearness by, and nothing it draws is usually hidden). */
        GDIOcclusionQueries* PrepareOcclusion(const RenderView& InView)
        {
            if(!GRenderOcclusionCulling.GetBool() || !GDISupportsOcclusionQueries() || !IsPerspective(InView))
            {
                ReleaseOcclusion();
                return nullptr;
            }
            const uint32_t Sets = std::clamp<uint32_t>(GDIGetFramesInFlight(), 1, RenderProxy::OCCLUSION_SETS_MAX);
            if(Sets != OcclusionSets)
            {
                ReleaseOcclusion();
                OcclusionSets = Sets;
            }
            OcclusionAt = (OcclusionAt + 1) % OcclusionSets;
            GDIOcclusionQueries*& Set = Occlusion[OcclusionAt];
            if(!Set || OcclusionWanted > Set->GetCapacity())
            {
                uint32_t Capacity = Set ? Set->GetCapacity() : OCCLUSION_QUERIES_MIN;
                while(Capacity < OcclusionWanted && Capacity < OCCLUSION_QUERIES_MAX) { Capacity *= 2; }
                if(Set) { Set->Release(); }   // its answers with it: a frame without CPU culling
                Set = GDICreateOcclusionQueries(Capacity);
            }
            return Set;
        }

        void ReleaseOcclusion()
        {
            for(GDIOcclusionQueries*& Set : Occlusion)
            {
                if(Set) { Set->Release(); }
                Set = nullptr;
            }
            OcclusionAt  = 0;
            LastOccluded = LastQueries = 0;
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
         * With InOcclusion (the main view's; RENDERPIPELINE.md, "Occlusion
         * culling"): each visible proxy but an occluder has its box queried
         * against the occluders' depth -- unless it is so near the eye the
         * near plane may cut it -- and its draws predicated on the answer,
         * so the GPU skips them if it is hidden this frame. One the last
         * answer read found hidden is not drawn at all; its query alone
         * says when it shows again, a frame late. Occluders queue their
         * depth for the PrePass.
         */
        void QueueDraws(RenderContext& InContext, const RenderView& InView, const SceneCaptureRequest* InCapture,
                        uint64_t& OutVisible, uint64_t& OutCulled, uint64_t& OutStatic,
                        GDIOcclusionQueries* InOcclusion = nullptr)
        {
            InContext.BeginSceneDraws();

            /* This set's last round's answers -- complete, as its block has
             * finished (FramesInFlight blocks back) -- then this round's queries. Without answers nothing is hidden on
             * the CPU; the GPU still skips what this frame's find hidden. */
            const bool bOcclusion = InOcclusion != nullptr;
            const bool bAnswers   = bOcclusion && GDIReadOcclusionResults(InOcclusion, OcclusionAnswers);
            const bool bPredicate = bOcclusion && InContext.CanPredicateOcclusion();
            const double NearReach = bOcclusion ? NearPlaneReach(InView) * 1.5 : 0.0;
            if(bOcclusion) { InContext.SetOcclusionQueries(InOcclusion); }
            /* Nothing to test against without an occluder in view: no queries. */
            const bool bQueries = bOcclusion && std::any_of(Proxies.begin(), Proxies.end(), [&InView](const RenderProxy* InProxy)
            {
                return InProxy->IsVisible() && InProxy->IsOccluder()
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
            const double LightLength = Lighting.InsetLightDirection.GetLength();
            const bool   bInsetLight = Lighting.bInsetLight && LightLength > 1e-9 && !InView.Bounds.IsEmpty();
            const EngineCore::Vector3 InsetSweep = bInsetLight
                ? Lighting.InsetLightDirection * ((InView.Bounds.Max - InView.Bounds.Min).GetLength() / LightLength)
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
                int32_t* Query     = bOcclusion ? &Proxy->OcclusionQueries[OcclusionAt] : nullptr;
                if(bOcclusion)
                {
                    LastQuery        = *Query;
                    *Query           = -1;
                    Proxy->bOccluded = false;
                }
                if(!Proxy->IsVisible() || Excluded(Proxy))
                {
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
                                         && size_t(LastQuery) < OcclusionAnswers.size() && OcclusionAnswers[size_t(LastQuery)] == 0;
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
                    InContext.SetDrawOcclusion(-1);
                    InContext.SetDrawStreamingScale(0.0f);
                    ++VisibleCount;
                }
                if(bQueries && Proxy->IsOccluder())
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
                    if(!Proxy->IsVisible() || Excluded(Proxy) || (bSunView && !Inset.empty() && HasInset(Proxy)))
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
                OcclusionWanted = OcclusionWantedNow;
                LastOccluded    = OccludedCount;
                LastQueries     = std::min<uint64_t>(OcclusionWantedNow, InOcclusion->GetCapacity());
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
        RenderView                View;
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
        RenderLighting            Lighting;
        RenderPostProcess         PostProcess;

        /* Last frame's Projection * View and viewport, for motion blur; none
         * on the first frame or after a resize (a jump, not motion). */
        EngineCore::Matrix4x4     PrevViewProjection;
        EngineCore::Size          PrevViewport;
        bool                      bHasPrevView = false;

        /* TAA's jitter: where in its 8-frame Halton(2, 3) cycle the main view is. */
        uint32_t                  JitterIndex = 0;

        /*
         * THE SCENE'S RENDER TARGETS, and the whole reason anything reaches
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
        SceneTargets Targets{"RenderCore.Scene"};

        /*
         * IMAGE-BASED LIGHTING (RENDERPIPELINE.md, "Image-based lighting"): the
         * probe atlas and cube strip; the stage targets a reflection probe's
         * faces render through; which probe each of the atlas's four probe
         * blocks holds (by entity key) and whether its capture is in it; and
         * whether the sky block is, for which sky. UpdateImageBasedLighting
         * keeps them; ApplyImageBasedLighting hands the context what the next
         * stages light with.
         */
        IBLResources IBL;
        SceneTargets ProbeStages{"RenderCore.ProbeStages"};
        struct ProbeSlot { uint64_t Key = 0; bool bValid = false; bool bPending = false; };
        ProbeSlot    Slots[RENDER_REFLECTION_PROBES_MAX];
        bool         bSkyValid = false;
        uint64_t     SkyHash   = 0;
        float        ProbeData[RENDER_REFLECTION_PROBES_MAX * 3][4] = {};
        uint32_t     ProbeDataCount = 0;

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

        /* A reflection probe's six faces, HDR, into the strip, prefiltered into atlas block InBlock. */
        bool CaptureReflectionProbe(RenderContext& InContext, const ReflectionProbeRequest& InProbe, const uint32_t InBlock)
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
            RenderView Views[6];
            const uint32_t ViewCount = BuildCaptureViews(Cube, View, Face, Face, Views);

            /* Lit by the sky alone (no probe sees another, or itself), the sky behind. */
            InContext.SetImageBasedLighting(&IBL, nullptr, 0, bSkyValid);
            InContext.SetSkyBackground(true);
            bool bCopied = ViewCount == 6;
            for(uint32_t Index = 0; Index < ViewCount; ++Index)
            {
                SCOPED_DRAW_EVENT(GetCubeFaceName(Index));
                InContext.SetView(Views[Index]);
                uint64_t Visible = 0, Culled = 0, Static = 0;
                QueueDraws(InContext, Views[Index], &Cube, Visible, Culled, Static);
                InContext.RenderSceneStages(ProbeStages, ClearColor);
                bCopied = InContext.CopyCaptureToStrip(ProbeStages, IBL, Index) && bCopied;
            }
            InContext.SetSkyBackground(false);
            return bCopied && InContext.PrefilterIBLBlock(IBL, InBlock);
        }

        /* The sky block when the sky changed, then the probes in view: a block each, captured when due
         * or new to it -- two a frame at most, the rest next frame. Leaves the context on View. */
        void UpdateImageBasedLighting(RenderContext& InContext)
        {
            ProbeDataCount = 0;
            if(!InContext.CanImageBasedLight())
            {
                bSkyValid = false;
                ApplyImageBasedLighting(InContext);
                return;
            }
            const uint64_t Hash = SkyHashOf();
            if(!bSkyValid || Hash != SkyHash)
            {
                SCOPED_DRAW_EVENT("SkyLight");
                bSkyValid = InContext.EnsureIBLResources(IBL, 128) && InContext.RenderSkyToStrip(IBL)
                         && InContext.PrefilterIBLBlock(IBL, 4);
                SkyHash = Hash;
            }

            const uint32_t Count = std::min<uint32_t>(CaptureList.ProbeCount, RENDER_REFLECTION_PROBES_MAX);
            const auto Listed = [&](const uint64_t InKey)
            {
                for(uint32_t Index = 0; Index < Count; ++Index) { if(CaptureList.Probes[Index].Key == InKey) { return true; } }
                return false;
            };
            int32_t BlockOf[RENDER_REFLECTION_PROBES_MAX];
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
                            Slots[Block] = ProbeSlot{ Probe.Key, false, true };
                            BlockOf[Index] = int32_t(Block);
                            break;
                        }
                    }
                }
                if(BlockOf[Index] >= 0 && Probe.bDue) { Slots[BlockOf[Index]].bPending = true; }
            }

            uint32_t Captures = 0;
            for(uint32_t Index = 0; Index < Count && Captures < 2; ++Index)
            {
                if(BlockOf[Index] < 0) { continue; }
                ProbeSlot& Slot = Slots[BlockOf[Index]];
                if(!Slot.bPending && Slot.bValid) { continue; }
                SCOPED_DRAW_EVENT("ReflectionProbe " + std::to_string(BlockOf[Index]));
                Slot.bValid   = CaptureReflectionProbe(InContext, CaptureList.Probes[Index], uint32_t(BlockOf[Index]));
                Slot.bPending = false;
                ++Captures;
            }
            if(Captures > 0)
            {
                InContext.SetView(View);
            }

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
        P->Targets.Release();
        P->ProbeStages.Release();
        P->ReleaseOcclusion();
        for(GDIRenderTarget** Each : { &P->IBL.Atlas, &P->IBL.Strip })
        {
            if(*Each) { GDIReleaseRenderTarget(*Each); delete *Each; *Each = nullptr; }
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
        P->View = InView;
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
        P->Lighting = InLighting;
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
        P->PostProcess = InPostProcess;
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
        InContext.SetView(P->View);
        InContext.SetLighting(P->Lighting);
        InContext.UpdateTextureStreaming();   // the frame's first scene: last frame's demand to loads and commits
        P->ApplyImageBasedLighting(InContext);   // last frame's, for the captures below; brought up to date before the scene

        /* Created on first use and resized with the viewport -- see Impl's
         * own comment on why a scene owns these at all. Early-outs on every
         * frame whose viewport has not changed, which is nearly all of them. */
        if(P->View.ViewportPixels.Width > 0 && P->View.ViewportPixels.Height > 0)
        {
            P->Targets.Allocate(uint(P->View.ViewportPixels.Width), uint(P->View.ViewportPixels.Height));
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
            float GpuMs[RGT_Count];
            const void* Owner = InContext.BeginTimedBlock(this, Stats.bShowFrame, GpuMs);
            for(RenderScene* Each : GLiveScenes)
            {
                if(Each == Owner) { Each->P->AddGpuTimes(GpuMs); }
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
        uint64_t VisibleCount = 0, CulledCount = 0, StaticCount = 0;

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
            const uint32_t ViewCount = BuildCaptureViews(Request, P->View, Stages->Width, Stages->Height, Views);
            for(uint32_t Face = 0; Face < ViewCount; ++Face)
            {
                SCOPED_DRAW_EVENT(bCube ? GetCubeFaceName(Face) : "Mirror");
                InContext.SetView(Views[Face]);
                uint64_t Visible = 0, Culled = 0, Static = 0;
                P->QueueDraws(InContext, Views[Face], &Request, Visible, Culled, Static);
                InContext.RenderSceneStages(*Stages, Clear);
                InContext.FinishTargetFace(Request.Target, Face);
                bCaptured = true;
            }
        }
        P->CaptureList.Count = 0;
        if(bCaptured)
        {
            InContext.SetView(P->View);
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
            P->UpdateImageBasedLighting(InContext);
        }
        InContext.WriteGpuTimestamp(RGT_SceneCapture, true);

        /* TAA's jitter (RENDERPIPELINE.md, "Temporal antialiasing"): the main view drawn a
         * sub-pixel off, a different way each frame (Halton 2, 3), so the resolve averages
         * many samples a pixel. P->View stays unjittered: the captures above, the next
         * frame's PrevViewProjection the resolve reprojects with, what draws after. */
        RenderView Drawn = P->View;
        InContext.SetTemporalJitter(0.0f, 0.0f);
        const PostProcessSettings& Stack = P->PostProcess.Settings;
        if(Stack.bAntialiasing && Stack.AaMethod == 1 && P->Targets.bHDR && P->Targets.Width > 0 && P->Targets.Height > 0)
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
            P->JitterIndex = (P->JitterIndex + 1) % 8;
            const double X = (Halton(P->JitterIndex + 1, 2) - 0.5) * 2.0 / double(P->Targets.Width);    // NDC
            const double Y = (Halton(P->JitterIndex + 1, 3) - 0.5) * 2.0 / double(P->Targets.Height);
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

        GDIOcclusionQueries* Occlusion = P->PrepareOcclusion(Drawn);
        const auto QueueStart = std::chrono::steady_clock::now();
        P->QueueDraws(InContext, Drawn, nullptr, VisibleCount, CulledCount, StaticCount, Occlusion);
        const float QueueMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - QueueStart).count());

        /* The post process stack: the volumes' blend (WorldBridge), its
         * materials by stage and order, then the effects' settings. */
        const PostProcessSettings& Post = P->PostProcess.Settings;
        for(uint32_t Index = 0; Index < Post.MaterialCount && Index < RENDER_POST_MATERIAL_MAX; ++Index)
        {
            InContext.SubmitPostProcess(Post.Materials[Index].Path, Post.Materials[Index].Stage);
        }
        const bool bSameViewport = P->bHasPrevView && P->PrevViewport.Width == P->View.ViewportPixels.Width
                                && P->PrevViewport.Height == P->View.ViewportPixels.Height;
        InContext.SetPostProcess(Post, bSameViewport ? &P->PrevViewProjection : nullptr);
        P->PrevViewProjection = P->View.Projection * P->View.View;
        P->PrevViewport       = P->View.ViewportPixels;
        P->bHasPrevView       = true;

        /* Base Pass, Lighting, Unlit, the translucent stages, PostProcess;
         * leaves the final colour bound, for the present and the overlay. */
        InContext.SetUnlit(P->ViewMode == ERenderViewMode::Unlit);
        InContext.RenderSceneStages(P->Targets, P->ClearColor, /*bInTimed*/ true, P->bClearGradient ? &P->ClearBottom : nullptr);
        InContext.SetUnlit(false);
        InContext.SetCustomPassWorld(nullptr);
        P->PassWorld = nullptr;   // set again by the next frame's SetCustomPassWorld, if it has a World
        {
            float   CpuMs[RGT_Count];
            int32_t Draws[RGT_Count];
            InContext.GetStageCpuTimes(CpuMs, Draws);
            P->AddCpuTimes(CpuMs, Draws, QueueMs);
        }
        InContext.SetView(P->View);   // unjittered again, for what draws over the picture

        /* The targets this frame drew, for the view mode's list; one of them
         * over the picture in the Render Target view mode. */
        {
            if(P->ViewMode == ERenderViewMode::RenderTarget)
            {
                InContext.ShowTarget(P->Targets, P->ViewTarget);
            }
            std::vector<std::string> Names;
            InContext.ListViewableTargets(P->Targets, Names);
            std::lock_guard<std::mutex> Guard(P->TargetsLock);
            if(Names != P->TargetNames) { P->TargetNames = std::move(Names); }
        }

        /* The UI (RENDERPIPELINE.md, "UI"): over the post-processed picture,
         * before the readback so a capture shows it, and under "stat unit". */
        InContext.DrawUI(P->UI, P->Targets.Width, P->Targets.Height);

#if WITH_EDITOR
        /* Read back before the overlay; the readback flushes and resumes the frame. */
        const auto DeliverCaptures = [this](std::vector<std::pair<uint64_t, MaterialPreviewMailbox*>>& InCaptures)
        {
            for(const auto& Each : InCaptures)
            {
                EngineCore::Image Pixels;
                if(P->Targets.Final) { Pixels = P->Targets.Final->ToImage(); }
                if(Each.second)
                {
                    Each.second->Deliver(Each.first, std::move(Pixels),
                                         P->Targets.Final ? std::string() : std::string("the scene has no render target"));
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
            SetOcclusionStats(Occlusion ? P->LastOccluded : 0, Occlusion ? P->LastQueries : 0);
        }

#if WITH_EDITOR
        /* Tool overlay over everything, unculled; consumed each frame. */
        if(!P->DebugLines.empty())
        {
            /* No depth test: an outline or a gizmo shows through what it marks. */
            GDISetDepthState(IDepthStateDesc());
            SCOPED_DRAW_EVENT("DebugLines");
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
            InContext.DrawStatUnit(P->Targets.Width, P->Targets.Height, Text);
        }

        InContext.EndDrawEvent();   // "Scene"
        GDIEndRendering();

        /* Drawn: what each proxy is now is what it was, for the next frame's velocity. */
        for(RenderProxy* Proxy : P->Proxies)
        {
            Proxy->EndFrameMotion();
        }

        /* After EndRendering: the final colour is now in its presentable state. */
        P->PresentTarget.store(P->Targets.Final, std::memory_order_release);
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
        P->Targets.Release();
        P->ProbeStages.Release();
        P->ReleaseOcclusion();
        InContext.ReleaseIBLResources(P->IBL);
        P->bSkyValid = false;
        for(Impl::ProbeSlot& Each : P->Slots) { Each = Impl::ProbeSlot(); }
    }
}
