#version 450 core
// PPReflectionResolve.fsh -- screen-space reflections accumulated over frames
// (RenderCore/GLOBAL_ILLUMINATION.md, Phase 4; Stachowiak 2015): each frame's
// rough reflections are one ray a pixel drawn from GGX's visible normals
// (PPScreenReflections.fsh), noisy alone; blended here into a history,
// reprojected through the surface's depth and world motion with last frame's
// view (unjittered), and clipped to this frame's 3 x 3 neighbourhood as TAA's
// history is (Karis 2014), so a moving view does not smear. rgb: radiance; a: trust.
// Screen-space GI's half-size bounce (PPScreenGI.fsh) is resolved the same way:
// the depth and motion are read from the full-size targets under each pixel.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Current)      // this frame's reflections
MATERIAL_SAMPLER2D(5, History)      // the last resolve
MATERIAL_SAMPLER2D(6, SceneDepth)
MATERIAL_SAMPLER2D(7, Velocity)     // xyz: world motion since the last frame

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_PrevViewProjection;   // the last frame's view, unjittered
    vec4 PP_Resolve;              // x: 1 if History holds the last frame's; y: how much of the history to keep
    vec4 PP_Sizes;                // zw: this target's size
UBO_BLOCK_END(PP_Block)

void main(void)
{
    ivec2 Texel = ivec2(gl_FragCoord.xy);
    ivec2 Max   = ivec2(PP_Sizes.zw) - 1;
    vec4  Now   = texelFetch(Current, Texel, 0);
    if(PP_Resolve.x < 0.5)
    {
        Output = Now;
        return;
    }

    // This frame's neighbourhood: what the history may be.
    vec4 Low = Now, High = Now;
    for(int Y = -1; Y <= 1; ++Y)
    {
        for(int X = -1; X <= 1; ++X)
        {
            vec4 Each = texelFetch(Current, clamp(Texel + ivec2(X, Y), ivec2(0), Max), 0);
            Low  = min(Low, Each);
            High = max(High, Each);
        }
    }

    // Where this pixel's surface was last frame.
    ivec2 FullSize = ivec2(Scene_DrawnSize(vec2(textureSize(SceneDepth, 0))));   // what this frame drew of it
    ivec2 Full     = clamp(ivec2(Interpolants.TexCoord * vec2(FullSize)), ivec2(0), FullSize - 1);
    float Depth    = texelFetch(SceneDepth, Full, 0).r;
    if(Depth >= 1.0)
    {
        Output = Now;
        return;
    }
    vec3 World    = PP_WorldAt(TargetUvToNdc(Interpolants.TexCoord), Depth);
    vec3 Motion   = texelFetch(Velocity, Full, 0).xyz;
    vec4 PrevClip = PP_PrevViewProjection * vec4(World - Motion, 1.0);
    vec2 PrevUV   = PrevClip.w > 1e-4 ? NdcToTargetUv(PrevClip.xy / PrevClip.w) : vec2(-1.0);
    if(any(lessThan(PrevUV, vec2(0.0))) || any(greaterThan(PrevUV, vec2(1.0))))
    {
        Output = Now;
        return;
    }
    bool bHalf = textureSize(History, 0).x * 2 <= textureSize(SceneDepth, 0).x + 1;   // the half-size resolves
    vec4 Past  = clamp(texture(History, bHalf ? Scene_PrevViewUvHalf(PrevUV) : Scene_PrevViewUv(PrevUV)), Low, High);
    Output = mix(Now, Past, PP_Resolve.y);
}
