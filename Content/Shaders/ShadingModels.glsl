// ShadingModels.glsl -- a Lit surface's BRDF by shading model
// (RenderCore/SHADING_MODELS.md): what a surface is to the light, its lobes,
// and the G-buffer's custom data (GBuffer4) that carries a model's inputs from
// the Base Pass to the Lighting and IBL passes. Needs Common.glsl.
//
//   Default      GGX, Smith-Schlick, Schlick Fresnel, Lambert (Karis 2013)
//   ClearCoat    Default under a second GGX lobe, F0 0.04, Kelemen visibility
//                (Filament 4.9; Kelemen & Szirmay-Kalos 2001)
//   Anisotropic  anisotropic GGX, height-correlated anisotropic Smith
//                (Burley 2012; Heitz 2014), Kulla-Conty's roughness split (2017)
//   Cloth        Charlie sheen (Estevez & Kulla 2017), Neubelt visibility
//                (Neubelt & Pettineo 2013), wrapped diffuse (Filament 4.12)
//   Subsurface   Default plus transmission through Thickness, wrapped
//                (Green 2004; Barre-Brisebois & Bouchard 2011; Filament 4.11)
//   Skin         Default with transmission; its diffuse blurred in screen space
//                where the scene runs PPSubsurface (Jimenez et al. 2015)

#ifndef SHADING_MODELS_GLSL
#define SHADING_MODELS_GLSL

const float SHADING_PI = 3.14159265;

// What a surface reflects of every light: computed once, shared by each.
struct LightingSurface
{
    int   Model;              // SHADING_MODEL_*
    vec3  Albedo;             // linear
    float Metallic;
    float Roughness;          // perceptual, 0.04-1
    vec3  F0;
    vec3  N;
    vec3  V;
    float NdotV;

    float ClearCoat;          // Clear Coat
    float CoatRoughness;
    vec3  CoatN;
    float Anisotropy;         // Anisotropic
    vec3  T;
    vec3  B;
    vec3  Sheen;              // Cloth, linear
    vec3  Subsurface;         // Cloth's wrap tint; Subsurface's and Skin's transmitted colour, linear
    float Thickness;          // Subsurface, Skin
};

// --- Lobes ------------------------------------------------------------------

vec3 SM_FresnelSchlick(vec3 InF0, float InCos)
{
    return InF0 + (vec3(1.0) - InF0) * pow(1.0 - InCos, 5.0);
}

float SM_DistributionGGX(float InAlpha, float InNdotH)
{
    float Alpha2 = InAlpha * InAlpha;
    float Denom  = InNdotH * InNdotH * (Alpha2 - 1.0) + 1.0;
    return Alpha2 / (SHADING_PI * Denom * Denom);
}

// Burley's anisotropic GGX, in Filament's form.
float SM_DistributionGGXAniso(float InAlphaT, float InAlphaB, float InTdotH, float InBdotH, float InNdotH)
{
    float A2 = InAlphaT * InAlphaB;
    vec3  W  = vec3(InAlphaB * InTdotH, InAlphaT * InBdotH, A2 * InNdotH);
    float W2 = A2 / dot(W, W);
    return A2 * W2 * W2 / SHADING_PI;
}

// Heitz's height-correlated Smith for the anisotropic GGX, folded with 1 / (4 NdotL NdotV).
float SM_VisibilityAniso(float InAlphaT, float InAlphaB, float InTdotV, float InBdotV, float InTdotL, float InBdotL,
                         float InNdotV, float InNdotL)
{
    float LambdaV = InNdotL * length(vec3(InAlphaT * InTdotV, InAlphaB * InBdotV, InNdotV));
    float LambdaL = InNdotV * length(vec3(InAlphaT * InTdotL, InAlphaB * InBdotL, InNdotL));
    return 0.5 / max(LambdaV + LambdaL, 1e-5);
}

// Kelemen's visibility for the clear coat, folded with 1 / (4 NdotL NdotV).
float SM_VisibilityKelemen(float InLdotH)
{
    return 0.25 / max(InLdotH * InLdotH, 1e-4);
}

// The Charlie sheen distribution (Estevez & Kulla 2017).
float SM_DistributionCharlie(float InAlpha, float InNdotH)
{
    float InvAlpha = 1.0 / max(InAlpha, 1e-3);
    float Sin2h    = max(1.0 - InNdotH * InNdotH, 0.0078125);   // 2^-7: fp16 safe
    return (2.0 + InvAlpha) * pow(Sin2h, InvAlpha * 0.5) / (2.0 * SHADING_PI);
}

// Neubelt and Pettineo's cloth visibility, folded with 1 / (4 NdotL NdotV).
float SM_VisibilityNeubelt(float InNdotV, float InNdotL)
{
    return 1.0 / max(4.0 * (InNdotL + InNdotV - InNdotL * InNdotV), 1e-4);
}

