// Shadows.glsl -- the shadow maps' lookups (RenderCore/RENDERPIPELINE.md's
// "Shadows"), included by Lighting.glsl where LIGHTING_SHADOWS is defined.
// The includer declares the two atlases first:
//
//   ShadowDirectional   the sun's: one view, or 1-4 cascades in 2x2 quarters
//   ShadowLocal         point and spot lights' views, 6 x 4 tiles
//
// Each holds window depth (0 near, 1 far) and is point-sampled; the
// comparison and its filter (Scene_ShadowParams.y texels each way) are here.
// Each matrix takes a world position to (u, v, depth) * w in its atlas, its
// tile included; a filter tap never leaves the tile it started in.
//
// Inset shadows (InsetShadow) read the includer's Lighting_Block too: per-object
// views of the sun in the local atlas's spare tiles.

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

// How lit InPoint -- (u, v, depth) in InAtlas -- is, filtered inside the tile
// InTileTexels wide it lands in: 1 lit, 0 in shadow.
float FilterShadowAtlas(sampler2D InAtlas, vec3 InPoint, float InBias, int InTileTexels)
{
    float Size    = Scene_ShadowParams.w;
    ivec2 Texel   = ivec2(floor(InPoint.xy * Size));
    ivec2 TileMin = (Texel / InTileTexels) * InTileTexels;
    ivec2 TileMax = TileMin + ivec2(InTileTexels - 1);
    float Depth   = InPoint.z - InBias;

    int   Radius = int(Scene_ShadowParams.y);
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
    return FilterShadowAtlas(InAtlas, Atlas, InBias, InTileTexels);
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
    float Lit    = SampleShadowAtlas(ShadowDirectional, Scene_CascadeMatrices[Cascade], InWorld + Offset,
                                     SlopeBias(Scene_LightShadow.z, NdotL), Tile);
    return mix(Lit, 1.0, smoothstep(Last * 0.9, Last, ViewDepth));
}

// A point or spot light's shadow at InWorld. InFirst is its first matrix in
// Scene_LocalShadowMatrices; a point light's face is the major axis away from it.
float LocalShadow(int InFirst, bool bInSpot, float InCosOuter, vec3 InLightPosition, vec3 InWorld, vec3 InNormal,
                  vec3 InToLight, float InBias, float InNormalBias)
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
    return SampleShadowAtlas(ShadowLocal, Scene_LocalShadowMatrices[Matrix], InWorld + Offset, SlopeBias(InBias, NdotL), Tile);
}

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
        Lit = min(Lit, FilterShadowAtlas(ShadowLocal, Atlas, Bias, Tile));
    }
    return Lit;
}
