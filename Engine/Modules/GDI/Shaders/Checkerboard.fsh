#version 450 core

#include <Common.glsl>

OUTPUT(0, vec4, Output)

void main(void)
{
    GDIVSToPSInterpolants VSToPSInterpolants = GetInterpolants();
    vec2 TexCoord = VSToPSInterpolants.TexCoord;
    vec2 Pos = floor(TexCoord * 50.f);
    float PatternMask = mod(Pos.x + mod(Pos.y, 2), 2);
    Output = PatternMask * vec4(0.53, 0.53, 0.53, 1.0);
}
