#version 450 core
// OpacityShadowCaster.vsh -- a particle drawn into a light's opacity shadow map
// (RenderCore/LIT_TRANSLUCENCY.md, Phase 7): each instance's quad turned square to
// the light (the shadow view's right and up), at the instance's centre, its width
// the instance transform's -- not the camera-facing quad the scene draws. Its
// alpha rides to OpacityShadowCaster.fsh in the colour.

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>

ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)
ATTRIB(2, in, mat4, InInstanceTransform)
ATTRIB(7, in, vec4, InInstanceColor)

void main(void)
{
    mat4 Instance = InInstanceTransform;
    Instance[0].w = 0.0;   // a flipbook's frame rides there (MaterialTemplate_VS.glsl)
    Instance[1].w = 0.0;
    Instance[2].w = 0.0;
    vec3  Centre = (Transform.Model * vec4(Instance[3].xyz, 1.0)).xyz;
    float Width  = length(Instance[0].xyz);
    mat4  View   = ViewParameters.View;
    vec3  Right  = vec3(View[0][0], View[1][0], View[2][0]);
    vec3  Up     = vec3(View[0][1], View[1][1], View[2][1]);
    vec3  World  = Centre + (Right * InPosition.x + Up * InPosition.y) * Width;
    vec4  ViewPosition = View * vec4(World, 1.0);
    vec4  Clip   = ViewParameters.Projection * ViewPosition;
    CalculateInterpolants(Clip.xyz, InTexCoord, -normalize(cross(Right, Up)), vec4(InInstanceColor.a, 0.0, 0.0, 1.0),
                          World, ViewPosition.z);
    gl_Position = Clip;
}