// Karis's fit of the split-sum's environment BRDF (as IBLCommon.glsl's IBL_EnvBRDF):
// F0 * A + B, the directional albedo of single-scattering GGX.
vec3 SM_EnvBRDF(vec3 InF0, float InRoughness, float InNdotV)
{
    const vec4 C0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 C1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4  R    = InRoughness * C0 + C1;
    float A004 = min(R.x * R.x, exp2(-9.28 * InNdotV)) * R.x + R.y;
    vec2  AB   = vec2(-1.04, 1.04) * A004 + R.zw;
    return InF0 * AB.x + AB.y;
}

// Multiple scattering's energy, which single-scattering GGX loses between
// microfacets: the specular times 1 + F0 (1 / E - 1), E its directional albedo
// at F0 = 1 (Turquin 2019; Kulla & Conty 2017; Fdez-Aguera 2019). Rough
// metals brighten most. 1 where the light list turns it off
// ([Rendering] EnergyCompensation), or there is no light list.
vec3 SM_EnergyCompensation(vec3 InF0, float InRoughness, float InNdotV)
{
#ifdef LIGHT_LIST_GLSL
    if(LightList_Flag(LIGHT_LIST_FLAG_ENERGY))
    {
        float E = max(SM_EnvBRDF(vec3(1.0), InRoughness, InNdotV).x, 0.05);
        return vec3(1.0) + InF0 * (1.0 / E - 1.0);
    }
#endif
    return vec3(1.0);
}

// The anisotropic roughnesses along T and B (Kulla & Conty 2017).
void SM_AnisotropicAlphas(in LightingSurface S, out float OutAlphaT, out float OutAlphaB)
{
    float Alpha = S.Roughness * S.Roughness;
    OutAlphaT = max(Alpha * (1.0 + S.Anisotropy), 0.002);
    OutAlphaB = max(Alpha * (1.0 - S.Anisotropy), 0.002);
}

// A clear coat's change to the base under it: the base's F0 seen through a
// layer of index 1.5 (Filament's fit), and its roughness no less than the coat's.
vec3 SM_F0UnderCoat(vec3 InF0)
{
    return clamp(InF0 * (InF0 * (0.941892 - 0.263008 * InF0) + 0.346479) - 0.0285998, 0.0, 1.0);
}

// The transmitted share of a light from behind a Subsurface or Skin surface
// (Filament 4.11): forward scattering toward the eye as a spherical Gaussian,
// back scattering wrapped round the terminator; none through a thick one.
float SM_Transmission(in LightingSurface S, vec3 InL, float InNdotL)
{
    const float Power = 12.234;
    float ScatterVdotL = clamp(dot(S.V, -InL), 0.0, 1.0);
    float Forward      = exp2(ScatterVdotL * Power - Power);
    float Back         = clamp(InNdotL * S.Thickness + (1.0 - S.Thickness), 0.0, 1.0) * 0.5;
    return mix(Back, 1.0, Forward) * (1.0 - S.Thickness);
}

// --- One light --------------------------------------------------------------

