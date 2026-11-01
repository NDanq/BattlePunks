#version 450 core
// PPBloomDownsample.fsh -- Bloom, down the chain: a 13-tap filter into a
// target half the size of its source (Jimenez, "Next Generation Post
// Processing in Call of Duty"). The first step reads the scene, adds back
// the emissive the 8-bit scene colour clipped (GBuffer3, EncodeGBufferEmissive),
// and keeps only what passes the threshold, with a soft knee so the glow does
// not switch on hard. Every level is stored EncodeBloom'd, so it can hold
// brightness past 1.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)     // the scene, or the level above (EncodeBloom)
MATERIAL_SAMPLER2D(5, Emissive)   // GBuffer3, for the first step

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Bloom;   // x threshold, y knee, z 1: the first step, w 1: Emissive is this frame's
    vec4 PP_Sizes;   // xy one SOURCE texel, view UV; zw the largest UV Source may be sampled at (its drawn area)
UBO_BLOCK_END(PP_Block)

vec3 Tap(vec2 InUV)
{
    vec2 At    = min(InUV * Scene_Spare[3].xy, PP_Sizes.zw);   // Scene_ViewUv, with this level's own edge
    vec3 Color = texture(Source, At).rgb;
    if(PP_Bloom.z < 0.5)
    {
        return DecodeBloom(Color);
    }
    if(PP_Bloom.w > 0.5)
    {
        Color += max(DecodeGBufferEmissive(texture(Emissive, At)) - 1.0, vec3(0.0));
    }
    return Color;
}

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
    Output = vec4(EncodeBloom(Color), 1.0);
}
