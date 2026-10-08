#version 450

#include <Common.glsl>

OUTPUT(0, vec4, Output)

SAMPLER2D(2, Stencil)

void main(void) {
    GDIVSToPSInterpolants VSToPSInterpolants = GetInterpolants();
    vec2 TexCoord = VSToPSInterpolants.TexCoord;
    vec4 StencilTexture = texture(Stencil, TexCoord);
    if(StencilTexture.a == 0)
    {
        discard;
    }
    else
    {
        Output = StencilTexture;
    }
}
