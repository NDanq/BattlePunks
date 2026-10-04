// Lighting.glsl -- a Lit material's shading: the sun (one directional light),
// up to 16 point and spot lights, and a sky/ground ambient
// (SceneUniforms.glsl). GGX distribution, Smith-Schlick geometry and Schlick
// Fresnel. Colours arrive display space and are lit linear; the caller
// encodes the result back.
//
// Shadows: only where LIGHTING_SHADOWS is defined -- DeferredLighting.fsh,
// which binds the shadow atlases (Shadows.glsl). A forward Translucent Lit
// material is lit unshadowed: its texture slots are its own.

const float LIGHTING_PI = 3.14159265;

vec3 ToLinear(vec3 InColor)  { return pow(max(InColor, vec3(0.0)), vec3(2.2)); }
vec3 ToDisplay(vec3 InColor) { return pow(max(InColor, vec3(0.0)), vec3(1.0 / 2.2)); }

// Lit light into the scene colour: linear in an HDR scene (the tone map pass
// makes it display space), display space in an 8-bit one.
vec3 ToSceneColor(vec3 InLinear) { return Scene_Output.x > 0.5 ? max(InLinear, vec3(0.0)) : ToDisplay(InLinear); }

// Scene_View's unlit colours (above 0.5): base colour and emissive (Unlit), base colour
// alone, or emissive alone over 16 -- a material preview's channels, which the bake
// measures materials by (RenderTypes.h's EMaterialPreviewChannel).
vec3 ViewChannelColor(vec3 InBaseColor, vec3 InEmissive)
{
    if(Scene_View.x > 2.5) { return ToLinear(InEmissive) * (1.0 / 16.0); }
    if(Scene_View.x > 1.5) { return ToLinear(InBaseColor); }
    return ToLinear(InBaseColor) + ToLinear(InEmissive);
}

#ifdef LIGHTING_SHADOWS
#include <Shadows.glsl>
#endif

// What a surface reflects of every light: computed once, shared by each.
struct LightingSurface
{
    vec3  Albedo;
    float Metallic;
    float Roughness;
    vec3  F0;
    vec3  N;
    vec3  V;
    float NdotV;
};

// Linear radiance toward the eye from one light: L toward it, InRadiance its
// linear colour times intensity and falloff.
vec3 EvaluateLight(in LightingSurface S, vec3 L, vec3 InRadiance)
{
    vec3  H     = normalize(S.V + L);
    float NdotL = max(dot(S.N, L), 0.0);
    if(NdotL <= 0.0) { return vec3(0.0); }
    float NdotH = max(dot(S.N, H), 0.0);
    float VdotH = max(dot(S.V, H), 0.0);

    // GGX / Trowbridge-Reitz.
    float Alpha  = S.Roughness * S.Roughness;
    float Alpha2 = Alpha * Alpha;
    float Denom  = NdotH * NdotH * (Alpha2 - 1.0) + 1.0;
    float D      = Alpha2 / (LIGHTING_PI * Denom * Denom);

    // Smith with Schlick-GGX, k for direct light.
    float K = (S.Roughness + 1.0) * (S.Roughness + 1.0) / 8.0;
    float G = (S.NdotV / (S.NdotV * (1.0 - K) + K)) * (NdotL / (NdotL * (1.0 - K) + K));

    vec3 F = S.F0 + (vec3(1.0) - S.F0) * pow(1.0 - VdotH, 5.0);

    vec3 Specular = D * G * F / max(4.0 * S.NdotV * NdotL, 1e-4);
    vec3 Diffuse  = (vec3(1.0) - F) * (1.0 - S.Metallic) * S.Albedo / LIGHTING_PI;
    return (Diffuse + Specular) * InRadiance * NdotL;
}

#ifdef LIGHTING_SHADOWS
// A Modulated shadow's multiplier: toward InTint (display space) as InLit
// falls, scaled by InReach (the light's own fade). Only on faces toward the
// light -- a face turned away is not "in shadow", just unlit -- faded in over
// the terminator.
vec3 ModulatedShadow(float InLit, float InReach, float InNdotL, vec3 InTint)
{
    float Amount = (1.0 - InLit) * InReach * smoothstep(0.0, 0.2, InNdotL);
    return mix(vec3(1.0), ToLinear(InTint), Amount);
}
#endif

// A point or spot light's falloff: inverse-square-like, windowed to reach
// exactly zero at its range.
float LocalFalloff(float InDistance, float InRange)
{
    float X      = InDistance / max(InRange, 1e-4);
    float Window = clamp(1.0 - X * X * X * X, 0.0, 1.0);
    return Window * Window / (1.0 + 16.0 * X * X);
}

