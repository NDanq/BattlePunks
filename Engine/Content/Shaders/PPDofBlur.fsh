#version 450 core
// PPDofBlur.fsh -- Depth of Field, 2 of 3: a 16-tap disc at half resolution,
// as wide as the pixel's own circle of confusion. A tap counts only as far as
// its own circle reaches back to this pixel, so sharp foreground does not
// smear over a blurred background.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Prepared)    // PPDofPrepare's output: rgb, CoC in alpha

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Focus;   // z max blur (px, full res)
    vec4 PP_Sizes;   // xy one half-resolution texel
UBO_BLOCK_END(PP_Block)

const vec2 Disc[16] = vec2[16](
    vec2( 0.000,  0.000), vec2( 0.527,  0.085), vec2(-0.040,  0.536), vec2(-0.670, -0.179),
    vec2(-0.419, -0.616), vec2( 0.440, -0.639), vec2(-0.757,  0.349), vec2( 0.574,  0.685),
    vec2( 0.039, -0.986), vec2( 0.976,  0.140), vec2(-0.231,  0.957), vec2(-0.998, -0.054),
    vec2( 0.695, -0.719), vec2(-0.724, -0.689), vec2( 0.272,  0.962), vec2( 0.910, -0.415));

void main(void)
{
    vec2  UV     = Interpolants.TexCoord;
    vec4  Centre = texture(Prepared, UV);
    float Coc    = abs(Centre.a * 2.0 - 1.0);
    float Radius = Coc * PP_Focus.z * 0.5;   // half-resolution pixels

    vec3  Sum    = vec3(0.0);
    float Weight = 0.0;
    for(int Index = 0; Index < 16; ++Index)
    {
        vec2  Offset  = Disc[Index] * Radius;
        vec4  Tap     = texture(Prepared, UV + Offset * PP_Sizes.xy);
        float TapCoc  = abs(Tap.a * 2.0 - 1.0) * PP_Focus.z * 0.5;
        float Reaches = clamp(TapCoc - length(Offset) + 1.0, 0.0, 1.0);
        float W       = Index == 0 ? 1.0 : Reaches;
        Sum    += Tap.rgb * W;
        Weight += W;
    }
    Output = vec4(Sum / max(Weight, 1e-4), Centre.a);
}
