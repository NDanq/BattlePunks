#version 450 core
// PPAmbientApply.fsh -- screen-space ambient occlusion folded into the G-buffer
// (RENDERPIPELINE.md, "Ambient occlusion"): drawn over GBuffer0 with a
// multiplying blend (zero, source colour), so its colour stays and its alpha --
// the material's Ambient Occlusion -- is multiplied by the screen's. The
// Lighting and IBL passes then read one occlusion, and need no texture slot
// of their own for it.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Occlusion)   // PPAmbientOcclusion's (or PPAmbientBlur's), half resolution

void main(void)
{
    Output = vec4(1.0, 1.0, 1.0, texture(Occlusion, Scene_ViewUvHalf(Interpolants.TexCoord)).r);
}
