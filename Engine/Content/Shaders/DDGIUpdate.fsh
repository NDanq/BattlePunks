#version 450 core
// DDGIUpdate.fsh -- one GI Volume probe's tile in the irradiance or the distance
// atlas (DDGI.glsl; RenderCore/GLOBAL_ILLUMINATION.md, Phase 6) from its cube
// strip (DDGICopy.fsh). Drawn over the whole atlas, it writes only the probe's
// tile (by gl_FragCoord, as IBLPrefilter.fsh places its blocks). Irradiance: the
// strip's colour, cosine-weighted about each texel's direction; distance: its
// alpha and alpha squared over a cos^50 lobe, divided by the longest distance.
// A rasterised update is the whole sphere at once, not a few noisy rays, so it
// replaces the tile outright rather than blending into it.
//
// Batched (PP_Settings.x < 0; GLOBAL_ILLUMINATION.md, "Relighting"): every
// probe from PP_Sizes.y, PP_Sizes.z of them, at once -- each tile's probe from
// its place, its strip the probe's row of the radiance atlas DDGIRelight.fsh
// wrote (six faces side by side, PP_Sizes.w probes a column).

#include <PostProcessCommon.glsl>
#include <IBLCommon.glsl>

MATERIAL_SAMPLER2D(4, Strip)   // 6S x S: rgb radiance, a distance

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // x: the probe, or -1 batched; y: the tile's side (8 irradiance, 16 distance); z: 1 for distance; w: tiles a row
    vec4 PP_Sizes;      // x: the volume's longest distance, world units; batched: y the first probe, z how many, w probes a column
UBO_BLOCK_END(PP_Block)

vec2 GStripOrigin = vec2(0.0);   // the probe's strip's corner, texels (batched: its row of the radiance atlas)

vec4 SampleStrip(vec3 InDirection)
{
    vec3 A = abs(InDirection);
    int  Face;
    if(A.x >= A.y && A.x >= A.z) { Face = InDirection.x > 0.0 ? 0 : 1; }
    else if(A.y >= A.z)          { Face = InDirection.y > 0.0 ? 2 : 3; }
    else                         { Face = InDirection.z > 0.0 ? 4 : 5; }
    vec3 Forward, Right, Up;
    IBL_FaceBasis(Face, Forward, Right, Up);
    float Depth = max(dot(InDirection, Forward), 1e-6);
    vec2  Ndc   = vec2(dot(InDirection, Right), dot(InDirection, Up)) / Depth;
    vec2  Atlas = vec2(textureSize(Strip, 0));
    float Size  = PP_Settings.x < 0.0 ? 16.0 : Atlas.y;   // a face's side
    float Inset = 0.5 / Size;
    vec2  Uv    = clamp(NdcToTargetUv(Ndc), vec2(Inset), vec2(1.0 - Inset));
    return textureLod(Strip, (GStripOrigin + vec2(float(Face) + Uv.x, Uv.y) * Size) / Atlas, 0.0);
}

vec2 Hammersley(int InIndex, int InCount)
{
    uint Bits = uint(InIndex);
    Bits = (Bits << 16u) | (Bits >> 16u);
    Bits = ((Bits & 0x55555555u) << 1u) | ((Bits & 0xAAAAAAAAu) >> 1u);
    Bits = ((Bits & 0x33333333u) << 2u) | ((Bits & 0xCCCCCCCCu) >> 2u);
    Bits = ((Bits & 0x0F0F0F0Fu) << 4u) | ((Bits & 0xF0F0F0F0u) >> 4u);
    Bits = ((Bits & 0x00FF00FFu) << 8u) | ((Bits & 0xFF00FF00u) >> 8u);
    return vec2(float(InIndex) / float(InCount), float(Bits) * 2.3283064365386963e-10);
}

void main(void)
{
    int   Probe  = int(PP_Settings.x);
    float Side   = PP_Settings.y;
    int   PerRow = int(PP_Settings.w);
    if(PP_Settings.x < 0.0)
    {
        ivec2 Tile = ivec2(gl_FragCoord.xy / Side);
        Probe = Tile.y * PerRow + Tile.x;
        if(Tile.x >= PerRow || Probe < int(PP_Sizes.y) || Probe >= int(PP_Sizes.y + PP_Sizes.z)) { discard; }
        int PerColumn = max(int(PP_Sizes.w), 1);
        GStripOrigin = vec2(float(Probe / PerColumn) * 96.0, float(Probe % PerColumn) * 16.0);
    }
    vec2  Local  = gl_FragCoord.xy - vec2(float(Probe % PerRow), float(Probe / PerRow)) * Side;
    if(any(lessThan(Local, vec2(0.0))) || Local.x >= Side || Local.y >= Side) { discard; }

    // The texel's direction: DDGI_TileUv's mapping, inverted.
    vec3 N = IBL_OctDecode(clamp((Local - 0.5) / (Side - 1.0), 0.0, 1.0));
    vec3 Up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(Up, N));
    vec3 B  = cross(N, T);

    bool  bDistance = PP_Settings.z > 0.5;
    float Longest   = max(PP_Sizes.x, 1.0);
    const int Count = 256;
    vec3 Sum = vec3(0.0);
    for(int Index = 0; Index < Count; ++Index)
    {
        vec2  Xi       = Hammersley(Index, Count);
        float Phi      = 2.0 * IBL_PI * Xi.x;
        // Cosine-weighted for irradiance; cos^50 (pdf-weighted, so a plain mean) for distance.
        float CosTheta = bDistance ? pow(1.0 - Xi.y, 1.0 / 51.0) : sqrt(1.0 - Xi.y);
        float SinTheta = sqrt(max(1.0 - CosTheta * CosTheta, 0.0));
        vec4  Seen     = SampleStrip(T * (SinTheta * cos(Phi)) + B * (SinTheta * sin(Phi)) + N * CosTheta);
        if(bDistance)
        {
            float D = clamp(Seen.a / Longest, 0.0, 1.0);
            Sum += vec3(D, D * D, 0.0);
        }
        else
        {
            Sum += Seen.rgb;
        }
    }
    Output = vec4(Sum / float(Count), 1.0);
}
