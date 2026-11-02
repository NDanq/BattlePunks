// Wetness.glsl -- the weather's wet surfaces (RenderCore/RENDERPIPELINE.md, "Weather"),
// applied to what the G-buffer holds, so no material needs authoring for it. Include
// after SceneUniforms.glsl. The Lighting stage (DeferredLighting.fsh), image-based
// lighting (IBLLighting.fsh) and screen-space reflections (PPScreenReflections.fsh)
// all call it, so a surface's light, its probe reflections and its traced ones agree.
//
// How wet a pixel is: Scene_Weather.x where the surface faces the sky, fading out by
// the time it stands upright -- walls, the undersides of things and the inside of a
// roof stay dry. Wet porous surfaces (dielectric, rough) darken -- water fills the
// pores and the light that enters them is absorbed sooner -- and every wet surface's
// roughness falls toward a water film's (after Lagarde, "Water drop 2a", 2013). Shading
// models: Default, Clear Coat and Anisotropic only.

#ifndef WETNESS_GLSL
#define WETNESS_GLSL

const float WET_ROUGHNESS = 0.2;    // the smoothest a soaked surface gets (no standing water: no puddles)
const float WET_SMOOTHING = 0.5;    // its roughness x this, soaked, but no smoother than WET_ROUGHNESS
const float WET_DARKEN    = 0.35;   // linear albedo x this, a soaked porous surface's

// 0 dry to 1 soaked, for a pixel of shading model InModel (ShadingModels.glsl) with world
// normal InNormal. Only the hard surfaces' models: foliage cards (Subsurface), skin, cloth
// and volumes keep their look -- a wet leaf's normal says little about where water stands.
float WetAmount(int InModel, vec3 InNormal)
{
    if(InModel != SHADING_MODEL_DEFAULT && InModel != SHADING_MODEL_CLEARCOAT && InModel != SHADING_MODEL_ANISOTROPIC)
    {
        return 0.0;
    }
    return clamp(Scene_Weather.x, 0.0, 1.0) * smoothstep(0.2, 0.7, InNormal.y);
}

// The roughness a wet surface shows.
float WetRoughness(float InWet, float InRoughness)
{
    float Soaked = min(InRoughness, max(WET_ROUGHNESS, InRoughness * WET_SMOOTHING));
    return mix(InRoughness, Soaked, InWet);
}

// InOutAlbedo (linear) darkened and InOutRoughness smoothed by InWet.
void ApplyWetness(float InWet, float InMetallic, inout vec3 InOutAlbedo, inout float InOutRoughness)
{
    if(InWet <= 0.0) { return; }
    float Porosity = (1.0 - clamp(InMetallic, 0.0, 1.0)) * clamp((InOutRoughness - 0.1) / 0.6, 0.0, 1.0);
    InOutAlbedo   *= mix(1.0, WET_DARKEN, InWet * Porosity);
    InOutRoughness = WetRoughness(InWet, InOutRoughness);
}

#endif
