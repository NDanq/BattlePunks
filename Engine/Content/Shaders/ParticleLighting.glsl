// ParticleLighting.glsl -- per-particle lighting (RenderCore/LIT_TRANSLUCENCY.md,
// Phase 6): a Translucent Unlit material with Particle Lighting Per Particle has
// each instance lit once, at its centre, in its own vertex shader
// (MaterialTemplate_VS.glsl), and its colour times that light -- so an overdrawn
// plume costs what an unlit one does. The Volume model's light (ShadingModels.glsl):
// the sun through its shadow maps, every point and spot light in the list (point
// lights for the area ones; no tiles here, every light is looked at), the probes'
// or a GI Volume's irradiance up and down averaged, through a Henyey-Greenstein
// phase of MATERIAL_PARTICLE_PHASE. Included by the vertex template only.

#ifndef PARTICLE_LIGHTING_GLSL
#define PARTICLE_LIGHTING_GLSL

#include <IBLCommon.glsl>
MATERIAL_SAMPLER2D(4, Particle_IBLAtlas)       // slot 0: the probe atlas, as a forward Lit material's
#include <DDGI.glsl>
MATERIAL_SAMPLER2D(15, ShadowDirectional)      // engine slots 11 and 12, as forward Lit materials'
MATERIAL_SAMPLER2D(16, ShadowLocal)
MATERIAL_SAMPLER2D(19, TranslucentOpacity)     // engine slot 15: translucent casters' opacity (Phase 7)
#define LIGHTING_TRANSLUCENT_SHADOWS 1
#define LIGHTING_SHADOW_RADIUS 0
#include <Shadows.glsl>

#ifndef MATERIAL_PARTICLE_PHASE
#define MATERIAL_PARTICLE_PHASE 0.0
#endif

// Henyey-Greenstein times 4 pi over pi: an even medium as bright as Lambert facing the light.
float Particle_Phase(vec3 InL, vec3 InV)
{
    float G     = clamp(float(MATERIAL_PARTICLE_PHASE), -0.9, 0.9);
    float Denom = max(1.0 + G * G + 2.0 * G * dot(InL, InV), 1e-4);
    return (1.0 - G * G) / (Denom * sqrt(Denom)) / 3.14159265;
}

// The light a particle at InPosition sends toward the eye, linear, before its colour.
vec3 Particle_Light(vec3 InPosition)
{
    vec3 V = normalize(Scene_CameraPosition.xyz - InPosition);

    // The sun, shadowed (its maps' one tap; the normal offset toward it).
    vec3  SunL   = normalize(Scene_LightDirection.xyz);
    float Shadow = 1.0;
    vec3  Tint   = vec3(1.0);
    if(Scene_LightShadow.x >= 0.0)
    {
        float Lit = DirectionalShadow(InPosition, SunL, SunL);
        if(Scene_LightShadow.x == 1.0) { Tint = mix(SrgbToLinear(Scene_LightShadowTint.rgb), vec3(1.0), Lit); }
        else                           { Shadow = Lit; }
    }
    vec3 Light = SrgbToLinear(Scene_LightColor.rgb) * Scene_LightDirection.w * Particle_Phase(SunL, V) * Shadow * Tint;

    // Every point and spot light.
    int Count = int(LightList_Header.x);
    for(int Index = 0; Index < Count; ++Index)
    {
        vec4  PositionRange  = LightList_Lights[Index * LIGHT_LIST_VEC4S + 0];
        vec4  ColorIntensity = LightList_Lights[Index * LIGHT_LIST_VEC4S + 1];
        vec4  DirectionType  = LightList_Lights[Index * LIGHT_LIST_VEC4S + 2];
        vec4  Cone           = LightList_Lights[Index * LIGHT_LIST_VEC4S + 3];
        vec3  ToLight  = PositionRange.xyz - InPosition;
        float Distance = length(ToLight);
        if(Distance >= PositionRange.w) { continue; }
        vec3  L = ToLight / max(Distance, 1e-4);
        float X = Distance / max(PositionRange.w, 1e-4);
        float Window = clamp(1.0 - X * X * X * X, 0.0, 1.0);
        float Attenuation = Window * Window / (1.0 + 16.0 * X * X);
        bool  bSpot = DirectionType.w > 0.5 && DirectionType.w < 1.5;
        if(bSpot) { Attenuation *= smoothstep(Cone.x, Cone.y, dot(-L, DirectionType.xyz)); }
        if(DirectionType.w > 1.5 && dot(InPosition - PositionRange.xyz, DirectionType.xyz) <= 0.0) { continue; }   // behind a rect
        if(Attenuation <= 0.0) { continue; }
        float Lit  = 1.0;
        int   Slot = int(Cone.z);
        vec4  ConeShadow = Slot >= 0 ? Scene_Locals[Slot * 6 + 3] : vec4(-1.0);
        if(ConeShadow.z >= 0.0)
        {
            vec4 Biases = Scene_Locals[Slot * 6 + 5];
            Lit = LocalShadow(int(ConeShadow.z), bSpot, ConeShadow.x, PositionRange.xyz, InPosition, L, L, Biases.x, Biases.y, Biases.zw);
            if(ConeShadow.w == 1.0) { Lit = mix(1.0, Lit, clamp(Attenuation * 4.0, 0.0, 1.0)); }
        }
        Light += SrgbToLinear(ColorIntensity.rgb) * ColorIntensity.a * Attenuation * Particle_Phase(L, V) * Lit;
    }

    // The ambient, from every side alike.
    vec3 Ambient = 0.5 * (SrgbToLinear(Scene_SkyColor.rgb) + SrgbToLinear(Scene_GroundColor.rgb)) * Scene_SkyColor.a;
    if(Scene_IBL.x > 0.5)
    {
        Ambient = 0.5 * (IBL_GatherIrradiance(Particle_IBLAtlas, InPosition, vec3(0.0, 1.0, 0.0))
                       + IBL_GatherIrradiance(Particle_IBLAtlas, InPosition, vec3(0.0, -1.0, 0.0)));
        float Held, HeldDown;
        vec3  Volume = 0.5 * (DDGI_Irradiance(InPosition, vec3(0.0, 1.0, 0.0), V, Held)
                            + DDGI_Irradiance(InPosition, vec3(0.0, -1.0, 0.0), V, HeldDown));
        Ambient = mix(Ambient, Volume, Held);
    }
    return Light + Ambient;
}

#endif
