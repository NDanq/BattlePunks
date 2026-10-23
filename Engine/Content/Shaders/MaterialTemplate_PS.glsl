#version 450 core
// MaterialTemplate_PS.glsl -- every material's pixel stage (RenderCore/MATERIALS.md).
// Cooked once per permutation by cook_material_graphs.py, with Material.glsl
// mapped to the material's <Name>.mtl.glsl.
//
// Permutation defines, each 0 or 1:
//   MATERIAL_DOMAIN_OPAQUE / _MASKED / _TRANSLUCENT / _POSTPROCESS
//   MATERIAL_SHADING_LIT / _UNLIT
//   MATERIAL_SHADING_MODEL_DEFAULT / _CLEARCOAT / _ANISOTROPIC / _CLOTH / _SUBSURFACE / _SKIN
//     (a Lit material's BRDF: RenderCore/SHADING_MODELS.md; an Unlit one's is Default)
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

// The shading model as Common.glsl's SHADING_MODEL_* number (no #elif in the cook).
#if MATERIAL_SHADING_MODEL_CLEARCOAT
#define MATERIAL_SHADING_MODEL_ID 1
#endif
#if MATERIAL_SHADING_MODEL_ANISOTROPIC
#define MATERIAL_SHADING_MODEL_ID 2
#endif
#if MATERIAL_SHADING_MODEL_CLOTH
#define MATERIAL_SHADING_MODEL_ID 3
#endif
#if MATERIAL_SHADING_MODEL_SUBSURFACE
#define MATERIAL_SHADING_MODEL_ID 4
#endif
#if MATERIAL_SHADING_MODEL_SKIN
#define MATERIAL_SHADING_MODEL_ID 5
#endif
#if MATERIAL_SHADING_MODEL_VOLUME
#define MATERIAL_SHADING_MODEL_ID 6
#endif
#ifndef MATERIAL_SHADING_MODEL_ID
#define MATERIAL_SHADING_MODEL_ID 0
#endif

#if MATERIAL_SHADING_LIT
#if !MATERIAL_GBUFFER
// A forward Lit material lights itself from the probe atlas too: slot 0, free outside PostProcess.
#include <IBLCommon.glsl>
MATERIAL_SAMPLER2D(4, Material_IBLAtlas)
#define LIGHTING_IBL_ATLAS Material_IBLAtlas
// And from a GI Volume's probes where one is (DDGI.glsl, engine slots 9 and 10; LIT_TRANSLUCENCY.md, Phase 3).
#include <DDGI.glsl>
// And reflects what is on screen, over the probes (ForwardReflections.glsl, engine slots 13 and 14; Phase 4).
#include <ForwardReflections.glsl>
#define LIGHTING_FORWARD_REFLECTIONS 1
// And is shadowed (LIT_TRANSLUCENCY.md, Phase 2): the shadow atlases at the engine's
// slots 11 and 12, filtered [Shadows] ForwardFilter texels each way; no inset or contact shadows.
MATERIAL_SAMPLER2D(15, ShadowDirectional)
MATERIAL_SAMPLER2D(16, ShadowLocal)
MATERIAL_SAMPLER2D(19, TranslucentOpacity)   // engine slot 15: translucent casters' opacity (Phase 7)
#define LIGHTING_TRANSLUCENT_SHADOWS 1
#define LIGHTING_SHADOWS 1
#define LIGHTING_SHADOW_RADIUS int(Scene_ShadowParams.z)
// A medium shadows itself only by what lies in front of it (LIT_TRANSLUCENCY.md, Phase 8):
// three quarters of a slice toward the light, past its own optical depth.
#if MATERIAL_SHADING_MODEL_VOLUME
#define LIGHTING_TRANSLUCENT_SELF 0.75
#endif
#endif
#include <Lighting.glsl>
#include <BakedLighting.glsl>
#endif

#if MATERIAL_GBUFFER
// The draw's lightmap (BakedLighting.glsl): slot 0, which only a forward Lit material uses otherwise.
MATERIAL_SAMPLER2D(4, Material_Lightmap)

// RENDERPIPELINE.md, "The G-buffer". Display-space colours, as authored.
OUTPUT(0, vec4, GBuffer0)   // rgb: Base Color; a: Ambient Occlusion
OUTPUT(1, vec4, GBuffer1)   // rg: world normal, octahedral; a: 1 a lit pixel, 2/3 one whose ambient is baked (EncodeGBufferNormal)
OUTPUT(2, vec4, GBuffer2)   // Metallic, Roughness, Specular, shading model (EncodeShadingModel)
OUTPUT(3, vec4, GBuffer3)   // Emissive, past 1 too (EncodeGBufferEmissive)
OUTPUT(4, vec4, GBuffer4)   // the shading model's custom data (ShadingModels.glsl's EncodeGBufferCustom)
OUTPUT(5, vec4, Velocity)   // xyz: world motion since the frame before; a: 1, written (the Velocity target; none bound: dropped)
#else
OUTPUT(0, vec4, Output)
#endif

