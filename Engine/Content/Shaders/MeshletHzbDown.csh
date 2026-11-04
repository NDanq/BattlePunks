#version 450
// MeshletHzbDown.csh -- a depth pyramid's next level (RenderCore/
// RENDERPIPELINE.md, "Meshlets"): half the level before each way, rounded up,
// each texel the farthest of its 2 x 2 there -- and, in the last column or row
// of an odd size, the one after too.

#include <ComputeCommon.glsl>

layout(local_size_x = 8, local_size_y = 8) in;

COMPUTE_PARAMS_BEGIN(MeshletHzbParams)
    uvec4 Mh_Source;        // the level before's width, height, offset (floats)
    uvec4 Mh_Destination;   // this level's width, height, offset (floats)
COMPUTE_PARAMS_END(MeshletHzbParams)

STORAGE_BUFFER(1, MeshletPyramid) { float Pyramid[]; };

void main()
{
    uvec2 At = gl_GlobalInvocationID.xy;
    if(At.x >= Mh_Destination.x || At.y >= Mh_Destination.y)
    {
        return;
    }
    uvec2 Last  = Mh_Source.xy - 1u;
    uvec2 Reach = uvec2(At.x + 1u == Mh_Destination.x && (Mh_Source.x & 1u) != 0u ? 3u : 2u,
                        At.y + 1u == Mh_Destination.y && (Mh_Source.y & 1u) != 0u ? 3u : 2u);
    float Far   = 0.0;
    for(uint Y = 0u; Y < Reach.y; ++Y)
    {
        for(uint X = 0u; X < Reach.x; ++X)
        {
            uvec2 Texel = min(At * 2u + uvec2(X, Y), Last);
            Far = max(Far, Pyramid[Mh_Source.z + Texel.y * Mh_Source.x + Texel.x]);
        }
    }
    Pyramid[Mh_Destination.z + At.y * Mh_Destination.x + At.x] = Far;
}
