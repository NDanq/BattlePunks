#version 450 core
// PPDofComposite.fsh -- Depth of Field, 3 of 3: the sharp scene where the
// circle of confusion is small, the blurred half-resolution one where it is
// large.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneColor)
MATERIAL_SAMPLER2D(5, Blurred)     // PPDofBlur's output: rgb, CoC in alpha

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Focus;   // z max blur (px, full res)
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2  UV      = Interpolants.TexCoord;
    vec3  Sharp   = texture(SceneColor, UV).rgb;
    vec4  Blur    = texture(Blurred, UV);
    float Coc     = abs(Blur.a * 2.0 - 1.0);
    // Under about a pixel of blur, the sharp image is the better one.
    float Amount  = smoothstep(0.0, 1.0, Coc * PP_Focus.z * 0.5);
    Output = vec4(mix(Sharp, Blur.rgb, Amount), 1.0);
}
