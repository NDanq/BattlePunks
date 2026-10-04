#version 440 core
// StatsOverlay.fsh -- the text overlay ("stat unit", RenderContext::DrawStatUnit):
// a panel of fixed-width text over the final image, drawn full screen with
// alpha blending. Each pixel finds its character cell, its character in
// Ov_Text and that glyph's pixel in Font (OverlayFont.h: 5 x 7 glyphs side by
// side, ASCII 32 first). Outside the panel it is discarded.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Font)   // a: 1 where a glyph's pixel is set

UBO_BLOCK_BEGIN(2, Overlay_Block)
    vec4  Ov_Layout;      // xy panel's top-left, pixels from the top-left; z font pixel size; w target width
    vec4  Ov_Grid;        // x columns, y rows, z target height, w padding in pixels
    vec4  Ov_TextColor;   // rgba
    vec4  Ov_PanelColor;  // rgba
    uvec4 Ov_Text[60];    // characters, 4 to a uint (low byte first), row by row
UBO_BLOCK_END(Overlay_Block)

void main(void)
{
    // From the UV, not gl_FragCoord: NDC +y is up on screen on every backend
    // (the engine's matrices see to it), while gl_FragCoord's origin is not.
    vec2 Pixel = vec2(Interpolants.TexCoord.x, 1.0 - Interpolants.TexCoord.y) * vec2(Ov_Layout.w, Ov_Grid.z);
    float Scale = Ov_Layout.z;
    vec2  Cell  = vec2(6.0, 9.0) * Scale;   // a 5 x 7 glyph and its spacing
    vec2  Grid  = vec2(Ov_Grid.x, Ov_Grid.y);
    vec2  Local = Pixel - Ov_Layout.xy;
    if(any(lessThan(Local, vec2(0.0))) || any(greaterThanEqual(Local, Grid * Cell + 2.0 * Ov_Grid.w)))
    {
        discard;
    }

    vec4 Color = Ov_PanelColor;
    vec2 Text  = Local - Ov_Grid.w;
    if(all(greaterThanEqual(Text, vec2(0.0))) && all(lessThan(Text, Grid * Cell)))
    {
        ivec2 C     = ivec2(Text / Cell);
        int   Index = C.y * int(Ov_Grid.x) + C.x;
        uint  Word  = Ov_Text[Index / 16][(Index / 4) % 4];
        int   Code  = int((Word >> uint((Index % 4) * 8)) & 0xFFu);
        ivec2 G     = ivec2((Text - vec2(C) * Cell) / Scale);
        if(Code >= 32 && Code < 127 && G.x < 5 && G.y < 7)
        {
            float On = texelFetch(Font, ivec2((Code - 32) * 5 + G.x, G.y), 0).a;
            Color = mix(Color, Ov_TextColor, On);
        }
    }
    Output = Color;
}
