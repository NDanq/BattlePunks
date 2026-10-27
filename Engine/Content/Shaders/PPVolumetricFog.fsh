#version 450 core
// PPVolumetricFog.fsh -- volumetric fog's march, at half resolution (RenderCore/
// RENDERPIPELINE.md, "Volumetric fog"), after the translucent stages and before
// TAA, which smooths its per-pixel jitter.
//
// From the eye to the surface each pixel shows (or Max Distance, for the sky),
// in Steps jittered samples through exponential height fog. Each sample is lit
// by the sun -- tested against its cascaded shadow map, one tap, so its shafts
// show -- scattered toward the eye by a Henyey-Greenstein phase; by the point and
// spot lights in reach, unshadowed; and by a little of the sky. rgb the light
// scattered toward the eye, a what is left of the scene behind (transmittance):
// PPFogComposite.fsh lays it over the scene as scene * a + rgb.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneDepth)          // full resolution, window depth
MATERIAL_SAMPLER2D(5, ShadowDirectional)   // the sun's shadow atlas (Shadows.glsl's layout)
MATERIAL_SAMPLER2D(19, TranslucentOpacity) // engine slot 15: translucent casters' opacity atlas (TranslucentShadows.glsl)
#include <TranslucentShadows.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    // In the place of the passes' mat4, four vec4s of settings.
    vec4 PP_Fog;        // x: density, per world unit, at and below y; y: its fall-off upward, per world unit; z: y; w: Max Distance
    vec4 PP_Scatter;    // x: phase anisotropy (0 even, toward 1 forward); y: sun; z: sky; w: point and spot lights
    vec4 PP_Albedo;     // rgb: the fog's colour (what it scatters); a: 1 to shadow the sun's light
    vec4 PP_Steps;      // x: samples along each ray
    vec4 PP_Unused;
    vec4 PP_Sizes;      // zw: full resolution
UBO_BLOCK_END(PP_Block)

const float FOG_PI = 3.14159265;

vec3 Linear(vec3 InDisplay) { return SrgbToLinear(InDisplay); }

float Density(vec3 InWorld)
{
    return PP_Fog.x * exp(-max(InWorld.y - PP_Fog.z, 0.0) * PP_Fog.y);
}

// The sun's light at InWorld: its cascade by view depth, one depth test.
float SunVisibility(vec3 InWorld)
{
    int Count = int(Scene_LightShadow.y);
    if(PP_Albedo.a < 0.5 || Scene_ShadowParams.x < 0.5 || Count <= 0) { return 1.0; }
    float ViewDepth = dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w;
    if(ViewDepth >= Scene_CascadeSplits[Count - 1]) { return 1.0; }
    int Cascade = 0;
    for(int Index = 0; Index < Count - 1; ++Index)
    {
        if(ViewDepth >= Scene_CascadeSplits[Index]) { Cascade = Index + 1; }
    }
    vec4 Clip = Scene_CascadeMatrices[Cascade] * vec4(InWorld, 1.0);
    if(Clip.w <= 0.0) { return 1.0; }
    vec3 Atlas = Clip.xyz / Clip.w;
    if(Atlas.z >= 1.0 || Atlas.z <= 0.0) { return 1.0; }
    ivec2 Texel = clamp(ivec2(Atlas.xy * Scene_ShadowParams.w), ivec2(0), ivec2(Scene_ShadowParams.w) - 1);
    if(texelFetch(ShadowDirectional, Texel, 0).r + 0.001 < Atlas.z) { return 0.0; }
    // And through the translucent casters -- shafts cut by a plume (LIT_TRANSLUCENCY.md, Phase 7).
    return exp(-Translucent_Depth(TranslucentOpacity, Scene_CascadeMatrices[Cascade], InWorld, false, Translucent_CascadeRange(Cascade), 0.0));
}

float Phase(float InCos, float InG)
{
    float G2 = InG * InG;
    return (1.0 - G2) / (4.0 * FOG_PI * pow(max(1.0 + G2 - 2.0 * InG * InCos, 1e-4), 1.5));
}

void main(void)
{
    vec2  FullUV = Interpolants.TexCoord;
    ivec2 Full   = clamp(ivec2(FullUV * PP_Sizes.zw), ivec2(0), ivec2(PP_Sizes.zw) - 1);
    float Depth  = texelFetch(SceneDepth, Full, 0).r;
    vec3  Eye    = Scene_CameraPosition.xyz;
    vec3  Far    = PP_WorldAt(TargetUvToNdc(FullUV), Depth < 1.0 ? Depth : 0.9999);
    vec3  Ray    = Far - Eye;
    float Length = min(length(Ray), PP_Fog.w);
    vec3  D      = Ray / max(length(Ray), 1e-4);

    int   Steps  = int(clamp(PP_Steps.x, 4.0, 64.0));
    float Noise  = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float Step   = Length / float(Steps);

    vec3  SunL     = normalize(Scene_LightDirection.xyz);
    vec3  Sun      = Linear(Scene_LightColor.rgb) * Scene_LightDirection.w * PP_Scatter.y * Phase(dot(D, SunL), PP_Scatter.x);
    vec3  Sky      = Linear(Scene_SkyColor.rgb) * Scene_SkyColor.a * PP_Scatter.z / (4.0 * FOG_PI);
    vec3  Albedo   = PP_Albedo.rgb;
    int   Locals   = PP_Scatter.w > 0.0 ? int(Scene_LocalCounts.x) : 0;

    vec3  Scattered     = vec3(0.0);
    float Transmittance = 1.0;
    for(int Index = 0; Index < Steps; ++Index)
    {
        vec3  P = Eye + D * ((float(Index) + Noise) * Step);
        float Sigma = Density(P);
        if(Sigma <= 1e-9) { continue; }
        vec3 Light = Sun * SunVisibility(P) + Sky;
        for(int L = 0; L < Locals; ++L)
        {
            vec4  PositionRange  = Scene_Locals[L * 6 + 0];
            vec4  ColorIntensity = Scene_Locals[L * 6 + 1];
            vec4  DirectionType  = Scene_Locals[L * 6 + 2];
            vec4  Cone           = Scene_Locals[L * 6 + 3];
            vec3  ToLight  = PositionRange.xyz - P;
            float Distance = length(ToLight);
            if(Distance >= PositionRange.w) { continue; }
            // Lighting.glsl's LocalFalloff, and a spot's cone.
            float X       = Distance / max(PositionRange.w, 1e-4);
            float Window  = clamp(1.0 - X * X * X * X, 0.0, 1.0);
            float Falloff = Window * Window / (1.0 + 16.0 * X * X);
            if(DirectionType.w > 0.5) { Falloff *= smoothstep(Cone.x, Cone.y, dot(-ToLight / max(Distance, 1e-4), DirectionType.xyz)); }
            Light += Linear(ColorIntensity.rgb) * ColorIntensity.a * Falloff * PP_Scatter.w / (4.0 * FOG_PI);
        }
        // This step's in-scattering, integrated over the step (energy conserving).
        float Extinction = exp(-Sigma * Step);
        Scattered     += Transmittance * Light * Albedo * (1.0 - Extinction);
        Transmittance *= Extinction;
    }
    Output = vec4(Scattered, Transmittance);
}
