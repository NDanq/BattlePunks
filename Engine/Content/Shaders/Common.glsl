#if WITH_OPENGL
#include <OpenGLCommon.glsl>
#endif
#if WITH_VULKAN
#include <VulkanCommon.glsl>
#endif

struct GDIViewParameters
{
    mat4 Projection;
    mat4 View;
    vec3 MouseCoords;       // vec3(MouseX, MouseY, MouseDown)
    vec4 Resolution;        // vec4(ViewportWidth, ViewportHeight, 1.0/ViewportWidth, 1.0/ViewportHeight)
};

struct GDITransform
{
    vec3 PositionWorldSpace;
    mat4 Model;
    mat4 PrevModel;         // Model the frame before (Model if it is new): for velocity
};

// GDIVSToPSInterpolants is kept as a struct so GetInterpolants() can return it
// by value.  The actual VS->PS interface uses a GLSL interface block (below),
// which is valid on both OpenGL 4.x core and Vulkan without any #if guards.
//
// Normal is world space; shaders without one (quads, the blit) write -Z.
// Color, WorldPosition and ViewDepth come after it, so older pixel shaders
// read a valid prefix; only the material templates write them for real
// (RenderCore/MATERIALS.md, "Interpolants").
struct GDIVSToPSInterpolants
{
    vec3 Position;
    vec2 TexCoord;
    vec3 Normal;
};

#if VERTEX_SHADER
layout(location=0) out Interp_Block
{
    vec3  Position;
    vec2  TexCoord;
    vec3  Normal;
    vec4  Color;
    vec3  WorldPosition;
    float ViewDepth;
    vec2  LightmapUV;     // a mesh's UV1, its lightmap UVs (BakedLighting.glsl); 0 elsewhere
    vec3  WorldMotion;    // how far the surface moved in the world since the frame before (the Base Pass's velocity)
    vec4  WorldTangent;   // a mesh's tangent in world space, w the bitangent's sign (MeshAsset.h); 0 without one
} Interpolants;

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord, vec3 InNormal, vec4 InColor, vec3 InWorldPosition, float InViewDepth,
                            vec2 InLightmapUV )
{
    Interpolants.WorldMotion   = vec3(0.0);   // the material template's vertex stage sets its own after
    Interpolants.WorldTangent  = vec4(0.0);   // and its tangent
    Interpolants.Position      = InPosition;
    Interpolants.TexCoord      = InTexCoord;
    Interpolants.Normal        = InNormal;
    Interpolants.Color         = InColor;
    Interpolants.WorldPosition = InWorldPosition;
    Interpolants.ViewDepth     = InViewDepth;
    Interpolants.LightmapUV    = InLightmapUV;
}

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord, vec3 InNormal, vec4 InColor, vec3 InWorldPosition, float InViewDepth )
{
    CalculateInterpolants(InPosition, InTexCoord, InNormal, InColor, InWorldPosition, InViewDepth, vec2(0.0));
}

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord, vec3 InNormal )
{
    CalculateInterpolants(InPosition, InTexCoord, InNormal, vec4(1.0), vec3(0.0), 0.0);
}

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord )
{
    CalculateInterpolants(InPosition, InTexCoord, vec3(0.0, 0.0, -1.0));
}
#endif

#if PIXEL_SHADER
layout(location=0) in Interp_Block
{
    vec3  Position;
    vec2  TexCoord;
    vec3  Normal;
    vec4  Color;
    vec3  WorldPosition;
    float ViewDepth;
    vec2  LightmapUV;     // a mesh's UV1, its lightmap UVs (BakedLighting.glsl); 0 elsewhere
    vec3  WorldMotion;    // how far the surface moved in the world since the frame before (the Base Pass's velocity)
    vec4  WorldTangent;   // a mesh's tangent in world space, w the bitangent's sign (MeshAsset.h); 0 without one
} Interpolants;

GDIVSToPSInterpolants GetInterpolants()
{
    GDIVSToPSInterpolants Out;
    Out.Position = Interpolants.Position;
    Out.TexCoord = Interpolants.TexCoord;
    Out.Normal   = Interpolants.Normal;
    return Out;
}
#endif

// Core uniform blocks — binding 0 and 1 are reserved on all platforms.
//
// OpenGL keeps the simple one-liner: DECLARE_UBO there expands to a plain
// `uniform GDIViewParameters ViewParameters;` (OpenGLCommon.glsl), which is
// legal GLSL and never passed through SPIR-V-Cross at all.
//
// Vulkan/D3D12 declare the block FLAT (one field per line, no nested
// struct), then a local variable of the original struct type copied from
// those fields, so every call site below this file (Basic.vsh's
// `ViewParameters.Projection`, etc.) keeps working unchanged. See
// VulkanCommon.glsl's own comment on why this could not stay a generic
// DECLARE_UBO macro under cook_shaders.py's preprocessor.
#if WITH_OPENGL
DECLARE_UBO(0, VP_Block, GDIViewParameters, ViewParameters)
DECLARE_UBO(1, TR_Block, GDITransform,      Transform)
#endif

