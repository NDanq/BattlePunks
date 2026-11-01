#version 450 core
// PPFxaa.fsh -- antialiasing: FXAA (after Lottes' FXAA 3.11, quality path).
// Finds an edge by local luma contrast, walks along it both ways to its ends,
// and blends across it by how far this pixel is from the nearer end. Last in
// the stack, so every other effect's edges are smoothed too.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, SceneColor)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Fxaa;    // x edge threshold, y search steps, z subpixel amount
    vec4 PP_Sizes;   // xy one texel
UBO_BLOCK_END(PP_Block)

float LumaAt(vec2 InUV) { return PP_Luma(textureLod(SceneColor, Scene_ViewUv(InUV), 0.0).rgb); }

void main(void)
{
    vec2  UV = Interpolants.TexCoord;
    vec2  T  = PP_Sizes.xy;
    vec3  Centre = texture(SceneColor, Scene_ViewUv(UV)).rgb;
    float M  = PP_Luma(Centre);
    float N  = LumaAt(UV + vec2(0.0,  T.y)), S = LumaAt(UV + vec2(0.0, -T.y));
    float E  = LumaAt(UV + vec2( T.x, 0.0)), W = LumaAt(UV + vec2(-T.x, 0.0));
    float Max = max(M, max(max(N, S), max(E, W)));
    float Min = min(M, min(min(N, S), min(E, W)));
    float Range = Max - Min;
    if(Range < max(0.0312, Max * PP_Fxaa.x))
    {
        Output = vec4(Centre, 1.0);
        return;
    }

    float NW = LumaAt(UV + vec2(-T.x,  T.y)), NE = LumaAt(UV + vec2( T.x,  T.y));
    float SW = LumaAt(UV + vec2(-T.x, -T.y)), SE = LumaAt(UV + vec2( T.x, -T.y));

    // Horizontal or vertical edge.
    float Horizontal = abs(NW + NE - 2.0 * N) + 2.0 * abs(W + E - 2.0 * M) + abs(SW + SE - 2.0 * S);
    float Vertical   = abs(NW + SW - 2.0 * W) + 2.0 * abs(N + S - 2.0 * M) + abs(NE + SE - 2.0 * E);
    bool  bHorizontal = Horizontal >= Vertical;

    // Which side of this pixel the edge is on.
    float Luma1 = bHorizontal ? S : W;
    float Luma2 = bHorizontal ? N : E;
    float Gradient1 = abs(Luma1 - M), Gradient2 = abs(Luma2 - M);
    bool  bSide1 = Gradient1 >= Gradient2;
    float Gradient = 0.25 * max(Gradient1, Gradient2);
    float Step = bHorizontal ? T.y : T.x;
    float EdgeLuma;
    if(bSide1) { Step = -Step; EdgeLuma = 0.5 * (Luma1 + M); }
    else       { EdgeLuma = 0.5 * (Luma2 + M); }

    // Walk along the edge both ways until its luma changes.
    vec2 Along = bHorizontal ? vec2(T.x, 0.0) : vec2(0.0, T.y);
    vec2 Start = UV + (bHorizontal ? vec2(0.0, Step * 0.5) : vec2(Step * 0.5, 0.0));
    vec2 P1 = Start - Along, P2 = Start + Along;
    float End1 = LumaAt(P1) - EdgeLuma, End2 = LumaAt(P2) - EdgeLuma;
    bool  bDone1 = abs(End1) >= Gradient, bDone2 = abs(End2) >= Gradient;
    int   Steps = int(PP_Fxaa.y);
    for(int Index = 1; Index < Steps && !(bDone1 && bDone2); ++Index)
    {
        float Stride = Index < 4 ? 1.0 : 2.0;
        if(!bDone1) { P1 -= Along * Stride; End1 = LumaAt(P1) - EdgeLuma; bDone1 = abs(End1) >= Gradient; }
        if(!bDone2) { P2 += Along * Stride; End2 = LumaAt(P2) - EdgeLuma; bDone2 = abs(End2) >= Gradient; }
    }

    float Distance1 = bHorizontal ? UV.x - P1.x : UV.y - P1.y;
    float Distance2 = bHorizontal ? P2.x - UV.x : P2.y - UV.y;
    bool  bNearer1  = Distance1 < Distance2;
    float Nearest   = min(Distance1, Distance2);
    float Length    = Distance1 + Distance2;
    bool  bCentreLess = M < EdgeLuma;
    bool  bCorrect    = ((bNearer1 ? End1 : End2) < 0.0) != bCentreLess;
    float Offset      = bCorrect ? -Nearest / Length + 0.5 : 0.0;

    // Sub-pixel aliasing: a single bright or dark pixel.
    float Average  = (2.0 * (N + S + E + W) + NW + NE + SW + SE) / 12.0;
    float Subpixel = clamp(abs(Average - M) / Range, 0.0, 1.0);
    Subpixel = smoothstep(0.0, 1.0, Subpixel);
    Offset = max(Offset, Subpixel * Subpixel * PP_Fxaa.z);

    vec2 Final = UV + (bHorizontal ? vec2(0.0, Offset * Step) : vec2(Offset * Step, 0.0));
    Output = vec4(texture(SceneColor, Scene_ViewUv(Final)).rgb, 1.0);
}
