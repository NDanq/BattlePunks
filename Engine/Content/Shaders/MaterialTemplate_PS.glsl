#version 440 core
// MaterialTemplate_PS.glsl -- every material's pixel stage (RenderCore/MATERIALS.md).
// Cooked once per permutation by cook_material_graphs.py, with Material.glsl
// mapped to the material's <Name>.mtl.glsl.
//
// Permutation defines, each 0 or 1:
//   MATERIAL_DOMAIN_OPAQUE / _MASKED / _TRANSLUCENT / _POSTPROCESS
//   MATERIAL_SHADING_LIT / _UNLIT

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>
#include <Material.glsl>
#if MATERIAL_SHADING_LIT
#include <Lighting.glsl>
#endif

OUTPUT(0, vec4, Output)

void main(void)
{
    MaterialInputs Inputs;
    Inputs.TexCoord      = Interpolants.TexCoord;
    Inputs.Color         = Interpolants.Color;
    Inputs.WorldPosition = Interpolants.WorldPosition;
    Inputs.WorldNormal   = normalize(Interpolants.Normal);
    Inputs.ClipPosition  = Interpolants.Position;
    Inputs.FragCoord     = gl_FragCoord;
    Inputs.ViewDepth     = Interpolants.ViewDepth;
    Inputs.DeviceDepth   = gl_FragCoord.z;

    MaterialSurface Surface = DefaultMaterialSurface(Inputs);
    CalcMaterial(Inputs, Surface);

#if MATERIAL_DOMAIN_MASKED
    if(Surface.OpacityMask < Material_ClipValue) { discard; }
#endif

#if MATERIAL_SHADING_LIT
    // Two Sided lights a back face as its own front: the normal flips.
    vec3 Normal = normalize(Surface.Normal);
    if(Material_TwoSided > 0.5 && !gl_FrontFacing) { Normal = -Normal; }
    vec3 Color = ToDisplay(ToLinear(Surface.Emissive) + ShadeSurface(Surface, Inputs.WorldPosition, Normal));
#else
    vec3 Color = Surface.Emissive;
#endif

    float Alpha = 1.0;
#if MATERIAL_DOMAIN_TRANSLUCENT
    Alpha = clamp(Surface.Opacity, 0.0, 1.0);
#endif
    Output = vec4(Color, Alpha);
}
