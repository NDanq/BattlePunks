#version 450 core
// IBLPrefilter.fsh -- one block of the probe atlas (IBLCommon.glsl) from a
// capture's cube strip: drawn over the whole atlas, it writes its block's
// levels and irradiance and discards the rest. Level 0 is a copy; each rougher
// level a GGX-weighted average (importance sampled, N = V = R); the
// irradiance a cosine-weighted one.

#include <PostProcessCommon.glsl>
#include <IBLCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Settings;   // x: the block; y: 1 if the strip holds display colours (an 8-bit capture) to linearise
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, Strip)   // slot 0

vec3 Radiance(vec3 InDirection)
{
    vec3 Value = IBL_SampleStrip(Strip, InDirection);
    return PP_Settings.y > 0.5 ? pow(max(Value, vec3(0.0)), vec3(2.2)) : Value;
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

void Frame(vec3 N, out vec3 T, out vec3 B)
{
    vec3 Up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T = normalize(cross(Up, N));
    B = cross(N, T);
}

vec3 Prefiltered(vec3 N, float InRoughness)
{
    float Alpha = InRoughness * InRoughness;
    vec3  T, B;
    Frame(N, T, B);
    int   Count = InRoughness < 0.3 ? 64 : 192;
    vec3  Sum = vec3(0.0);
    float Weight = 0.0;
    for(int Index = 0; Index < Count; ++Index)
    {
        vec2  Xi       = Hammersley(Index, Count);
        float Phi      = 2.0 * IBL_PI * Xi.x;
        float CosTheta = sqrt((1.0 - Xi.y) / (1.0 + (Alpha * Alpha - 1.0) * Xi.y));
        float SinTheta = sqrt(1.0 - CosTheta * CosTheta);
        vec3  H = normalize(T * (SinTheta * cos(Phi)) + B * (SinTheta * sin(Phi)) + N * CosTheta);
        vec3  L = 2.0 * dot(N, H) * H - N;
        float NdotL = dot(N, L);
        if(NdotL > 0.0)
        {
            Sum    += Radiance(L) * NdotL;
            Weight += NdotL;
        }
    }
    return Sum / max(Weight, 1e-4);
}

vec3 Irradiance(vec3 N)
{
    vec3 T, B;
    Frame(N, T, B);
    const int Count = 256;
    vec3 Sum = vec3(0.0);
    for(int Index = 0; Index < Count; ++Index)
    {
        vec2  Xi       = Hammersley(Index, Count);
        float Phi      = 2.0 * IBL_PI * Xi.x;
        float CosTheta = sqrt(1.0 - Xi.y);   // cosine-weighted
        float SinTheta = sqrt(Xi.y);
        Sum += Radiance(T * (SinTheta * cos(Phi)) + B * (SinTheta * sin(Phi)) + N * CosTheta);
    }
    return Sum / float(Count);
}

bool Inside(vec2 InTexel, vec3 InRect, out vec2 OutUv)
{
    vec2 Local = InTexel - InRect.xy;
    OutUv = (Local - 0.5) / max(InRect.z - 1.0, 1.0);
    return all(greaterThanEqual(Local, vec2(0.0))) && all(lessThan(Local, vec2(InRect.z)));
}

void main(void)
{
    int  Block = int(PP_Settings.x);
    vec2 Texel = gl_FragCoord.xy - IBL_BlockOrigin(Block);
    if(any(lessThan(Texel, vec2(0.0))) || Texel.x >= 384.0 || Texel.y >= 256.0) { discard; }

    vec2 Uv;
    if(Inside(Texel, IBL_IRRADIANCE_RECT, Uv))
    {
        Output = vec4(Irradiance(IBL_OctDecode(Uv)), 1.0);
        return;
    }
    for(int Level = 0; Level < IBL_LEVELS; ++Level)
    {
        if(Inside(Texel, IBL_LevelRect(Level), Uv))
        {
            vec3 N = IBL_OctDecode(Uv);
            Output = vec4(Level == 0 ? Radiance(N) : Prefiltered(N, float(Level) / float(IBL_LEVELS - 1)), 1.0);
            return;
        }
    }
    discard;
}
