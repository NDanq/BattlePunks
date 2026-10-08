#version 450 core
// ShadowDepth.fsh -- the Shadows stage's pixel shader (RenderCore/RENDERPIPELINE.md):
// a caster draws only its depth into a shadow atlas tile, with its own
// material's vertex shader (so World Position Offset matches) and this. The
// colour target is masked off; the output exists for backends that want one.

#include <MaterialPlatform.glsl>
#include <Common.glsl>

OUTPUT(0, vec4, Output)

void main(void)
{
    Output = vec4(0.0);
}
