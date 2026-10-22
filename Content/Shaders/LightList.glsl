// LightList.glsl -- the view's point and spot lights as the light block holds
// them (GDI binding 22; RenderContext's LightListBlock; RenderCore/
// GLOBAL_ILLUMINATION.md, Phase 2): clustered shading's lists [Olsson 2012]
// in Drobot's z-binned form [Drobot 2017].
//
//   LightList_Lights   six vec4s a light, sorted by view depth:
//                      [0] xyz position, w range; [1] rgb colour (display), a intensity;
//                      [2] xyz direction (spot), w 1 a spot; [3] x cos outer, y cos inner,
//                      z its Scene_Locals slot (its shadow there), or -1; [4], [5] reserved
//   LightList_Tiles    LIGHT_TILES_X x LIGHT_TILES_Y tiles of the target, row by row,
//                      each LIGHT_TILE_WORDS words of bits over the list
//   LightList_ZBins    LIGHT_ZBINS slices of view depth, logarithmic from
//                      LightList_Depth.x: each the first (low 16 bits) and last
//                      (high 16) index reaching it; first 0xFFFF where none does

#ifndef LIGHT_LIST_GLSL
#define LIGHT_LIST_GLSL

#define LIGHT_LIST_MAX   256
#define LIGHT_LIST_VEC4S 6
#define LIGHT_TILES_X    32
#define LIGHT_TILES_Y    18
#define LIGHT_TILE_WORDS 8
#define LIGHT_ZBINS      64

UBO_BLOCK_BEGIN(22, Light_Block)
    vec4  LightList_Header;   // x: lights; yz: a tile's size, pixels
    vec4  LightList_Depth;    // x: the first slice's near view depth; y: slices per natural log of depth over it
    vec4  LightList_Lights[1536];   // LIGHT_LIST_MAX * LIGHT_LIST_VEC4S
    uvec4 LightList_Tiles[1152];    // LIGHT_TILES_X * LIGHT_TILES_Y * LIGHT_TILE_WORDS / 4
    uvec4 LightList_ZBins[16];      // LIGHT_ZBINS / 4
UBO_BLOCK_END(Light_Block)

// Word InIndex of the tiles' masks, and slice InBin's range.
uint LightList_TileWord(int InIndex) { return LightList_Tiles[InIndex >> 2][InIndex & 3]; }
uint LightList_ZBin(int InBin)       { return LightList_ZBins[InBin >> 2][InBin & 3]; }

// The tile and slice of a pixel at InFragCoord, InViewDepth units in front of the eye.
int LightList_TileOf(vec2 InFragCoord)
{
    ivec2 Tile = clamp(ivec2(InFragCoord / max(LightList_Header.yz, vec2(1.0))), ivec2(0), ivec2(LIGHT_TILES_X - 1, LIGHT_TILES_Y - 1));
    return (Tile.y * LIGHT_TILES_X + Tile.x) * LIGHT_TILE_WORDS;
}
int LightList_SliceOf(float InViewDepth)
{
    float Near = LightList_Depth.x;
    return InViewDepth <= Near ? 0 : clamp(int(log(InViewDepth / Near) * LightList_Depth.y), 0, LIGHT_ZBINS - 1);
}

// Of word InWord's bits, those for the list's indices InFirst to InLast.
uint LightList_Within(uint InMask, int InWord, int InFirst, int InLast)
{
    int Low  = InWord * 32;
    int From = max(InFirst - Low, 0);
    int To   = min(InLast - Low, 31);
    if(From > To) { return 0u; }
    uint Span = To - From == 31 ? 0xFFFFFFFFu : ((1u << uint(To - From + 1)) - 1u) << uint(From);
    return InMask & Span;
}

#endif
