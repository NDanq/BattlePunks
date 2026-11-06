#version 450 core

#include <Common.glsl>

ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)

void main(void)
{
    vec4 Position = ViewParameters.Projection * ViewParameters.View * Transform.Model * vec4(InPosition, 1.0);
    CalculateInterpolants(Position.rgb, InTexCoord);
    gl_Position = Position;
}