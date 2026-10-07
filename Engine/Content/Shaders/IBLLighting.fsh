#version 440 core
// IBLLighting.fsh -- image-based lighting over the G-buffer, added to the
// lit scene (RENDERPIPELINE.md, "Image-based lighting"): what the Lighting
// stage's hemisphere ambient was, from the probes and the sky instead.

#include <PostProcessCommon.glsl>
#include <IBLCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // x: 1 if AmbientOcclusion holds this frame's occlusion; y: 1 if ScreenReflections holds this frame's
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, GBuffer0)          // rgb: Base Color; a: Ambient Occlusion
MATERIAL_SAMPLER2D(5, GBuffer1)          // rgb: normal * 0.5 + 0.5; a: lit
MATERIAL_SAMPLER2D(6, GBuffer2)          // Metallic, Roughness, Specular
MATERIAL_SAMPLER2D(7, SceneDepth)
MATERIAL_SAMPLER2D(8, AmbientOcclusion)  // half size
MATERIAL_SAMPLER2D(9, Atlas)
MATERIAL_SAMPLER2D(10, ScreenReflections)   // rgb: reflected radiance; a: trust (PPScreenReflections.fsh)

void main(void)
{
    ivec2 Texel = ivec2(gl_FragCoord.xy);
    vec4  Gb1   = texelFetch(GBuffer1, Texel, 0);
    if(Gb1.a < 0.5 || Scene_View.x > 0.5) { discard; }   // unlit pixels, and every pixel in the Unlit view mode
    vec4  Gb0   = texelFetch(GBuffer0, Texel, 0);
    vec4  Gb2   = texelFetch(GBuffer2, Texel, 0);
    float Depth = texelFetch(SceneDepth, Texel, 0).r;

    vec4 World    = Scene_InvViewProjection * vec4(Interpolants.Position.xy, Depth * 2.0 - 1.0, 1.0);
    vec3 Position = World.xyz / World.w;
    vec3 N        = normalize(Gb1.rgb * 2.0 - 1.0);
    vec3 V        = normalize(Scene_CameraPosition.xyz - Position);

    float Occlusion = Gb0.a;
    if(PP_Settings.x > 0.5) { Occlusion *= texture(AmbientOcclusion, Interpolants.TexCoord).r; }
    vec3  Albedo    = pow(max(Gb0.rgb, vec3(0.0)), vec3(2.2));
    float Metallic  = Gb2.r;
    float Roughness = clamp(Gb2.g, 0.04, 1.0);
    vec3  F0        = mix(vec3(0.08 * Gb2.b), Albedo, Metallic);

    // A baked surface (alpha 0.75, BakedLighting.glsl) has its diffuse ambient already.
    float Diffuse = Gb1.a < 0.875 ? 0.0 : 1.0;
    vec4 Screen = PP_Settings.y > 0.5 ? texelFetch(ScreenReflections, Texel, 0) : vec4(0.0);
    Output = vec4(IBL_LightReflected(Atlas, Position, N, V, Albedo, Metallic, Roughness, F0, Occlusion, Diffuse, Screen), 1.0);
}
