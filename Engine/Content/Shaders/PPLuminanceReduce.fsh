#version 450 core
// PPLuminanceReduce.fsh -- auto exposure's meter, reduced: each texel the mean
// of a PP_Reduce.x-texel square of the target before (64 x 64 -> 8 x 8 -> 1 x 1),
// both the weighted log luminance and the weight (PPLuminance.fsh).

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Source)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Reduce;   // x: the square's side, texels
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    int   Side = int(clamp(PP_Reduce.x, 1.0, 16.0));
    ivec2 Base = ivec2(gl_FragCoord.xy) * Side;
    vec2  Sum  = vec2(0.0);
    for(int Y = 0; Y < Side; ++Y)
    {
        for(int X = 0; X < Side; ++X)
        {
            Sum += texelFetch(Source, Base + ivec2(X, Y), 0).rg;
        }
    }
    Output = vec4(Sum / float(Side * Side), 0.0, 1.0);
}
