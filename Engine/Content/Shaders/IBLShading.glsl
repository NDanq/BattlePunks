// IBLShading.glsl -- image-based light by shading model (RenderCore/SHADING_MODELS.md):
// IBLCommon.glsl's probes and sky, lobe by lobe as ShadingModels.glsl's
// EvaluateLightModel lights them. Include both of those first.

#ifndef IBL_SHADING_GLSL
#define IBL_SHADING_GLSL

// The image-based light S receives at InPosition. OutDiffuse: the diffuse light
// before the albedo multiplies it (Skin keeps it apart), times InDiffuse (0 where
// baked lighting holds it). OutSpecular: everything else -- the specular lobes,
// Cloth's sheen, and Subsurface's and Skin's light through from behind.
// InScreen: a screen-space reflection (PPScreenReflections.fsh), rgb blended
// over the top layer's probes by a; zero where there is none. InBounce:
// screen-space GI (PPScreenGI.fsh) -- rgb the light its rays found, a the share
// of them that found any; the probes light the rest. With DDGI.glsl included
// first, a GI Volume's probes stand in for the reflection probes' irradiance
// where it is.
void IBL_LightModelBounce(sampler2D InAtlas, vec3 InPosition, in LightingSurface S, float InOcclusion, float InDiffuse,
                          vec4 InScreen, vec4 InBounce, out vec3 OutDiffuse, out vec3 OutSpecular)
{
    vec3 Ambient = IBL_GatherIrradiance(InAtlas, InPosition, S.N);
#ifdef DDGI_GLSL
    if(InDiffuse > 0.0)
    {
        float Held;
        vec3  Volume = DDGI_Irradiance(InPosition, S.N, S.V, Held);
        Ambient = mix(Ambient, Volume, Held);
    }
#endif
    vec3 Irradiance = InBounce.rgb + Ambient * (1.0 - clamp(InBounce.a, 0.0, 1.0));
    float Screen    = clamp(InScreen.a, 0.0, 1.0);

    if(S.Model == SHADING_MODEL_CLOTH)
    {
        // Prefiltered radiance times the sheen's DG average (Filament 4.12.2).
        vec3 R = reflect(-S.V, S.N);
        vec3 Sheen = mix(IBL_GatherSpecular(InAtlas, InPosition, R, S.Roughness), InScreen.rgb, Screen);
        OutDiffuse  = Irradiance * InOcclusion * InDiffuse;
        OutSpecular = Sheen * S.Sheen * IBL_SheenDG(InAtlas, S.NdotV, S.Roughness)
                    * IBL_SpecularOcclusion(S.NdotV, InOcclusion, S.Roughness);
        return;
    }

    // The base layer's reflection direction: an anisotropic surface's bent toward
    // the grain's normal, more the rougher it is (Revie 2011; McAuley 2015; Filament 5.3.4.1).
    vec3 BaseN = S.N;
    if(S.Model == SHADING_MODEL_ANISOTROPIC)
    {
        vec3  Direction   = S.Anisotropy >= 0.0 ? S.B : S.T;
        vec3  AnisoT      = cross(Direction, S.V);
        vec3  AnisoN      = cross(AnisoT, Direction);
        float Bend        = abs(S.Anisotropy) * clamp(5.0 * S.Roughness, 0.0, 1.0);
        BaseN = normalize(mix(S.N, AnisoN, Bend));
    }

    vec3  F0        = S.F0;
    float Roughness = S.Roughness;
    float Through   = 1.0;
    vec3  Coat      = vec3(0.0);
    if(S.Model == SHADING_MODEL_CLEARCOAT)
    {
        float CoatNdotV = max(dot(S.CoatN, S.V), 1e-4);
        float Fc        = (0.04 + 0.96 * pow(1.0 - CoatNdotV, 5.0)) * S.ClearCoat;
        vec3  CoatR     = reflect(-S.V, S.CoatN);
        vec3  CoatLight = mix(IBL_GatherSpecular(InAtlas, InPosition, CoatR, S.CoatRoughness), InScreen.rgb, Screen);
        Coat      = CoatLight * Fc * IBL_SpecularOcclusion(CoatNdotV, InOcclusion, S.CoatRoughness);
        Through   = 1.0 - Fc;
        F0        = mix(F0, SM_F0UnderCoat(F0), S.ClearCoat);
        Roughness = mix(Roughness, max(Roughness, S.CoatRoughness), S.ClearCoat);
        Screen    = 0.0;   // the coat took the screen's reflection
    }

    vec3 R        = reflect(-S.V, BaseN);
    vec3 Specular = mix(IBL_GatherSpecular(InAtlas, InPosition, R, Roughness), InScreen.rgb, Screen);
    vec3 EnvBrdf  = IBL_EnvBRDF(F0, Roughness, S.NdotV);
    float SpecOcc = IBL_SpecularOcclusion(S.NdotV, InOcclusion, Roughness);
    OutDiffuse  = Irradiance * (1.0 - S.Metallic) * InOcclusion * InDiffuse * Through;
    OutSpecular = Specular * EnvBrdf * SM_EnergyCompensation(F0, Roughness, S.NdotV) * SpecOcc * Through + Coat;

    if(S.Model == SHADING_MODEL_SUBSURFACE || S.Model == SHADING_MODEL_SKIN)
    {
        // The sky and probes behind it, through it: the back scatter of SM_Transmission, averaged.
        OutSpecular += IBL_GatherIrradiance(InAtlas, InPosition, -S.N) * S.Subsurface * (0.5 * (1.0 - S.Thickness));
    }
}

void IBL_LightModel(sampler2D InAtlas, vec3 InPosition, in LightingSurface S, float InOcclusion, float InDiffuse,
                    vec4 InScreen, out vec3 OutDiffuse, out vec3 OutSpecular)
{
    IBL_LightModelBounce(InAtlas, InPosition, S, InOcclusion, InDiffuse, InScreen, vec4(0.0), OutDiffuse, OutSpecular);
}

#endif
