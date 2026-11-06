#version 450 core

#include <Common.glsl>

OUTPUT(0, vec4, Output)

UBO_BEGIN(2, Basic_Block)
    SHADER_UNIFORM(vec3, TestColor)
UBO_END(Basic_Block)

void main(void)
{
    Output = vec4(TestColor, 1.0);
}
