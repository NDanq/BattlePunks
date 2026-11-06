#version 450 core
// PPDepthHalf.fsh -- the opaque depth at half size for forward reflections' march
// (ForwardReflections.glsl; LIT_TRANSLUCENCY_PERF.md): each texel the nearest of the
// four under it, so a thin edge still stops a ray. Into an R32F target.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)   // the full-size depth, window depth

void main(void)
{
    ivec2 Full = ivec2(gl_FragCoord.xy) * 2;
    ivec2 Last = textureSize(Source, 0) - 1;
    float Near = texelFetch(Source, min(Full, Last), 0).r;
    Near = min(Near, texelFetch(Source, min(Full + ivec2(1, 0), Last), 0).r);
    Near = min(Near, texelFetch(Source, min(Full + ivec2(0, 1), Last), 0).r);
    Near = min(Near, texelFetch(Source, min(Full + ivec2(1, 1), Last), 0).r);
    Output = vec4(Near, 0.0, 0.0, 1.0);
}
