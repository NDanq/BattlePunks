#version 450 core
// Sky.fsh -- the Sky Light's HDRI behind the scene, linear light (an HDR
// scene's background; RENDERPIPELINE.md, "Image-based lighting"): each pixel
// looks along its view ray. Without an HDRI, the sky and ground colours.

#include <PostProcessCommon.glsl>
#include <IBLCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_SkyColor;      // rgb linear, times the intensity
    vec4 PP_GroundColor;
    vec4 PP_Unused0;
    vec4 PP_Unused1;
    vec4 PP_Settings;      // x: 1 with an HDRI; y: its intensity; z: its rotation (radians)
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, Environment)   // slot 0: the HDRI (RGBE8)

void main(void)
{
    vec4 Far = Scene_InvViewProjection * vec4(Interpolants.Position.xy, 1.0, 1.0);
    vec3 Dir = normalize(Far.xyz / Far.w - Scene_CameraPosition.xyz);
    vec3 Radiance = PP_Settings.x > 0.5 ? IBL_SampleEquirect(Environment, Dir, PP_Settings.z) * PP_Settings.y
                                        : mix(PP_GroundColor.rgb, PP_SkyColor.rgb, clamp(Dir.y * 0.5 + 0.5, 0.0, 1.0));
    Output = vec4(Radiance, 1.0);
}
