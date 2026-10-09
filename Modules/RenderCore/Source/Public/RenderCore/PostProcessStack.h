#ifndef RENDERCORE_POSTPROCESSSTACK_H
#define RENDERCORE_POSTPROCESSSTACK_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * A post process stack (*.pp.nmgraph; RENDERPIPELINE.md's "Post process"):
 * which effects run and their settings, and the PostProcess materials to run
 * before or after them. NodeGraph's PostProcess schema writes it into the
 * graph's <gen_code> as ".ppstack" text; this reads that text, so a game build
 * needs neither NodeGraph nor an XML library.
 *
 * The same struct holds a frame's blend of every volume the camera is in
 * (BlendPostProcess), and travels to the render thread by value: POD.
 */
namespace RenderCore
{
    enum : uint32_t
    {
        RENDER_POST_MATERIAL_MAX  = 8,     // material passes a stack (or a frame) may run
        RENDER_POST_PATH_MAX      = 128,   // RENDER_MATERIAL_PATH_MAX
        RENDER_BLOOM_LEVELS_MAX   = 6,
    };

    enum EPostProcessStage : uint8_t
    {
        PPS_BeforeEffects = 0,   // before Depth of Field
        PPS_AfterEffects  = 1,   // after Bloom, before Antialiasing
    };

    struct PostProcessMaterial
    {
        char    Path[RENDER_POST_PATH_MAX] = {};
        uint8_t Stage = PPS_AfterEffects;
        float   Order = 0.0f;
    };

    struct PostProcessSettings
    {
        /* Each effect's switch: in a stack, "it has the node"; blended, "it runs". */
        uint8_t bAmbientOcclusion = 0;   // runs before the lighting, not in the post chain
        float   AoIntensity = 1.0f;
        float   AoRadius    = 60.0f;     // world units
        float   AoSamples   = 12.0f;
        float   AoPower     = 1.5f;
        uint8_t bAoBlur     = 1;

        /* Screen-space reflections (RENDERPIPELINE.md): over the probes' specular, in the IBL pass. */
        uint8_t bScreenReflections = 0;
        float   SsrIntensity    = 1.0f;
        float   SsrMaxRoughness = 0.45f;
        float   SsrMaxDistance  = 1500.0f;     // world units
        float   SsrThickness    = 25.0f;       // world units
        float   SsrSteps        = 32.0f;

        /* Volumetric fog (RENDERPIPELINE.md): after the translucent stages, before TAA. */
        uint8_t bVolumetricFog   = 0;
        float   FogDensity       = 0.00003f;   // per world unit, at and below FogBaseHeight
        float   FogHeightFalloff = 0.0008f;    // per world unit above it
        float   FogBaseHeight    = 0.0f;
        float   FogMaxDistance   = 8000.0f;
        float   FogAnisotropy    = 0.6f;
        float   FogSun           = 1.0f;
        float   FogSky           = 1.0f;
        float   FogLights        = 1.0f;
        float   FogAlbedo[3]     = { 1.0f, 1.0f, 1.0f };
        float   FogSteps         = 24.0f;
        uint8_t bFogShadows      = 1;

        uint8_t bDepthOfField = 0;
        float   DofFocusDistance = 800.0f;
        float   DofFocusRange    = 400.0f;
        float   DofMaxBlur       = 8.0f;     // pixels at full resolution
        uint8_t bDofNearBlur     = 1;

        uint8_t bMotionBlur = 0;
        float   MotionIntensity = 0.5f;      // fraction of a frame's motion
        float   MotionMaxLength = 32.0f;     // pixels
        float   MotionSamples   = 8.0f;

        uint8_t bBloom = 0;
        float   BloomThreshold = 0.8f;
        float   BloomIntensity = 0.6f;
        float   BloomRadius    = 0.75f;
        float   BloomLevels    = 5.0f;
        float   BloomTint[3]   = { 1.0f, 1.0f, 1.0f };

        uint8_t bAntialiasing = 0;
        uint8_t AaQuality     = 1;           // 0 Low, 1 Medium, 2 High
        uint8_t AaMethod      = 0;           // 0 FXAA, 1 TAA (RENDERPIPELINE.md, "Temporal antialiasing")
        float   AaSharpness   = 0.35f;       // TAA: sharpened back after the resolve, 0-1
        float   AaEdgeThreshold = 0.125f;

        /* An HDR scene's exposure and tone map (the first post pass). Off: exposure 0, None (clamp). */
        uint8_t bToneMapping = 0;
        float   Exposure     = 0.0f;         // stops (EV): the scene times 2^Exposure
        uint8_t ToneMapper   = 2;            // 0 None, 1 Reinhard, 2 ACES

        /* Eye adaptation (RENDERPIPELINE.md, "Auto exposure"): with Tone Mapping, the scene's
         * metered average exposed toward Key, within MinEV-MaxEV stops, eased at the speeds. */
        uint8_t bAutoExposure = 0;
        float   AeKey         = 0.18f;
        float   AeMinEV       = -2.0f;
        float   AeMaxEV       = 4.0f;
        float   AeSpeedUp     = 3.0f;        // toward a brighter view, per second
        float   AeSpeedDown   = 1.0f;        // toward a darker one
        float   AeCentreWeight = 0.5f;

