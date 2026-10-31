#version 450 core
// @PLUGIN_NAME@Tint.fsh -- @PLUGIN_NAME@'s pass (@PLUGIN_NAME@Pass.cpp): the
// finished picture, warmed toward sepia by PP_Tint.x. Named for the plugin:
// a custom pass finds its shader by file name, and every plugin's shaders
// share one namespace. Cook it with NimbleCook shaders -project <game>.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Picture)          // input 0: the picture so far

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_Unused;                     // RenderPassParams::Matrix
    vec4 PP_Tint;                       // RenderPassParams::A -- x: strength, 0 to 1
    vec4 PP_Unused2;                    // RenderPassParams::B
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec3  Colour = texture(Picture, Interpolants.TexCoord).rgb;
    float Grey   = dot(Colour, vec3(0.299, 0.587, 0.114));
    vec3  Sepia  = vec3(Grey * 1.07, Grey * 0.74, Grey * 0.43);
    Output = vec4(mix(Colour, Sepia, clamp(PP_Tint.x, 0.0, 1.0)), 1.0);
}
