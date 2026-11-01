#version 450 core
// PPDofPrepare.fsh -- Depth of Field, 1 of 3: at half resolution, the scene
// (a 4-tap average) and each pixel's circle of confusion from the scene depth,
// signed (negative near, positive far) and stored in alpha as 0.5 + CoC / 2.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneColor)   // full resolution
MATERIAL_SAMPLER2D(5, SceneDepth)   // full resolution, window depth

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Focus;   // x focus distance, y focus range, z max blur (px, full res), w near blur (0/1)
    vec4 PP_Sizes;   // xy one full-resolution texel, zw full resolution
UBO_BLOCK_END(PP_Block)

float CircleOfConfusion(vec2 InUV)
{
    ivec2 Texel = clamp(ivec2(InUV * PP_Sizes.zw), ivec2(0), ivec2(PP_Sizes.zw) - 1);
    float Depth = texelFetch(SceneDepth, Texel, 0).r;
    if(Depth >= 1.0) { return 1.0; }   // the sky: as far as it gets
    float Distance = length(PP_WorldAt(TargetUvToNdc(InUV), Depth) - Scene_CameraPosition.xyz);
    float Half     = PP_Focus.y * 0.5;
    float Far      = clamp((Distance - (PP_Focus.x + Half)) / PP_Focus.y, 0.0, 1.0);
    float Near     = clamp(((PP_Focus.x - Half) - Distance) / max(Half, 1.0), 0.0, 1.0) * PP_Focus.w;
    return Far - Near;
}

void main(void)
{
    vec2 UV  = Interpolants.TexCoord;
    vec2 Off = PP_Sizes.xy * 0.5;
    vec3 Color = (texture(SceneColor, Scene_ViewUv(UV + vec2(-Off.x, -Off.y))).rgb + texture(SceneColor, Scene_ViewUv(UV + vec2(Off.x, -Off.y))).rgb
                + texture(SceneColor, Scene_ViewUv(UV + vec2(-Off.x,  Off.y))).rgb + texture(SceneColor, Scene_ViewUv(UV + vec2(Off.x,  Off.y))).rgb) * 0.25;
    Output = vec4(Color, CircleOfConfusion(UV) * 0.5 + 0.5);
}
