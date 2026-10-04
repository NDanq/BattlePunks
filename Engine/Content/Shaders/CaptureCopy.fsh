#version 440 core
// CaptureCopy.fsh -- a scene capture's face, copied into its render target
// (RenderContext::FinishTargetFace; RenderCore/RENDERPIPELINE.md, "Scene
// capture"): the viewport is the face's place, so TexCoord spans the face.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)   // the face's final colour

void main(void)
{
    Output = vec4(texture(Source, Interpolants.TexCoord).rgb, 1.0);
}