#if !MATERIAL_DOMAIN_POSTPROCESS
// OCCLUSION REVEAL (RenderCore/RENDERPIPELINE.md, "Revealing players"): a draw marked
// Reveal Players (Material_Reserved0, written per draw) has its pixels nearer the eye
// than a player, inside the circle round them, dithered away -- a 4x4 ordered screen
// door, all of it but Scene_RevealParams.z in the middle, none at the rim.
float RevealDither(vec2 InPixel)
{
    const float Bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 Cell = ivec2(mod(floor(InPixel), 4.0));
    return (Bayer[Cell.y * 4 + Cell.x] + 0.5) / 16.0;
}

bool RevealHides(vec3 InClip, float InViewDepth, vec2 InPixel)
{
    if(Material_Reserved0 < 0.5 || Scene_RevealParams.x < 0.5 || InViewDepth <= 1.0e-3) { return false; }
    vec2 Ndc = InClip.xy / InViewDepth;   // a perspective view's clip w is its view depth
    for(int Index = 0; Index < 4; ++Index)
    {
        if(float(Index) >= Scene_RevealParams.x) { break; }
        vec4 Point = Scene_Reveal[Index];
        if(InViewDepth >= Point.w - Scene_RevealParams.w) { continue; }   // not in front of them
        vec2  Away = vec2((Ndc.x - Point.x) * Scene_RevealParams.y, Ndc.y - Point.y);
        float Out  = length(Away) / max(Point.z, 1.0e-4);
        if(Out >= 1.0) { continue; }
        float Keep = mix(Scene_RevealParams.z, 1.0, smoothstep(0.55, 1.0, Out));
        if(RevealDither(InPixel) >= Keep) { return true; }
    }
    return false;
}
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
    Inputs.WorldTangent  = Interpolants.WorldTangent;

    MaterialSurface Surface = DefaultMaterialSurface(Inputs);
    CalcMaterial(Inputs, Surface);

#if MATERIAL_DOMAIN_MASKED
    if(Surface.OpacityMask < Material_ClipValue) { discard; }
#endif
#if !MATERIAL_DOMAIN_POSTPROCESS
    if(RevealHides(Inputs.ClipPosition.xyz, Inputs.ViewDepth, gl_FragCoord.xy)) { discard; }
#endif

#if MATERIAL_SHADING_LIT
    // Two Sided lights a back face as its own front: the normal flips. The
    // engine is left-handed (RenderView.h), so a face toward the camera
    // winds clockwise on screen: GL's back face.
    vec3 Normal = normalize(Surface.Normal);
    if(Material_TwoSided > 0.5 && gl_FrontFacing)
    {
        Normal = -Normal;
        Surface.ClearCoatNormal = -Surface.ClearCoatNormal;
    }
    // The shading models' unset inputs (MaterialCommon.glsl's defaults): Cloth's
    // sheen the square root of its base colour (Filament 4.12), an anisotropic
    // grain along the UV's U: the mesh's tangent, smooth across its triangles,
    // or without one the UV's derivatives, constant across each.
    if(Surface.SheenColor.r < 0.0) { Surface.SheenColor = sqrt(clamp(Surface.BaseColor, 0.0, 1.0)); }
#if MATERIAL_SHADING_MODEL_ANISOTROPIC
    if(dot(Surface.Tangent, Surface.Tangent) < 1e-8)
    {
        vec4 MeshTangent = Material_MeshTangent(Inputs, Normal);
        Surface.Tangent  = MeshTangent.w != 0.0 ? MeshTangent.xyz : Material_TangentAlongU(Inputs, Normal, Inputs.TexCoord);
    }
#endif
#endif

#if MATERIAL_SHADING_LIT
    // Baked lighting is ambient diffuse light: added here, and the live ambient's
    // diffuse part left out -- unless the bake has no sky, whose ambient then still applies.
    vec3  Baked          = vec3(0.0);
    float AmbientDiffuse = 1.0;
    if(Material_BakedLighting.x > 0.5 && Scene_View.x < 0.5)   // the Unlit view mode shows no light, baked or live
    {
        vec3 Diffuse = ToLinear(clamp(Surface.BaseColor, 0.0, 1.0)) * (1.0 - clamp(Surface.Metallic, 0.0, 1.0))
                     * clamp(Surface.AmbientOcclusion, 0.0, 1.0);
#if MATERIAL_GBUFFER
        Baked = Baked_Light(Material_Lightmap, Interpolants.LightmapUV, Normal) * Diffuse;
#else
        Baked = Baked_AmbientCube(Normal) * Diffuse;
#endif
        AmbientDiffuse = Scene_Baked.y > 0.5 ? 0.0 : 1.0;
    }
