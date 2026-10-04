// IBLCommon.glsl -- image-based lighting (RenderCore/RENDERPIPELINE.md,
// "Image-based lighting"): the probe atlas, its octahedral maps, and how a
// surface is lit from it. Needs SceneUniforms.glsl.
//
// THE ATLAS (RGBA16F, 768 x 768, linear light) is five blocks of 384 x 256,
// two to a row: blocks 0-3 the reflection probes, block 4 the sky. A block is
// an octahedral map (+Z the centre, -Z the corners) at six roughnesses and a
// diffuse irradiance:
//
//     level 0  (0,   0)   256   roughness 0 (a copy)
//     level 1  (256, 0)   128   roughness 0.2
//     level 2  (256, 128)  64   0.4
//     level 3  (256, 192)  32   0.6
//     level 4  (256, 224)  16   0.8
//     level 5  (256, 240)   8   1.0
//     irradiance (320, 128) 32  cosine-weighted (radiance averaged: x albedo is diffuse)
//
// Texel (x, y) of a target is gl_FragCoord (x + 0.5, y + 0.5) when drawn and
// (x + 0.5, y + 0.5) / size when sampled, on every backend, so blocks are
// placed by those and never by a viewport offset.

#define IBL_PROBES_MAX  4
#define IBL_SKY_BLOCK   4
#define IBL_ATLAS_SIZE  768.0
#define IBL_LEVELS      6
#define IBL_PI          3.14159265359

vec2 IBL_BlockOrigin(int InBlock)
{
    return vec2(float(InBlock % 2) * 384.0, float(InBlock / 2) * 256.0);
}

// A level's square in its block: xy its corner, z its side (texels).
vec3 IBL_LevelRect(int InLevel)
{
    if(InLevel <= 0) { return vec3(0.0, 0.0, 256.0); }
    if(InLevel == 1) { return vec3(256.0, 0.0, 128.0); }
    if(InLevel == 2) { return vec3(256.0, 128.0, 64.0); }
    if(InLevel == 3) { return vec3(256.0, 192.0, 32.0); }
    if(InLevel == 4) { return vec3(256.0, 224.0, 16.0); }
    return vec3(256.0, 240.0, 8.0);
}
const vec3 IBL_IRRADIANCE_RECT = vec3(320.0, 128.0, 32.0);

// --- Octahedral maps -------------------------------------------------------
vec2 IBL_OctWrap(vec2 V)
{
    return (1.0 - abs(V.yx)) * vec2(V.x >= 0.0 ? 1.0 : -1.0, V.y >= 0.0 ? 1.0 : -1.0);
}

// A direction to 0-1 across the map.
vec2 IBL_OctEncode(vec3 N)
{
    N /= (abs(N.x) + abs(N.y) + abs(N.z));
    vec2 P = N.z >= 0.0 ? N.xy : IBL_OctWrap(N.xy);
    return P * 0.5 + 0.5;
}

vec3 IBL_OctDecode(vec2 InUv)
{
    vec2 F = InUv * 2.0 - 1.0;
    vec3 N = vec3(F.x, F.y, 1.0 - abs(F.x) - abs(F.y));
    float T = clamp(-N.z, 0.0, 1.0);
    N.x += N.x >= 0.0 ? -T : T;
    N.y += N.y >= 0.0 ? -T : T;
    return normalize(N);
}

// InRect (a level or the irradiance) of InBlock, along InDirection: bilinear,
// kept half a texel inside the square.
vec3 IBL_SampleRect(sampler2D InAtlas, int InBlock, vec3 InRect, vec3 InDirection)
{
    vec2 Texel = IBL_BlockOrigin(InBlock) + InRect.xy + 0.5 + IBL_OctEncode(InDirection) * (InRect.z - 1.0);
    return texture(InAtlas, Texel / IBL_ATLAS_SIZE).rgb;
}

// The reflection at InRoughness (perceptual, 0-1): between the two nearest levels.
vec3 IBL_SampleSpecular(sampler2D InAtlas, int InBlock, vec3 InDirection, float InRoughness)
{
    float Level = clamp(InRoughness, 0.0, 1.0) * float(IBL_LEVELS - 1);
    int   Lower = int(floor(Level));
    int   Upper = min(Lower + 1, IBL_LEVELS - 1);
    vec3  A = IBL_SampleRect(InAtlas, InBlock, IBL_LevelRect(Lower), InDirection);
    vec3  B = IBL_SampleRect(InAtlas, InBlock, IBL_LevelRect(Upper), InDirection);
    return mix(A, B, Level - float(Lower));
}