// Linear radiance leaving the surface toward the eye, from every light and the ambient --
// its diffuse part times InAmbientDiffuse: 0 where baked lighting already holds it
// (BakedLighting.glsl).
vec3 ShadeSurface(in MaterialSurface Surface, vec3 InWorldPosition, vec3 InNormal, float InAmbientDiffuse)
{
    LightingSurface S;
    S.Albedo    = ToLinear(Surface.BaseColor);
    S.Metallic  = clamp(Surface.Metallic, 0.0, 1.0);
    S.Roughness = clamp(Surface.Roughness, 0.04, 1.0);
    S.N         = normalize(InNormal);
    S.V         = normalize(Scene_CameraPosition.xyz - InWorldPosition);
    S.NdotV     = max(dot(S.N, S.V), 1e-4);
    // Schlick Fresnel's F0; Specular 0.5 is a 4% dielectric.
    S.F0        = mix(vec3(0.08 * clamp(Surface.Specular, 0.0, 1.0)), S.Albedo, S.Metallic);

    // Modulated shadows darken everything lit here, toward their tint.
    vec3 Modulate = vec3(1.0);

    // The sun.
    vec3  SunL       = normalize(Scene_LightDirection.xyz);
    float SunShadow  = 1.0;
#ifdef LIGHTING_SHADOWS
    if(Scene_LightShadow.x >= 0.0)
    {
        float Lit = DirectionalShadow(InWorldPosition, S.N, SunL);
        if(Scene_LightShadow.x == 1.0) { Modulate *= ModulatedShadow(Lit, 1.0, dot(S.N, SunL), Scene_LightShadowTint.rgb); }
        else                           { SunShadow = Lit; }
    }
#endif
    vec3 Direct = EvaluateLight(S, SunL, ToLinear(Scene_LightColor.rgb) * Scene_LightDirection.w) * SunShadow;

    // Point and spot lights.
    int Count = int(Scene_LocalCounts.x);
    for(int Index = 0; Index < Count; ++Index)
    {
        vec4  PositionRange  = Scene_Locals[Index * 6 + 0];
        vec4  ColorIntensity = Scene_Locals[Index * 6 + 1];
        vec4  DirectionType  = Scene_Locals[Index * 6 + 2];
        vec4  ConeShadow     = Scene_Locals[Index * 6 + 3];

        vec3  ToLight  = PositionRange.xyz - InWorldPosition;
        float Distance = length(ToLight);
        if(Distance >= PositionRange.w) { continue; }
        vec3  L        = ToLight / max(Distance, 1e-4);

        float Attenuation = LocalFalloff(Distance, PositionRange.w);
        if(DirectionType.w > 0.5)
        {
            // Spot: full inside the inner cone, nothing past the outer.
            Attenuation *= smoothstep(ConeShadow.x, ConeShadow.y, dot(-L, DirectionType.xyz));
        }
        if(Attenuation <= 0.0) { continue; }

        float Shadow = 1.0;
#ifdef LIGHTING_SHADOWS
        if(ConeShadow.z >= 0.0)
        {
            vec4  Biases = Scene_Locals[Index * 6 + 5];
            float Lit    = LocalShadow(int(ConeShadow.z), DirectionType.w > 0.5, ConeShadow.x, PositionRange.xyz,
                                       InWorldPosition, S.N, L, Biases.x, Biases.y);
            if(ConeShadow.w == 1.0)
            {
                // Fades out with the light: full where it is at least a quarter strength.
                float Reach = clamp(Attenuation * 4.0, 0.0, 1.0);
                Modulate *= ModulatedShadow(Lit, Reach, dot(S.N, L), Scene_Locals[Index * 6 + 4].rgb);
            }
            else { Shadow = Lit; }
        }
#endif
        Direct += EvaluateLight(S, L, ToLinear(ColorIntensity.rgb) * ColorIntensity.a * Attenuation) * Shadow;
    }

    // +Y is up, so the sky is toward +Y.
    float Up       = clamp(S.N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  Ambient  = mix(ToLinear(Scene_GroundColor.rgb), ToLinear(Scene_SkyColor.rgb), Up) * Scene_SkyColor.a;
    vec3  Indirect = Ambient * (S.Albedo * (1.0 - S.Metallic) * InAmbientDiffuse + S.F0 * (1.0 - S.Roughness) * 0.5)
                   * Surface.AmbientOcclusion;
    // Image-based lighting replaces it: in its own pass over the G-buffer (IBLLighting.fsh), or
    // here for a forward Lit material, which has the atlas (LIGHTING_IBL_ATLAS).
    if(Scene_IBL.x > 0.5)
    {
#ifdef LIGHTING_IBL_ATLAS
        Indirect = IBL_Light(LIGHTING_IBL_ATLAS, InWorldPosition, S.N, S.V, S.Albedo, S.Metallic, S.Roughness, S.F0,
                             Surface.AmbientOcclusion, InAmbientDiffuse);
#else
        Indirect = vec3(0.0);
#endif
    }

    return (Direct + Indirect) * Modulate;
}

vec3 ShadeSurface(in MaterialSurface Surface, vec3 InWorldPosition, vec3 InNormal)
{
    return ShadeSurface(Surface, InWorldPosition, InNormal, 1.0);
}
