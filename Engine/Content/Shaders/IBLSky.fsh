#version 450 core
// IBLSky.fsh -- the sky as a cube strip (6S x S, +X -X +Y -Y +Z -Z), for the
// atlas's sky block (IBLCommon.glsl): the Sky Light's HDRI if it has one,
// else its sky and ground colours, linear, times their intensity.

#include <PostProcessCommon.glsl>
#include <IBLCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_SkyColor;      // rgb linear, times the intensity
    vec4 PP_GroundColor;
    vec4 PP_Unused0;
    vec4 PP_Unused1;
    vec4 PP_Settings;      // x: 1 with an HDRI; y: its intensity; z: its rotation (radians); w: a face's side
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, Environment)   // slot 0: the HDRI (RGBE8)

void main(void)
{
    float Size = PP_Settings.w;
    int   Face = int(gl_FragCoord.x / Size);
    vec2  Uv   = vec2(gl_FragCoord.x - float(Face) * Size, gl_FragCoord.y) / Size;
    vec3  Forward, Right, Up;
    IBL_FaceBasis(Face, Forward, Right, Up);
    vec2  Ndc  = TargetUvToNdc(Uv);   // row 0 is the face's top
    vec3  Dir  = normalize(Forward + Right * Ndc.x + Up * Ndc.y);

    vec3 Radiance;
    if(PP_Settings.x > 0.5)
    {
        Radiance = IBL_SampleEquirect(Environment, Dir, PP_Settings.z) * PP_Settings.y;
    }
    else
    {
        Radiance = mix(PP_GroundColor.rgb, PP_SkyColor.rgb, clamp(Dir.y * 0.5 + 0.5, 0.0, 1.0));
    }
    Output = vec4(Radiance, 1.0);
}
