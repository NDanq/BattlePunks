#ifndef RENDERCORE_SCENECAPTURE_H
#define RENDERCORE_SCENECAPTURE_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/Components/RenderComponent.h"   // RENDER_MATERIAL_PATH_MAX
#include "Public/RenderCore/RenderView.h"

/*
 * Scene capture requests (RENDERPIPELINE.md, "Scene capture"): what
 * WorldBridge decides a frame's SceneCaptureProbes need, handed to the render
 * thread by value (trivially copyable: RenderCommand.h's capture rule), and
 * the views RenderScene renders each one from.
 */
namespace RenderCore
{
    enum : uint32_t
    {
        RENDER_CAPTURES_MAX         = 8,    // probes captured in one frame; the rest wait
        RENDER_CAPTURE_EXCLUDED_MAX = 32,   // entities a probe leaves out (its owner's subtree)
    };

    struct SceneCaptureRequest
    {
        char                Target[RENDER_MATERIAL_PATH_MAX] = {};
        uint8_t             Mode = 0;                // ESceneCaptureMode
        uint32_t            Resolution = 0;          // 0: the target's own
        EngineCore::Vector3 Position;                // the probe; a mirror's plane passes through it
        EngineCore::Vector3 Normal{0.0, 1.0, 0.0};   // a mirror's facing (the probe's local +Y)
        float               NearClip = 5.0f;
        float               FarClip  = 5000.0f;
        uint8_t             bShadows = 0;            // draw shadow maps for each view (the probe's CaptureShadows)
        uint32_t            ExcludedCount = 0;
        GameCore::EntityID  Excluded[RENDER_CAPTURE_EXCLUDED_MAX];
    };

    struct SceneCaptureList
    {
        uint32_t            Count = 0;
        SceneCaptureRequest Requests[RENDER_CAPTURES_MAX];
    };

    /* A cube face's forward and up, +X -X +Y -Y +Z -Z -- the order of a
     * RenderTargetTex3D's strip. MaterialCommon.glsl's
     * Material_SampleCubeStrip must agree. */
    void CubeFaceBasis(uint32_t InFace, EngineCore::Vector3& OutForward, EngineCore::Vector3& OutUp);

    /* The views InRequest renders from, at InWidth x InHeight: six cube faces
     * (90 degrees, NearClip to FarClip), or one mirror view -- InMain's camera
     * reflected in the plane, with InMain's projection, so a mirrored screen
     * point samples it (Material_MirrorUV). Returns how many; 0 for a mirror
     * InMain sees from behind. */
    uint32_t BuildCaptureViews(const SceneCaptureRequest& InRequest, const RenderView& InMain, uint32_t InWidth, uint32_t InHeight,
                               RenderView (&OutViews)[6]);
}

#endif //RENDERCORE_SCENECAPTURE_H
