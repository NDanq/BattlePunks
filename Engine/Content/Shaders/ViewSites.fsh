#version 450 core
// ViewSites.fsh -- SPLITSCREEN's dynamic split (RenderCore/RENDERPIPELINE.md,
// "Views"; RenderContext::CompositeViewSites): each pixel of the window from
// the view whose site is nearest it -- a Voronoi cell per view, two views a
// half-plane each -- and a divider along the cells' edges, faded in by its
// alpha. Every view is window-sized, so each samples at this pixel's UV.

#include <PostProcessCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_SitesA;     // the views' sites in window pixels (row 0 the top): views 0 (xy) and 1 (zw)
    vec4 PP_SitesB;     // views 2 (xy) and 3 (zw)
    vec4 PP_Params;     // x: how many views (2-4); y: the divider's width in pixels; z: its alpha
    vec4 PP_Color;      // the divider's colour
    vec4 PP_Sizes;      // xy: the window in pixels
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, View0)
MATERIAL_SAMPLER2D(5, View1)
MATERIAL_SAMPLER2D(6, View2)
MATERIAL_SAMPLER2D(7, View3)

vec2 SiteOf(int InIndex)
{
    return InIndex == 0 ? PP_SitesA.xy : InIndex == 1 ? PP_SitesA.zw : InIndex == 2 ? PP_SitesB.xy : PP_SitesB.zw;
}

void main(void)
{
    vec2 Uv    = Interpolants.TexCoord;
    vec2 Pixel = Uv * PP_Sizes.xy;
    int  Count = clamp(int(PP_Params.x + 0.5), 1, 4);

    // The nearest site's view.
    int   Nearest = 0;
    float Best    = 1.0e30;
    for(int Index = 0; Index < Count; ++Index)
    {
        vec2  Away = Pixel - SiteOf(Index);
        float Far  = dot(Away, Away);
        if(Far < Best) { Best = Far; Nearest = Index; }
    }

    // How far this pixel is from its cell's edge: the nearest bisector with another site.
    vec2  Own  = SiteOf(Nearest);
    float Edge = 1.0e30;
    for(int Index = 0; Index < Count; ++Index)
    {
        if(Index == Nearest) { continue; }
        vec2  Other = SiteOf(Index);
        vec2  ToOther = Pixel - Other;
        vec2  ToOwn   = Pixel - Own;
        float Apart   = max(length(Other - Own), 1.0e-3);
        Edge = min(Edge, (dot(ToOther, ToOther) - dot(ToOwn, ToOwn)) / (2.0 * Apart));
    }

    vec3 Color = Nearest == 0 ? textureLod(View0, Uv, 0.0).rgb
               : Nearest == 1 ? textureLod(View1, Uv, 0.0).rgb
               : Nearest == 2 ? textureLod(View2, Uv, 0.0).rgb
               :                textureLod(View3, Uv, 0.0).rgb;

    // The divider, half its width either side of the edge, a pixel of antialiasing.
    float Line = clamp(PP_Params.y * 0.5 + 0.5 - Edge, 0.0, 1.0) * PP_Params.z;
    Output = vec4(mix(Color, PP_Color.rgb, Line), 1.0);
}
