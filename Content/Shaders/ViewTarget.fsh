#version 440 core
// ViewTarget.fsh -- the Render Target view mode (RenderTypes.h's ERenderViewMode;
// the editor's Show > View Mode): one of the frame's render targets stretched
// over the view (RenderContext::ShowTarget).

#include <PostProcessCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_View;    // x: 0 a colour target as it is; 1 a depth target, grey (a shadow atlas); 2 the scene's depth, as distance
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, Source)

void main(void)
{
    vec4 Texel = texture(Source, Interpolants.TexCoord);
    if(PP_View.x > 1.5)
    {
        // Distance from the eye, near dark to far light; the far plane (cleared, 1) white.
        vec3  World    = PP_WorldAt(Interpolants.Position.xy, Texel.r);
        float Distance = length(World - Scene_CameraPosition.xyz);
        float Shade    = Texel.r >= 1.0 ? 1.0 : 1.0 - exp(-Distance / 1500.0);
        Output = vec4(vec3(Shade), 1.0);
        return;
    }
    if(PP_View.x > 0.5)
    {
        Output = vec4(vec3(Texel.r), 1.0);
        return;
    }
    Output = vec4(Texel.rgb, 1.0);
}
