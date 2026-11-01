#ifndef RENDERCORE_SHADOWSETUP_H
#define RENDERCORE_SHADOWSETUP_H

#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderLighting.h"
#include "Public/RenderCore/RenderView.h"

/*
 * Where each shadow map of a frame looks from, and where it lands in the two
 * shadow atlases (RENDERPIPELINE.md's "Shadows"). Pure math -- no GDI -- so
 * the smoke test checks it on the Null backend.
 *
 *   Directional atlas  the sun: one view over the whole atlas, or 1-4
 *                      cascades in its 2x2 quarters.
 *   Local atlas        6 x 4 tiles: a spot light takes one, a point light six
 *                      (its cube faces, +X -X +Y -Y +Z -Z); then each inset
 *                      shadow one of those left, an orthographic view of the
 *                      sun fitted round one caster.
 *
 * Every matrix follows RenderView's: row-major, column vectors, LookAt views
 * whose +Z is forward. Projections give GL clip space (z_ndc in [-1, 1]); GDI
 * remaps that per backend, so the stored depth is z_ndc * 0.5 + 0.5 on all.
 */
namespace RenderCore
{
    enum : uint32_t
    {
        SHADOW_LOCAL_ATLAS_COLUMNS = 6,
        SHADOW_LOCAL_ATLAS_ROWS    = 4,
        SHADOW_POINT_FACES         = 6,
        SHADOW_VIEW_MAX            = RENDER_CASCADE_MAX + SHADOW_LOCAL_ATLAS_COLUMNS * SHADOW_LOCAL_ATLAS_ROWS,
    };

    enum EShadowAtlas : uint8_t
    {
        SA_Directional = 0,
        SA_Local       = 1,
        SA_Count
    };

    struct ShadowView
    {
        EngineCore::Matrix4x4 View;          // for the caster pass, as RenderView::View
        EngineCore::Matrix4x4 Projection;    // as RenderView::Projection
        EngineCore::Matrix4x4 WorldToAtlas;  // world -> (u, v, depth, 1) * w in its atlas, tile included

        uint8_t        Atlas = SA_Directional;
        uint32_t       TileX = 0, TileY = 0, TileSize = 0;   // texels, in its atlas
        GameCore::AABB WorldBounds;                          // what it can see: casters outside are skipped

        /* An inset shadow's view: which of BuildShadowFrame's InCasters it
         * draws -- that one alone. -1 for a light's view. */
        int32_t        Inset = -1;

        /* An inset view: the part of the camera's screen it can shadow, as
         * NDC x and y min then max (y up) -- the prism behind its caster,
         * swept to the far plane and clipped to the view (InsetScreenNdc).
         * The Lighting pass skips every pixel outside. */
        float          ScreenNdc[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    };

    /* A frame's shadow views and what the lights index into. */
    struct ShadowFrame
    {
        ShadowView Views[SHADOW_VIEW_MAX];
        uint32_t   ViewCount = 0;

        uint32_t   AtlasSize = 0;            // both atlases' side, texels; 0: no shadows this frame

        /* The sun: its first view and how many (1 plain, 1-4 cascaded);
         * Splits[i] is cascade i's far edge, as view depth from the eye. */
        int32_t    DirectionalFirstView = -1;
        uint32_t   CascadeCount         = 0;
        float      CascadeSplits[RENDER_CASCADE_MAX] = {};

        /* Per RenderLighting::Locals entry: its first view (a point light's
         * six follow), or -1 for none. */
        int32_t    LocalFirstView[RENDER_LOCAL_LIGHT_MAX] = {};

        /* Inset shadows: InsetCount views from InsetFirstView, the casters
         * that got a tile, in the order offered. */
        int32_t    InsetFirstView = -1;
        uint32_t   InsetCount     = 0;

        bool IsSunView(const uint32_t InView) const
        {
            return DirectionalFirstView >= 0 && InView >= uint32_t(DirectionalFirstView)
                && InView < uint32_t(DirectionalFirstView) + CascadeCount;
        }

        bool UsesAtlas(const EShadowAtlas InAtlas) const
        {
            for(uint32_t Index = 0; Index < ViewCount; ++Index)
            {
                if(Views[Index].Atlas == InAtlas) { return true; }
            }
            return false;
        }
    };

    /*
     * InLighting's shadow views against InCamera. Nothing when
     * InLighting.Shadows.bEnabled is 0. A directional light's views cover
     * the camera's frustum out to its ShadowDistance (times DistanceScale);
     * cascades split that by CascadeSplitLambda and are texel-snapped so they
     * do not shimmer as the camera moves. At most MaxShadowedLights local
     * lights (and RENDER_SHADOWED_LOCAL_LIGHT_MAX) get views, in Locals order.
     *
     * Then InCasters, most deserving first, while the local atlas has tiles
     * left and up to MaxInsetShadows (and RENDER_INSET_SHADOW_MAX): each an
     * orthographic view along InLighting's inset light holding the caster's
     * sphere, texel-snapped. None without an inset light. InReceivers bounds
     * what they can shadow on screen (InsetScreenNdc).
     */
    RENDERCORE_API void BuildShadowFrame(const RenderLighting& InLighting, const RenderView& InCamera, ShadowFrame& OutFrame,
                                         const InsetShadowCaster* InCasters = nullptr, uint32_t InCasterCount = 0,
                                         const GameCore::AABB* InReceivers = nullptr);

    /* The cascades' far edges between InNear and InFar: InLambda 0 uniform,
     * 1 logarithmic. */
    /* The NDC rectangle (x, y min then max) InCamera sees of InCaster's
     * sphere, grown by InPad world units and swept along InDirection (unit)
     * to past InCamera's far plane -- clipped to the view, so what is behind
     * the eye or off screen drops out. Empty (min above max) if none of it is
     * in view; the whole screen if it holds the view. InReceivers, if given,
     * bounds everything a shadow can land on (what the view draws): the
     * prism is cut to it first, so it stops at the floor. */
    RENDERCORE_API void InsetScreenNdc(const InsetShadowCaster& InCaster, const EngineCore::Vector3& InDirection, double InPad,
                                       const RenderView& InCamera, float (&OutNdc)[4], const GameCore::AABB* InReceivers = nullptr);

    /* The shadow views the frame's captures share (GLOBAL_ILLUMINATION.md, "Capture budget"): the
     * sun's as one view over the whole directional atlas holding InRegion (no cascades: every view
     * depth lands in it), then the shadowed local lights whose reach overlaps InRegion, as
     * BuildShadowFrame places them. No insets. Drawn once, then sampled by every capture view. */
    RENDERCORE_API void BuildCaptureShadowFrame(const RenderLighting& InLighting, const GameCore::AABB& InRegion, ShadowFrame& OutFrame);

    RENDERCORE_API void ComputeCascadeSplits(float InNear, float InFar, uint32_t InCount, float InLambda, float* OutSplits);

    /* The [Shadows] config section (ShadowSettings.cpp). GAME THREAD: the
     * config registry is not thread-safe. */
    RENDERCORE_API RenderShadowSettings ReadShadowSettings();
}

#endif //RENDERCORE_SHADOWSETUP_H
