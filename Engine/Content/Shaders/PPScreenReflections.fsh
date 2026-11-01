#version 450 core
// PPScreenReflections.fsh -- screen-space reflections (RenderCore/RENDERPIPELINE.md,
// "Screen-space reflections"), between the Lighting stage and the image-based
// lighting pass, which blends them over the probes' specular.
//
// For each lit pixel smooth enough: the view reflected about its normal, marched
// through the scene depth in world space (steps growing along the ray, the start
// jittered per pixel), then refined between the last two samples. A hit -- a
// sample behind the depth there by less than Thickness, on a surface facing the
// ray -- is reprojected into the last frame (its world motion off, last frame's
// unjittered view) and the last frame's finished linear colour is read there:
// the reflection with all of last frame's light in it. rgb the reflection, a how
// much to trust it -- fading toward the screen's edges, with roughness, along the
// ray and where the history is missing; 0 where the probes must do.

#include <PostProcessCommon.glsl>
#include <Wetness.glsl>

MATERIAL_SAMPLER2D(4, GBuffer1)     // rg: normal, octahedral; a: lit
MATERIAL_SAMPLER2D(5, GBuffer2)     // Metallic, Roughness, Specular, shading model
MATERIAL_SAMPLER2D(6, SceneDepth)
MATERIAL_SAMPLER2D(7, LastColor)    // the last frame's scene colour, linear, before the tone map
MATERIAL_SAMPLER2D(8, Velocity)     // xyz: world motion since the last frame
MATERIAL_SAMPLER2D(9, GBuffer4)     // the shading model's custom data: a Clear Coat's coat (ShadingModels.glsl)

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_PrevViewProjection;   // the last frame's view, unjittered, GL clip space
    vec4 PP_Ssr;                  // x: intensity; y: most roughness reflected; z: steps; w: thickness, world units
    vec4 PP_Sizes;                // x: longest ray, world units; y: 1 if LastColor holds the last frame's; zw: full resolution
                                  // (the G-buffer's; this target may be half that)
UBO_BLOCK_END(PP_Block)

float ViewDepthOf(vec3 InWorld) { return dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w; }

// A microfacet normal from GGX's distribution of visible normals (Heitz 2018,
// "Sampling the GGX Distribution of Visible Normals"), isotropic: InV the view
// in the surface's frame (z the normal), InAlpha the roughness squared.
vec3 SampleVisibleGGX(vec3 InV, float InAlpha, vec2 InU)
{
    vec3  Vh     = normalize(vec3(InAlpha * InV.x, InAlpha * InV.y, InV.z));
    float LenSq  = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3  T1     = LenSq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(LenSq) : vec3(1.0, 0.0, 0.0);
    vec3  T2     = cross(Vh, T1);
    float Radius = sqrt(InU.x);
    float Phi    = 6.28318531 * InU.y;
    float P1     = Radius * cos(Phi);
    float P2     = Radius * sin(Phi);
    float Mix    = 0.5 * (1.0 + Vh.z);
    P2 = (1.0 - Mix) * sqrt(max(1.0 - P1 * P1, 0.0)) + Mix * P2;
    vec3  Nh     = P1 * T1 + P2 * T2 + sqrt(max(0.0, 1.0 - P1 * P1 - P2 * P2)) * Vh;
    return normalize(vec3(InAlpha * Nh.x, InAlpha * Nh.y, max(0.0, Nh.z)));
}

// The scene's surface under InUV: its world position, and whether there is one.
bool SurfaceAt(vec2 InUV, vec2 InSize, out vec3 OutWorld, out ivec2 OutTexel)
{
    OutTexel = clamp(ivec2(InUV * InSize), ivec2(0), ivec2(InSize) - 1);
    float Depth = texelFetch(SceneDepth, OutTexel, 0).r;
    OutWorld = PP_WorldAt(TargetUvToNdc(InUV), Depth);
    return Depth < 1.0;
}

