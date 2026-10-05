// PostProcessCommon.glsl -- what the post process stack's passes share
// (RenderCore/RENDERPIPELINE.md, "Post process"). Each PP*.fsh is a full-screen
// pass drawn with the shared full-screen vertex shader and the material
// binding layout: its settings in a std140 block at binding 2 (PP_Block), the
// scene block at 3, its inputs as material textures 0-7 (bindings 4-11).
//
// Interpolants.TexCoord is this pixel's UV in whatever target is bound, the
// same UV every input samples with (a target's (0,0) is its top-left, NDC
// (-1, +1): TargetUvToNdc, Common.glsl); Interpolants.Position.xy is its NDC.

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>

OUTPUT(0, vec4, Output)

float PP_Luma(vec3 InColor) { return dot(InColor, vec3(0.299, 0.587, 0.114)); }

// The world position at NDC InClip with window depth InDepth (0 near, 1 far).
vec3 PP_WorldAt(vec2 InClip, float InDepth)
{
    vec4 World = Scene_InvViewProjection * vec4(InClip, InDepth * 2.0 - 1.0, 1.0);
    return World.xyz / World.w;
}
