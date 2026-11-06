#version 450 core

#include <Common.glsl>

OUTPUT(0, vec4, Output)

UBO_BEGIN(2, Mesh_Block)
    SHADER_UNIFORM(vec3, Tint)
UBO_END(Mesh_Block)

// A mesh with no material: its Tint, lit from above (-Y, since +Y is down).
void main(void)
{
    vec3  Normal  = normalize(GetInterpolants().Normal);
    vec3  ToLight = normalize(vec3(0.35, -1.0, -0.55));
    float Diffuse = max(dot(Normal, ToLight), 0.0);
    Output = vec4(Tint * (0.3 + 0.7 * Diffuse), 1.0);
}
