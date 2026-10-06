#version 440 core
// PPMotionBlur.fsh -- camera motion blur: each pixel's world position (from
// the scene depth) projected with last frame's view-projection gives how far
// it moved on screen; the scene is averaged along that streak. Moving objects
// are not tracked -- only the camera's own motion blurs.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneColor)
MATERIAL_SAMPLER2D(5, SceneDepth)

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_PrevViewProjection;   // last frame's, GL clip space
    vec4 PP_Motion;               // x intensity, y max length (px), z samples
    vec4 PP_Sizes;                // xy one texel, zw resolution
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2  UV    = Interpolants.TexCoord;
    float Depth = texelFetch(SceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec2  Ndc   = Interpolants.Position.xy;

    vec4 Prev    = PP_PrevViewProjection * vec4(PP_WorldAt(Ndc, min(Depth, 0.9999)), 1.0);
    vec2 PrevNdc = Prev.w > 1e-5 ? Prev.xy / Prev.w : Ndc;

    // Where this pixel was, in UV (v runs down, NDC y up), scaled and limited in pixels.
    vec2  Velocity = (Ndc - PrevNdc) * vec2(0.5, -0.5) * PP_Motion.x;
    vec2  Pixels   = Velocity * PP_Sizes.zw;
    float Length   = length(Pixels);
    if(Length > PP_Motion.y) { Velocity *= PP_Motion.y / Length; }

    // Each pixel's taps start at a different fraction of a step (interleaved
    // gradient noise), so the streak is grain, not bands.
    int   Samples = int(clamp(PP_Motion.z, 2.0, 32.0));
    float Jitter  = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3  Sum     = vec3(0.0);
    for(int Index = 0; Index < Samples; ++Index)
    {
        float T = (float(Index) + Jitter) / float(Samples) - 0.5;
        Sum += texture(SceneColor, UV - Velocity * T).rgb;
    }
    Output = vec4(Sum / float(Samples), 1.0);
}
