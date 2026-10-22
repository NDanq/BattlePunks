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

vec3 ToLinear(vec3 InColor)  { return SrgbToLinear(InColor); }
vec3 ToDisplay(vec3 InColor) { return LinearToSrgb(InColor); }

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

// The surface and its BRDF, by shading model (SHADING_MODELS.md): LightingSurface
// and EvaluateLightModel. Default's lobes are GGX, Smith-Schlick and Schlick
// Fresnel over Lambert, exactly as before shading models.
#include <ShadingModels.glsl>
#ifdef LIGHTING_IBL_ATLAS
#include <IBLShading.glsl>
#endif

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

#ifdef LIGHTING_CONTACT_SHADOWS
// A contact shadow at a pixel toward InL, at most InMaxLength (RENDERPIPELINE.md, "Contact shadows"):
// a short march toward the light through the scene depth, for the shadows the
// shadow maps are too coarse to hold -- under a pot's rim, where a column meets
// its base, between roof tiles. 1 lit, down to 1 - strength. Screen space: a
// caster off screen or hidden casts nothing. LIGHTING_SCENE_DEPTH names the
// pass's depth texture (window depth, GL's 0-1); only the Lighting stage has one.
float ContactShadowWithin(vec3 InWorldPosition, vec3 InNormal, vec3 InL, float InMaxLength)
{
    float Length = min(Scene_ContactShadow.x, InMaxLength);
    if(Length <= 0.0 || dot(InNormal, InL) <= 0.0) { return 1.0; }

    int   Steps    = int(clamp(Scene_ContactShadow.z, 4.0, 32.0));
    vec2  Size     = vec2(textureSize(LIGHTING_SCENE_DEPTH, 0));
    vec3  Forward  = Scene_CameraForward.xyz;
    float Distance = dot(InWorldPosition, Forward) - Scene_CameraForward.w;
    // Off the surface first, further where the depth buffer is coarser; jittered per
    // pixel (interleaved gradient noise) so a few samples do not band.
    vec3  Start    = InWorldPosition + InNormal * (0.3 + Distance * 0.0008);
    float Noise    = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float StepLength = Length / float(Steps);
    for(int Index = 0; Index < Steps; ++Index)
    {
        float Along = (float(Index) + Noise) * StepLength;
        vec3  Sample = Start + InL * Along;
        vec4  Clip   = Scene_ViewProjection * vec4(Sample, 1.0);
        if(Clip.w <= 1e-4) { break; }
        vec2  Uv = NdcToTargetUv(Clip.xy / Clip.w);
        if(any(lessThan(Uv, vec2(0.0))) || any(greaterThan(Uv, vec2(1.0)))) { break; }
        float Depth = texelFetch(LIGHTING_SCENE_DEPTH, ivec2(Uv * Size), 0).r;
        if(Depth >= 1.0) { continue; }   // the sky hides nothing
        vec4  World   = Scene_InvViewProjection * vec4(Clip.xy / Clip.w, Depth * 2.0 - 1.0, 1.0);
        float Surface = dot(World.xyz / World.w, Forward) - Scene_CameraForward.w;
        float Behind  = (dot(Sample, Forward) - Scene_CameraForward.w) - Surface;
        // Behind something, by more than the depth buffer's own error, by less than its thickness.
        if(Behind > 0.05 + Surface * 0.0005 && Behind < Scene_ContactShadow.y)
        {
            // Softer the further along it hit, and gone by the far distance a contact shadow matters at.
            float Fade = (1.0 - Along / Length) * (1.0 - smoothstep(2500.0, 5000.0, Distance));
            return 1.0 - clamp(Scene_ContactShadow.w, 0.0, 1.0) * Fade;
        }
    }
    return 1.0;
}

// The sun's: its full length.
float ContactShadow(vec3 InWorldPosition, vec3 InNormal, vec3 InL)
{
    return ContactShadowWithin(InWorldPosition, InNormal, InL, 1.0e9);
}
#endif

