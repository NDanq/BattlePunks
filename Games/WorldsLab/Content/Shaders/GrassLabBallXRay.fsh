#version 450 core
// GrassLabBallXRay.fsh -- where the grass hides the ball, its silhouette shows
// through, tinted (WorldsLabGrassLab.cpp, BallXRayPass): the ball's mask and
// its own depth (GrassLabBallMask.fsh) against the scene's depth, alpha
// blended over the scene colour. Elsewhere alpha 0: nothing changes.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Mask)
MATERIAL_SAMPLER2D(5, MaskDepth)
MATERIAL_SAMPLER2D(11, SceneDepth)

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_Unused;
    vec4 PP_Tint;       // rgb: the silhouette's colour (linear), a: its opacity
    vec4 PP_Edge;       // x: the rim's extra opacity
UBO_BLOCK_END(PP_Block)

float Hidden(ivec2 InPixel)
{
    if(texelFetch(Mask, InPixel, 0).r < 0.5) { return 0.0; }
    return texelFetch(MaskDepth, InPixel, 0).r > texelFetch(SceneDepth, InPixel, 0).r + 0.00002 ? 1.0 : 0.0;
}

void main(void)
{
    ivec2 Pixel = ivec2(gl_FragCoord.xy);
    float Here  = Hidden(Pixel);
    if(Here < 0.5)
    {
        Output = vec4(0.0);
        return;
    }
    // A rim where the mask ends, so the shape reads through dense grass.
    float Edge = 0.0;
    for(int Y = -2; Y <= 2; Y += 2)
    {
        for(int X = -2; X <= 2; X += 2)
        {
            Edge = max(Edge, 1.0 - step(0.5, texelFetch(Mask, Pixel + ivec2(X, Y), 0).r));
        }
    }
    Output = vec4(PP_Tint.rgb, clamp(PP_Tint.a + Edge * PP_Edge.x, 0.0, 1.0));
}
