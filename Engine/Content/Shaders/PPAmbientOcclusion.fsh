#version 440 core
// PPAmbientOcclusion.fsh -- screen-space ambient occlusion, at half
// resolution, between the Base Pass and the Lighting stage (so only the
// ambient light is darkened; RENDERPIPELINE.md, "Ambient occlusion").
//
// For each pixel: its world position from the scene depth and its normal from
// the G-buffer; Samples points in the hemisphere round that normal, within
// Radius, each projected back to the screen. A point the scene's surface
// there is in front of is occluded -- counted only as far as that surface is
// within Radius, so a far wall does not darken a silhouette. The hemisphere
// is turned a different way per pixel (interleaved gradient noise); the blur
// pass smooths that grain away.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneDepth)   // full resolution, window depth
MATERIAL_SAMPLER2D(5, GBuffer1)     // rgb: world normal * 0.5 + 0.5; a: lit

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_ViewProjection;   // this frame's, GL clip space
    vec4 PP_Ao;               // x intensity, y radius, z samples, w power
    vec4 PP_Sizes;            // zw full resolution
UBO_BLOCK_END(PP_Block)

const float AO_PI = 3.14159265;

vec3 WorldAtUV(vec2 InUV, out float OutDepth)
{
    ivec2 Texel = clamp(ivec2(InUV * PP_Sizes.zw), ivec2(0), ivec2(PP_Sizes.zw) - 1);
    OutDepth = texelFetch(SceneDepth, Texel, 0).r;
    return PP_WorldAt(InUV * 2.0 - 1.0, OutDepth);
}

void main(void)
{
    vec2  UV = Interpolants.TexCoord;
    ivec2 Texel = clamp(ivec2(UV * PP_Sizes.zw), ivec2(0), ivec2(PP_Sizes.zw) - 1);
    vec4  Gb1 = texelFetch(GBuffer1, Texel, 0);
    float Depth;
    vec3  P = WorldAtUV(UV, Depth);
    if(Depth >= 1.0 || Gb1.a < 0.5)
    {
        Output = vec4(1.0);   // the sky, or not a lit surface: nothing to occlude
        return;
    }
    vec3 N = normalize(Gb1.rgb * 2.0 - 1.0);

    // A basis round N, turned by this pixel's noise.
    float Noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3  Helper = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3  T = normalize(cross(Helper, N));
    vec3  B = cross(N, T);

    float Radius   = PP_Ao.y;
    int   Samples  = int(clamp(PP_Ao.z, 4.0, 32.0));
    vec3  Eye      = Scene_CameraPosition.xyz;
    float Occluded = 0.0;
    for(int Index = 0; Index < Samples; ++Index)
    {
        // Cosine-weighted directions on a golden-angle spiral, nearer the
        // centre first, so the samples fill the hemisphere evenly.
        float U1    = (float(Index) + 0.5) / float(Samples);
        float Angle = 2.0 * AO_PI * fract(float(Index) * 0.618034 + Noise);
        float Ring  = sqrt(U1);
        vec3  Local = vec3(Ring * cos(Angle), Ring * sin(Angle), sqrt(max(1.0 - U1, 0.0)));
        float Scale = mix(0.15, 1.0, U1 * U1);
        vec3  S     = P + N * (Radius * 0.02) + (T * Local.x + B * Local.y + N * Local.z) * Radius * Scale;

        vec4 Clip = PP_ViewProjection * vec4(S, 1.0);
        if(Clip.w <= 1e-4) { continue; }
        vec2 SampleUV = Clip.xy / Clip.w * 0.5 + 0.5;
        if(any(lessThan(SampleUV, vec2(0.0))) || any(greaterThan(SampleUV, vec2(1.0)))) { continue; }

        float SceneDepth_;
        vec3  Surface = WorldAtUV(SampleUV, SceneDepth_);
        if(SceneDepth_ >= 1.0) { continue; }
        float SurfaceDistance = length(Surface - Eye);
        float SampleDistance  = length(S - Eye);
        if(SurfaceDistance < SampleDistance - Radius * 0.02)
        {
            // In front of the sample: it occludes, fading out past Radius.
            Occluded += smoothstep(0.0, 1.0, Radius / max(length(Surface - P), 1e-3));
        }
    }

    // Intensity up to 1 fades the effect in; past 1 deepens it.
    float Ambient = pow(clamp(1.0 - Occluded / float(Samples), 0.0, 1.0), PP_Ao.w);
    float Amount  = clamp(PP_Ao.x, 0.0, 4.0);
    Ambient = Amount <= 1.0 ? mix(1.0, Ambient, Amount) : pow(Ambient, Amount);
    Output  = vec4(vec3(Ambient), 1.0);
}
