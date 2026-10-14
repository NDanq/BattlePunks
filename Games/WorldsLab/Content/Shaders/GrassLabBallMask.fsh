#version 450 core
// GrassLabBallMask.fsh -- the ball's mesh group (WorldsLabGrassLab.cpp, BallXRayPass):
// drawn with its material's vertex shader into a mask of its own, with its
// own depth. 1 wherever the ball is.

#include <MaterialPlatform.glsl>
#include <Common.glsl>

OUTPUT(0, vec4, Output)

void main(void)
{
    Output = vec4(1.0);
}
