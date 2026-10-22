#version 450 core
// LTCDecode.fsh -- the area lights' LTC tables (fit_ltc.py, LTCTables.inl;
// RenderCore/GLOBAL_ILLUMINATION.md, Phase 3) onto the GPU, once: drawn over a
// 64 x 128 half-float target from an RGBA8 texture that holds the fitted floats
// a texel each, bit for bit. Rows 0-63: each cell's inverse matrix (a, b, c, d);
// rows 64-127: its amplitudes (x, y). Cell (i, j) is texel (i, j), roughness
// along a row.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Packed)   // 128 texels a row: the matrices' floats, then the amplitudes'

float PackedFloat(int InIndex)
{
    return uintBitsToFloat(packUnorm4x8(texelFetch(Packed, ivec2(InIndex % 128, InIndex / 128), 0)));
}

void main(void)
{
    ivec2 Texel = ivec2(gl_FragCoord.xy);
    if(Texel.y < 64)
    {
        int First = (Texel.y * 64 + Texel.x) * 4;
        Output = vec4(PackedFloat(First), PackedFloat(First + 1), PackedFloat(First + 2), PackedFloat(First + 3));
        return;
    }
    int First = 64 * 64 * 4 + ((Texel.y - 64) * 64 + Texel.x) * 2;
    Output = vec4(PackedFloat(First), PackedFloat(First + 1), 0.0, 1.0);
}
