#version 450 core
// GrassLabTrailPublish.fsh -- the trail (GrassLabTrailUpdate.fsh, half float)
// into the render target the grass material samples (GrassTrail.nmrt2d,
// RGBA8): rg the push's direction (biased into 0..1), b how hard, a how flat.
// All zeros is the grass at rest -- what the material reads before anything
// draws into the target (outside Play), so it never bends by accident.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Trail)

void main(void)
{
    vec3  Value    = texture(Trail, Interpolants.TexCoord).xyz;
    float Strength = min(length(Value.xy), 1.0);
    vec2  Way      = Strength > 0.001 ? Value.xy / length(Value.xy) : vec2(0.0);
    Output = vec4(Way * 0.5 + 0.5, Strength, clamp(Value.z, 0.0, 1.0));
}
