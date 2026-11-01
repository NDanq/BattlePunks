#version 450 core
// DDGIStore.fsh -- what a GI Volume probe's cube face saw, kept so the probe can
// be lit again without drawing the scene again (DDGIRelight.fsh;
// RenderCore/GLOBAL_ILLUMINATION.md, "Relighting"). Drawn after the face's
// stages, its viewport the face's tile in the volume's cache (probe i's six
// faces side by side in its row), from the face's G-buffer, depth and lit
// colour, every one at this texel's place in the face:
//
//   Output (RGBA16F)       rgb: the diffuse albedo, linear (Base Color times
//                          1 - Metallic); a: the distance from the probe to
//                          what the texel sees, world units -- -1 for the sky
//   Second (RGBA32F)       rgb: the emissive, linear -- or, where no lit
//                          surface was drawn (an Unlit material), the colour
//                          it drew, which no light changes; a: the normal
//                          (octahedral, 10 bits each) and whether its diffuse
//                          ambient is live (not baked), packed as a whole
//                          number a float holds exactly

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, GBuffer0)     // rgb: Base Color (display); a: occlusion
MATERIAL_SAMPLER2D(5, GBuffer1)     // rg: normal, octahedral; a: lit (1), baked ambient (2/3)
MATERIAL_SAMPLER2D(6, GBuffer2)     // Metallic, Roughness, Specular, shading model
MATERIAL_SAMPLER2D(7, GBuffer3)     // Emissive (EncodeGBufferEmissive)
MATERIAL_SAMPLER2D(8, SceneDepth)   // window depth
MATERIAL_SAMPLER2D(9, SceneColor)   // the face as lit, linear

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // xy: the tile's corner in the cache, texels
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

OUTPUT(1, vec4, Second)

void main(void)
{
    ivec2 Texel = ivec2(gl_FragCoord.xy) - ivec2(PP_Settings.xy);
    float Depth = texelFetch(SceneDepth, Texel, 0).r;
    if(Depth >= 1.0)
    {
        Output = vec4(0.0, 0.0, 0.0, -1.0);
        Second = vec4(0.0);
        return;
    }
    vec2  Uv       = (vec2(Texel) + 0.5) / vec2(textureSize(SceneDepth, 0));
    float Distance = length(PP_WorldAt(TargetUvToNdc(Uv), Depth) - Scene_CameraPosition.xyz);
    vec4  Gb1      = texelFetch(GBuffer1, Texel, 0);
    if(Gb1.a < 0.5)
    {
        // Nothing lit drew here: an Unlit material's colour, as it is.
        Output = vec4(0.0, 0.0, 0.0, Distance);
        Second = vec4(max(texelFetch(SceneColor, Texel, 0).rgb, vec3(0.0)), 0.0);
        return;
    }
    vec4  Gb0      = texelFetch(GBuffer0, Texel, 0);
    vec4  Gb2      = texelFetch(GBuffer2, Texel, 0);
    vec3  Albedo   = SrgbToLinear(Gb0.rgb) * (1.0 - clamp(Gb2.r, 0.0, 1.0));
    vec3  Emissive = SrgbToLinear(DecodeGBufferEmissive(texelFetch(GBuffer3, Texel, 0)));
    vec2  Normal   = floor(clamp(Gb1.rg, 0.0, 1.0) * 1023.0 + 0.5);
    float Live     = Gb1.a < 0.875 ? 0.0 : 1.0;
    Output = vec4(Albedo, Distance);
    Second = vec4(Emissive, (Normal.x * 1024.0 + Normal.y) * 2.0 + Live);
}
