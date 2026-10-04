#version 440 core
// ClearGradient.fsh -- a scene's background as a vertical gradient
// (Renderer::SetClearGradient; RenderCore/RENDERPIPELINE.md, "Background"):
// drawn over the clear, under everything else. A half-step of dither keeps
// the 8-bit target from banding across a large, slow ramp. Also the solid
// clear of an HDR scene (top = bottom), which GDIClear's 8-bit colour cannot
// hold once un-tone-mapped.

#include <PostProcessCommon.glsl>
#include <ToneMapping.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Top;      // rgb, display space
    vec4 PP_Bottom;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    // TexCoord.y is 0 at the bottom of the view on every backend.
    vec3  Color  = mix(PP_Bottom.rgb, PP_Top.rgb, clamp(Interpolants.TexCoord.y, 0.0, 1.0));
    float Dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    // In an HDR scene, what comes out of the tone map as this colour (ToneMapping.glsl).
    Output = vec4(SceneEncodeDisplay(Color + Dither / 255.0), 1.0);
}
