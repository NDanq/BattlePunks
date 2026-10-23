#version 450 core
// OpacityShadowCaster.fsh -- a particle's optical depth into a light's opacity
// shadow map (RenderCore/LIT_TRANSLUCENCY.md, Phase 7; Kim and Neumann 2001):
// four slices of depth between the view's translucent casters' nearest and
// farthest (Opacity_Params.xy, window depth), one a channel, added together
// (blend one, one). A soft disc across the sprite times its alpha times the
// emitter's Shadow Density is its opacity a; its optical depth -ln(1 - a)
// goes to the two slices either side of its depth, by how near each is, so a
// particle moving through the range shades smoothly. Shadows.glsl reads it back.

#include <MaterialPlatform.glsl>
#include <Common.glsl>

UBO_BLOCK_BEGIN(2, Opacity_Block)
    vec4 Opacity_Params;   // x, y: the casters' nearest and farthest window depth in this view; z: Shadow Density
                           // (a mesh: its material's Translucent Shadow Opacity); w: 1 a mesh, the same everywhere
UBO_BLOCK_END(Opacity_Block)

OUTPUT(0, vec4, Output)

void main(void)
{
    vec2  Across  = Interpolants.TexCoord * 2.0 - 1.0;
    float Disc    = clamp(1.0 - length(Across), 0.0, 1.0);
    float Opacity = clamp(Opacity_Params.w > 0.5 ? Opacity_Params.z : Disc * Disc * Interpolants.Color.r * Opacity_Params.z, 0.0, 0.999);
    float Depth   = -log(1.0 - Opacity);
    if(Depth <= 1e-5) { discard; }

    float Span  = max(Opacity_Params.y - Opacity_Params.x, 1e-6);
    float Slice = clamp((gl_FragCoord.z - Opacity_Params.x) / Span, 0.0, 1.0) * 4.0 - 0.5;
    float Low   = floor(Slice);
    float High  = Slice - Low;
    vec4  Out   = vec4(0.0);
    for(int Index = 0; Index < 4; ++Index)
    {
        float Weight = float(Index) == Low ? 1.0 - High : (float(Index) == Low + 1.0 ? High : 0.0);
        Out[Index] = Weight;
    }
    // Below the first slice's middle or past the last's, all of it to the end slice.
    if(Low < 0.0)  { Out = vec4(1.0, 0.0, 0.0, 0.0); }
    if(Low >= 3.0) { Out = vec4(0.0, 0.0, 0.0, 1.0); }
    Output = Out * Depth;
}
