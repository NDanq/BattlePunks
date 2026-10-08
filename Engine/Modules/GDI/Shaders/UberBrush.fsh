#version 450

#include <Common.glsl>

OUTPUT(0, vec4, Output)

UBO_BEGIN(2, UberBrush_Block)
    SHADER_UNIFORM(vec4, ColorOpacity)
UBO_END(UberBrush_Block)

SAMPLER2D(3, Stencil)

void main() {
    GDIVSToPSInterpolants VSToPSInterpolants = GetInterpolants();
    vec2 TexCoord = VSToPSInterpolants.TexCoord;

    vec4 Tex = texture(Stencil, TexCoord);

    if(Tex.a == 0) discard;
    vec3 Mask = vec3( 1 - Tex.r,
                    1 - Tex.g,
                    1 - Tex.b );
    Output = vec4( ColorOpacity.rgb * Mask, ColorOpacity.a );
}
