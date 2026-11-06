// Shadows.glsl -- the shadow maps' lookups (RenderCore/RENDERPIPELINE.md's
// "Shadows"), included by Lighting.glsl where LIGHTING_SHADOWS is defined.
// The includer declares the two atlases first:
//
//   ShadowDirectional   the sun's: one view, or 1-4 cascades in 2x2 quarters
//   ShadowLocal         point and spot lights' views, 6 x 4 tiles
//
// Each holds window depth (0 near, 1 far) and is point-sampled; the
// comparison and its filter (Scene_ShadowParams.y texels each way, or
// Lighting_InsetTint.w for inset shadows) are here.
// Each matrix takes a world position to (u, v, depth) * w in its atlas, its
// tile included; a filter tap never leaves the tile it started in.
//
// Inset shadows (InsetShadow) read the includer's Lighting_Block too: per-object
// views of the sun in the local atlas's spare tiles -- only where it defines
// LIGHTING_INSET_SHADOWS (DeferredLighting.fsh; a forward draw has no such block).
// LIGHTING_SHADOW_RADIUS, if the includer defines it, is the filter's radius
// instead of Scene_ShadowParams.y (forward draws: Scene_ShadowParams.z).

#ifndef LIGHTING_SHADOW_RADIUS
#define LIGHTING_SHADOW_RADIUS int(Scene_ShadowParams.y)
#endif

// Translucent shadows (LIT_TRANSLUCENCY.md, Phase 7): where the includer defines
// LIGHTING_TRANSLUCENT_SHADOWS and declares TranslucentOpacity (engine slot 15),
// the light through the particles that cast is the opaque shadow's times
// exp(-the optical depth in front): four slices between the view's casters'
// nearest and farthest window depth (OpacityShadowCaster.fsh), the directional
// atlas's in the left half, the local's in the right, at half resolution.
// LIGHTING_TRANSLUCENT_SELF (Phase 8): how far toward the light, in slices, a
// receiver that is itself in the medium looks, so it is not shadowed by itself.
#ifndef LIGHTING_TRANSLUCENT_SELF
#define LIGHTING_TRANSLUCENT_SELF 0.0
#endif
#ifdef LIGHTING_TRANSLUCENT_SHADOWS
#include <TranslucentShadows.glsl>
#endif
float TranslucentLight(mat4 InMatrix, vec3 InWorld, bool bInLocal, vec2 InRange)
{
#ifdef LIGHTING_TRANSLUCENT_SHADOWS
    return exp(-Translucent_Depth(TranslucentOpacity, InMatrix, InWorld, bInLocal, InRange, float(LIGHTING_TRANSLUCENT_SELF)));
#else
    return 1.0;
#endif
}

// Receiver bias. Depth: InBias, more the more the light grazes (slope-scaled).
// Normal: along the surface normal, at least a texel and a half of the shadow
// map where InWorld is (InTexelWorld), so a grazing face does not shadow itself.
float SlopeBias(float InBias, float InNdotL)
{
    float Tangent = sqrt(max(1.0 - InNdotL * InNdotL, 0.0)) / max(InNdotL, 0.05);
    return InBias * (1.0 + min(Tangent, 10.0));
}

vec3 NormalOffset(vec3 InNormal, float InNdotL, float InNormalBias, float InTexelWorld)
{
    return InNormal * max(InNormalBias, 1.5 * InTexelWorld) * (1.0 + (1.0 - InNdotL));
}

// How lit InPoint -- (u, v, depth) in InAtlas -- is, filtered InRadius texels
// each way inside the tile InTileTexels wide it lands in: 1 lit, 0 in shadow.
float FilterShadowAtlas(sampler2D InAtlas, vec3 InPoint, float InBias, int InTileTexels, int InRadius)
{
    float Size    = Scene_ShadowParams.w;
    ivec2 Texel   = ivec2(floor(InPoint.xy * Size));
    ivec2 TileMin = (Texel / InTileTexels) * InTileTexels;
    ivec2 TileMax = TileMin + ivec2(InTileTexels - 1);
    float Depth   = InPoint.z - InBias;

    int   Radius = InRadius;
    float Lit    = 0.0;
    float Taps   = 0.0;
    for(int Y = -Radius; Y <= Radius; ++Y)
    {
        for(int X = -Radius; X <= Radius; ++X)
        {
            ivec2 Tap = clamp(Texel + ivec2(X, Y), TileMin, TileMax);
            Lit  += Depth <= texelFetch(InAtlas, Tap, 0).r ? 1.0 : 0.0;
            Taps += 1.0;
        }
    }
    return Lit / Taps;
}