void main(void)
{
    vec2  Size  = PP_Sizes.zw;
    // The full-size texel under this pixel: the march may be drawn at half size ([Rendering] HalfResReflections).
    ivec2 Texel = clamp(ivec2(Interpolants.TexCoord * Size), ivec2(0), ivec2(Size) - 1);
    vec4  Gb1   = texelFetch(GBuffer1, Texel, 0);
    vec4  Gb2   = texelFetch(GBuffer2, Texel, 0);
    vec3  N = DecodeGBufferNormal(Gb1);
    float Roughness = WetRoughness(WetAmount(DecodeShadingModel(Gb2.a), N), Gb2.g);   // a wet surface reflects sharper (Wetness.glsl)
    // A Clear Coat reflects off its coat (SHADING_MODELS.md): its normal and roughness.
    if(DecodeShadingModel(Gb2.a) == SHADING_MODEL_CLEARCOAT)
    {
        vec4 Coat = texelFetch(GBuffer4, Texel, 0);
        if(Coat.r > 0.0)
        {
            N         = DecodeUnitVector(Coat.ba);
            Roughness = Coat.g;
        }
    }
    float MaxRoughness = max(PP_Ssr.y, 0.01);
    if(Gb1.a < 0.5 || Roughness >= MaxRoughness || PP_Sizes.y < 0.5 || PP_Ssr.x <= 0.0)
    {
        Output = vec4(0.0);
        return;
    }
    float Depth = texelFetch(SceneDepth, Texel, 0).r;
    vec3  P = PP_WorldAt(TargetUvToNdc(Interpolants.TexCoord), Depth);
    vec3  V = normalize(Scene_CameraPosition.xyz - P);
    vec3  R = reflect(-V, N);

    float Distance = ViewDepthOf(P);
    float Noise    = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    // Stochastic (PP_Sizes.y 2: PPReflectionResolve.fsh accumulates): the ray off a
    // microfacet normal drawn from GGX's visible normals (Heitz 2018), new each
    // frame, so over frames a rough surface's reflection is its lobe's.
    bool bStochastic = PP_Sizes.y > 1.5;
    if(bStochastic && Roughness > 0.05)
    {
        vec3 T = normalize(cross(abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), N));
        vec3 B = cross(N, T);
        vec2 U = fract(vec2(Noise, fract(52.9829189 * fract(dot(gl_FragCoord.yx, vec2(0.06711056, 0.00583715)))))
                       + fract(Scene_Time.x * vec2(13.7548, 7.5698)));
        vec3 H = SampleVisibleGGX(vec3(dot(V, T), dot(V, B), dot(V, N)), Roughness * Roughness, U);
        vec3 Drawn = reflect(-V, T * H.x + B * H.y + N * H.z);
        if(dot(Drawn, N) > 0.0) { R = Drawn; }
    }
    int   Steps    = int(clamp(PP_Ssr.z, 8.0, 64.0));
    float Longest  = max(PP_Sizes.x, 1.0);
    vec3  Start    = P + N * (0.5 + Distance * 0.001);

    // Steps grow along the ray (t^2), so near detail is found and far reflections still reach.
    float Before = 0.0;
    float HitAt  = -1.0;
    for(int Index = 1; Index <= Steps; ++Index)
    {
        float F     = (float(Index) - 1.0 + Noise) / float(Steps);
        float Along = F * F * Longest;
        vec3  S     = Start + R * Along;
        vec4  Clip  = Scene_ViewProjection * vec4(S, 1.0);
        if(Clip.w <= 1e-4) { break; }
        vec2  UV = NdcToTargetUv(Clip.xy / Clip.w);
        if(any(lessThan(UV, vec2(0.0))) || any(greaterThan(UV, vec2(1.0)))) { break; }
        vec3  Surface;
        ivec2 At;
        if(SurfaceAt(UV, Size, Surface, At))
        {
            float Behind = ViewDepthOf(S) - ViewDepthOf(Surface);
            if(Behind > 0.0 && Behind < PP_Ssr.w + Along * 0.02)
            {
                HitAt = Along;
                break;
            }
        }
        Before = Along;
    }
    if(HitAt < 0.0)
    {
        Output = vec4(0.0);
        return;
    }

    // Refined between the last miss and the hit: halve toward the surface.
    float Low = Before, High = HitAt;
    for(int Index = 0; Index < 5; ++Index)
    {
        float Mid   = (Low + High) * 0.5;
        vec3  S     = Start + R * Mid;
        vec4  Clip  = Scene_ViewProjection * vec4(S, 1.0);
        vec2  UV    = NdcToTargetUv(Clip.xy / max(Clip.w, 1e-4));
        vec3  Surface;
        ivec2 At;
        bool  bThere = SurfaceAt(UV, Size, Surface, At);
        if(bThere && ViewDepthOf(S) > ViewDepthOf(Surface)) { High = Mid; } else { Low = Mid; }
    }
    vec3  S    = Start + R * High;
    vec4  Clip = Scene_ViewProjection * vec4(S, 1.0);
    vec2  HitUV = NdcToTargetUv(Clip.xy / max(Clip.w, 1e-4));
    vec3  Hit;
    ivec2 HitTexel;
    if(!SurfaceAt(HitUV, Size, Hit, HitTexel))
    {
        Output = vec4(0.0);
        return;
    }
    // A surface the ray meets from behind is not what it would see.
    vec3 HitNormal = DecodeGBufferNormal(texelFetch(GBuffer1, HitTexel, 0));
    if(dot(HitNormal, R) > 0.1)
    {
        Output = vec4(0.0);
        return;
    }

    // Where the hit was last frame, and its colour then.
    vec3 Motion   = texelFetch(Velocity, HitTexel, 0).xyz;
    vec4 PrevClip = PP_PrevViewProjection * vec4(Hit - Motion, 1.0);
    if(PrevClip.w <= 1e-4)
    {
        Output = vec4(0.0);
        return;
    }
    vec2 PrevUV = NdcToTargetUv(PrevClip.xy / PrevClip.w);
    if(any(lessThan(PrevUV, vec2(0.0))) || any(greaterThan(PrevUV, vec2(1.0))))
    {
        Output = vec4(0.0);
        return;
    }
    vec3 Color = max(texture(LastColor, Scene_PrevViewUv(PrevUV)).rgb, vec3(0.0));

    // Trust: away from the screen's edges (both ends), smooth enough, not at the ray's end.
    vec2  Edge   = min(HitUV, 1.0 - HitUV);
    vec2  Edge2  = min(PrevUV, 1.0 - PrevUV);
    float Border = smoothstep(0.0, 0.08, min(min(Edge.x, Edge.y), min(Edge2.x, Edge2.y)));
    // Sharp rays fade with roughness well before Max Roughness; stochastic ones, which
    // are the lobe's, only near it.
    float Smooth = 1.0 - smoothstep(MaxRoughness * (bStochastic ? 0.85 : 0.6), MaxRoughness, Roughness);
    float Reach  = 1.0 - smoothstep(0.7, 1.0, High / Longest);
    float Trust  = clamp(Border * Smooth * Reach * PP_Ssr.x, 0.0, 1.0);
    Output = vec4(Color, Trust);
}