#if WITH_VULKAN
layout(std140, set=0, binding=0) uniform VP_Block { mat4 Projection; mat4 View; vec3 MouseCoords; vec4 Resolution; };
GDIViewParameters ViewParameters = GDIViewParameters(Projection, View, MouseCoords, Resolution);

layout(std140, set=0, binding=1) uniform TR_Block { vec3 PositionWorldSpace; mat4 Model; mat4 PrevModel; };
GDITransform Transform = GDITransform(PositionWorldSpace, Model, PrevModel);
#endif

// Emissive past 1 in an 8-bit G-buffer target (GBuffer3): rgb the colour
// divided by its brightest channel (when that is over 1), a that divisor as
// log2 over GBUFFER_EMISSIVE_LOG_RANGE octaves -- up to 4096. The Lighting
// stage reads it back whole; Bloom adds what the 8-bit scene colour clipped.
#define GBUFFER_EMISSIVE_LOG_RANGE 12.0
vec4 EncodeGBufferEmissive(vec3 InEmissive)
{
    vec3  Emissive = max(InEmissive, vec3(0.0));
    float Scale    = clamp(max(Emissive.r, max(Emissive.g, Emissive.b)), 1.0, exp2(GBUFFER_EMISSIVE_LOG_RANGE));
    return vec4(clamp(Emissive / Scale, 0.0, 1.0), log2(Scale) / GBUFFER_EMISSIVE_LOG_RANGE);
}
vec3 DecodeGBufferEmissive(vec4 InEncoded)
{
    return InEncoded.rgb * exp2(InEncoded.a * GBUFFER_EMISSIVE_LOG_RANGE);
}

// A unit vector as two 0-1 numbers: its octahedral map (Cigolle et al. 2014,
// "A Survey of Efficient Representations for Independent Unit Vectors").
// GBuffer1's normal (10 bits each), and the custom data's coat normal and
// tangent (8 bits each).
vec2 EncodeUnitVector(vec3 InVector)
{
    vec3 N  = InVector / max(abs(InVector.x) + abs(InVector.y) + abs(InVector.z), 1e-6);
    vec2 Xy = N.z >= 0.0 ? N.xy : (1.0 - abs(N.yx)) * vec2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
    return Xy * 0.5 + 0.5;
}
vec3 DecodeUnitVector(vec2 InEncoded)
{
    vec2  F = InEncoded * 2.0 - 1.0;
    vec3  N = vec3(F, 1.0 - abs(F.x) - abs(F.y));
    float T = clamp(-N.z, 0.0, 1.0);
    N.xy += vec2(N.x >= 0.0 ? -T : T, N.y >= 0.0 ? -T : T);
    return normalize(N);
}

// GBuffer1 (RENDERPIPELINE.md, "The G-buffer"): the normal, octahedral, and in
// alpha whether the pixel is lit -- 1, or 2/3 where its diffuse ambient is
// baked (exact in RGB10A2's two alpha bits). Readers test a < 0.5 (unlit) and
// a < 0.875 (baked).
vec4 EncodeGBufferNormal(vec3 InNormal, bool bInBaked)
{
    return vec4(EncodeUnitVector(InNormal), 0.0, bInBaked ? 2.0 / 3.0 : 1.0);
}
vec3 DecodeGBufferNormal(vec4 InEncoded)
{
    return DecodeUnitVector(InEncoded.xy);
}

// GBuffer2's alpha: the pixel's shading model (SHADING_MODELS.md;
// RenderCore's EMaterialShadingModel).
#define SHADING_MODEL_DEFAULT     0
#define SHADING_MODEL_CLEARCOAT   1
#define SHADING_MODEL_ANISOTROPIC 2
#define SHADING_MODEL_CLOTH       3
#define SHADING_MODEL_SUBSURFACE  4
#define SHADING_MODEL_SKIN        5
float EncodeShadingModel(int InModel) { return float(InModel) / 255.0; }
int   DecodeShadingModel(float InEncoded) { return int(InEncoded * 255.0 + 0.5); }

// Bloom's chain in 8-bit targets: brightness 0-BLOOM_CHAIN_RANGE stored as
// sqrt(v / range), so the faint tail of a glow keeps its precision.
#define BLOOM_CHAIN_RANGE 64.0
vec3 EncodeBloom(vec3 InColor) { return sqrt(clamp(InColor / BLOOM_CHAIN_RANGE, 0.0, 1.0)); }
vec3 DecodeBloom(vec3 InStored) { return InStored * InStored * BLOOM_CHAIN_RANGE; }

// Render targets are stored top-down on every backend (GDI.h): row 0 is the
// top of the image, NDC y = +1, texture v = 0 and gl_FragCoord.y = 0.5. So a
// target's UV (0, 0) is its top-left, NDC (-1, +1), and these convert between
// the two. A texture made from an image is the same way up: row 0 its top.
vec2 NdcToTargetUv(vec2 InNdc) { return vec2(InNdc.x * 0.5 + 0.5, 0.5 - InNdc.y * 0.5); }
vec2 TargetUvToNdc(vec2 InUv)  { return vec2(InUv.x * 2.0 - 1.0, 1.0 - InUv.y * 2.0); }
