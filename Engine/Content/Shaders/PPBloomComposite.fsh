#version 440 core
// PPBloomComposite.fsh -- Bloom, the end: the scene plus the half-resolution
// glow (the top of the chain, EncodeBloom'd), times intensity and tint.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneColor)
MATERIAL_SAMPLER2D(5, Glow)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Bloom;   // rgb tint, a intensity
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2 UV = Interpolants.TexCoord;
    vec3 Color = texture(SceneColor, UV).rgb + DecodeBloom(texture(Glow, UV).rgb) * PP_Bloom.rgb * PP_Bloom.a;
    Output = vec4(Color, 1.0);
}
