#version 450 core
// UI.fsh -- RenderCore's UI pass (RenderContext::DrawUI): the command's texture
// (material slot 0) tinted by the vertex colour, alpha blended over the
// post-processed picture. Text is the same draw: a font atlas is white with
// its glyphs in alpha, and a list's solid fills sample the atlas's white texel.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, UITexture)

void main(void)
{
    Output = texture(UITexture, Interpolants.TexCoord) * Interpolants.Color;
}
