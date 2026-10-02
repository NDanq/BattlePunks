#version 440 core

#include <Common.glsl>

OUTPUT(0, vec4, Output)

SAMPLER2D(2, ScreenTexture)

void main(void)
{
    GDIVSToPSInterpolants VSToPSInterpolants = GetInterpolants();

    Output = texture(ScreenTexture, VSToPSInterpolants.TexCoord);
}
