#version 440 core
// DeferredLighting.fsh -- the Lighting stage (RenderCore/RENDERPIPELINE.md):
// one full-screen pass over the G-buffer the Base Pass wrote, drawn with the
// shared full-screen vertex shader (MaterialDefault, Fullscreen input) and the
// material binding layout. Writes the scene colour of every lit pixel --
// exactly what the forward template writes for the same surface -- and
// discards the rest, so the clear colour (and later stages) show there.

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>

// Material texture slots 0-7 (GDI bindings 4-11).
MATERIAL_SAMPLER2D(4, GBuffer0)           // rgb: Base Color; a: Ambient Occlusion
MATERIAL_SAMPLER2D(5, GBuffer1)           // rgb: normal * 0.5 + 0.5; a: lit
MATERIAL_SAMPLER2D(6, GBuffer2)           // Metallic, Roughness, Specular
MATERIAL_SAMPLER2D(7, GBuffer3)           // Emissive, past 1 too (EncodeGBufferEmissive)
MATERIAL_SAMPLER2D(8, SceneDepth)         // r: window depth, 0 near to 1 far
MATERIAL_SAMPLER2D(9, ShadowDirectional)  // the sun's shadow atlas (Shadows.glsl)
MATERIAL_SAMPLER2D(10, ShadowLocal)       // point and spot lights' shadow atlas
MATERIAL_SAMPLER2D(11, AmbientOcclusion)  // r: screen-space ambient occlusion, half size (PPAmbientOcclusion.fsh)

// The pass's own settings, at the material block's binding.
UBO_BLOCK_BEGIN(2, Lighting_Block)
    vec4 Lighting_Params;   // x: 1 if AmbientOcclusion holds this frame's occlusion
UBO_BLOCK_END(Lighting_Block)

// Only here are lights shadowed: the atlases above are this pass's.
#define LIGHTING_SHADOWS 1
#include <Lighting.glsl>

OUTPUT(0, vec4, Output)

void main(void)
{
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

    MaterialSurface Surface;
    Surface.BaseColor        = Gb0.rgb;
    Surface.AmbientOcclusion = Gb0.a;
    Surface.Normal           = normalize(Gb1.rgb * 2.0 - 1.0);
    Surface.Metallic         = Gb2.r;
    Surface.Roughness        = Gb2.g;
    Surface.Specular         = Gb2.b;
    Surface.Emissive         = DecodeGBufferEmissive(Gb3);
    // Screen-space occlusion takes away ambient light only: ShadeSurface
    // applies AmbientOcclusion to its indirect term.
    if(Lighting_Params.x > 0.5)
    {
        Surface.AmbientOcclusion *= texture(AmbientOcclusion, Interpolants.TexCoord).r;
    }
    Surface.Opacity          = 1.0;
    Surface.OpacityMask      = 1.0;

    vec3 Color = ToSceneColor(ToLinear(Surface.Emissive) + ShadeSurface(Surface, WorldPosition, Surface.Normal));
    Output = vec4(Color, 1.0);
}
