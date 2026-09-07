#version 440 core

#include <Common.glsl>

ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)

void main(void)
{
    CalculateInterpolants(InPosition, InTexCoord);
    gl_Position = vec4(InPosition, 1.0);
}