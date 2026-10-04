#ifndef RENDERCORE_SCENECAPTUREPROBECOMPONENT_H
#define RENDERCORE_SCENECAPTUREPROBECOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/Components/RenderComponent.h"   // RENDER_MATERIAL_PATH_MAX

namespace RenderCore
{
    /* How a probe captures (SceneCaptureProbeComponent::Mode). */
    enum ESceneCaptureMode : uint8_t
    {
        SCM_PlanarMirror    = 0,   // into a RenderTargetTex2D: the view reflected in the probe's plane
        SCM_Omnidirectional = 1,   // into a RenderTargetTex3D: six faces around the probe
    };

    /* When (SceneCaptureProbeComponent::Frequency). */
    enum ESceneCaptureFrequency : uint8_t
    {
        SCF_EveryFrame = 0,
        SCF_Interval   = 1,   // every Interval seconds
        SCF_Once       = 2,   // the first frame it is seen, then never again
        SCF_Manual     = 3,   // only when CaptureNow is set (a script, the editor's Capture Now)
    };

    /*
     * "Scene Capture Probe" (RENDERPIPELINE.md, "Scene capture"). Renders the
     * scene from this entity into its Target render target, which materials
     * sample with the Texture or Reflect node.
     *
     *   Planar Mirror    Target is a RenderTargetTex2D (.nmrt2d). The mirror's
     *                    plane passes through the entity, facing its local +Y;
     *                    each capture is the current camera reflected in it, for
     *                    the Reflect node to sample at the mirrored screen point.
     *   Omnidirectional  Target is a RenderTargetTex3D (.nmrt3d): six 90-degree
     *                    faces around the entity, sampled by direction.
     *
     * Resolution is the capture's side (a cube's face); 0 keeps the target's
     * own. The capture extent: NearClip to FarClip from the probe (or from
     * the reflected camera), and for a mirror ExtentX by ExtentZ, its half
     * size on the plane -- what the gizmo shows, and outside the view no
     * capture is made. HideOwner leaves this entity and its children out of
     * its own capture (a mirror, a chrome ball). CaptureNow asks for one
     * capture on the next frame, whatever the frequency, and clears itself.
     */
    struct RENDERCORE_API SceneCaptureProbeComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Target[RENDER_MATERIAL_PATH_MAX] = {};
        uint8_t Mode       = SCM_Omnidirectional;
        int32_t Resolution = 256;
        uint8_t Frequency  = SCF_EveryFrame;
        float   Interval   = 1.0f;
        float   NearClip   = 5.0f;
        float   FarClip    = 5000.0f;
        float   ExtentX    = 200.0f;
        float   ExtentZ    = 200.0f;
        uint8_t bHideOwner  = 1;
        uint8_t bEnabled    = 1;
        uint8_t bCaptureNow = 0;
    };
}

#endif //RENDERCORE_SCENECAPTUREPROBECOMPONENT_H
