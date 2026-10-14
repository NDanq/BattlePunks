#version 450
// GrassLabTrailStats.csh -- how far the ball has rolled, kept on the GPU
// (WorldsLabGrassLab.cpp, TrailStatsPass): one thread a frame adds the step
// from where the ball was to where it is now. "GrassLab.Stats 1" reads it back.

#include <ComputeCommon.glsl>

layout(local_size_x = 1) in;

COMPUTE_PARAMS_BEGIN(StatsParams)
    vec4 Stats_Ball;    // xyz: the ball's centre this frame; w: 1 to start over
COMPUTE_PARAMS_END(StatsParams)

// Last: xyz where it was last frame, w 1 once set. Totals: x the distance (world units), y frames counted.
STORAGE_BUFFER(1, StatsData) { vec4 Last; vec4 Totals; };

void main()
{
    if(Stats_Ball.w > 0.5)
    {
        Last   = vec4(0.0);
        Totals = vec4(0.0);
    }
    vec3 Now = Stats_Ball.xyz;
    if(Last.w > 0.5) { Totals.x += distance(Now, Last.xyz); }
    Totals.y += 1.0;
    Last = vec4(Now, 1.0);
}
