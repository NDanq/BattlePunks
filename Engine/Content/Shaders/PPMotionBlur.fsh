#version 450 core
// PPMotionBlur.fsh -- motion blur: each pixel's world position (from the
// scene depth), less how far its surface moved in the world since last frame
// (the Base Pass's velocity target: an entity's movement, a skinned mesh's
// pose; 0 where nothing wrote it), projected with last frame's view-projection
// gives how far it moved on screen -- the camera's motion and its own.
//
// The blur is a reconstruction filter (McGuire et al., "A Reconstruction
// Filter for Plausible Motion Blur", 2012), in one pass: each pixel finds the
// strongest motion around it, and gathers along that, weighting each tap by
// which is in front and whose streak reaches the other -- so a moving thing
// streaks out over a still background, and a still one in front stays sharp.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneColor)
MATERIAL_SAMPLER2D(5, SceneDepth)
MATERIAL_SAMPLER2D(6, SceneVelocity)   // xyz world motion since last frame; a 1 where written

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_PrevViewProjection;   // last frame's, GL clip space
    vec4 PP_Motion;               // x intensity, y max length (px), z samples, w 1: SceneVelocity is the frame's
    vec4 PP_Sizes;                // xy one texel, zw resolution
UBO_BLOCK_END(PP_Block)

// Soft depth tests, in world units: Y in front of X (or level), Y behind X (or level).
const float DEPTH_EXTENT = 5.0;
float InFront(float InX, float InY) { return clamp(1.0 - (InY - InX) / DEPTH_EXTENT, 0.0, 1.0); }
float Behind(float InX, float InY)  { return clamp(1.0 - (InX - InY) / DEPTH_EXTENT, 0.0, 1.0); }

// Whether a streak of half length InRadius reaches InDistance: falling off (cone), or wholly (cylinder).
float Cone(float InDistance, float InRadius)     { return clamp(1.0 - InDistance / InRadius, 0.0, 1.0); }
float Cylinder(float InDistance, float InRadius) { return 1.0 - smoothstep(0.95 * InRadius, 1.05 * InRadius, InDistance); }

// How far the surface at InPixel moved on screen since last frame, in pixels
// (scaled by Intensity, limited to Max Length); OutDepth its view depth.
vec2 MotionAt(ivec2 InPixel, out float OutDepth)
{
    ivec2 P     = clamp(InPixel, ivec2(0), ivec2(PP_Sizes.zw) - 1);
    float Depth = texelFetch(SceneDepth, P, 0).r;
    vec2  Ndc   = TargetUvToNdc((vec2(P) + 0.5) * PP_Sizes.xy);
    vec3  World = PP_WorldAt(Ndc, min(Depth, 0.9999));
    OutDepth    = dot(World, Scene_CameraForward.xyz) - Scene_CameraForward.w;
    if(PP_Motion.w > 0.5)
    {
        World -= texelFetch(SceneVelocity, P, 0).xyz;   // where this surface was
    }
    vec4 Prev    = PP_PrevViewProjection * vec4(World, 1.0);
    vec2 PrevNdc = Prev.w > 1e-5 ? Prev.xy / Prev.w : Ndc;

    // In pixels: x right, y down (NDC y runs up).
    vec2  Pixels = (Ndc - PrevNdc) * vec2(0.5, -0.5) * PP_Sizes.zw * PP_Motion.x;
    float Length = length(Pixels);
    return Length > PP_Motion.y ? Pixels * (PP_Motion.y / Length) : Pixels;
}

void main(void)
{
    ivec2 Pixel = ivec2(gl_FragCoord.xy);
    float DepthC;
    vec2  MotionC = MotionAt(Pixel, DepthC);

    // The strongest motion around: two rings of taps, out to half Max Length
    // -- the reach of a streak centred on its surface.
    vec2  MotionN = MotionC;
    float Reach   = max(PP_Motion.y * 0.5, 1.0);
    for(int Ring = 1; Ring <= 2; ++Ring)
    {
        for(int Y = -1; Y <= 1; ++Y)
        {
            for(int X = -1; X <= 1; ++X)
            {
                if(X == 0 && Y == 0) { continue; }
                float Ignored;
                vec2 Each = MotionAt(Pixel + ivec2(vec2(X, Y) * Reach * float(Ring) * 0.5), Ignored);
                if(dot(Each, Each) > dot(MotionN, MotionN)) { MotionN = Each; }
            }
        }
    }
    vec3 Centre = texelFetch(SceneColor, Pixel, 0).rgb;
    if(dot(MotionN, MotionN) < 0.25)
    {
        Output = vec4(Centre, 1.0);   // nothing near moved half a pixel
        return;
    }

    // Each streak spans its motion centred on its surface: half of it either side.
    float RadiusC = max(length(MotionC) * 0.5, 0.5);
    float Weight  = 1.0 / RadiusC;
    vec3  Sum     = Centre * Weight;

    // Each pixel's taps start at a different fraction of a step (interleaved
    // gradient noise), so the streak is grain, not bands.
    int   Samples = int(clamp(PP_Motion.z, 2.0, 32.0));
    float Jitter  = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    for(int Index = 0; Index < Samples; ++Index)
    {
        float T      = (float(Index) + Jitter) / float(Samples) - 0.5;
        vec2  Offset = MotionN * T;
        float Distance = length(Offset);
        if(Distance < 0.5) { continue; }   // the centre itself
        ivec2 Tap    = Pixel + ivec2(round(Offset));
        float DepthY;
        float RadiusY = max(length(MotionAt(Tap, DepthY)) * 0.5, 0.5);

        // A tap in front whose streak reaches here; one behind that this
        // pixel's own streak reaches; or both moving, overlapping.
        float A = InFront(DepthC, DepthY) * Cone(Distance, RadiusY)
                + Behind(DepthC, DepthY) * Cone(Distance, RadiusC)
                + Cylinder(Distance, RadiusY) * Cylinder(Distance, RadiusC) * 2.0;
        Sum    += A * texture(SceneColor, (vec2(Pixel) + 0.5 + Offset) * PP_Sizes.xy).rgb;   // filtered: no grain from rounding
        Weight += A;
    }
    Output = vec4(Sum / Weight, 1.0);
}