// What one light does to the surface: InL toward it, InRadiance its linear
// colour times intensity and falloff. OutDiffuse is the diffuse light before
// the albedo multiplies it (Skin's screen-space blur takes it apart);
// OutSpecular everything else -- the specular lobes and Cloth's sheen.
// OutTransmitted: light through a Subsurface or Skin surface, which its own
// shadow does not stop (the caller decides what does).
void EvaluateLightModel(in LightingSurface S, vec3 InL, vec3 InRadiance,
                        out vec3 OutDiffuse, out vec3 OutSpecular, out vec3 OutTransmitted)
{
    OutDiffuse     = vec3(0.0);
    OutSpecular    = vec3(0.0);
    OutTransmitted = vec3(0.0);
    float NdotLRaw = dot(S.N, InL);

    if(S.Model == SHADING_MODEL_SUBSURFACE || S.Model == SHADING_MODEL_SKIN)
    {
        OutTransmitted = S.Subsurface * (SM_Transmission(S, InL, NdotLRaw) / SHADING_PI) * InRadiance;
    }

    if(S.Model == SHADING_MODEL_CLOTH)
    {
        // Wrapped diffuse, tinted by Subsurface Color where it wraps (Filament 4.12).
        const float Wrap = 0.5;
        float Wrapped = clamp((NdotLRaw + Wrap) / ((1.0 + Wrap) * (1.0 + Wrap)), 0.0, 1.0);
        OutDiffuse = (Wrapped / SHADING_PI) * clamp(S.Subsurface + vec3(NdotLRaw), 0.0, 1.0) * InRadiance;
        if(NdotLRaw <= 0.0) { return; }
        vec3  H     = normalize(S.V + InL);
        float NdotH = max(dot(S.N, H), 0.0);
        float Alpha = S.Roughness * S.Roughness;
        OutSpecular = SM_DistributionCharlie(Alpha, NdotH) * SM_VisibilityNeubelt(S.NdotV, NdotLRaw) * S.Sheen
                    * InRadiance * NdotLRaw;
        return;
    }

    float NdotL = max(NdotLRaw, 0.0);
    vec3  H     = normalize(S.V + InL);
    float VdotH = max(dot(S.V, H), 0.0);

    // The base: Default's maths exactly, unless a lobe below replaces its specular.
    vec3 F0 = S.F0;
    float Roughness = S.Roughness;
    float CoatFresnel = 0.0;
    vec3  Coat = vec3(0.0);
    if(S.Model == SHADING_MODEL_CLEARCOAT)
    {
        vec3  CoatH     = normalize(S.V + InL);
        float CoatNdotL = max(dot(S.CoatN, InL), 0.0);
        float CoatNdotH = max(dot(S.CoatN, CoatH), 0.0);
        float LdotH     = max(dot(InL, CoatH), 0.0);
        float CoatAlpha = S.CoatRoughness * S.CoatRoughness;
        CoatFresnel = (0.04 + 0.96 * pow(1.0 - LdotH, 5.0)) * S.ClearCoat;
        Coat = vec3(SM_DistributionGGX(CoatAlpha, CoatNdotH) * SM_VisibilityKelemen(LdotH) * CoatFresnel) * CoatNdotL * InRadiance;
        F0        = mix(F0, SM_F0UnderCoat(F0), S.ClearCoat);
        Roughness = mix(Roughness, max(Roughness, S.CoatRoughness), S.ClearCoat);
    }
    if(NdotL <= 0.0)
    {
        OutSpecular = Coat;
        return;
    }
    float NdotH = max(dot(S.N, H), 0.0);
    vec3  F     = SM_FresnelSchlick(F0, VdotH);

    vec3 Specular;
    if(S.Model == SHADING_MODEL_ANISOTROPIC)
    {
        float AlphaT, AlphaB;
        SM_AnisotropicAlphas(S, AlphaT, AlphaB);
        float D   = SM_DistributionGGXAniso(AlphaT, AlphaB, dot(S.T, H), dot(S.B, H), NdotH);
        float Vis = SM_VisibilityAniso(AlphaT, AlphaB, dot(S.T, S.V), dot(S.B, S.V), dot(S.T, InL), dot(S.B, InL), S.NdotV, NdotL);
        Specular = D * Vis * F;
    }
    else
    {
        // GGX / Trowbridge-Reitz; Smith with Schlick-GGX, k for direct light.
        float Alpha = Roughness * Roughness;
        float D = SM_DistributionGGX(Alpha, NdotH);
        float K = (Roughness + 1.0) * (Roughness + 1.0) / 8.0;
        float G = (S.NdotV / (S.NdotV * (1.0 - K) + K)) * (NdotL / (NdotL * (1.0 - K) + K));
        Specular = D * G * F / max(4.0 * S.NdotV * NdotL, 1e-4);
    }
    vec3 DiffuseShare = (vec3(1.0) - F) * (1.0 - S.Metallic) / SHADING_PI;

    Specular *= SM_EnergyCompensation(F0, Roughness, S.NdotV);

    // Under a clear coat the base gets what the coat lets through.
    float Through = 1.0 - CoatFresnel;
    OutDiffuse  = DiffuseShare * InRadiance * NdotL * Through;
    OutSpecular = Specular * InRadiance * NdotL * Through + Coat;
}

// --- The G-buffer's custom data (GBuffer4) ------------------------------------
//
//   ClearCoat    r Clear Coat, g Clear Coat Roughness, ba the coat normal (octahedral)
//   Anisotropic  r Anisotropy * 0.5 + 0.5, gb the tangent (octahedral)
//   Cloth        rgb Sheen Color (display space), a Subsurface Color's mean (the wrap's
//                tint is grey in the G-buffer; a forward Cloth material keeps its colour)
//   Subsurface   rgb Subsurface Color (display space), a Thickness
//   Skin         rgb Subsurface Color (display space), a Thickness

