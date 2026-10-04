#version 440 core
// MaterialTemplate_VS.glsl -- every material's vertex stage (RenderCore/MATERIALS.md).
// Cooked once per permutation by cook_material_graphs.py, with Material.glsl
// mapped to the material's <Name>.mtl.glsl (or MaterialDefault.mtl.glsl for
// the shared vertex shaders of materials without World Position Offset).
//
// Permutation defines, each 0 or 1:
//   MATERIAL_VERTEX_QUAD / _MESH / _FULLSCREEN   which vertex input it reads
//   MATERIAL_HAS_OFFSET                          add CalcMaterialOffset()

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>
#include <Material.glsl>

// Quads and the full-screen pass: position and UV. Meshes (GDICreateMesh)
// add the normal at 8, the colour at 9 and the lightmap UVs (UV1) at 10 --
// 2-7 belong to instanced quads.
ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)
#if MATERIAL_VERTEX_MESH
ATTRIB(8, in, vec3, InNormal)
ATTRIB(9, in, vec4, InColor)
ATTRIB(10, in, vec2, InUV1)
#endif

void main(void)
{
#if MATERIAL_VERTEX_FULLSCREEN
    // Already in NDC, like Screen.vsh.
    CalculateInterpolants(InPosition, InTexCoord, vec3(0.0, 0.0, -1.0), vec4(1.0), InPosition, 0.0);
    gl_Position = vec4(InPosition, 1.0);
#else
    MaterialInputs Inputs;
    Inputs.TexCoord      = InTexCoord;
    Inputs.WorldPosition = (Transform.Model * vec4(InPosition, 1.0)).xyz;
#if MATERIAL_VERTEX_MESH
    // The inverse transpose, so a non-uniform Scale keeps normals perpendicular.
    Inputs.WorldNormal = normalize(transpose(inverse(mat3(Transform.Model))) * InNormal);
    Inputs.Color       = InColor;
#else
    // A quad faces -Z: its transformed Y cross X, which survives any scale -- even a flat Z.
    Inputs.WorldNormal = normalize(cross(Transform.Model[1].xyz, Transform.Model[0].xyz));
    Inputs.Color       = vec4(1.0);
#endif

    vec3 World = Inputs.WorldPosition;
#if MATERIAL_HAS_OFFSET
    World += CalcMaterialOffset(Inputs);
#endif

    // View row 2 is forward (LookAt), so view-space z is the depth in front of the eye.
    vec4 ViewPosition = ViewParameters.View * vec4(World, 1.0);
    vec4 Clip         = ViewParameters.Projection * ViewPosition;
    // As is: the shared vertex shaders have no material block, so the pixel
    // stage takes it into the draw's atlas square (BakedLighting.glsl).
#if MATERIAL_VERTEX_MESH
    vec2 LightmapUV = InUV1;
#else
    vec2 LightmapUV = vec2(0.0);
#endif
    CalculateInterpolants(Clip.xyz, InTexCoord, Inputs.WorldNormal, Inputs.Color, World, ViewPosition.z, LightmapUV);
    gl_Position = Clip;
#endif
}
