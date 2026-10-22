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
        RENDER_REFLECTION_PROBES_MAX = 16,  // reflection probes lighting a view at once (IBLCommon.glsl's IBL_PROBES_MAX)
    };

    /* A Reflection Probe lighting this view: where, its influence, and whether
     * its capture is due (its Frequency, or Capture Now). RenderScene captures
     * it then -- or whenever it holds no capture of it (IBL.h). */
    struct ReflectionProbeRequest
    {
        uint64_t            Key = 0;                 // its entity, packed: the same probe from frame to frame
        EngineCore::Vector3 Position;
        float               Radius    = 1000.0f;
        float               Extent[3] = {};          // box half sizes; all 0: a sphere
        float               Intensity = 1.0f;
        float               NearClip  = 5.0f;
        float               FarClip   = 20000.0f;
        uint32_t            Resolution = 128;        // a face's side
        uint8_t             bDue      = 0;
        uint8_t             bShadows  = 1;
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

        /* The Reflection Probes lighting the view, nearest the camera first. */
        uint32_t               ProbeCount = 0;
        ReflectionProbeRequest Probes[RENDER_REFLECTION_PROBES_MAX];
    };

    /* A cube face's forward and up, +X -X +Y -Y +Z -Z -- the order of a
     * RenderTargetTex3D's strip. MaterialCommon.glsl's
     * Material_SampleCubeStrip must agree. */
    void CubeFaceBasis(uint32_t InFace, EngineCore::Vector3& OutForward, EngineCore::Vector3& OutUp);

    /* "Face +X" .. "Face -Z", in CubeFaceBasis's order: a face's draw event. */
    const char* GetCubeFaceName(uint32_t InFace);

    /* The views InRequest renders from, at InWidth x InHeight: six cube faces
     * (90 degrees, NearClip to FarClip), or one mirror view -- InMain's camera
     * reflected in the plane, with InMain's projection, so a mirrored screen
     * point samples it (Material_MirrorUV). Returns how many; 0 for a mirror
     * InMain sees from behind. */
    uint32_t BuildCaptureViews(const SceneCaptureRequest& InRequest, const RenderView& InMain, uint32_t InWidth, uint32_t InHeight,
                               RenderView (&OutViews)[6]);
}

#endif //RENDERCORE_SCENECAPTURE_H