        PostProcessMaterial Materials[RENDER_POST_MATERIAL_MAX];
        uint32_t            MaterialCount = 0;

        bool IsEmpty() const
        {
            return !bAmbientOcclusion && !bScreenReflections && !bVolumetricFog && !bDepthOfField && !bMotionBlur && !bBloom && !bAntialiasing && !bToneMapping && !bAutoExposure
                && MaterialCount == 0;
        }
    };

    /* .ppstack text into OutSettings (cleared first). False, with OutError
     * naming the line, on a line it cannot read; unknown keys are ignored, so
     * an older engine reads a newer stack. */
    RENDERCORE_API bool ParsePostProcessStack(const std::string& InText, PostProcessSettings& OutSettings, std::string& OutError);

    /*
     * InStack laid over InOutBlend at weight InWeight (0-1), as a volume the
     * camera is InWeight inside: numbers blend linearly from what is there --
     * from the effect off, if nothing turned it on yet, which scales its
     * strength (Intensity, Max Blur) up from 0 -- and switches (Near Blur,
     * Quality, Antialiasing itself) take InStack's at InWeight 0.5 or more.
     * An effect InStack has no node for is left alone. Its materials are
     * added at 0.5 or more, up to RENDER_POST_MATERIAL_MAX.
     */
    RENDERCORE_API void BlendPostProcess(PostProcessSettings& InOutBlend, const PostProcessSettings& InStack, float InWeight);

    /*
     * A Post Process component's weight for a camera at distance InDistance
     * outside its volume (0 inside): 1 inside, fading to 0 over
     * InBlendDistance outside; with no blend distance, 1 inside and 0 out.
     */
    RENDERCORE_API float GetPostProcessVolumeWeight(float InDistance, float InBlendDistance);

    /*
     * One post process setting a sequence (or a game) can override by name,
     * on top of whatever the volumes blended: what Sequencer's Post Process
     * Setting tracks key. Overriding a setting also turns its effect on.
     */
    enum class EPostProcessParam : uint8_t
    {
        Exposure = 0,        // stops; turns Tone Mapping on
        DofFocusDistance,    // world units; turns Depth of Field on
        DofFocusRange,
        DofMaxBlur,          // pixels
        BloomIntensity,      // turns Bloom on
        BloomThreshold,
        FogDensity,          // per world unit; turns Volumetric Fog on
        MotionIntensity,     // turns Motion Blur on
        AoIntensity,         // turns Ambient Occlusion on
        SsrIntensity,        // turns Screen Reflections on
        Count
    };

    /* Never null; "" out of range. The name tracks use. */
    RENDERCORE_API const char* PostProcessParamName(EPostProcessParam InParam);
    /* False, OutParam untouched, for an unknown name. */
    RENDERCORE_API bool        FindPostProcessParam(const char* InName, EPostProcessParam& OutParam);
    /* InValue into InOutSettings' InParam, and its effect on. */
    RENDERCORE_API void        ApplyPostProcessParam(PostProcessSettings& InOutSettings, EPostProcessParam InParam, float InValue);

    /*
     * What is laid over a World's volumes, every frame it is set: Stacks
     * blended after them (in order, as volumes of their weight), then Params
     * written over the result. A Sequencer sets it for its World while a
     * sequence plays, and clears it after. Game thread.
     */
    struct PostProcessOverride
    {
        struct Stack
        {
            std::string Path;            // a .pp.nmgraph, as a Post Process component names it
            float       Weight = 1.0f;   // 0..1
        };
        struct Param
        {
            EPostProcessParam Param = EPostProcessParam::Exposure;
            float             Value = 0.0f;
        };
        std::vector<Stack> Stacks;
        std::vector<Param> Params;

        bool IsEmpty() const { return Stacks.empty() && Params.empty(); }
    };

    /* InWorld's override (an empty one clears it); keyed by address, so clear
     * it before the World goes. */
    RENDERCORE_API void SetPostProcessOverride(const void* InWorld, const PostProcessOverride& InOverride);
    /* A copy of InWorld's; empty if none. */
    RENDERCORE_API PostProcessOverride GetPostProcessOverride(const void* InWorld);
    /* InWorld's override applied to InOutSettings (WorldBridge, after the volumes). */
    RENDERCORE_API void ApplyPostProcessOverride(const void* InWorld, PostProcessSettings& InOutSettings);

    /*
     * Stacks by runtime path ("Content/PostProcess/Arch.pp.nmgraph"), read
     * through the file provider on first use; null (warned once) if it cannot
     * be read. A material graph path ("...material.nmgraph") is a stack of that
     * one material, after the effects -- what a Post Process component named
     * before stacks. Thread-safe; the editor invalidates a stack it saves.
     */
    class RENDERCORE_API PostProcessStackCache
    {
    public:
        static std::shared_ptr<const PostProcessSettings> Find(const std::string& InPath);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);
    };
}

#endif //RENDERCORE_POSTPROCESSSTACK_H
