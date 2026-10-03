#version 440 core
// MaterialTemplate_PS.glsl -- every material's pixel stage (RenderCore/MATERIALS.md).
// Cooked once per permutation by cook_material_graphs.py, with Material.glsl
// mapped to the material's <Name>.mtl.glsl.
//
// Permutation defines, each 0 or 1:
//   MATERIAL_DOMAIN_OPAQUE / _MASKED / _TRANSLUCENT / _POSTPROCESS
//   MATERIAL_SHADING_LIT / _UNLIT
//
// Opaque and Masked Lit materials draw in the Base Pass and write the
// G-buffer, which DeferredLighting.fsh lights; every other permutation writes
// its colour (RenderCore/RENDERPIPELINE.md).

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>
#include <Material.glsl>
#if MATERIAL_SHADING_LIT && (MATERIAL_DOMAIN_OPAQUE || MATERIAL_DOMAIN_MASKED)
#define MATERIAL_GBUFFER 1
#else
#define MATERIAL_GBUFFER 0
#endif

#if MATERIAL_SHADING_LIT
#include <Lighting.glsl>
#endif

#if MATERIAL_GBUFFER
// RENDERPIPELINE.md, "The G-buffer". Display-space colours, as authored.
OUTPUT(0, vec4, GBuffer0)   // rgb: Base Color; a: Ambient Occlusion
OUTPUT(1, vec4, GBuffer1)   // rgb: world normal * 0.5 + 0.5; a: 1, a lit pixel
OUTPUT(2, vec4, GBuffer2)   // Metallic, Roughness, Specular
OUTPUT(3, vec4, GBuffer3)   // Emissive, past 1 too (EncodeGBufferEmissive)
#else
OUTPUT(0, vec4, Output)
#endif

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
    // Two Sided lights a back face as its own front: the normal flips. The
    // engine is left-handed (RenderView.h), so a face toward the camera
    // winds clockwise on screen: GL's back face.
    vec3 Normal = normalize(Surface.Normal);
    if(Material_TwoSided > 0.5 && gl_FrontFacing) { Normal = -Normal; }
#endif

#if MATERIAL_GBUFFER
    GBuffer0 = vec4(clamp(Surface.BaseColor, 0.0, 1.0), clamp(Surface.AmbientOcclusion, 0.0, 1.0));
    GBuffer1 = vec4(Normal * 0.5 + 0.5, 1.0);
    GBuffer2 = vec4(clamp(Surface.Metallic, 0.0, 1.0), clamp(Surface.Roughness, 0.0, 1.0),
                    clamp(Surface.Specular, 0.0, 1.0), 0.0);
    GBuffer3 = EncodeGBufferEmissive(Surface.Emissive);
#else

#if MATERIAL_SHADING_LIT
    vec3 Color = ToDisplay(ToLinear(Surface.Emissive) + ShadeSurface(Surface, Inputs.WorldPosition, Normal));
#else
    vec3 Color = Surface.Emissive;
#endif

    float Alpha = 1.0;
#if MATERIAL_DOMAIN_TRANSLUCENT
    Alpha = clamp(Surface.Opacity, 0.0, 1.0);
#endif
    Output = vec4(Color, Alpha);
#endif
}
