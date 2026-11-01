#version 450 core
// DDGIRelight.fsh -- a GI Volume's probes lit again from what their faces saw
// (DDGIStore.fsh), without drawing the scene (RenderCore/GLOBAL_ILLUMINATION.md,
// "Relighting"): each texel of the volume's cache is a surface (or the sky) at a
// known place, lit here by the sun (the capture shadow maps), every point and
// spot light (their shadows too), the sky and the reflection probes, and the
// volume itself -- its last irradiance, so each relight adds a bounce. Writes
// the radiance into the volume's radiance atlas, laid out as the cache (probe
// i's six faces side by side, PP_Settings.z probes a column), which
// DDGIUpdate.fsh then turns into the probes' irradiance. Lambert's diffuse and
// the default model's specular toward the probe -- what the capture's Lighting
// and IBL passes would give a surface, less its ambient occlusion and wetness.

#include <MaterialPlatform.glsl>
#include <Common.glsl>
#include <SceneUniforms.glsl>
#include <MaterialCommon.glsl>

MATERIAL_SAMPLER2D(4, Cache0)              // rgb: diffuse albedo, linear; a: distance (-1: the sky)
MATERIAL_SAMPLER2D(5, Cache1)              // rgb: emissive, linear; a: the normal and whether its ambient is live, packed
MATERIAL_SAMPLER2D(6, Atlas)               // the image-based lighting atlas (IBLCommon.glsl)
MATERIAL_SAMPLER2D(10, ShadowDirectional)  // the capture shadow maps (Shadows.glsl)
MATERIAL_SAMPLER2D(11, ShadowLocal)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // y: the volume's longest distance; z: probes a column of the cache
    vec4 PP_Sizes;
UBO_BLOCK_END(PP_Block)

#define LIGHTING_SHADOWS 1
#define LIGHTING_IBL_ATLAS Atlas
#include <IBLCommon.glsl>
#include <DDGI.glsl>
#include <Lighting.glsl>

OUTPUT(0, vec4, Output)

void main(void)
{
    ivec2 Texel  = ivec2(gl_FragCoord.xy);
    int   PerCol = max(int(PP_Settings.z), 1);
    int   Column = Texel.x / 96;
    int   Face   = (Texel.x % 96) / 16;
    int   Probe  = Column * PerCol + Texel.y / 16;
    vec2  Local  = (vec2(Texel.x % 16, Texel.y % 16) + 0.5) / 16.0;

    // The direction the texel looks along, as the face's camera saw it (DDGIUpdate.fsh's SampleStrip, inverted).
    vec3 Forward, Right, Up;
    IBL_FaceBasis(Face, Forward, Right, Up);
    vec2 Ndc       = TargetUvToNdc(Local);
    vec3 Direction = normalize(Forward + Right * Ndc.x + Up * Ndc.y);

    vec4 C0 = texelFetch(Cache0, Texel, 0);
    if(C0.a < 0.0)
    {
        // The sky: as the sky block holds it now, sharp.
        Output = vec4(Scene_IBL.z > 0.5 ? IBL_SampleSpecular(Atlas, IBL_SKY_BLOCK, Direction, 0.0) : vec3(0.0), 1.0);
        return;
    }
    vec4 C1 = texelFetch(Cache1, Texel, 0);
    if(dot(C0.rgb, vec3(1.0)) <= 0.0)
    {
        Output = vec4(C1.rgb, 1.0);   // unlit, or black: its emissive alone
        return;
    }

    // Where the probe is, then the surface.
    vec3  First   = LightList_DDGI[0].xyz;
    vec3  Spacing = LightList_DDGI[1].xyz;
    ivec3 Count   = ivec3(LightList_DDGI[2].xyz + 0.5);
    ivec3 Cell    = ivec3(Probe % Count.x, (Probe / Count.x) % Count.y, Probe / (Count.x * Count.y));
    vec3  Eye     = First + vec3(Cell) * Spacing;
    vec3  Position = Eye + Direction * C0.a;

    float Packed   = C1.a;
    float Live     = mod(Packed, 2.0);
    float Pair     = floor(Packed * 0.5);
    vec2  Encoded  = vec2(floor(Pair / 1024.0), mod(Pair, 1024.0)) / 1023.0;
    vec3  Normal   = DecodeUnitVector(Encoded);

    LightingSurface S = MakeLightingSurface(SHADING_MODEL_DEFAULT, C0.rgb, 0.0, 1.0, 0.5, Normal, -Direction);
    vec3 Diffuse, Specular, Transmitted;

    // The sun.
    vec3  SunL   = normalize(Scene_LightDirection.xyz);
    float Shadow = 1.0;
    vec3  Modulate = vec3(1.0);
    if(Scene_LightShadow.x >= 0.0)
    {
        float Lit = DirectionalShadow(Position, S.N, SunL);
        if(Scene_LightShadow.x == 1.0) { Modulate *= ModulatedShadow(Lit, 1.0, dot(S.N, SunL), Scene_LightShadowTint.rgb); }
        else                           { Shadow = Lit; }
    }
    EvaluateLightModel(S, SunL, ToLinear(Scene_LightColor.rgb) * Scene_LightDirection.w, Diffuse, Specular, Transmitted);
    vec3 OutDiffuse  = Diffuse * Shadow;
    vec3 OutSpecular = Specular * Shadow;

    // Every point and spot light: the list's tiles are a view's, and this is none.
    int Lights = int(LightList_Header.x);
    for(int Index = 0; Index < Lights; ++Index)
    {
        LightingLocal(S, Index, Position, false, OutDiffuse, OutSpecular, Modulate);
    }

    // Ambient: the probes and the sky, and the volume's last irradiance (a bounce).
    vec3 IndirectDiffuse, IndirectSpecular;
    if(Scene_IBL.x > 0.5)
    {
        IBL_LightModel(Atlas, Position, S, 1.0, Live, vec4(0.0), IndirectDiffuse, IndirectSpecular);
    }
    else
    {
        float Toward = clamp(S.N.y * 0.5 + 0.5, 0.0, 1.0);
        IndirectDiffuse  = mix(ToLinear(Scene_GroundColor.rgb), ToLinear(Scene_SkyColor.rgb), Toward) * Scene_SkyColor.a * Live;
        IndirectSpecular = vec3(0.0);
    }
    vec3 Radiance = C1.rgb + (S.Albedo * (OutDiffuse + IndirectDiffuse) + OutSpecular + IndirectSpecular) * Modulate;
    Output = vec4(max(Radiance, vec3(0.0)), 1.0);
}
