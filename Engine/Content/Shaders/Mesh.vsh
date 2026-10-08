#version 450 core

#include <Common.glsl>

// An indexed mesh (GDICreateMesh): position, UV and a per-vertex normal at
// location 8 -- 2-7 belong to the instanced quads.
ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)
ATTRIB(8, in, vec3, InNormal)

void main(void)
{
    vec4 Position = ViewParameters.Projection * ViewParameters.View * Transform.Model * vec4(InPosition, 1.0);

    // The inverse transpose, so a non-uniform Scale keeps normals perpendicular.
    vec3 Normal = normalize(transpose(inverse(mat3(Transform.Model))) * InNormal);

    CalculateInterpolants(Position.rgb, InTexCoord, Normal);
    gl_Position = Position;
}