vec3 IBL_SampleIrradiance(sampler2D InAtlas, int InBlock, vec3 InNormal)
{
    return IBL_SampleRect(InAtlas, InBlock, IBL_IRRADIANCE_RECT, InNormal);
}

// --- A capture's cube strip (6S x S: +X -X +Y -Y +Z -Z) ----------------------
// The faces as SceneCapture.cpp renders them; MaterialCommon.glsl's
// Material_SampleCubeStrip reads them the same way.
void IBL_FaceBasis(int InFace, out vec3 OutForward, out vec3 OutRight, out vec3 OutUp)
{
    OutForward = InFace == 0 ? vec3(1.0, 0.0, 0.0) : InFace == 1 ? vec3(-1.0, 0.0, 0.0)
               : InFace == 2 ? vec3(0.0, 1.0, 0.0) : InFace == 3 ? vec3(0.0, -1.0, 0.0)
               : InFace == 4 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
    vec3 Up  = (InFace == 2 || InFace == 3) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    OutRight = normalize(cross(Up, OutForward));   // the engine's LookAt: R = Up x D, U = D x R
    OutUp    = cross(OutForward, OutRight);
}

vec3 IBL_SampleStrip(sampler2D InStrip, vec3 InDirection)
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
    float Size  = float(textureSize(InStrip, 0).y);
    float Inset = 0.5 / Size;
    vec2  Uv    = clamp(Ndc * 0.5 + 0.5, vec2(Inset), vec2(1.0 - Inset));
    return texture(InStrip, vec2((float(Face) + Uv.x) / 6.0, Uv.y)).rgb;
}

// --- An HDRI (RGBE8, equirectangular) ---------------------------------------
vec3 IBL_DecodeRgbe(vec4 InTexel)
{
    vec4 Bytes = floor(InTexel * 255.0 + 0.5);
    return Bytes.a > 0.0 ? (Bytes.rgb + 0.5) * exp2(Bytes.a - 136.0) : vec3(0.0);
}

// InDirection's radiance: +Y at the top row, turned InRotation radians about +Y.
// RGBE cannot be filtered, so four texels are decoded and blended.
vec3 IBL_SampleEquirect(sampler2D InTexture, vec3 InDirection, float InRotation)
{
    vec3  D = normalize(InDirection);
    float U = atan(D.x, D.z) / (2.0 * IBL_PI) + 0.5 + InRotation / (2.0 * IBL_PI);
    float V = acos(clamp(D.y, -1.0, 1.0)) / IBL_PI;
    ivec2 Size  = textureSize(InTexture, 0);
    vec2  Texel = vec2(U * float(Size.x), V * float(Size.y)) - 0.5;
    ivec2 Base  = ivec2(floor(Texel));
    vec2  F     = Texel - vec2(Base);
    vec3  Sum   = vec3(0.0);
    for(int Corner = 0; Corner < 4; ++Corner)
    {
        ivec2 Offset = ivec2(Corner & 1, Corner >> 1);
        ivec2 At     = Base + Offset;
        At.x = ((At.x % Size.x) + Size.x) % Size.x;   // wraps around the horizon
        At.y = clamp(At.y, 0, Size.y - 1);
        float Weight = (Offset.x == 1 ? F.x : 1.0 - F.x) * (Offset.y == 1 ? F.y : 1.0 - F.y);
        Sum += IBL_DecodeRgbe(texelFetch(InTexture, At, 0)) * Weight;
    }
    return Sum;
}

// --- Lighting a surface -----------------------------------------------------
// Karis's fit of the split-sum's environment BRDF (Unreal, mobile): F0 * A + B.
vec3 IBL_EnvBRDF(vec3 InF0, float InRoughness, float InNdotV)
{
    const vec4 C0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 C1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4  R    = InRoughness * C0 + C1;
    float A004 = min(R.x * R.x, exp2(-9.28 * InNdotV)) * R.x + R.y;
    vec2  AB   = vec2(-1.04, 1.04) * A004 + R.zw;
    return InF0 * AB.x + AB.y;
}

// Lagarde's specular occlusion from ambient occlusion.
float IBL_SpecularOcclusion(float InNdotV, float InOcclusion, float InRoughness)
{
    return clamp(pow(InNdotV + InOcclusion, exp2(-16.0 * InRoughness - 1.0)) - 1.0 + InOcclusion, 0.0, 1.0);
}

