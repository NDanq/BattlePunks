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

// A lightmap at InUV, bilinear by hand: RGBE cannot be filtered. Atlas UV, v
// down from the top row as every texture is stored.
vec3 Baked_SampleLightmap(sampler2D InLightmap, vec2 InUV)
{
    ivec2 Size  = textureSize(InLightmap, 0);
    vec2  Texel = InUV * vec2(Size) - 0.5;
    ivec2 Base  = ivec2(floor(Texel));
    vec2  F     = Texel - vec2(Base);
    vec3  A = Baked_DecodeRgbe(texelFetch(InLightmap, clamp(Base,               ivec2(0), Size - 1), 0));
    vec3  B = Baked_DecodeRgbe(texelFetch(InLightmap, clamp(Base + ivec2(1, 0), ivec2(0), Size - 1), 0));
    vec3  C = Baked_DecodeRgbe(texelFetch(InLightmap, clamp(Base + ivec2(0, 1), ivec2(0), Size - 1), 0));
    vec3  D = Baked_DecodeRgbe(texelFetch(InLightmap, clamp(Base + ivec2(1, 1), ivec2(0), Size - 1), 0));
    return mix(mix(A, B, F.x), mix(C, D, F.x), F.y);
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
        return Baked_SampleLightmap(InLightmap, InUV1 * Material_LightmapScaleOffset.xy + Material_LightmapScaleOffset.zw);
    }
    return vec3(0.0);
}
