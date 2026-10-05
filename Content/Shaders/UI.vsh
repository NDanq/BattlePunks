#version 440 core
// UI.vsh -- RenderCore's UI pass (RenderContext::DrawUI, RENDERPIPELINE.md "UI"):
// a UIDrawList's triangles, in pixels from the top-left of the view, to clip
// space through ViewParameters.Projection -- DrawUI's orthographic one -- so
// each backend's own flip applies as it does to the scene. Its dynamic mesh
// has the indexed layout (GDIMeshDesc); the UI reads position, UV and colour.

#include <Common.glsl>

ATTRIB(0, in, vec3, InPosition)
ATTRIB(1, in, vec2, InTexCoord)
ATTRIB(9, in, vec4, InColor)

void main(void)
{
    vec4 Clip = ViewParameters.Projection * vec4(InPosition.xy, 0.0, 1.0);
    CalculateInterpolants(Clip.xyz, InTexCoord, vec3(0.0, 0.0, -1.0), InColor, vec3(0.0), 0.0);
    gl_Position = Clip;
}