// A material's surface as the lights see it: InModel's BRDF (SHADING_MODEL_*),
// with the model's own inputs (SHADING_MODELS.md). An unset Tangent runs
// across the normal in no particular direction.
LightingSurface LightingSurfaceOf(in MaterialSurface Surface, int InModel, vec3 InWorldPosition, vec3 InNormal)
{
    LightingSurface S = MakeLightingSurface(InModel, ToLinear(Surface.BaseColor), Surface.Metallic, Surface.Roughness,
                                            Surface.Specular, InNormal, normalize(Scene_CameraPosition.xyz - InWorldPosition));
    SetLightingSurfaceModel(S, Surface.ClearCoat, Surface.ClearCoatRoughness, Surface.ClearCoatNormal, Surface.Anisotropy,
                            Surface.Tangent, Surface.SheenColor, Surface.SubsurfaceColor, Surface.Thickness);
    return S;
}

// Linear radiance leaving S toward the eye, from every light and the ambient,
// in two parts: OutDiffuse, the diffuse light before S's albedo multiplies it --
// its ambient part times InAmbientDiffuse, 0 where baked lighting already holds
// it (BakedLighting.glsl) -- and OutSpecular, the rest. InOcclusion darkens the
// ambient only.
void ShadeSurfaceSplit(in LightingSurface S, vec3 InWorldPosition, float InOcclusion, float InAmbientDiffuse,
                       out vec3 OutDiffuse, out vec3 OutSpecular)
{
    bool bTransmits = S.Model == SHADING_MODEL_SUBSURFACE || S.Model == SHADING_MODEL_SKIN;
    vec3 Diffuse, Specular, Transmitted;

    // Modulated shadows darken everything lit here, toward their tint.
    vec3 Modulate = vec3(1.0);

    // The sun.
    vec3  SunL       = normalize(Scene_LightDirection.xyz);
    float SunShadow  = 1.0;
    float SunThrough = 1.0;   // what of the sun reaches a transmitting surface's back (Jimenez et al. 2010)
#ifdef LIGHTING_SHADOWS
    if(Scene_LightShadow.x >= 0.0)
    {
        if(bTransmits) { SunThrough = exp(-DirectionalShadowDepth(InWorldPosition, S.N) / SUBSURFACE_MEAN_FREE_PATH); }
        float Lit = DirectionalShadow(InWorldPosition, S.N, SunL);
        // A live sun's inset shadows (a baked sun's DeferredLighting.fsh lays over the pixel).
        if(Lighting_InsetLight.w < 0.5) { Lit = min(Lit, InsetShadow(InWorldPosition, S.N, SunL)); }
#ifdef LIGHTING_CONTACT_SHADOWS
        Lit = min(Lit, ContactShadow(InWorldPosition, S.N, SunL));
#endif
        if(Scene_LightShadow.x == 1.0) { Modulate *= ModulatedShadow(Lit, 1.0, dot(S.N, SunL), Scene_LightShadowTint.rgb); }
        else                           { SunShadow = Lit; }
    }
#endif
    EvaluateLightModel(S, SunL, ToLinear(Scene_LightColor.rgb) * Scene_LightDirection.w, Diffuse, Specular, Transmitted);
    OutDiffuse  = Diffuse * SunShadow;
    OutSpecular = Specular * SunShadow + Transmitted * SunThrough;

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
        // Facing away and not transmitting: nothing from this light (nor a shadow lookup to find so).
        if(!bTransmits && S.Model != SHADING_MODEL_CLOTH && dot(S.N, L) <= 0.0
           && (S.Model != SHADING_MODEL_CLEARCOAT || dot(S.CoatN, L) <= 0.0)) { continue; }

        float Shadow = 1.0;
#ifdef LIGHTING_SHADOWS
        if(ConeShadow.z >= 0.0)
        {
            vec4  Biases = Scene_Locals[Index * 6 + 5];
            float Lit    = LocalShadow(int(ConeShadow.z), DirectionType.w > 0.5, ConeShadow.x, PositionRange.xyz,
                                       InWorldPosition, S.N, L, Biases.x, Biases.y);
#ifdef LIGHTING_CONTACT_SHADOWS
            // A shadowed point or spot light's contact shadows too ([Shadows] ContactLocalLights):
            // no further than the light itself.
            if(Scene_ContactLocal.x > 0.5) { Lit = min(Lit, ContactShadowWithin(InWorldPosition, S.N, L, Distance * 0.9)); }
#endif
            if(ConeShadow.w == 1.0)
            {
                // Fades out with the light: full where it is at least a quarter strength.
                float Reach = clamp(Attenuation * 4.0, 0.0, 1.0);
                Modulate *= ModulatedShadow(Lit, Reach, dot(S.N, L), Scene_Locals[Index * 6 + 4].rgb);
            }
            else { Shadow = Lit; }
        }
#endif
        EvaluateLightModel(S, L, ToLinear(ColorIntensity.rgb) * ColorIntensity.a * Attenuation, Diffuse, Specular, Transmitted);
        // A point or spot light's transmission is unshadowed: Thickness alone limits it.
        OutDiffuse  += Diffuse * Shadow;
        OutSpecular += Specular * Shadow + Transmitted;
    }

    // +Y is up, so the sky is toward +Y.
    float Up      = clamp(S.N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  Ambient = mix(ToLinear(Scene_GroundColor.rgb), ToLinear(Scene_SkyColor.rgb), Up) * Scene_SkyColor.a;
    vec3  IndirectDiffuse  = Ambient * (1.0 - S.Metallic) * InAmbientDiffuse * InOcclusion;
    vec3  IndirectSpecular = Ambient * S.F0 * (1.0 - S.Roughness) * 0.5 * InOcclusion;
    // Image-based lighting replaces it: in its own pass over the G-buffer (IBLLighting.fsh), or
    // here for a forward Lit material, which has the atlas (LIGHTING_IBL_ATLAS).
    if(Scene_IBL.x > 0.5)
    {
#ifdef LIGHTING_IBL_ATLAS
        IBL_LightModel(LIGHTING_IBL_ATLAS, InWorldPosition, S, InOcclusion, InAmbientDiffuse, vec4(0.0),
                       IndirectDiffuse, IndirectSpecular);
#else
        IndirectDiffuse  = vec3(0.0);
        IndirectSpecular = vec3(0.0);
#endif
    }

    OutDiffuse  = (OutDiffuse + IndirectDiffuse) * Modulate;
    OutSpecular = (OutSpecular + IndirectSpecular) * Modulate;
}

// S's whole light: ShadeSurfaceSplit's two parts, the albedo in.
vec3 ShadeLightingSurface(in LightingSurface S, vec3 InWorldPosition, float InOcclusion, float InAmbientDiffuse)
{
    vec3 Diffuse, Specular;
    ShadeSurfaceSplit(S, InWorldPosition, InOcclusion, InAmbientDiffuse, Diffuse, Specular);
    return S.Albedo * Diffuse + Specular;
}

// Linear radiance leaving a Default surface toward the eye, from every light and
// the ambient -- its diffuse part times InAmbientDiffuse: 0 where baked lighting
// already holds it (BakedLighting.glsl).
vec3 ShadeSurface(in MaterialSurface Surface, vec3 InWorldPosition, vec3 InNormal, float InAmbientDiffuse)
{
    LightingSurface S = LightingSurfaceOf(Surface, SHADING_MODEL_DEFAULT, InWorldPosition, InNormal);
    return ShadeLightingSurface(S, InWorldPosition, Surface.AmbientOcclusion, InAmbientDiffuse);
}

vec3 ShadeSurface(in MaterialSurface Surface, vec3 InWorldPosition, vec3 InNormal)
{
    return ShadeSurface(Surface, InWorldPosition, InNormal, 1.0);
}
