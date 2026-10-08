#version 450 core
// PPBloomUpsample.fsh -- Bloom, back up the chain: a level's own downsample
// plus a 9-tap tent of the level below it (already summed up the chain),
// scaled by the radius -- how much each wider level adds. Written to a second
// chain, not blended in: the levels are EncodeBloom'd, which does not add.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)   // the smaller level, summed (EncodeBloom)
MATERIAL_SAMPLER2D(5, Level)    // this level's downsample (EncodeBloom)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Bloom;   // x radius (the weight added)
    vec4 PP_Sizes;   // xy one SOURCE texel
UBO_BLOCK_END(PP_Block)

vec3 Tap(vec2 InUV) { return DecodeBloom(texture(Source, InUV).rgb); }

void main(void)
{
    vec2 UV = Interpolants.TexCoord;
    vec2 T  = PP_Sizes.xy;
    vec3 Wide = Tap(UV) * 4.0
              + (Tap(UV + vec2(T.x, 0.0)) + Tap(UV - vec2(T.x, 0.0)) + Tap(UV + vec2(0.0, T.y)) + Tap(UV - vec2(0.0, T.y))) * 2.0
              + Tap(UV + T) + Tap(UV - T) + Tap(UV + vec2(T.x, -T.y)) + Tap(UV + vec2(-T.x, T.y));
    vec3 Color = DecodeBloom(texture(Level, UV).rgb) + Wide / 16.0 * PP_Bloom.x;
    Output = vec4(EncodeBloom(Color), 1.0);
}
