#version 450 core
// DDGICopy.fsh -- a GI Volume probe's cube face into the volume's strip
// (DDGI.glsl; RenderCore/GLOBAL_ILLUMINATION.md, Phase 6), as CaptureCopy.fsh
// copies a reflection probe's: rgb the face's linear scene colour, a the
// distance from the probe (the face's camera) to what each texel sees -- the
// volume's longest distance where it sees the sky. The viewport is the face's
// place, so TexCoord spans the face; the scene block still holds its view.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)       // the face's scene colour, linear
MATERIAL_SAMPLER2D(5, SceneDepth)   // the face's depth

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // x: the volume's longest distance, world units
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2  Uv      = Interpolants.TexCoord;
    ivec2 Size    = textureSize(SceneDepth, 0);
    float Depth   = texelFetch(SceneDepth, clamp(ivec2(Uv * vec2(Size)), ivec2(0), Size - 1), 0).r;
    float Longest = max(PP_Settings.x, 1.0);
    float Reach   = Depth >= 1.0 ? Longest : min(length(PP_WorldAt(TargetUvToNdc(Uv), Depth) - Scene_CameraPosition.xyz), Longest);
    Output = vec4(max(texture(Source, Uv).rgb, vec3(0.0)), Reach);
}
