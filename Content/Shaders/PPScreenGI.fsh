#version 450 core
// PPScreenGI.fsh -- screen-space diffuse GI (RenderCore/GLOBAL_ILLUMINATION.md,
// Phase 5; Ritschel 2009, Mara 2016), at half size, between the Lighting stage
// and the image-based lighting pass, which takes it over the probes' irradiance.
//
// For each lit pixel: a few cosine-weighted rays about its normal (new each
// frame), marched through the scene depth as PPScreenReflections.fsh's are; a
// hit on a surface facing the ray is reprojected into the last frame and the
// last frame's finished linear colour read there -- light that surface sent
// out, this pass's own bounce included, so light bounces again each frame.
// PPReflectionResolve.fsh accumulates it over frames. rgb: the hits' radiance
// summed over all the rays (a Lambertian surface's irradiance / pi, the misses
// none); a: the share of rays that hit -- the probes light the rest.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, GBuffer1)     // rg: normal, octahedral; a: lit
MATERIAL_SAMPLER2D(5, SceneDepth)
MATERIAL_SAMPLER2D(6, LastColor)    // the last frame's scene colour, linear, before the tone map
MATERIAL_SAMPLER2D(7, Velocity)     // xyz: world motion since the last frame

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_PrevViewProjection;   // the last frame's view, unjittered, GL clip space
    vec4 PP_Gi;                   // x: intensity; y: rays a pixel; z: radius, world units; w: steps a ray
    vec4 PP_Sizes;                // xy: this target's size; zw: full resolution
UBO_BLOCK_END(PP_Block)

float ViewDepthOf(vec3 InWorld) { return dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w; }

// The scene's surface under InUV (full-resolution targets): its world position, and whether there is one.
bool SurfaceAt(vec2 InUV, vec2 InSize, out vec3 OutWorld, out ivec2 OutTexel)
{
    OutTexel = clamp(ivec2(InUV * InSize), ivec2(0), ivec2(InSize) - 1);
    float Depth = texelFetch(SceneDepth, OutTexel, 0).r;
    OutWorld = PP_WorldAt(TargetUvToNdc(InUV), Depth);
    return Depth < 1.0;
}

float Hash(vec2 InAt) { return fract(52.9829189 * fract(dot(InAt, vec2(0.06711056, 0.00583715)))); }

void main(void)
{
    vec2  Full  = PP_Sizes.zw;
    vec2  UV    = Interpolants.TexCoord;
    ivec2 Texel = clamp(ivec2(UV * Full), ivec2(0), ivec2(Full) - 1);
    vec4  Gb1   = texelFetch(GBuffer1, Texel, 0);
    float Depth = texelFetch(SceneDepth, Texel, 0).r;
    if(Gb1.a < 0.5 || Depth >= 1.0 || PP_Gi.x <= 0.0)
    {
        Output = vec4(0.0);
        return;
    }
    vec3  N        = DecodeGBufferNormal(Gb1);
    vec3  P        = PP_WorldAt(TargetUvToNdc(UV), Depth);
    float Distance = ViewDepthOf(P);
    vec3  Start    = P + N * (0.5 + Distance * 0.002);
    vec3  T        = normalize(cross(abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), N));
    vec3  B        = cross(N, T);

    int   Rays   = int(clamp(PP_Gi.y, 1.0, 16.0));
    int   Steps  = int(clamp(PP_Gi.w, 4.0, 32.0));
    float Radius = max(PP_Gi.z, 1.0);
    vec2  Frame  = fract(Scene_Time.x * vec2(13.7548, 7.5698));
    float Jitter = fract(Hash(gl_FragCoord.xy) + Frame.x);

    vec3  Sum  = vec3(0.0);
    float Hits = 0.0;
    for(int Ray = 0; Ray < Rays; ++Ray)
    {
        // Cosine-weighted about N: a disc's point lifted onto the hemisphere.
        vec2  U   = fract(vec2(Hash(gl_FragCoord.xy + vec2(float(Ray) * 7.0, 3.0)), Hash(gl_FragCoord.yx + vec2(5.0, float(Ray) * 11.0)))
                          + Frame + float(Ray) * vec2(0.618034, 0.754878));
        float R   = sqrt(U.x);
        float Phi = 6.28318531 * U.y;
        vec3  D   = normalize(T * (R * cos(Phi)) + B * (R * sin(Phi)) + N * sqrt(max(1.0 - U.x, 0.0)));

        // Steps grow along the ray (t^2): near contact is found, the radius still reached.
        for(int Index = 1; Index <= Steps; ++Index)
        {
            float F     = (float(Index) - 1.0 + Jitter) / float(Steps);
            float Along = F * F * Radius;
            vec3  S     = Start + D * Along;
            vec4  Clip  = Scene_ViewProjection * vec4(S, 1.0);
            if(Clip.w <= 1e-4) { break; }
            vec2 At = NdcToTargetUv(Clip.xy / Clip.w);
            if(any(lessThan(At, vec2(0.0))) || any(greaterThan(At, vec2(1.0)))) { break; }
            vec3  Surface;
            ivec2 HitTexel;
            if(!SurfaceAt(At, Full, Surface, HitTexel)) { continue; }
            float Behind = ViewDepthOf(S) - ViewDepthOf(Surface);
            if(Behind <= 0.0) { continue; }
            if(Behind > 15.0 + Along * 0.1) { continue; }   // passed behind something, not into it
            vec4 HitGb1 = texelFetch(GBuffer1, HitTexel, 0);
            if(dot(DecodeGBufferNormal(HitGb1), D) > 0.1) { break; }   // its back: not what the ray would see

            // Where the hit was last frame, and its colour then.
            vec3 Motion   = texelFetch(Velocity, HitTexel, 0).xyz;
            vec4 PrevClip = PP_PrevViewProjection * vec4(Surface - Motion, 1.0);
            if(PrevClip.w <= 1e-4) { break; }
            vec2 PrevUV = NdcToTargetUv(PrevClip.xy / PrevClip.w);
            if(any(lessThan(PrevUV, vec2(0.0))) || any(greaterThan(PrevUV, vec2(1.0)))) { break; }
            // Fades toward the radius, so where the march gives up the probes take over smoothly.
            float Weight = 1.0 - smoothstep(0.7, 1.0, F);
            Sum  += max(textureLod(LastColor, PrevUV, 0.0).rgb, vec3(0.0)) * Weight;
            Hits += Weight;
            break;
        }
    }
    float Intensity = clamp(PP_Gi.x, 0.0, 1.0);
    Output = vec4(Sum / float(Rays), Hits / float(Rays)) * Intensity;
}
