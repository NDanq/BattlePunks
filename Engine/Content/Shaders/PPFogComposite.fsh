#version 450 core
// PPFogComposite.fsh -- volumetric fog laid over the scene (RenderCore/
// RENDERPIPELINE.md, "Volumetric fog"): its half-resolution march, filtered up,
// blended (one, source alpha) so the scene becomes scene * transmittance + light.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Fog)   // rgb: scattered light; a: transmittance (PPVolumetricFog.fsh)

void main(void)
{
    Output = texture(Fog, Interpolants.TexCoord);
}
