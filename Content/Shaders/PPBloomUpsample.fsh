#version 440 core
// PPBloomUpsample.fsh -- Bloom, back up the chain: a 9-tap tent of a smaller
// level, added (blended One + One) into the next larger one, scaled by the
// radius -- how much each wider level adds.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Bloom;   // x radius (the weight added)
    vec4 PP_Sizes;   // xy one SOURCE texel
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2 UV = Interpolants.TexCoord;
    vec2 T  = PP_Sizes.xy;
    vec3 Color = texture(Source, UV).rgb * 4.0
               + (texture(Source, UV + vec2(T.x, 0.0)).rgb + texture(Source, UV - vec2(T.x, 0.0)).rgb
                + texture(Source, UV + vec2(0.0, T.y)).rgb + texture(Source, UV - vec2(0.0, T.y)).rgb) * 2.0
               + texture(Source, UV + T).rgb + texture(Source, UV - T).rgb
               + texture(Source, UV + vec2(T.x, -T.y)).rgb + texture(Source, UV + vec2(-T.x, T.y)).rgb;
    Output = vec4(Color / 16.0 * PP_Bloom.x, 1.0);
}
