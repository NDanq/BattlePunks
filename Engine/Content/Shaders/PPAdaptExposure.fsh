#version 440 core
// PPAdaptExposure.fsh -- auto exposure's adaptation (RenderCore/RENDERPIPELINE.md,
// "Auto exposure"): last frame's adapted log luminance eased toward this frame's
// metered one, faster toward a brighter scene than a darker (as an eye adapts),
// into a 1 x 1 the tone map reads. r: the adapted log2 luminance.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Measured)   // 1 x 1: weighted log luminance, weight (PPLuminanceReduce.fsh)
MATERIAL_SAMPLER2D(5, Previous)   // 1 x 1: last frame's adapted (this shader's)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Adapt;   // x: the frame's seconds; y: speed toward brighter, z: toward darker (per second);
                     // w: 1 if Previous holds last frame's (else snap to the metered)
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2  Metered = texelFetch(Measured, ivec2(0), 0).rg;
    float Target  = Metered.r / max(Metered.g, 1e-4);
    if(PP_Adapt.w < 0.5)
    {
        Output = vec4(Target, 0.0, 0.0, 1.0);
        return;
    }
    float Before = texelFetch(Previous, ivec2(0), 0).r;
    float Speed  = Target > Before ? PP_Adapt.y : PP_Adapt.z;
    float Rate   = 1.0 - exp(-max(PP_Adapt.x, 0.0) * max(Speed, 0.0));
    Output = vec4(Before + (Target - Before) * Rate, 0.0, 0.0, 1.0);
}
