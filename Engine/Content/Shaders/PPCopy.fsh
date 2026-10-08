#version 450 core
// PPCopy.fsh -- a texture's texels into a target the same size, as they are:
// the scene colour kept as the next frame's last colour, when TAA's history is
// not there to be it (screen-space reflections read it; RENDERPIPELINE.md).

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)

void main(void)
{
    Output = vec4(texelFetch(Source, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
}
