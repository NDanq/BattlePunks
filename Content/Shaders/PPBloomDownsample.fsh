#version 440 core
// PPBloomDownsample.fsh -- Bloom, down the chain: a 13-tap filter into a
// target half the size of its source (Jimenez, "Next Generation Post
// Processing in Call of Duty"). The first step also keeps only what passes
// the threshold, with a soft knee so the glow does not switch on hard.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Bloom;   // x threshold, y knee, z 1: apply the threshold
    vec4 PP_Sizes;   // xy one SOURCE texel
UBO_BLOCK_END(PP_Block)

vec3 Tap(vec2 InUV) { return texture(Source, InUV).rgb; }

void main(void)
{
    vec2 UV = Interpolants.TexCoord;
    vec2 T  = PP_Sizes.xy;
    vec3 A = Tap(UV + T * vec2(-2.0, -2.0)), B = Tap(UV + T * vec2(0.0, -2.0)), C = Tap(UV + T * vec2(2.0, -2.0));
    vec3 D = Tap(UV + T * vec2(-1.0, -1.0)), E = Tap(UV + T * vec2(1.0, -1.0));
    vec3 F = Tap(UV + T * vec2(-2.0,  0.0)), G = Tap(UV),                    H = Tap(UV + T * vec2(2.0,  0.0));
    vec3 I = Tap(UV + T * vec2(-1.0,  1.0)), J = Tap(UV + T * vec2(1.0,  1.0));
    vec3 K = Tap(UV + T * vec2(-2.0,  2.0)), L = Tap(UV + T * vec2(0.0,  2.0)), M = Tap(UV + T * vec2(2.0,  2.0));
    vec3 Color = (D + E + I + J) * 0.125 + (A + B + G + F) * 0.03125 + (B + C + H + G) * 0.03125
               + (F + G + L + K) * 0.03125 + (G + H + M + L) * 0.03125;

    if(PP_Bloom.z > 0.5)
    {
        float Bright = max(Color.r, max(Color.g, Color.b));
        float Knee   = max(PP_Bloom.x * PP_Bloom.y, 1e-4);
        float Soft   = clamp(Bright - PP_Bloom.x + Knee, 0.0, 2.0 * Knee);
        Soft         = Soft * Soft / (4.0 * Knee);
        float Keep   = max(Soft, Bright - PP_Bloom.x) / max(Bright, 1e-4);
        Color       *= Keep;
    }
    Output = vec4(Color, 1.0);
}
