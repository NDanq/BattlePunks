#version 450 core
// PPSubsurface.fsh -- Skin's screen-space subsurface scattering
// (RenderCore/SHADING_MODELS.md, Phase 5): the Lighting and IBL passes' Skin
// diffuse light (before albedo; a 1 where it is skin) blurred by a separable
// kernel of the skin's diffusion profile (Jimenez et al. 2015, "Separable
// Subsurface Scattering"; the profile is d'Eon and Luebke's sum of Gaussians,
// GPU Gems 3 chapter 14), its width in world units turned into pixels at each
// pixel's depth, following the surface: a tap off the skin, or far off its
// depth, counts as the centre. Run twice: across into a scratch target, then
// down -- that second time multiplied by the pixel's albedo and added to the
// scene colour (PP_Params.z), skin only.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Diffuse)      // rgb: diffuse light before albedo, linear; a: 1 skin
MATERIAL_SAMPLER2D(5, SceneDepth)
MATERIAL_SAMPLER2D(6, GBuffer0)     // rgb: Base Color, display space (the composite's)

#define SSS_TAPS_MAX 25
UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Params;                  // xy: the blur's direction (1, 0) or (0, 1); z: 1 to composite; w: taps
    vec4 PP_Scale;                   // x: the profile's width, world units; y: 1 / tan(half the vertical field of view);
                                     // z: the view's width over its height
    vec4 PP_Sizes;                   // xy: one texel, UV; zw: the target's size
    vec4 PP_Kernel[SSS_TAPS_MAX];    // rgb: weight; a: offset, -3 to 3 (the centre first, at 0)
UBO_BLOCK_END(PP_Block)

float ViewDepthAt(vec2 InUV)
{
    ivec2 Texel = clamp(ivec2(InUV * PP_Sizes.zw), ivec2(0), ivec2(PP_Sizes.zw) - 1);
    vec3  World = PP_WorldAt(TargetUvToNdc(InUV), texelFetch(SceneDepth, Texel, 0).r);
    return max(dot(World, Scene_CameraForward.xyz) - Scene_CameraForward.w, 1e-3);
}

void main(void)
{
    vec2 UV     = Interpolants.TexCoord;
    vec4 Centre = texture(Diffuse, UV);
    bool bComposite = PP_Params.z > 0.5;
    if(Centre.a < 0.5)
    {
        if(bComposite) { discard; }   // not skin: the scene keeps what it has
        Output = Centre;
        return;
    }

    // The profile's width at this depth, in UV, a third per unit of kernel offset.
    float Depth = ViewDepthAt(UV);
    float Ndc   = PP_Scale.x * PP_Scale.y / Depth;
    vec2  Step  = PP_Params.xy * vec2(0.5 * Ndc / PP_Scale.z, 0.5 * Ndc) * (1.0 / 3.0);

    vec3 Sum = Centre.rgb * PP_Kernel[0].rgb;
    int  Taps = int(PP_Params.w);
    for(int Index = 1; Index < SSS_TAPS_MAX; ++Index)
    {
        if(Index >= Taps) { break; }
        vec2 Tap   = UV + PP_Kernel[Index].a * Step;
        vec4 Light = texture(Diffuse, Tap);
        // Follow the surface (Jimenez's SSSS_FOLLOW_SURFACE): toward the centre the
        // further the tap's depth is from it, and wholly where the tap is not skin.
        float Away = clamp(0.0432 * PP_Scale.y * abs(Depth - ViewDepthAt(Tap)) / max(PP_Scale.x, 1e-4), 0.0, 1.0);
        Light.rgb  = mix(Light.rgb, Centre.rgb, Light.a < 0.5 ? 1.0 : Away);
        Sum += Light.rgb * PP_Kernel[Index].rgb;
    }

    if(bComposite)
    {
        vec3 Albedo = SrgbToLinear(texelFetch(GBuffer0, ivec2(gl_FragCoord.xy), 0).rgb);
        Output = vec4(Sum * Albedo, 0.0);   // added to the scene colour
        return;
    }
    Output = vec4(Sum, 1.0);
}
