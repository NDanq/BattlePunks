#version 450 core
// DeferredLighting.fsh -- the Lighting stage (RenderCore/RENDERPIPELINE.md):
// one full-screen pass over the G-buffer the Base Pass wrote, drawn with the
// shared full-screen vertex shader (MaterialDefault, Fullscreen input) and the
// material binding layout. Writes the scene colour of every lit pixel --
// exactly what the forward template writes for the same surface -- and
// discards the rest, so the clear colour (and later stages) show there.
// Each pixel is lit by its own shading model (RenderCore/SHADING_MODELS.md).

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>

// Material texture slots 0-7 (GDI bindings 4-11).
MATERIAL_SAMPLER2D(4, GBuffer0)           // rgb: Base Color; a: Ambient Occlusion, the screen's folded in (PPAmbientApply.fsh)
MATERIAL_SAMPLER2D(5, GBuffer1)           // rg: normal, octahedral; a: lit (1), or lit with its ambient baked (2/3)
MATERIAL_SAMPLER2D(6, GBuffer2)           // Metallic, Roughness, Specular, shading model
MATERIAL_SAMPLER2D(7, GBuffer3)           // Emissive, past 1 too (EncodeGBufferEmissive)
MATERIAL_SAMPLER2D(8, GBuffer4)           // the shading model's custom data (ShadingModels.glsl)
MATERIAL_SAMPLER2D(9, SceneDepth)         // r: window depth, 0 near to 1 far
MATERIAL_SAMPLER2D(10, ShadowDirectional) // the sun's shadow atlas (Shadows.glsl)
MATERIAL_SAMPLER2D(11, ShadowLocal)       // point and spot lights' shadow atlas

// The pass's own settings, at the material block's binding.
UBO_BLOCK_BEGIN(2, Lighting_Block)
    vec4 Lighting_Params;      // x: 1 if Subsurface (output 1) is bound: a Skin pixel's diffuse goes there;
                               // y: inset shadows; z, w: their depth and normal bias (the sun's)
    vec4 Lighting_InsetLight;  // xyz: unit, toward the inset shadows' sun; w: 1 if it is baked --
                               // Modulated over the whole pixel, baked light included
    vec4 Lighting_InsetTint;   // rgb: that Modulated shadow's tint, display space; w: the insets' PCF radius, texels
    vec4 Lighting_Insets[4];   // x: its matrix in Scene_LocalShadowMatrices; y: its texel, world units;
                               // zw: its tile's corner, atlas UV (Shadows.glsl's InsetShadow)
    vec4 Lighting_InsetRects[4]; // the pixels each can shadow (gl_FragCoord): xy min, zw max
UBO_BLOCK_END(Lighting_Block)

// Only here are lights shadowed: the atlases above are this pass's -- and only here
// is there a scene depth for the sun's contact shadows to march through.
#define LIGHTING_SHADOWS 1
#define LIGHTING_CONTACT_SHADOWS 1
#define LIGHTING_SCENE_DEPTH SceneDepth
#include <Lighting.glsl>

OUTPUT(0, vec4, Output)
OUTPUT(1, vec4, Subsurface)   // a Skin pixel's diffuse light before its albedo, a 1 (PPSubsurface.fsh blurs it); else 0

void main(void)
{
    Subsurface = vec4(0.0);

    // The same texel every target wrote at this pixel.
    ivec2 Texel = ivec2(gl_FragCoord.xy);
    vec4  Gb1   = texelFetch(GBuffer1, Texel, 0);
    if(Gb1.a < 0.5) { discard; }

    vec4  Gb0   = texelFetch(GBuffer0, Texel, 0);
    vec4  Gb2   = texelFetch(GBuffer2, Texel, 0);
    vec4  Gb3   = texelFetch(GBuffer3, Texel, 0);
    float Depth = texelFetch(SceneDepth, Texel, 0).r;

    // The full-screen quad's interpolated position is this pixel's NDC, in the
    // GL convention every backend's matrices use; Vulkan and D3D12 remap the
    // projection so their stored depth is GL's window depth too.
    vec4 Clip  = vec4(Interpolants.Position.xy, Depth * 2.0 - 1.0, 1.0);
    vec4 World = Scene_InvViewProjection * Clip;
    vec3 WorldPosition = World.xyz / World.w;

    vec3 Emissive = DecodeGBufferEmissive(Gb3);

    // Unlit view mode, or a preview's channel: no light at all.
    if(Scene_View.x > 0.5)
    {
        Output = vec4(ToSceneColor(ViewChannelColor(Gb0.rgb, Emissive)), 1.0);
        return;
    }

    // The surface, by its shading model. Ambient Occlusion -- the material's
    // times the screen's -- takes away ambient light only (ShadeSurfaceSplit).
    int Model = DecodeShadingModel(Gb2.a);
    LightingSurface S = MakeLightingSurface(Model, ToLinear(Gb0.rgb), Gb2.r, Gb2.g, Gb2.b, DecodeGBufferNormal(Gb1),
                                            normalize(Scene_CameraPosition.xyz - WorldPosition));
    if(Model != SHADING_MODEL_DEFAULT)
    {
        DecodeGBufferCustom(S, texelFetch(GBuffer4, Texel, 0));
    }

    // A baked surface carries its diffuse ambient in its emissive (BakedLighting.glsl).
    float AmbientDiffuse = Gb1.a < 0.875 ? 0.0 : 1.0;
    vec3 Diffuse, Specular;
    ShadeSurfaceSplit(S, WorldPosition, Gb0.a, AmbientDiffuse, Diffuse, Specular);

    vec3 Linear = ToLinear(Emissive) + Specular;
    if(Model == SHADING_MODEL_SKIN && Lighting_Params.x > 0.5) { Subsurface = vec4(Diffuse, 1.0); }
    else                                                       { Linear += S.Albedo * Diffuse; }

    // A baked sun has no live light to take away: its inset shadows darken
    // everything here toward its tint, the bake's light (in the emissive) too.
    if(Lighting_InsetLight.w > 0.5 && Lighting_Params.y > 0.5)
    {
        float Lit = InsetShadow(WorldPosition, S.N, Lighting_InsetLight.xyz);
        vec3  Darken = ModulatedShadow(Lit, 1.0, dot(S.N, Lighting_InsetLight.xyz), Lighting_InsetTint.rgb);
        Linear *= Darken;
        Subsurface.rgb *= Darken;
    }
    Output = vec4(ToSceneColor(Linear), 1.0);
}
