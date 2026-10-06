#version 440 core
// PPAmbientBlur.fsh -- ambient occlusion's blur: a 4 x 4 average at half
// resolution, each tap weighted by how near its depth is to this pixel's, so
// occlusion does not bleed across a silhouette onto what is behind it.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Occlusion)    // PPAmbientOcclusion's output, half resolution
MATERIAL_SAMPLER2D(5, SceneDepth)   // full resolution, window depth

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Ao;      // y radius
    vec4 PP_Sizes;   // xy one half-resolution texel, zw full resolution
UBO_BLOCK_END(PP_Block)

float DistanceAt(vec2 InUV)
{
    ivec2 Texel = clamp(ivec2(InUV * PP_Sizes.zw), ivec2(0), ivec2(PP_Sizes.zw) - 1);
    float Depth = texelFetch(SceneDepth, Texel, 0).r;
    return length(PP_WorldAt(TargetUvToNdc(InUV), Depth) - Scene_CameraPosition.xyz);
}

void main(void)
{
    vec2  UV     = Interpolants.TexCoord;
    float Centre = DistanceAt(UV);
    float Sum    = 0.0;
    float Weight = 0.0;
    for(int Y = -2; Y < 2; ++Y)
    {
        for(int X = -2; X < 2; ++X)
        {
            vec2  Tap = UV + (vec2(X, Y) + 0.5) * PP_Sizes.xy;
            float W   = 1.0 / (1.0 + abs(DistanceAt(Tap) - Centre) / max(PP_Ao.y * 0.25, 1.0));
            Sum    += texture(Occlusion, Tap).r * W;
            Weight += W;
        }
    }
    Output = vec4(vec3(Sum / max(Weight, 1e-4)), 1.0);
}
