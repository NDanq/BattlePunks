#version 450 core
// IBLLighting.fsh -- image-based lighting over the G-buffer, added to the
// lit scene (RENDERPIPELINE.md, "Image-based lighting"): what the Lighting
// stage's hemisphere ambient was, from the probes and the sky instead, by
// each pixel's shading model (SHADING_MODELS.md).

#include <PostProcessCommon.glsl>
#include <IBLCommon.glsl>
#include <LightList.glsl>
#include <ShadingModels.glsl>
#include <IBLShading.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // x: 1 if Subsurface (output 1) is bound: a Skin pixel's diffuse goes there;
                        // y: 1 if ScreenReflections holds this frame's
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, GBuffer0)          // rgb: Base Color; a: Ambient Occlusion, the screen's folded in
MATERIAL_SAMPLER2D(5, GBuffer1)          // rg: normal, octahedral; a: lit
MATERIAL_SAMPLER2D(6, GBuffer2)          // Metallic, Roughness, Specular, shading model
MATERIAL_SAMPLER2D(7, GBuffer4)          // the shading model's custom data
MATERIAL_SAMPLER2D(8, SceneDepth)
MATERIAL_SAMPLER2D(9, Atlas)
MATERIAL_SAMPLER2D(10, ScreenReflections)   // rgb: reflected radiance; a: trust (PPScreenReflections.fsh)

OUTPUT(1, vec4, Subsurface)   // added: a Skin pixel's diffuse before its albedo (DeferredLighting.fsh's)

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

    int Model = DecodeShadingModel(Gb2.a);
    LightingSurface S = MakeLightingSurface(Model, SrgbToLinear(Gb0.rgb), Gb2.r, Gb2.g, Gb2.b,
                                            DecodeGBufferNormal(Gb1), normalize(Scene_CameraPosition.xyz - Position));
    if(Model != SHADING_MODEL_DEFAULT)
    {
        DecodeGBufferCustom(S, texelFetch(GBuffer4, Texel, 0));
    }

    // A baked surface (alpha 2/3, BakedLighting.glsl) has its diffuse ambient already.
    float AmbientDiffuse = Gb1.a < 0.875 ? 0.0 : 1.0;
    vec4 Screen = PP_Settings.y > 0.5 ? texelFetch(ScreenReflections, Texel, 0) : vec4(0.0);
    vec3 Diffuse, Specular;
    IBL_LightModel(Atlas, Position, S, Gb0.a, AmbientDiffuse, Screen, Diffuse, Specular);

    bool bSplit = Model == SHADING_MODEL_SKIN && PP_Settings.x > 0.5;
    Output     = vec4(Specular + (bSplit ? vec3(0.0) : S.Albedo * Diffuse), 1.0);
    Subsurface = vec4(bSplit ? Diffuse : vec3(0.0), 0.0);
}