// How lit InWorld is in the tile InTileTexels wide that InMatrix projects it
// into: 1 lit, 0 in shadow. Outside the view, lit.
float SampleShadowAtlas(sampler2D InAtlas, mat4 InMatrix, vec3 InWorld, float InBias, int InTileTexels)
{
    vec4 Clip = InMatrix * vec4(InWorld, 1.0);
    if(Clip.w <= 0.0) { return 1.0; }
    vec3 Atlas = Clip.xyz / Clip.w;
    if(Atlas.z >= 1.0 || Atlas.z <= 0.0) { return 1.0; }
    return FilterShadowAtlas(InAtlas, Atlas, InBias, InTileTexels, LIGHTING_SHADOW_RADIUS);
}

// The sun's shadow at InWorld: its cascade by view depth, faded out over the
// last tenth of its reach.
float DirectionalShadow(vec3 InWorld, vec3 InNormal, vec3 InToLight)
{
    int Count = int(Scene_LightShadow.y);
    if(Count <= 0) { return 1.0; }
    float ViewDepth = dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w;
    float Last      = Scene_CascadeSplits[Count - 1];
    if(ViewDepth >= Last) { return 1.0; }

    int Cascade = 0;
    for(int Index = 0; Index < Count - 1; ++Index)
    {
        if(ViewDepth >= Scene_CascadeSplits[Index]) { Cascade = Index + 1; }
    }

    float NdotL  = clamp(dot(InNormal, InToLight), 0.0, 1.0);
    vec3  Offset = NormalOffset(InNormal, NdotL, Scene_LightShadow.w, Scene_CascadeTexels[Cascade]);
    int   Tile   = Count == 1 ? int(Scene_ShadowParams.w) : int(Scene_ShadowParams.w) / 2;
    // The depth bias in texels of this cascade: Shadow Bias x 256 of them (0.0015,
    // under half a texel), more the more the light grazes. It was a fraction of the
    // cascade's depth range, which also runs the caster reach (thousands of units)
    // toward the sun, so it grew to tens of world units -- and the 5 x 5 filter's
    // far taps, a texel's slope deeper each on a roof's sloped top, let light
    // through thin roofs and walls where they meet. The normal offset (at least
    // a texel and a half) is what keeps a face from shadowing itself. Window
    // depth per world unit is the matrix's z row (the views are orthographic).
    mat4  Matrix  = Scene_CascadeMatrices[Cascade];
    float PerUnit = length(vec3(Matrix[0][2], Matrix[1][2], Matrix[2][2]));
    float Lit     = SampleShadowAtlas(ShadowDirectional, Matrix, InWorld + Offset,
                                      SlopeBias(Scene_LightShadow.z * 256.0, NdotL) * Scene_CascadeTexels[Cascade] * PerUnit, Tile);
    vec4  Ranges  = Cascade < 2 ? Scene_Spare[0] : Scene_Spare[1];   // the cascades' translucent casters' depth spans
    Lit *= TranslucentLight(Matrix, InWorld + Offset, false, (Cascade == 0 || Cascade == 2) ? Ranges.xy : Ranges.zw);   // Translucent_CascadeRange's
    return mix(Lit, 1.0, smoothstep(Last * 0.9, Last, ViewDepth));
}

// How far, in world units, the sun's light travels through whatever is between
// it and InWorld: InWorld's depth in the sun's shadow map less the nearest
// caster's there, turned to world units by the cascade's depth scale (its
// matrix's z row; the views are orthographic). Taken a little inside the
// surface (Jimenez et al. 2010), so a face toward the sun measures 0. Light
// passing through a Subsurface or Skin surface is attenuated by it; 0 outside
// the shadow's reach.
// World units over which transmitted light falls to 1/e.
#define SUBSURFACE_MEAN_FREE_PATH 1.0
float DirectionalShadowDepth(vec3 InWorld, vec3 InNormal)
{
    int Count = int(Scene_LightShadow.y);
    if(Count <= 0) { return 0.0; }
    float ViewDepth = dot(InWorld, Scene_CameraForward.xyz) - Scene_CameraForward.w;
    if(ViewDepth >= Scene_CascadeSplits[Count - 1]) { return 0.0; }

    int Cascade = 0;
    for(int Index = 0; Index < Count - 1; ++Index)
    {
        if(ViewDepth >= Scene_CascadeSplits[Index]) { Cascade = Index + 1; }
    }
    mat4 Matrix = Scene_CascadeMatrices[Cascade];
    vec4 Clip   = Matrix * vec4(InWorld - InNormal * (1.5 * Scene_CascadeTexels[Cascade]), 1.0);
    vec3 Atlas  = Clip.xyz / Clip.w;
    if(Atlas.z >= 1.0 || Atlas.z <= 0.0) { return 0.0; }

    float Size    = Scene_ShadowParams.w;
    int   Tile    = Count == 1 ? int(Size) : int(Size) / 2;
    ivec2 Texel   = ivec2(floor(Atlas.xy * Size));
    ivec2 TileMin = (Texel / Tile) * Tile;
    float Caster  = texelFetch(ShadowDirectional, clamp(Texel, TileMin, TileMin + ivec2(Tile - 1)), 0).r;
    float PerUnit = length(vec3(Matrix[0][2], Matrix[1][2], Matrix[2][2]));
    return max(Atlas.z - Caster, 0.0) / max(PerUnit, 1e-8);
}

