#version 450 core
// Tonemap.fsh -- the HDR scene colour (linear) to display space: exposure
// (auto, then the stack's), the tone map operator, then 2.2 gamma
// (RenderCore/RENDERPIPELINE.md, "HDR and tone mapping"). Runs first in the
// post chain, so every effect after it (and every PostProcess material) sees
// display space as before.

#include <PostProcessCommon.glsl>
#include <ToneMapping.glsl>

MATERIAL_SAMPLER2D(4, Source)   // slot 0: the scene colour
MATERIAL_SAMPLER2D(5, Adapted)  // slot 1: auto exposure's adapted log2 luminance, 1 x 1 (PPAdaptExposure.fsh)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Exposure;   // x: 1 with auto exposure; y: its key (the grey the scene's average is taken to);
                        // z, w: its least and most correction, stops
    vec4 PP_Sharpen;    // x: after TAA, how much its softening is sharpened back (0 none)
UBO_BLOCK_END(PP_Block)

float MaxOf(vec3 InC) { return max(InC.r, max(InC.g, InC.b)); }

// The scene colour here, sharpened against its four neighbours by PP_Sharpen.x --
// in a tone-weighted space (c / (1 + max(c))), so a bright edge does not ring.
vec3 SceneAt(vec2 InUV)
{
    vec3 C = max(texture(Source, InUV).rgb, vec3(0.0));
    if(PP_Sharpen.x <= 0.0) { return C; }
    vec2 Texel = 1.0 / vec2(textureSize(Source, 0));
    vec3 Sum = vec3(0.0);
    Sum += max(texture(Source, InUV + vec2(Texel.x, 0.0)).rgb, vec3(0.0));
    Sum += max(texture(Source, InUV - vec2(Texel.x, 0.0)).rgb, vec3(0.0));
    Sum += max(texture(Source, InUV + vec2(0.0, Texel.y)).rgb, vec3(0.0));
    Sum += max(texture(Source, InUV - vec2(0.0, Texel.y)).rgb, vec3(0.0));
    vec3 W  = C / (1.0 + MaxOf(C));
    vec3 Nb = Sum * 0.25;
    Nb = Nb / (1.0 + MaxOf(Nb));
    vec3 Sharp = clamp(W + (W - Nb) * PP_Sharpen.x, vec3(0.0), vec3(0.999));
    return Sharp / (1.0 - MaxOf(Sharp));
}

void main(void)
{
    // Auto exposure brings the scene's adapted average to the key, within its
    // range; the stack's Exposure (Scene_Output.y) compensates on top.
    float Auto = 1.0;
    if(PP_Exposure.x > 0.5)
    {
        float Average = texelFetch(Adapted, ivec2(0), 0).r;
        Auto = exp2(clamp(log2(max(PP_Exposure.y, 1e-4)) - Average, PP_Exposure.z, PP_Exposure.w));
    }
    vec3 Linear = SceneAt(Interpolants.TexCoord) * Scene_Output.y * Auto;
    vec3 Mapped = ToneMap(Linear, Scene_Output.z);
    Output = vec4(pow(Mapped, vec3(1.0 / 2.2)), 1.0);
}
