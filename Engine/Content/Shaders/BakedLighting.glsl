// BakedLighting.glsl -- a draw's baked lighting (RenderCore/RENDERPIPELINE.md,
// "Baked lighting"): what Build > Bake Lightmaps made of the static lights,
// as the engine's part of the material block carries it. Included by the
// pixel template after Material.glsl, whose block declares:
//
//   Material_LightmapScaleOffset   UV1 * xy + zw: the draw's square of its atlas
//   Material_BakedLighting         x: 0 none, 1 its lightmap, 2 the probes' ambient cube
//   Material_AmbientCube[6]        rgb per face: +X -X +Y -Y +Z -Z
//
// Every value is "diffuse radiance" (irradiance over pi): a surface adds it
// times its diffuse albedo, as the sky ambient is added (Lighting.glsl).

// RGBE8, as the bake stores it (TextureAsset.h's EncodeRgbe).
vec3 Baked_DecodeRgbe(vec4 InTexel)
{
    vec4 Bytes = floor(InTexel * 255.0 + 0.5);
    return Bytes.a > 0.0 ? (Bytes.rgb + 0.5) * exp2(Bytes.a - 136.0) : vec3(0.0);
}

// A lightmap at InUV, bilinear by hand (RGBE cannot be filtered), within rows
// [InRowFirst, InRowFirst + InRows): a directional atlas's light is its top
// half, its directions the bottom. Atlas UV, v down from the top row as every
// texture is stored. bInRgbe: decode the light; else the direction's bytes.
vec4 Baked_SampleRegion(sampler2D InLightmap, vec2 InUV, int InRowFirst, int InRows, bool bInRgbe)
{
    ivec2 Size  = textureSize(InLightmap, 0);
    ivec2 Lo    = ivec2(0, InRowFirst);
    ivec2 Hi    = ivec2(Size.x - 1, InRowFirst + InRows - 1);
    vec2  Texel = InUV * vec2(Size.x, InRows) - 0.5;
    ivec2 Base  = ivec2(floor(Texel)) + Lo;
    vec2  F     = Texel - floor(Texel);
    vec4 Corner[4];
    for(int Index = 0; Index < 4; ++Index)
    {
        vec4 Raw = texelFetch(InLightmap, clamp(Base + ivec2(Index & 1, Index >> 1), Lo, Hi), 0);
        Corner[Index] = bInRgbe ? vec4(Baked_DecodeRgbe(Raw), 1.0) : Raw;
    }
    return mix(mix(Corner[0], Corner[1], F.x), mix(Corner[2], Corner[3], F.x), F.y);
}

vec3 Baked_SampleLightmap(sampler2D InLightmap, vec2 InUV)
{
    ivec2 Size = textureSize(InLightmap, 0);
    return Baked_SampleRegion(InLightmap, InUV, 0, Size.y, true).rgb;
}

// A directional lightmap at InUV for normal InN: the light, rebalanced by how
// much InN faces its dominant direction -- half-Lambert against the stored
// direction, divided by the same at the texel's own normal (alpha), so a flat
// surface reads the light as baked and a normal map shades it.
vec3 Baked_SampleDirectional(sampler2D InLightmap, vec2 InUV, vec3 InN)
{
    ivec2 Size  = textureSize(InLightmap, 0);
    int   Half  = Size.y / 2;
    vec3  Light = Baked_SampleRegion(InLightmap, InUV, 0, Half, true).rgb;
    vec4  Way   = Baked_SampleRegion(InLightmap, InUV, Half, Half, false);
    float HalfLambert = dot(InN, Way.xyz - 0.5) + 0.5;
    return Light * HalfLambert / max(Way.w, 1e-4);
}

// The probes' ambient cube toward unit normal InN: each axis's face by the
// normal's sign, weighted by its square.
vec3 Baked_AmbientCube(vec3 InN)
{
    vec3 Squared = InN * InN;
    return Squared.x * Material_AmbientCube[InN.x < 0.0 ? 1 : 0].rgb
         + Squared.y * Material_AmbientCube[InN.y < 0.0 ? 3 : 2].rgb
         + Squared.z * Material_AmbientCube[InN.z < 0.0 ? 5 : 4].rgb;
}

// The draw's baked light toward InNormal: its lightmap at InUV1 (the mesh's,
// as interpolated), the probes, or none.
vec3 Baked_Light(sampler2D InLightmap, vec2 InUV1, vec3 InNormal)
{
    if(Material_BakedLighting.x > 1.5) { return Baked_AmbientCube(InNormal); }
    if(Material_BakedLighting.x > 0.5)
    {
        vec2 UV = InUV1 * Material_LightmapScaleOffset.xy + Material_LightmapScaleOffset.zw;
        return Scene_Baked.z > 0.5 ? Baked_SampleDirectional(InLightmap, UV, InNormal) : Baked_SampleLightmap(InLightmap, UV);
    }
    return vec3(0.0);
}
