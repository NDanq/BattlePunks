#version 450 core
// PPLuminance.fsh -- auto exposure's meter, its first step (RenderCore/
// RENDERPIPELINE.md, "Auto exposure"): the HDR scene colour, linear, into a
// 64 x 64 target. Each texel is the mean log2 luminance of a 4 x 4 grid over
// the patch of the scene it covers, weighted toward the middle of the view
// (centre-weighted metering): r the weighted log luminance, g the weight.
// The reductions after it average both, and the adaptation divides, so the
// weighting survives them.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)   // the scene colour, linear

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Meter;   // x: centre weighting, 0 (even) to 1
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2  Centre = Interpolants.TexCoord;
    float Sum = 0.0;
    for(int Y = 0; Y < 4; ++Y)
    {
        for(int X = 0; X < 4; ++X)
        {
            vec2  UV = Centre + (vec2(float(X), float(Y)) - 1.5) * (1.0 / 256.0);   // a quarter of the cell apart
            vec3  C  = max(texture(Source, UV).rgb, vec3(0.0));
            float L  = dot(C, vec3(0.2126, 0.7152, 0.0722));
            Sum += log2(max(L, 1e-4));
        }
    }
    vec2  FromMiddle = (Centre - 0.5) * 2.0;
    float Weight = mix(1.0, max(1.0 - dot(FromMiddle, FromMiddle) * 0.5, 0.05), clamp(PP_Meter.x, 0.0, 1.0));
    Output = vec4((Sum / 16.0) * Weight, Weight, 0.0, 1.0);
}