#endif

#if MATERIAL_GBUFFER
    GBuffer0 = vec4(clamp(Surface.BaseColor, 0.0, 1.0), clamp(Surface.AmbientOcclusion, 0.0, 1.0));
    GBuffer1 = EncodeGBufferNormal(Normal, AmbientDiffuse < 0.5);
    GBuffer2 = vec4(clamp(Surface.Metallic, 0.0, 1.0), clamp(Surface.Roughness, 0.0, 1.0),
                    clamp(Surface.Specular, 0.0, 1.0), EncodeShadingModel(MATERIAL_SHADING_MODEL_ID));
    GBuffer4 = EncodeGBufferCustom(MATERIAL_SHADING_MODEL_ID, Surface.ClearCoat, Surface.ClearCoatRoughness,
                                   normalize(Surface.ClearCoatNormal), Surface.Anisotropy, Surface.Tangent,
                                   Surface.SheenColor, Surface.SubsurfaceColor, Surface.Thickness);
    // Emissive is display space as authored: the baked light joins it in linear.
    GBuffer3 = EncodeGBufferEmissive(Material_BakedLighting.x > 0.5 ? ToDisplay(ToLinear(Surface.Emissive) + Baked) : Surface.Emissive);
    Velocity = vec4(Interpolants.WorldMotion, 1.0);
#else

#if MATERIAL_SHADING_LIT
#ifdef MATERIAL_TRANSLUCENCY_SURFACE
    // Translucency Lighting Surface (the generated code defines it only for Translucent Lit) (LIT_TRANSLUCENCY.md, Phase 3): the body -- diffuse,
    // emissive -- covers Opacity of what is behind; the reflection off the surface does
    // not fade with it. Written premultiplied, blended (one, one minus source alpha).
    float Coverage = clamp(Surface.Opacity, 0.0, 1.0);
    vec3  Color;
    if(Scene_View.x > 0.5)
    {
        Color = ToSceneColor(ViewChannelColor(Surface.BaseColor, Surface.Emissive)) * Coverage;
    }
    else
    {
        LightingSurface Lit = LightingSurfaceOf(Surface, MATERIAL_SHADING_MODEL_ID, Inputs.WorldPosition, Normal);
        vec3 Diffuse, Specular;
        ShadeSurfaceSplit(Lit, Inputs.WorldPosition, Surface.AmbientOcclusion, AmbientDiffuse, Diffuse, Specular);
        Color = ToSceneColor((ToLinear(Surface.Emissive) + Baked + Lit.Albedo * Diffuse) * Coverage + Specular);
    }
#else
    vec3 Color = Scene_View.x > 0.5
               ? ToSceneColor(ViewChannelColor(Surface.BaseColor, Surface.Emissive))
               : ToSceneColor(ToLinear(Surface.Emissive) + Baked
                              + ShadeLightingSurface(LightingSurfaceOf(Surface, MATERIAL_SHADING_MODEL_ID, Inputs.WorldPosition, Normal),
                                                     Inputs.WorldPosition, Surface.AmbientOcclusion, AmbientDiffuse));
#endif
#else
#if MATERIAL_DOMAIN_POSTPROCESS
    // After the tone map: display space in, display space out.
    vec3 Color = Surface.Emissive;
#else
    // Display space as authored; an HDR scene holds it linear (ToneMapping.glsl).
    vec3 Color = Scene_Output.x > 0.5 ? SrgbToLinear(Surface.Emissive) : Surface.Emissive;
    // A preview's channel (Lighting.glsl's ViewChannelColor): an Unlit surface reflects nothing; it only glows.
    if(Scene_View.x > 1.5)
    {
        vec3 Linear = Scene_View.x > 2.5 ? SrgbToLinear(Surface.Emissive) * (1.0 / 16.0) : vec3(0.0);
        Color = Scene_Output.x > 0.5 ? Linear : LinearToSrgb(Linear);
    }
#endif
#endif

    float Alpha = 1.0;
#if MATERIAL_DOMAIN_TRANSLUCENT
    Alpha = clamp(Surface.Opacity, 0.0, 1.0);
    // A pixel that would change the scene by under half a step of 8 bits
    // (soft particles' corners and fringes) skips its blend: no colour write.
#ifdef MATERIAL_TRANSLUCENCY_SURFACE
    if(max(Alpha, max(Color.r, max(Color.g, Color.b))) < 0.5 / 255.0) { discard; }   // premultiplied: nothing to add or cover
#else
    if(Alpha * max(1.0, max(Color.r, max(Color.g, Color.b))) < 0.5 / 255.0) { discard; }
#endif
#endif
    Output = vec4(Color, Alpha);
#endif
}
