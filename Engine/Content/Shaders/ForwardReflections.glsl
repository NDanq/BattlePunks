// ForwardReflections.glsl -- screen-space reflections for forward Lit surfaces
// (RenderCore/LIT_TRANSLUCENCY.md, Phase 4): glass and water reflect what is on
// screen, as PPScreenReflections.fsh does for the G-buffer's. One mirror ray,
// marched through a copy of the opaque depth (the depth itself is bound for the
// depth test) and read from the last frame's finished linear colour at the hit,
// reprojected by the last frame's view -- the hit's own motion is not known here.
// Included by MaterialTemplate_PS.glsl for forward Lit materials; Lighting.glsl
// hands IBL_LightModel what it returns. rgb the reflection, a how much to trust
// it; 0 where the probes must do (off, too rough, a miss, off screen).

#ifndef FORWARD_REFLECTIONS_GLSL
#define FORWARD_REFLECTIONS_GLSL

MATERIAL_SAMPLER2D(17, Forward_LastColor)    // engine texture slot 13: the last frame's linear colour
MATERIAL_SAMPLER2D(18, Forward_SceneDepth)   // engine texture slot 14: this frame's opaque depth, copied (r: window depth)

float Forward_ViewDepthOf(vec3 InWorld) { return dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w; }

// The opaque surface under InUv, as a world position; false for the sky.
bool Forward_SurfaceAt(vec2 InUv, out vec3 OutWorld)
{
    vec2  Size  = Scene_ForwardReflections[1].zw;
    float Depth = texelFetch(Forward_SceneDepth, clamp(ivec2(InUv * Size), ivec2(0), ivec2(Size) - 1), 0).r;
    vec4  World = Scene_InvViewProjection * vec4(TargetUvToNdc(InUv), Depth * 2.0 - 1.0, 1.0);
    OutWorld = World.xyz / World.w;
    return Depth < 1.0;
}

vec4 Forward_ScreenReflection(vec3 InPosition, vec3 InNormal, vec3 InView, float InRoughness)
{
    vec4  Settings     = Scene_ForwardReflections[0];   // x intensity, y most roughness, z steps, w thickness
    float MaxRoughness = max(Settings.y, 0.01);
    if(Scene_ForwardReflections[1].y < 0.5 || Settings.x <= 0.0 || InRoughness >= MaxRoughness) { return vec4(0.0); }

    vec3  R       = reflect(-InView, InNormal);
    float Longest = max(Scene_ForwardReflections[1].x, 1.0);
    int   Steps   = int(clamp(Settings.z, 8.0, 24.0));
    // Interleaved gradient noise, shifted each frame (golden ratio) so TAA averages the steps' bands away.
    float Noise   = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))
                          + fract(Scene_Time.x * 60.0 * 0.61803399));
    vec3  Start   = InPosition + InNormal * (0.5 + Forward_ViewDepthOf(InPosition) * 0.001);

    float Before = 0.0;
    float HitAt  = -1.0;
    for(int Index = 1; Index <= Steps; ++Index)
    {
        float F     = (float(Index) - 1.0 + Noise) / float(Steps);
        float Along = F * F * Longest;
        vec3  S     = Start + R * Along;
        vec4  Clip  = Scene_ViewProjection * vec4(S, 1.0);
        if(Clip.w <= 1e-4) { break; }
        vec2  Uv = NdcToTargetUv(Clip.xy / Clip.w);
        if(any(lessThan(Uv, vec2(0.0))) || any(greaterThan(Uv, vec2(1.0)))) { break; }
        vec3 Surface;
        if(Forward_SurfaceAt(Uv, Surface))
        {
            float Behind = Forward_ViewDepthOf(S) - Forward_ViewDepthOf(Surface);
            if(Behind > 0.0 && Behind < Settings.w + Along * 0.02) { HitAt = Along; break; }
        }
        Before = Along;
    }
    if(HitAt < 0.0) { return vec4(0.0); }

    float Low = Before, High = HitAt;
    for(int Index = 0; Index < 4; ++Index)
    {
        float Mid  = (Low + High) * 0.5;
        vec3  S    = Start + R * Mid;
        vec4  Clip = Scene_ViewProjection * vec4(S, 1.0);
        vec3  Surface;
        bool  bThere = Forward_SurfaceAt(NdcToTargetUv(Clip.xy / max(Clip.w, 1e-4)), Surface);
        if(bThere && Forward_ViewDepthOf(S) > Forward_ViewDepthOf(Surface)) { High = Mid; } else { Low = Mid; }
    }
    vec4 Clip  = Scene_ViewProjection * vec4(Start + R * High, 1.0);
    vec2 HitUv = NdcToTargetUv(Clip.xy / max(Clip.w, 1e-4));
    vec3 Hit;
    if(!Forward_SurfaceAt(HitUv, Hit)) { return vec4(0.0); }

    vec4 PrevClip = Scene_PrevViewProjection * vec4(Hit, 1.0);
    if(PrevClip.w <= 1e-4) { return vec4(0.0); }
    vec2 PrevUv = NdcToTargetUv(PrevClip.xy / PrevClip.w);
    if(any(lessThan(PrevUv, vec2(0.0))) || any(greaterThan(PrevUv, vec2(1.0)))) { return vec4(0.0); }
    vec3 Color = max(textureLod(Forward_LastColor, PrevUv, 0.0).rgb, vec3(0.0));

    vec2  Edge   = min(HitUv, 1.0 - HitUv);
    vec2  Edge2  = min(PrevUv, 1.0 - PrevUv);
    float Border = smoothstep(0.0, 0.08, min(min(Edge.x, Edge.y), min(Edge2.x, Edge2.y)));
    float Smooth = 1.0 - smoothstep(MaxRoughness * 0.6, MaxRoughness, InRoughness);
    float Reach  = 1.0 - smoothstep(0.7, 1.0, High / Longest);
    return vec4(Color, clamp(Border * Smooth * Reach * Settings.x, 0.0, 1.0));
}

#endif
