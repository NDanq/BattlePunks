#ifndef NIMBLEEDITOR_LIGHTBAKER_H
#define NIMBLEEDITOR_LIGHTBAKER_H

#if WITH_EDITOR

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Public/RenderCore/BakedLightingAsset.h"
#include "Private/Editor/Lighting/LightBakeScene.h"

/*
 * The light baker: a CPU path tracer over a bake scene (LightBakeScene.h),
 * on every core. Widget-free and World-free, so a test can bake a scene it
 * builds by hand. RENDERPIPELINE.md, "Baked lighting", has the whole story.
 *
 *   1. Layout     each lightmapped instance gets a square of
 *                 sqrt(area) * TexelsPerUnit texels (16 to the atlas size),
 *                 shelf-packed into atlases, a texel of border round each;
 *                 its UV1 maps into its square.
 *   2. Texels     each instance's UV1 triangles are rasterised into its
 *                 square: a texel takes the world position and normal at
 *                 its centre -- or, just off a triangle's edge, at the
 *                 nearest point on it, so bilinear filtering never reads an
 *                 unlit texel along a chart's edge.
 *   3. Lighting   per texel, "diffuse radiance" (irradiance over pi):
 *                 direct light from every light, shadow rays jittered over
 *                 its source for soft shadows; then SamplesPerTexel
 *                 cosine-weighted paths, each hitting the sky or bouncing
 *                 off up to Bounces surfaces (their albedo, each lit by
 *                 every light), backfaces counting as black so light does
 *                 not leak through walls.
 *                 A texel whose short rays find the back of a surface is
 *                 inside or under something (a floor under a wall): it is
 *                 left out and filled from its neighbours, so walls do not
 *                 leak dark lines. Emissive surfaces light what sees them;
 *                 Stationary lights and skies give only their bounce. The
 *                 light's dominant direction is kept too (Atlas::Directions).
 *   4. Filtering  the noisy bounced part is smoothed over neighbouring
 *                 texels of the same instance with similar position and
 *                 normal; then lit texels grow into the empty ones round
 *                 them, so filtering at a chart's edge stays lit.
 *   5. Probes     a grid over the scene, each probe an ambient cube from
 *                 rays over the whole sphere plus each light's direct part;
 *                 a probe that mostly sees backfaces is inside something and
 *                 left out.
 *
 * Values match the renderer's: a white surface lit by a light shows Value as
 * EvaluateLight's diffuse term would make it, so a bake looks like the live
 * lighting it replaces, plus bounces and soft shadows.
 */
namespace LightBake
{
    struct Settings
    {
        float TexelsPerUnit   = 0.1f;
        int   Bounces         = 3;
        int   SamplesPerTexel = 128;
        float ProbeSpacing    = 200.0f;
        int   MaxAtlasSize    = 1024;
        int   Threads         = 0;   // 0: every core
    };

    struct Atlas
    {
        int                Size = 0;
        std::vector<float> Texels;       // RGB per texel, rows top first (v = 0 at the top)

        /* Directional lightmap, per texel: the light's dominant direction
         * times its directionality (0 all round, 1 from one direction), and
         * the half-Lambert at the texel's own normal that rebalances it --
         * what BakedLighting.glsl needs to shade a normal map. */
        std::vector<float> Directions;   // 4 per texel: xyz, w
    };

    /* Where an instance's lightmap is. */
    struct Placement
    {
        uint32_t Instance = 0;
        uint32_t Atlas    = 0;
        int      X = 0, Y = 0;            // its square's corner, texels
        int      Resolution = 0;          // its square's side, texels
        float    ScaleOffset[4] = {};     // atlas UV = UV1 * xy + zw
    };

    struct Result
    {
        std::vector<Atlas>        Atlases;
        std::vector<Placement>    Placements;
        RenderCore::BakedProbeGrid Probes;
    };

    /* InStage names what is running ("Lighting texels"); InFraction is its
     * progress, 0-1. Called on the baking thread. */
    using ProgressFn = std::function<void(const char* InStage, float InFraction)>;

    enum class EBakeStatus : uint8_t { Done, Cancelled, Failed };

    /* Runs on the calling thread (and its workers) until done or *InCancel
     * goes true. OutError says why for Failed. */
    EBakeStatus Bake(const Scene& InScene, const Settings& InSettings, Result& OutResult, std::string& OutError,
                     const ProgressFn& InProgress = ProgressFn(), const std::atomic<bool>* InCancel = nullptr);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_LIGHTBAKER_H
