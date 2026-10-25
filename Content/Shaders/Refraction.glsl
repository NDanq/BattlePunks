// Refraction.glsl -- what a Translucent surface sees through itself, bent
// (RenderCore/LIT_TRANSLUCENCY.md, Phase 11). The view ray refracts at the
// surface (1 / its index) and travels the gap to the opaque surface behind
// the pixel; where it arrives, projected, is where this frame's opaque scene
// is read (engine slots 13 and 14, copied before the translucent stage:
// Phase 10). So what sits right behind the surface barely moves and what is
// far behind moves more. One interface, not two: a thin pane wants an index
// near 1. Included by MaterialTemplate_PS.glsl for MATERIAL_REFRACTION.

#ifndef REFRACTION_GLSL
#define REFRACTION_GLSL

#include <ForwardReflections.glsl>   // the copy's samplers, Forward_SurfaceAt, Forward_ViewDepthOf

vec2 Refraction_UvOf(vec3 InWorld)
{
    vec4 Clip = Scene_ViewProjection * vec4(InWorld, 1.0);
    return NdcToTargetUv(Clip.xy / max(Clip.w, 1e-4));
}

// OutColor: linear, what is seen through the surface at InPosition. False when the
// copy is not bound this frame (no HDR, or nothing asked for it): draw as before.
bool Refraction_Behind(vec3 InPosition, vec3 InNormal, float InIndex, out vec3 OutColor)
{
    OutColor = vec3(0.0);
    if(Scene_Spare[2].y < 0.5) { return false; }

    vec3 V = normalize(Scene_CameraPosition.xyz - InPosition);
    vec3 N = dot(InNormal, V) < 0.0 ? -InNormal : InNormal;   // the side the eye is on
    vec2 Straight = Refraction_UvOf(InPosition);

    // The gap along the view ray to what is behind; the sky (or nothing): the reflections' longest ray.
    float Depth = Forward_ViewDepthOf(InPosition);
    vec3  Behind;
    float Gap = Forward_SurfaceAt(Straight, Behind) ? max(Forward_ViewDepthOf(Behind) - Depth, 0.0)
                                                    : max(Scene_ForwardReflections[1].x, 10.0);
    Gap /= max(dot(V, Scene_CameraForward.xyz), 0.1);

    vec3 T = refract(-V, N, 1.0 / max(InIndex, 1e-3));
    if(dot(T, T) < 1e-6) { T = -V; }   // past the critical angle (an index below 1): straight on
    vec2 Bent = Refraction_UvOf(InPosition + T * Gap);

    // Toward the screen's edge the bend fades out; where what lies at the bent point is
    // nearer than the surface (an object in front of it), the pixel reads straight behind.
    vec2  Edge  = min(Bent, 1.0 - Bent);
    float Trust = smoothstep(0.0, 0.05, min(Edge.x, Edge.y));
    vec3  There;
    if(Trust > 0.0 && Forward_SurfaceAt(Bent, There) && Forward_ViewDepthOf(There) < Depth) { Trust = 0.0; }

    OutColor = max(textureLod(Forward_SceneColor, mix(Straight, Bent, Trust), 0.0).rgb, vec3(0.0));
    return true;
}

#endif