// How much probe InIndex lights InPosition: 1 inside 80% of its reach, none at it.
float IBL_ProbeWeight(int InIndex, vec3 InPosition)
{
    vec4 CentreRadius = Scene_Probes[InIndex * 3];
    vec4 Extent       = Scene_Probes[InIndex * 3 + 1];
    vec3 Offset       = InPosition - CentreRadius.xyz;
    if(Extent.x > 0.0 && Extent.y > 0.0 && Extent.z > 0.0)
    {
        vec3 Q = abs(Offset) / Extent.xyz;
        return 1.0 - smoothstep(0.8, 1.0, max(Q.x, max(Q.y, Q.z)));
    }
    return 1.0 - smoothstep(0.8 * CentreRadius.w, CentreRadius.w, length(Offset));
}

// InDirection from InPosition as probe InIndex saw it: onto its box, if it has one.
vec3 IBL_ProbeDirection(int InIndex, vec3 InPosition, vec3 InDirection)
{
    vec4 CentreRadius = Scene_Probes[InIndex * 3];
    vec4 Extent       = Scene_Probes[InIndex * 3 + 1];
    if(!(Extent.x > 0.0 && Extent.y > 0.0 && Extent.z > 0.0)) { return InDirection; }
    vec3 Lo  = CentreRadius.xyz - Extent.xyz;
    vec3 Hi  = CentreRadius.xyz + Extent.xyz;
    vec3 Dir = InDirection;
    Dir = vec3(abs(Dir.x) < 1e-6 ? 1e-6 : Dir.x, abs(Dir.y) < 1e-6 ? 1e-6 : Dir.y, abs(Dir.z) < 1e-6 ? 1e-6 : Dir.z);
    vec3 T1 = (Hi - InPosition) / Dir;
    vec3 T2 = (Lo - InPosition) / Dir;
    vec3 Far = max(T1, T2);
    float T = min(Far.x, min(Far.y, Far.z));
    return T > 0.0 ? normalize(InPosition + InDirection * T - CentreRadius.xyz) : InDirection;
}

// The image-based light a surface receives: diffuse (InAlbedo linear) and
// specular, the probes' blended nearest first and the sky filling the rest.
vec3 IBL_Light(sampler2D InAtlas, vec3 InPosition, vec3 InNormal, vec3 InView, vec3 InAlbedo, float InMetallic,
               float InRoughness, vec3 InF0, float InOcclusion)
{
    float NdotV = clamp(dot(InNormal, InView), 1e-4, 1.0);
    vec3  R     = reflect(-InView, InNormal);
    vec3  Diffuse  = vec3(0.0);
    vec3  Specular = vec3(0.0);
    float Remaining = 1.0;
    int   Count = int(Scene_IBL.y);
    for(int Index = 0; Index < IBL_PROBES_MAX; ++Index)
    {
        if(Index >= Count || Remaining <= 0.0) { break; }
        float Weight = min(IBL_ProbeWeight(Index, InPosition), Remaining);
        if(Weight <= 0.0) { continue; }
        int   Block     = int(Scene_Probes[Index * 3 + 2].x);
        float Intensity = Scene_Probes[Index * 3 + 1].w;
        Specular += IBL_SampleSpecular(InAtlas, Block, IBL_ProbeDirection(Index, InPosition, R), InRoughness) * Weight * Intensity;
        Diffuse  += IBL_SampleIrradiance(InAtlas, Block, InNormal) * Weight * Intensity;
        Remaining -= Weight;
    }
    if(Remaining > 0.0 && Scene_IBL.z > 0.0)
    {
        Specular += IBL_SampleSpecular(InAtlas, IBL_SKY_BLOCK, R, InRoughness) * Remaining * Scene_IBL.z;
        Diffuse  += IBL_SampleIrradiance(InAtlas, IBL_SKY_BLOCK, InNormal) * Remaining * Scene_IBL.z;
    }
    vec3  EnvBrdf = IBL_EnvBRDF(InF0, InRoughness, NdotV);
    float SpecOcc = IBL_SpecularOcclusion(NdotV, InOcclusion, InRoughness);
    return Diffuse * InAlbedo * (1.0 - InMetallic) * InOcclusion + Specular * EnvBrdf * SpecOcc;
}
