#version 440 core
// PPTemporalAA.fsh -- temporal antialiasing's resolve (RenderCore/RENDERPIPELINE.md,
// "Temporal antialiasing"): this frame's HDR scene colour, drawn with a sub-pixel
// jitter, blended into the history -- every frame before it, reprojected -- so
// each pixel averages many jittered samples over time.
//
//   Reprojection: the world position under the nearest depth in the 3 x 3
//     (so edges follow the thing in front), less its world motion (the
//     velocity target), projected with last frame's unjittered view: where
//     that point was. Off screen, or with no history, the current colour alone.
//   History: sampled Catmull-Rom (5 taps), so the blend does not soften it,
//     then clipped toward the 3 x 3's colour range (mean and spread, in a
//     tone-mapped space): a colour the neighbourhood no longer has -- what
//     moved away, what was uncovered -- is pulled back, so it does not ghost.
//   Blend: in that tone-mapped space (c / (1 + max(c))), so a lone bright
//     sample cannot dominate; less history the faster the pixel moves.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Current)      // this frame's scene colour, linear (jittered)
MATERIAL_SAMPLER2D(5, History)      // last frame's resolve, linear
MATERIAL_SAMPLER2D(6, SceneDepth)   // window depth
MATERIAL_SAMPLER2D(7, Velocity)     // xyz: world motion since last frame (the Base Pass's)

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 PP_PrevViewProjection;   // last frame's view, unjittered, GL clip space
    vec4 PP_Taa;                  // x: 1 if History holds last frame's; y: history weight (0-1); z: clip width (sigmas)
    vec4 PP_Sizes;                // xy: this frame's jitter, target UV; zw: full resolution
UBO_BLOCK_END(PP_Block)

float MaxOf(vec3 InC) { return max(InC.r, max(InC.g, InC.b)); }
vec3 ToWeighted(vec3 InC)   { return InC / (1.0 + MaxOf(InC)); }
vec3 FromWeighted(vec3 InC) { return InC / max(1.0 - MaxOf(InC), 1e-4); }

// Catmull-Rom from 5 bilinear taps (the corners' weights are dropped: tiny, and negative).
vec3 SampleHistory(vec2 InUV, vec2 InSize)
{
    vec2 Position = InUV * InSize;
    vec2 Centre   = floor(Position - 0.5) + 0.5;
    vec2 F  = Position - Centre;
    vec2 W0 = F * (-0.5 + F * (1.0 - 0.5 * F));
    vec2 W1 = 1.0 + F * F * (-2.5 + 1.5 * F);
    vec2 W2 = F * (0.5 + F * (2.0 - 1.5 * F));
    vec2 W3 = F * F * (-0.5 + 0.5 * F);
    vec2 W12 = W1 + W2;
    vec2 Offset12 = W2 / max(W12, vec2(1e-4));
    vec2 UV0  = (Centre - 1.0) / InSize;
    vec2 UV3  = (Centre + 2.0) / InSize;
    vec2 UV12 = (Centre + Offset12) / InSize;
    vec3 Sum = vec3(0.0);
    float Weight = 0.0;
    Sum += texture(History, vec2(UV12.x, UV0.y)).rgb  * (W12.x * W0.y);  Weight += W12.x * W0.y;
    Sum += texture(History, vec2(UV0.x, UV12.y)).rgb  * (W0.x * W12.y);  Weight += W0.x * W12.y;
    Sum += texture(History, UV12).rgb                 * (W12.x * W12.y); Weight += W12.x * W12.y;
    Sum += texture(History, vec2(UV3.x, UV12.y)).rgb  * (W3.x * W12.y);  Weight += W3.x * W12.y;
    Sum += texture(History, vec2(UV12.x, UV3.y)).rgb  * (W12.x * W3.y);  Weight += W12.x * W3.y;
    return max(Sum / max(Weight, 1e-4), vec3(0.0));
}

void main(void)
{
    vec2  Size  = PP_Sizes.zw;
    ivec2 Texel = ivec2(gl_FragCoord.xy);
    ivec2 Last  = ivec2(Size) - 1;
    vec3  Here  = max(texelFetch(Current, Texel, 0).rgb, vec3(0.0));

    // The 3 x 3: its colour range (tone-mapped), and its nearest depth.
    vec3  Sum = vec3(0.0), SumSquares = vec3(0.0);
    float Nearest = 2.0;
    ivec2 NearestTexel = Texel;
    for(int Y = -1; Y <= 1; ++Y)
    {
        for(int X = -1; X <= 1; ++X)
        {
            ivec2 At = clamp(Texel + ivec2(X, Y), ivec2(0), Last);
            vec3  C  = ToWeighted(max(texelFetch(Current, At, 0).rgb, vec3(0.0)));
            Sum += C;
            SumSquares += C * C;
            float D = texelFetch(SceneDepth, At, 0).r;
            if(D < Nearest) { Nearest = D; NearestTexel = At; }
        }
    }
    if(PP_Taa.x < 0.5)
    {
        Output = vec4(Here, 1.0);
        return;
    }

    // Where the nearest surface was last frame.
    vec2 NearestUV = (vec2(NearestTexel) + 0.5) / Size;
    vec3 World     = PP_WorldAt(TargetUvToNdc(NearestUV), min(Nearest, 1.0));
    vec3 Motion    = Nearest < 1.0 ? texelFetch(Velocity, NearestTexel, 0).xyz : vec3(0.0);
    vec4 PrevClip  = PP_PrevViewProjection * vec4(World - Motion, 1.0);
    if(PrevClip.w <= 1e-4)
    {
        Output = vec4(Here, 1.0);
        return;
    }
    // From where the unjittered view has the point (the pixel centre, less the jitter):
    // a still scene reprojects to itself, not half a pixel off a new way each frame.
    vec2 Shift     = NdcToTargetUv(PrevClip.xy / PrevClip.w) - (NearestUV - PP_Sizes.xy);
    vec2 HistoryUV = Interpolants.TexCoord + Shift;
    if(any(lessThan(HistoryUV, vec2(0.0))) || any(greaterThan(HistoryUV, vec2(1.0))))
    {
        Output = vec4(Here, 1.0);   // came on screen this frame: nothing to remember
        return;
    }

    // The history, clipped toward what the neighbourhood holds now.
    vec3 Mean   = Sum / 9.0;
    vec3 Spread = sqrt(max(SumSquares / 9.0 - Mean * Mean, vec3(0.0)));
    vec3 Low    = Mean - Spread * PP_Taa.z;
    vec3 High   = Mean + Spread * PP_Taa.z;
    vec3 Before = clamp(ToWeighted(SampleHistory(HistoryUV, Size)), Low, High);

    // Less history the faster it moves (pixels a frame): a moving edge keeps up.
    float Pixels = length(Shift * Size);
    float Keep   = clamp(PP_Taa.y, 0.0, 0.98) * mix(1.0, 0.8, clamp(Pixels / 12.0, 0.0, 1.0));
    vec3  Blend  = mix(ToWeighted(Here), Before, Keep);
    Output = vec4(FromWeighted(Blend), 1.0);
}
