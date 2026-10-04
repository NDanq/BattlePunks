#ifndef RENDERCORE_POSTPROCESSSTACK_H
#define RENDERCORE_POSTPROCESSSTACK_H

#include <cstdint>
#include <memory>
#include <string>

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
        float   AaEdgeThreshold = 0.125f;

        /* An HDR scene's exposure and tone map (the first post pass). Off: exposure 0, None (clamp). */
        uint8_t bToneMapping = 0;
        float   Exposure     = 0.0f;         // stops (EV): the scene times 2^Exposure
        uint8_t ToneMapper   = 2;            // 0 None, 1 Reinhard, 2 ACES

        PostProcessMaterial Materials[RENDER_POST_MATERIAL_MAX];
        uint32_t            MaterialCount = 0;

        bool IsEmpty() const
        {
            return !bAmbientOcclusion && !bDepthOfField && !bMotionBlur && !bBloom && !bAntialiasing && !bToneMapping && MaterialCount == 0;
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
