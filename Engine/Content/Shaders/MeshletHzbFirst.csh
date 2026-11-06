#version 450
// MeshletHzbFirst.csh -- a depth pyramid's first level (RenderCore/
// RENDERPIPELINE.md, "Meshlets"): half the scene depth's drawn size each way,
// rounded up, each texel the farthest of the 2 x 2 depths under it -- and, in
// the last column or row of an odd size, the one after too, so every depth is
// under some texel. MeshletHzbDown.csh makes each level after it.

#include <ComputeCommon.glsl>

layout(local_size_x = 8, local_size_y = 8) in;

COMPUTE_PARAMS_BEGIN(MeshletHzbParams)
    uvec4 Mh_Source;        // the depth's drawn width, height
    uvec4 Mh_Destination;   // the level's width, height, offset (floats)
COMPUTE_PARAMS_END(MeshletHzbParams)

STORAGE_BUFFER(1, MeshletPyramid) { float Pyramid[]; };
COMPUTE_TEXTURE(SceneDepth);

void main()
{
    uvec2 At = gl_GlobalInvocationID.xy;
    if(At.x >= Mh_Destination.x || At.y >= Mh_Destination.y)
    {
        return;
    }
    ivec2 Last  = ivec2(Mh_Source.xy) - 1;
    ivec2 Reach = ivec2(At.x + 1u == Mh_Destination.x && (Mh_Source.x & 1u) != 0u ? 3 : 2,
                        At.y + 1u == Mh_Destination.y && (Mh_Source.y & 1u) != 0u ? 3 : 2);
    float Far   = 0.0;
    for(int Y = 0; Y < Reach.y; ++Y)
    {
        for(int X = 0; X < Reach.x; ++X)
        {
            ivec2 Texel = min(ivec2(At) * 2 + ivec2(X, Y), Last);
            Far = max(Far, texelFetch(SceneDepth, Texel, 0).r);
        }
    }
    Pyramid[Mh_Destination.z + At.y * Mh_Destination.x + At.x] = Far;
}
