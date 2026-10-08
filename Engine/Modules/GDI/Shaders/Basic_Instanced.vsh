#version 450

#include <Common.glsl>

ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec3, TexCoord)
ATTRIB(2, in, mat4, InstancedTransform)

void main() {
    vec4 Position = ViewParameters.Projection * ViewParameters.View * InstancedTransform * vec4(InPosition, 1.0);
    CalculateInterpolants(Position.rgb, TexCoord.xy);
    gl_Position = Position;
}
