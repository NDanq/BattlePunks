#version 440 core
// Tonemap.fsh -- the HDR scene colour (linear) to display space: exposure,
// the tone map operator, then 2.2 gamma (RenderCore/RENDERPIPELINE.md, "HDR
// and tone mapping"). Runs first in the post chain, so every effect after it
// (and every PostProcess material) sees display space as before.

#include <PostProcessCommon.glsl>
#include <ToneMapping.glsl>

MATERIAL_SAMPLER2D(4, Source)   // slot 0: the scene colour

void main(void)
{
    vec3 Linear = max(texture(Source, Interpolants.TexCoord).rgb, vec3(0.0)) * Scene_Output.y;
    vec3 Mapped = ToneMap(Linear, Scene_Output.z);
    Output = vec4(pow(Mapped, vec3(1.0 / 2.2)), 1.0);
}
