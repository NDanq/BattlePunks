#version 450 core
// GrassLabVignette.fsh -- darkens the corners of the finished picture: the
// smallest custom render pass (WorldsLabGrassLab.cpp, VignettePass; the
// first example in Engine/Docs/Guides/CustomRenderPassGuide.md).

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Picture)          // input 0: the picture so far

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_Unused;                     // RenderPassParams::Matrix
    vec4 PP_Vignette;                   // RenderPassParams::A -- x: strength, y: radius
    vec4 PP_Unused2;                    // RenderPassParams::B
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2  UV     = Interpolants.TexCoord;
    vec3  Colour = texture(Picture, Scene_ViewUv(UV)).rgb;   // the view's drawn area (dynamic resolution)
    float Edge   = smoothstep(PP_Vignette.y, 1.0, length(UV - 0.5) * 1.41421);
    Output = vec4(Colour * (1.0 - PP_Vignette.x * Edge), 1.0);
}
