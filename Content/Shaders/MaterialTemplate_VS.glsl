#version 440 core
// MaterialTemplate_VS.glsl -- every material's vertex stage (RenderCore/MATERIALS.md).
// Cooked once per permutation by cook_material_graphs.py, with Material.glsl
// mapped to the material's <Name>.mtl.glsl (or MaterialDefault.mtl.glsl for
// the shared vertex shaders of materials without World Position Offset).
//
// Permutation defines, each 0 or 1:
//   MATERIAL_VERTEX_QUAD / _MESH / _FULLSCREEN / _INSTANCEDMESH / _SKINNEDMESH
//                                                which vertex input it reads
//   MATERIAL_HAS_OFFSET                          add CalcMaterialOffset()

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>
#include <Material.glsl>
#if MATERIAL_VERTEX_SKINNEDMESH
#include <Skinning.glsl>
#endif

// Quads and the full-screen pass: position and UV. Meshes (GDICreateMesh)
// add the normal at 8, the colour at 9 and the lightmap UVs (UV1) at 10.
// An instanced mesh (GDISetInstanceBuffer) adds each instance's transform at
// 2-5 and its colour at 7 (GDIBuffer.h); a skinned mesh, its bone indices at
// 11 and weights at 12 (Skinning.glsl).
ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)
#if MATERIAL_VERTEX_MESH || MATERIAL_VERTEX_INSTANCEDMESH || MATERIAL_VERTEX_SKINNEDMESH
ATTRIB(8, in, vec3, InNormal)
ATTRIB(9, in, vec4, InColor)
ATTRIB(10, in, vec2, InUV1)
#endif
#if MATERIAL_VERTEX_SKINNEDMESH
ATTRIB(11, in, vec4, InJoints)
ATTRIB(12, in, vec4, InWeights)
#endif
#if MATERIAL_VERTEX_INSTANCEDMESH
ATTRIB(2, in, mat4, InInstanceTransform)
ATTRIB(7, in, vec4, InInstanceColor)
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
#if MATERIAL_VERTEX_INSTANCEDMESH
    // The instance's transform inside the model's; its colour on the vertex colour. A
    // particle's flipbook frame rides in its bottom row -- frame, columns, rows (0: none),
    // spare in an affine transform -- read here and cleared before it is used.
    mat4 Instance        = InInstanceTransform;
    vec3 Flipbook        = vec3(Instance[0].w, Instance[1].w, Instance[2].w);
    Instance[0].w = 0.0;
    Instance[1].w = 0.0;
    Instance[2].w = 0.0;
    if(Flipbook.y >= 1.0 && Flipbook.z >= 1.0)
    {
        vec2 Cell       = vec2(mod(Flipbook.x, Flipbook.y), floor(Flipbook.x / Flipbook.y));
        Inputs.TexCoord = (InTexCoord + Cell) / Flipbook.yz;
    }
    mat4 Model           = Transform.Model * Instance;
#else
    mat4 Model           = Transform.Model;
#endif
#if MATERIAL_VERTEX_SKINNEDMESH
    // Posed in model space first; the entity's transform after.
    vec4 SkinRow0, SkinRow1, SkinRow2;
    SkinRows(InJoints, InWeights, SkinRow0, SkinRow1, SkinRow2);
    vec3 LocalPosition = SkinPoint(SkinRow0, SkinRow1, SkinRow2, InPosition);
    vec3 LocalNormal   = SkinDirection(SkinRow0, SkinRow1, SkinRow2, InNormal);
#else
    vec3 LocalPosition = InPosition;
#if MATERIAL_VERTEX_MESH || MATERIAL_VERTEX_INSTANCEDMESH
    vec3 LocalNormal   = InNormal;
#endif
#endif
    Inputs.WorldPosition = (Model * vec4(LocalPosition, 1.0)).xyz;
#if MATERIAL_VERTEX_MESH || MATERIAL_VERTEX_INSTANCEDMESH || MATERIAL_VERTEX_SKINNEDMESH
    // The inverse transpose, so a non-uniform Scale keeps normals perpendicular.
    Inputs.WorldNormal = normalize(transpose(inverse(mat3(Model))) * LocalNormal);
#if MATERIAL_VERTEX_INSTANCEDMESH
    Inputs.Color       = InColor * InInstanceColor;
#else
    Inputs.Color       = InColor;
#endif
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
#if MATERIAL_VERTEX_MESH || MATERIAL_VERTEX_INSTANCEDMESH || MATERIAL_VERTEX_SKINNEDMESH
    vec2 LightmapUV = InUV1;
#else
    vec2 LightmapUV = vec2(0.0);
#endif
    CalculateInterpolants(Clip.xyz, Inputs.TexCoord, Inputs.WorldNormal, Inputs.Color, World, ViewPosition.z, LightmapUV);
    gl_Position = Clip;
#endif
}
