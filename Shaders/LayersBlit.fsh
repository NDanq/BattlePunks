#version 440 core

#include <Common.glsl>

OUTPUT(0, vec4, Output)

UBO_BEGIN(2, LayersBlit_Block)
    SHADER_UNIFORM(float, Opacity)
UBO_END(LayersBlit_Block)

SAMPLER2D(3, ScreenTexture)

void main(void)
{
    GDIVSToPSInterpolants VSToPSInterpolants = GetInterpolants();
    vec2 TexCoord = VSToPSInterpolants.TexCoord;
    TexCoord.y = 1.f - TexCoord.y;
    vec4 SceneColor = texture(ScreenTexture, TexCoord);
    float SceneColorAlpha = SceneColor.a;
    if(SceneColorAlpha < 0.0001)
    {
        discard;
    }

    vec3 SceneColorRGB = SceneColor.rgb;
    Output = vec4(SceneColorRGB, Opacity);
}