// A point or spot light's shadow at InWorld. InFirst is its first matrix in
// Scene_LocalShadowMatrices; a point light's face is the major axis away from it.
// InTranslucentRange: the light's translucent casters' depth span (Scene_Locals[6i+5].zw; Phase 7).
float LocalShadow(int InFirst, bool bInSpot, float InCosOuter, vec3 InLightPosition, vec3 InWorld, vec3 InNormal,
                  vec3 InToLight, float InBias, float InNormalBias, vec2 InTranslucentRange)
{
    int Matrix = InFirst;
    if(!bInSpot)
    {
        vec3 Away = InWorld - InLightPosition;
        vec3 Size = abs(Away);
        if(Size.x >= Size.y && Size.x >= Size.z) { Matrix += Away.x > 0.0 ? 0 : 1; }
        else if(Size.y >= Size.z)                { Matrix += Away.y > 0.0 ? 2 : 3; }
        else                                     { Matrix += Away.z > 0.0 ? 4 : 5; }
    }

    // A texel at this distance: the view is 2 * tan(half its angle) wide there.
    int   Tile      = int(Scene_ShadowParams.w) / 6;
    float TanHalf   = bInSpot ? sqrt(max(1.0 - InCosOuter * InCosOuter, 0.0)) / max(InCosOuter, 0.05) : 1.0;
    float Texel     = 2.0 * length(InWorld - InLightPosition) * TanHalf / float(Tile);
    float NdotL     = clamp(dot(InNormal, InToLight), 0.0, 1.0);
    vec3  Offset    = NormalOffset(InNormal, NdotL, InNormalBias, Texel);
    return SampleShadowAtlas(ShadowLocal, Scene_LocalShadowMatrices[Matrix], InWorld + Offset, SlopeBias(InBias, NdotL), Tile)
         * TranslucentLight(Scene_LocalShadowMatrices[Matrix], InWorld + Offset, true, InTranslucentRange);
}

#ifdef LIGHTING_INSET_SHADOWS
// The inset shadows at InWorld (Lighting_Block): the darkest of them. Each is an
// orthographic view of the sun holding one caster's sphere and no more, so it
// sees only the prism behind that caster: outside its tile, lit. Its depth
// spans the sphere alone -- a receiver past the far plane is behind the caster,
// so is held just inside it; one before the near plane is between it and the sun.
// Fragment shaders only: each inset also names the screen pixels it can reach.
float InsetShadow(vec3 InWorld, vec3 InNormal, vec3 InToLight)
{
    int Count = int(Lighting_Params.y);
    if(Count <= 0) { return 1.0; }

    int   Tile  = int(Scene_ShadowParams.w) / 6;
    float NdotL = clamp(dot(InNormal, InToLight), 0.0, 1.0);
    float Bias  = SlopeBias(Lighting_Params.z, NdotL);
    float Lit   = 1.0;
    for(int Index = 0; Index < Count; ++Index)
    {
        // Only the pixels behind its caster, as the CPU projected them: the rest skip the matrix and filter.
        vec4 Rect = Lighting_InsetRects[Index];
        if(any(lessThan(gl_FragCoord.xy, Rect.xy)) || any(greaterThan(gl_FragCoord.xy, Rect.zw))) { continue; }

        vec4 Inset  = Lighting_Insets[Index];   // x matrix, y texel (world), zw tile corner (atlas UV)
        vec3 Offset = NormalOffset(InNormal, NdotL, Lighting_Params.w, Inset.y);
        vec4 Clip   = Scene_LocalShadowMatrices[int(Inset.x)] * vec4(InWorld + Offset, 1.0);
        vec3 Atlas  = Clip.xyz / Clip.w;        // orthographic: w is 1

        vec2 InTile = (Atlas.xy - Inset.zw) * Scene_ShadowParams.w;   // texels into its tile
        if(any(lessThan(InTile, vec2(0.0))) || any(greaterThanEqual(InTile, vec2(float(Tile)))) || Atlas.z <= 0.0) { continue; }
        Atlas.z = min(Atlas.z, 0.9999);
        Lit = min(Lit, FilterShadowAtlas(ShadowLocal, Atlas, Bias, Tile, int(Lighting_InsetTint.w)));
    }
    return Lit;
}
#endif
