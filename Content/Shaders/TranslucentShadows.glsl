// TranslucentShadows.glsl -- reading the opacity shadow maps translucent casters
// draw (RenderCore/LIT_TRANSLUCENCY.md, Phase 7; OpacityShadowCaster.fsh): per light
// view, four slices of optical depth between the view's casters' nearest and
// farthest window depth, the directional atlas's tiles in the opacity atlas's
// left half, the local's in its right. Shared by Shadows.glsl (every shadow
// lookup), volumetric fog's march and a medium's ambient self-occlusion. The
// includer passes the atlas (engine slot 15); Scene_Spare[2].x is 1 while it is
// bound, Scene_Spare[0..1] hold the cascades' spans.

#ifndef TRANSLUCENT_SHADOWS_GLSL
#define TRANSLUCENT_SHADOWS_GLSL

// The optical depth in front of InWorld in the view InMatrix projects into: the
// slices before its depth summed, the one it is in partly, InSelf slices toward
// the light left out (a receiver in the medium: Phase 8). 0 outside the view.
float Translucent_Depth(sampler2D InOpacity, mat4 InMatrix, vec3 InWorld, bool bInLocal, vec2 InRange, float InSelf)
{
    if(Scene_Spare[2].x < 0.5 || InRange.y <= InRange.x) { return 0.0; }
    vec4 Clip = InMatrix * vec4(InWorld, 1.0);
    if(Clip.w <= 0.0) { return 0.0; }
    vec3 Atlas = Clip.xyz / Clip.w;
    if(any(lessThan(Atlas.xy, vec2(0.0))) || any(greaterThan(Atlas.xy, vec2(1.0)))) { return 0.0; }
    vec4  Slices = textureLod(InOpacity, vec2(Atlas.x * 0.5 + (bInLocal ? 0.5 : 0.0), Atlas.y), 0.0);
    float At     = clamp((Atlas.z - InRange.x) / (InRange.y - InRange.x), 0.0, 1.0) * 4.0 - InSelf;
    float Depth  = 0.0;
    for(int Index = 0; Index < 4; ++Index) { Depth += Slices[Index] * clamp(At - float(Index), 0.0, 1.0); }
    return Depth;
}

// The sun's cascade for InWorld (as DirectionalShadow picks it); -1 past the last.
int Translucent_Cascade(vec3 InWorld)
{
    int Count = int(Scene_LightShadow.y);
    if(Count <= 0) { return -1; }
    float ViewDepth = dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w;
    if(ViewDepth >= Scene_CascadeSplits[Count - 1]) { return -1; }
    int Cascade = 0;
    for(int Index = 0; Index < Count - 1; ++Index)
    {
        if(ViewDepth >= Scene_CascadeSplits[Index]) { Cascade = Index + 1; }
    }
    return Cascade;
}

vec2 Translucent_CascadeRange(int InCascade)
{
    vec4 Ranges = InCascade < 2 ? Scene_Spare[0] : Scene_Spare[1];
    return (InCascade == 0 || InCascade == 2) ? Ranges.xy : Ranges.zw;
}

// The optical depth between InWorld and the sun, through the casting particles and glass.
float Translucent_SunDepth(sampler2D InOpacity, vec3 InWorld, float InSelf)
{
    int Cascade = Translucent_Cascade(InWorld);
    if(Cascade < 0) { return 0.0; }
    return Translucent_Depth(InOpacity, Scene_CascadeMatrices[Cascade], InWorld, false, Translucent_CascadeRange(Cascade), InSelf);
}

#endif
