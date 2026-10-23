#version 450 core
// OpacityShadowMesh.vsh -- a Translucent material's mesh drawn into a light's opacity
// shadow map (RenderCore/LIT_TRANSLUCENCY.md: glass casting), its section in its own
// transform, seen from the light. Its opacity is the material's Translucent Shadow
// Opacity, the same everywhere (OpacityShadowCaster.fsh, Opacity_Params.w 1).

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>

ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)

void main(void)
{
    vec3 World        = (Transform.Model * vec4(InPosition, 1.0)).xyz;
    vec4 ViewPosition = ViewParameters.View * vec4(World, 1.0);
    vec4 Clip         = ViewParameters.Projection * ViewPosition;
    CalculateInterpolants(Clip.xyz, InTexCoord, vec3(0.0, 0.0, -1.0), vec4(1.0), World, ViewPosition.z);
    gl_Position = Clip;
}
