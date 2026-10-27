// DDGI.glsl -- a GI Volume's irradiance probes (RenderCore/GLOBAL_ILLUMINATION.md,
// Phase 6; Majercik 2019): looked up by the image-based lighting pass, which
// takes them in place of the reflection probes' irradiance where a volume is.
// Needs IBLCommon.glsl (and through it LightList.glsl, whose LightList_DDGI
// describes the volume).
//
// THE ATLASES: a probe's irradiance is an 8 x 8 octahedral tile (rgb, linear:
// the cosine-weighted mean radiance round it, as IBL's irradiance), its
// distances a 16 x 16 tile (r: the mean distance to what it sees in that
// direction, g: the mean square, both over a cos^50 lobe and divided by the
// volume's longest distance). Tiles run 64 to a row, probe i at column i % 64.
// A probe is grid cell x + Count.x * (y + Count.y * z). Both sampled as IBL's
// blocks are: bilinear, half a texel inside the tile -- at LOD 0 explicitly:
// neighbouring pixels either side of a cell boundary read different probes'
// tiles, and implicit derivatives that large let an anisotropic sampler smear
// taps across other tiles (thin lines along the cells on D3D12).

#ifndef DDGI_GLSL
#define DDGI_GLSL

MATERIAL_SAMPLER2D(13, DDGI_IrradianceAtlas)   // engine texture slot 9
MATERIAL_SAMPLER2D(14, DDGI_DistanceAtlas)     // engine texture slot 10

vec2 DDGI_TileUv(int InProbe, float InSize, vec2 InAtlas, vec3 InDirection)
{
    int  PerRow = int(LightList_DDGI[2].w);
    vec2 Origin = vec2(float(InProbe % PerRow), float(InProbe / PerRow)) * InSize;
    return (Origin + 0.5 + IBL_OctEncode(InDirection) * (InSize - 1.0)) / InAtlas;
}

// The volume's irradiance at InPosition for a surface facing InNormal, seen along
// InView (toward the eye). OutWeight: how far it holds there -- 1 inside the grid,
// fading to 0 half a spacing outside; 0 with no volume.
vec3 DDGI_Irradiance(vec3 InPosition, vec3 InNormal, vec3 InView, out float OutWeight)
{
    OutWeight = 0.0;
    float Intensity = LightList_DDGI[0].w;
    if(Intensity <= 0.0) { return vec3(0.0); }
    vec3  First   = LightList_DDGI[0].xyz;
    vec3  Spacing = LightList_DDGI[1].xyz;
    vec3  Count   = LightList_DDGI[2].xyz;
    float Longest = LightList_DDGI[1].w;

    vec3  Grid = (InPosition - First) / Spacing;
    vec3  Out  = max(max(-Grid, Grid - (Count - 1.0)), vec3(0.0));
    float Fade = clamp(1.0 - 2.0 * max(Out.x, max(Out.y, Out.z)), 0.0, 1.0);
    if(Fade <= 0.0) { return vec3(0.0); }

    // Looked up a little off the surface, toward the eye, so a probe just
    // behind it is not taken for one in front (Majercik 2021).
    float Least  = min(Spacing.x, min(Spacing.y, Spacing.z));
    vec3  Biased = InPosition + (InNormal * 0.2 + InView * 0.8) * (0.3 * Least);
    vec3  At     = clamp((Biased - First) / Spacing, vec3(0.0), Count - 1.0);
    ivec3 Base   = ivec3(min(floor(At), max(Count - 2.0, vec3(0.0))));
    vec3  Alpha  = clamp(At - vec3(Base), 0.0, 1.0);

    vec3  Sum   = vec3(0.0);
    float Total = 0.0;
    for(int Corner = 0; Corner < 8; ++Corner)
    {
        ivec3 Offset = ivec3(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1);
        ivec3 Cell   = min(Base + Offset, ivec3(Count) - 1);
        int   Probe  = Cell.x + int(Count.x) * (Cell.y + int(Count.y) * Cell.z);
        vec3  Where  = First + vec3(Cell) * Spacing;

        vec3  Tri    = mix(1.0 - Alpha, Alpha, vec3(Offset));
        float Weight = Tri.x * Tri.y * Tri.z;

        // Probes behind the surface count for less (a wrapped cosine).
        vec3  ToProbe = normalize(Where - InPosition);
        float Wrap    = (dot(ToProbe, InNormal) + 1.0) * 0.5;
        Weight *= Wrap * Wrap + 0.2;

        // Visibility (Chebyshev, from the distance moments; Donnelly 2006): a probe
        // that sees a wall nearer than this point, along the way to it, is behind one.
        vec3  FromProbe = Biased - Where;
        float Distance  = length(FromProbe);
        vec2  Moments   = textureLod(DDGI_DistanceAtlas, DDGI_TileUv(Probe, 16.0, LightList_DDGI[3].zw, FromProbe / max(Distance, 1e-4)), 0.0).rg;
        float Mean      = Moments.x * Longest;
        float MeanSq    = Moments.y * Longest * Longest;
        if(Distance > Mean)
        {
            float Variance = abs(MeanSq - Mean * Mean);
            float Excess   = Distance - Mean;
            float Chance   = Variance / (Variance + Excess * Excess);
            Weight *= max(Chance * Chance * Chance, 0.0);
        }
        Weight = max(Weight, 1e-6 * Tri.x * Tri.y * Tri.z);

        Sum   += textureLod(DDGI_IrradianceAtlas, DDGI_TileUv(Probe, 8.0, LightList_DDGI[3].xy, InNormal), 0.0).rgb * Weight;
        Total += Weight;
    }
    OutWeight = Fade;
    return Total > 0.0 ? Sum / Total * Intensity : vec3(0.0);
}

#endif
