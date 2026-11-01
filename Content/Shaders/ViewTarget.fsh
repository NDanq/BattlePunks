#version 450 core
// ViewTarget.fsh -- the Render Target view mode (RenderTypes.h's ERenderViewMode;
// the editor's Show > View Mode): one of the frame's render targets stretched
// over the view (RenderContext::ShowTarget); and a view's final colour into the
// window's composite (RenderContext::CompositeViews), upscaled when the view was
// drawn smaller ([Rendering] ResolutionScale).

#include <PostProcessCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_View;    // x: 0 a colour target as it is; 1 a depth target, grey (a shadow atlas); 2 the scene's depth, as distance;
                     //    3 a colour target upscaled, Catmull-Rom
    vec4 PP_Sizes;   // xy: the source's size, texels (mode 3)
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, Source)

// Catmull-Rom over the 4 x 4 texels round InUV, in nine bilinear taps (the middle two of
// each axis folded into one) -- sharper than bilinear, with no ringing to speak of.
vec3 CatmullRom(vec2 InUV, vec2 InSize)
{
    vec2 Position = InUV * InSize;
    vec2 Centre   = floor(Position - 0.5) + 0.5;
    vec2 F        = Position - Centre;
    vec2 W0 = F * (-0.5 + F * (1.0 - 0.5 * F));
    vec2 W1 = 1.0 + F * F * (-2.5 + 1.5 * F);
    vec2 W2 = F * (0.5 + F * (2.0 - 1.5 * F));
    vec2 W3 = F * F * (-0.5 + 0.5 * F);
    vec2 W12 = W1 + W2;
    vec2 T0  = (Centre - 1.0) / InSize;
    vec2 T3  = (Centre + 2.0) / InSize;
    vec2 T12 = (Centre + W2 / W12) / InSize;
    vec3 Sum = vec3(0.0);
    Sum += textureLod(Source, vec2(T0.x,  T0.y), 0.0).rgb  * W0.x  * W0.y;
    Sum += textureLod(Source, vec2(T12.x, T0.y), 0.0).rgb  * W12.x * W0.y;
    Sum += textureLod(Source, vec2(T3.x,  T0.y), 0.0).rgb  * W3.x  * W0.y;
    Sum += textureLod(Source, vec2(T0.x,  T12.y), 0.0).rgb * W0.x  * W12.y;
    Sum += textureLod(Source, vec2(T12.x, T12.y), 0.0).rgb * W12.x * W12.y;
    Sum += textureLod(Source, vec2(T3.x,  T12.y), 0.0).rgb * W3.x  * W12.y;
    Sum += textureLod(Source, vec2(T0.x,  T3.y), 0.0).rgb  * W0.x  * W3.y;
    Sum += textureLod(Source, vec2(T12.x, T3.y), 0.0).rgb  * W12.x * W3.y;
    Sum += textureLod(Source, vec2(T3.x,  T3.y), 0.0).rgb  * W3.x  * W3.y;
    return max(Sum, vec3(0.0));
}

void main(void)
{
    if(PP_View.x > 2.5)
    {
        Output = vec4(CatmullRom(Interpolants.TexCoord, max(PP_Sizes.xy, vec2(1.0))), 1.0);
        return;
    }
    vec4 Texel = texture(Source, Interpolants.TexCoord);
    if(PP_View.x > 1.5)
    {
        // Distance from the eye, near dark to far light; the far plane (cleared, 1) white.
        vec3  World    = PP_WorldAt(Interpolants.Position.xy, Texel.r);
        float Distance = length(World - Scene_CameraPosition.xyz);
        float Shade    = Texel.r >= 1.0 ? 1.0 : 1.0 - exp(-Distance / 1500.0);
        Output = vec4(vec3(Shade), 1.0);
        return;
    }
    if(PP_View.x > 0.5)
    {
        Output = vec4(vec3(Texel.r), 1.0);
        return;
    }
    Output = vec4(Texel.rgb, 1.0);
}