vec4 EncodeGBufferCustom(int InModel, float InClearCoat, float InCoatRoughness, vec3 InCoatNormal, float InAnisotropy,
                         vec3 InTangent, vec3 InSheen, vec3 InSubsurface, float InThickness)
{
    if(InModel == SHADING_MODEL_CLEARCOAT)
    {
        return vec4(clamp(InClearCoat, 0.0, 1.0), clamp(InCoatRoughness, 0.0, 1.0), EncodeUnitVector(InCoatNormal));
    }
    if(InModel == SHADING_MODEL_ANISOTROPIC)
    {
        return vec4(clamp(InAnisotropy, -1.0, 1.0) * 0.5 + 0.5, EncodeUnitVector(InTangent), 0.0);
    }
    if(InModel == SHADING_MODEL_CLOTH)
    {
        return vec4(clamp(InSheen, 0.0, 1.0), clamp(dot(InSubsurface, vec3(1.0 / 3.0)), 0.0, 1.0));
    }
    if(InModel == SHADING_MODEL_SUBSURFACE || InModel == SHADING_MODEL_SKIN)
    {
        return vec4(clamp(InSubsurface, 0.0, 1.0), clamp(InThickness, 0.0, 1.0));
    }
    return vec4(0.0);
}

// A surface's shared terms, from its linear albedo and its BRDF inputs.
LightingSurface MakeLightingSurface(int InModel, vec3 InAlbedo, float InMetallic, float InRoughness, float InSpecular,
                                    vec3 InNormal, vec3 InView)
{
    LightingSurface S;
    S.Model     = InModel;
    S.Albedo    = InAlbedo;
    S.Metallic  = clamp(InMetallic, 0.0, 1.0);
    S.Roughness = clamp(InRoughness, 0.04, 1.0);
    S.N         = normalize(InNormal);
    S.V         = InView;
    S.NdotV     = max(dot(S.N, S.V), 1e-4);
    // Schlick Fresnel's F0; Specular 0.5 is a 4% dielectric.
    S.F0        = mix(vec3(0.08 * clamp(InSpecular, 0.0, 1.0)), S.Albedo, S.Metallic);
    S.ClearCoat     = 0.0;
    S.CoatRoughness = S.Roughness;
    S.CoatN         = S.N;
    S.Anisotropy    = 0.0;
    S.T             = vec3(1.0, 0.0, 0.0);
    S.B             = vec3(0.0, 0.0, 1.0);
    S.Sheen         = vec3(0.0);
    S.Subsurface    = vec3(0.0);
    S.Thickness     = 1.0;
    if(InModel == SHADING_MODEL_CLOTH)
    {
        S.Metallic = 0.0;   // cloth is never metal; its specular is the sheen
    }
    return S;
}

// The model's own inputs, display-space colours as authored. InTangent need not be
// at right angles to the normal, or even set: a zero one takes any direction across it.
void SetLightingSurfaceModel(inout LightingSurface S, float InClearCoat, float InCoatRoughness, vec3 InCoatNormal,
                             float InAnisotropy, vec3 InTangent, vec3 InSheen, vec3 InSubsurface, float InThickness)
{
    S.ClearCoat     = clamp(InClearCoat, 0.0, 1.0);
    S.CoatRoughness = clamp(InCoatRoughness, 0.04, 1.0);
    S.CoatN         = normalize(InCoatNormal);
    S.Anisotropy    = clamp(InAnisotropy, -1.0, 1.0);
    vec3 T = InTangent - S.N * dot(S.N, InTangent);
    if(dot(T, T) < 1e-8)
    {
        T = cross(abs(S.N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), S.N);
    }
    S.T          = normalize(T);
    S.B          = cross(S.N, S.T);
    S.Sheen      = SrgbToLinear(InSheen);
    S.Subsurface = SrgbToLinear(InSubsurface);
    S.Thickness  = clamp(InThickness, 0.0, 1.0);
}

// The model's inputs back from GBuffer4.
void DecodeGBufferCustom(inout LightingSurface S, vec4 InCustom)
{
    if(S.Model == SHADING_MODEL_CLEARCOAT)
    {
        SetLightingSurfaceModel(S, InCustom.r, InCustom.g, DecodeUnitVector(InCustom.ba), 0.0, vec3(0.0), vec3(0.0), vec3(0.0), 1.0);
    }
    else if(S.Model == SHADING_MODEL_ANISOTROPIC)
    {
        SetLightingSurfaceModel(S, 0.0, S.Roughness, S.N, InCustom.r * 2.0 - 1.0, DecodeUnitVector(InCustom.gb),
                                vec3(0.0), vec3(0.0), 1.0);
    }
    else if(S.Model == SHADING_MODEL_CLOTH)
    {
        SetLightingSurfaceModel(S, 0.0, S.Roughness, S.N, 0.0, vec3(0.0), InCustom.rgb, vec3(InCustom.a), 1.0);
    }
    else if(S.Model == SHADING_MODEL_SUBSURFACE || S.Model == SHADING_MODEL_SKIN)
    {
        SetLightingSurfaceModel(S, 0.0, S.Roughness, S.N, 0.0, vec3(0.0), vec3(0.0), InCustom.rgb, InCustom.a);
    }
}

#endif
