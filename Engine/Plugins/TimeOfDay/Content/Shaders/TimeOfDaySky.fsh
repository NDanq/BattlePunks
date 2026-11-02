#version 450 core
// TimeOfDaySky.fsh -- the TimeOfDay plugin's sky (SkyPass, TimeOfDaySky.cpp):
// drawn over the background after the Lighting stage (ERenderHook::AfterLighting),
// only where the scene's depth is the far plane -- every other pixel is
// discarded before the march, so a view mostly of ground and walls pays for
// the sky it shows, not the whole screen. Per pixel, the view ray's single scattering through a
// planet's atmosphere -- Rayleigh (the blue, the reds at the horizon), Mie
// (the haze, the glow round the sun) and ozone's absorption (what keeps a
// twilight sky blue) -- lit by the sun and the moon, plus both discs and,
// as the sky darkens, stars over a faint airglow. Linear HDR out, times an
// exposure the CPU adapts to the zenith's brightness (SkyPass::Extract);
// the frame's tone map does the rest. The same pass draws the sky that
// image-based lighting captures (ERenderHook::Sky) and reflection probes see. TimeOfDaySky.cpp's EvaluateSky is the same model
// on the CPU, for the lights' colours.

#include <PostProcessCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    mat4 TOD_Sky;   // [0] xyz toward the sun, w its intensity
                    // [1] xyz toward the moon, w its intensity
                    // [2] x haze (Mie x), y cos of the sun disc's radius, z stars, w exposure (adapted)
                    // [3] xyz ground albedo, w the eye's height above sea level (m)
    vec4 TOD_Bounce;   // rgb: the ground's own radiance, lit by the sun, the moon and the sky (before the exposure)
    vec4 TOD_Flags;    // x: 1 to skip, before the march, every pixel where the scene depth is nearer than the far plane;
                       // y: 1 to keep only the far plane's pixels after it (the background pass with x off: the A/B switch)
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(11, TOD_SceneDepth)   // a custom pass's slot 7: the scene's depth, window depth (1 far)

const float PI            = 3.14159265;
const float PlanetRadius  = 6360e3;
const float AtmosRadius   = 6420e3;
const float RayleighScale = 8000.0;
const float MieScale      = 1200.0;
const vec3  RayleighBeta  = vec3(5.8e-6, 13.5e-6, 33.1e-6);
const float MieBetaBase   = 21e-6;
const float MieG          = 0.76;
const vec3  OzoneBeta     = vec3(0.650e-6, 1.881e-6, 0.085e-6);
const int   ViewSteps     = 16;
const int   LightSteps    = 6;

// Distance along InDir from InOrigin to the sphere of InRadius about the
// planet's centre (the far hit), or -1 if it is missed.
float SphereFar(vec3 InOrigin, vec3 InDir, float InRadius)
{
    float B = dot(InOrigin, InDir);
    float C = dot(InOrigin, InOrigin) - InRadius * InRadius;
    float D = B * B - C;
    if(D < 0.0) { return -1.0; }
    return -B + sqrt(D);
}

// Ozone: a layer 30 km thick round 25 km up.
float Ozone(float InHeight) { return max(0.0, 1.0 - abs(InHeight - 25000.0) / 15000.0); }

// Rayleigh, Mie and ozone optical depth from InPoint toward InLight, out of the atmosphere.
vec3 LightDepth(vec3 InPoint, vec3 InLight)
{
    float Length = SphereFar(InPoint, InLight, AtmosRadius);
    float Step   = Length / float(LightSteps);
    vec3  Depth  = vec3(0.0);
    for(int Index = 0; Index < LightSteps; ++Index)
    {
        vec3  P      = InPoint + InLight * (float(Index) + 0.5) * Step;
        float Height = length(P) - PlanetRadius;
        if(Height < 0.0) { return vec3(1e9); }   // the planet is in the way: night side
        Depth += vec3(exp(-Height / vec2(RayleighScale, MieScale)), Ozone(Height)) * Step;
    }
    return Depth;
}

vec3 Extinction(vec3 InDepth, vec3 InMieBeta)
{
    return RayleighBeta * InDepth.x + InMieBeta * 1.1 * InDepth.y + OzoneBeta * InDepth.z;
}

float PhaseRayleigh(float InCos) { return 3.0 / (16.0 * PI) * (1.0 + InCos * InCos); }
float PhaseMie(float InCos)
{
    float G2 = MieG * MieG;
    return 3.0 / (8.0 * PI) * ((1.0 - G2) * (1.0 + InCos * InCos))
         / ((2.0 + G2) * pow(1.0 + G2 - 2.0 * MieG * InCos, 1.5));
}

// Single scattering along the view ray, from the sun and the moon; also its
// transmittance, which dims the discs and stars behind it.
vec3 Scatter(vec3 InOrigin, vec3 InDir, vec3 InSun, float InSunI, vec3 InMoon, float InMoonI, float InHaze, out vec3 OutTransmit)
{
    vec3  MieBeta = vec3(MieBetaBase * InHaze);
    float Length  = SphereFar(InOrigin, InDir, AtmosRadius);
    float Step    = Length / float(ViewSteps);

    vec3 ViewDepth = vec3(0.0);
    vec3 SunR = vec3(0.0), SunM = vec3(0.0), MoonR = vec3(0.0), MoonM = vec3(0.0);
    for(int Index = 0; Index < ViewSteps; ++Index)
    {
        vec3  P       = InOrigin + InDir * (float(Index) + 0.5) * Step;
        float Height  = max(length(P) - PlanetRadius, 0.0);
        vec3  Density = vec3(exp(-Height / vec2(RayleighScale, MieScale)), Ozone(Height)) * Step;
        ViewDepth += Density;

        vec3 Atten = exp(-Extinction(ViewDepth + LightDepth(P, InSun), MieBeta));
        SunR += Atten * Density.x;
        SunM += Atten * Density.y;

        if(InMoonI > 0.0)
        {
            vec3 MoonAtten = exp(-Extinction(ViewDepth + LightDepth(P, InMoon), MieBeta));
            MoonR += MoonAtten * Density.x;
            MoonM += MoonAtten * Density.y;
        }
    }
    OutTransmit = exp(-Extinction(ViewDepth, MieBeta));

    float CosSun  = dot(InDir, InSun);
    float CosMoon = dot(InDir, InMoon);
    vec3 Sun  = InSunI  * (SunR  * RayleighBeta * PhaseRayleigh(CosSun)  + SunM  * MieBeta * PhaseMie(CosSun));
    vec3 Moon = InMoonI * (MoonR * RayleighBeta * PhaseRayleigh(CosMoon) + MoonM * MieBeta * PhaseMie(CosMoon));
    vec3 Sky  = Sun + Moon;
    // Single scattering tints a long, low path green; the light scattered more
    // than once, which this leaves out, whitens it instead. Sunsets (red over
    // green over blue) are untouched. EvaluateSky does the same.
    Sky.g = min(Sky.g, max(Sky.r, Sky.b));
    return Sky;
}

float Hash(vec3 InCell)
{
    return fract(sin(dot(InCell, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

// Stars: one maybe per cell of a grid on the unit sphere, twinkling.
vec3 Stars(vec3 InDir, float InTime)
{
    vec3  P     = InDir * 220.0;
    vec3  Cell  = floor(P);
    float Seed  = Hash(Cell);
    if(Seed < 0.975) { return vec3(0.0); }
    vec3  Star  = Cell + 0.5 + (vec3(Hash(Cell + 1.7), Hash(Cell + 3.1), Hash(Cell + 5.3)) - 0.5) * 0.6;
    float Dist  = length(P - Star);
    float Core  = smoothstep(0.3, 0.0, Dist);
    float Twinkle = 0.75 + 0.25 * sin(InTime * (1.5 + Seed * 4.0) + Seed * 60.0);
    vec3  Tint  = mix(vec3(0.75, 0.82, 1.0), vec3(1.0, 0.9, 0.75), Hash(Cell + 9.0));
    float Magnitude = pow((Seed - 0.975) / 0.025, 3.0);   // a few bright, most faint
    return Tint * Core * Twinkle * (0.15 + 2.5 * Magnitude);
}

void main(void)
{
    if(TOD_Flags.x > 0.5 && texelFetch(TOD_SceneDepth, ivec2(gl_FragCoord.xy), 0).r < 1.0) { discard; }
    vec3 Eye = Scene_CameraPosition.xyz;
    vec3 Dir = normalize(PP_WorldAt(TargetUvToNdc(Interpolants.TexCoord), 0.5) - Eye);

    vec3  SunDir   = normalize(TOD_Sky[0].xyz);
    float SunI     = TOD_Sky[0].w;
    vec3  MoonDir  = normalize(TOD_Sky[1].xyz);
    float MoonI    = TOD_Sky[1].w;
    float Haze     = TOD_Sky[2].x;
    float SunCos   = TOD_Sky[2].y;
    float StarsI   = TOD_Sky[2].z;
    float Exposure = TOD_Sky[2].w;
    vec3  Albedo   = TOD_Sky[3].xyz;
    vec3  Origin   = vec3(0.0, PlanetRadius + max(TOD_Sky[3].w, 1.0), 0.0);

    // Below the horizon: the horizon's own colour, darkening into the ground.
    float Below  = clamp(-Dir.y * 6.0, 0.0, 1.0);
    vec3  SkyDir = normalize(vec3(Dir.x, max(Dir.y, 0.002), Dir.z));

    vec3 Transmit;
    vec3 Sky = Scatter(Origin, SkyDir, SunDir, SunI, MoonDir, MoonI, Haze, Transmit);

    // The discs, through the air in front of them: limb-darkened sun, flat moon.
    float SunCosView = dot(SkyDir, SunDir);
    if(SunCosView > SunCos)
    {
        float Across = clamp((SunCosView - SunCos) / max(1.0 - SunCos, 1e-6), 0.0, 1.0);
        float Limb   = 0.4 + 0.6 * sqrt(Across);
        Sky += Transmit * SunI * 40.0 * Limb * smoothstep(0.0, 0.08, Across);
    }
    float MoonCos = mix(SunCos, 1.0, 0.25);
    if(dot(SkyDir, MoonDir) > MoonCos && MoonDir.y > -0.02)
    {
        Sky += Transmit * vec3(0.85, 0.88, 0.95) * MoonI * 120.0;
    }

    Sky *= Exposure;

    // Airglow, so a moonless night is the darkest blue rather than black; and
    // the stars, where the sky is dark enough to see them. After the
    // exposure: neither brightens as the eye adapts.
    Sky += vec3(0.004, 0.006, 0.014);
    float SkyLuma = dot(Sky, vec3(0.2126, 0.7152, 0.0722));
    float Night   = clamp(1.0 - SkyLuma * 6.0, 0.0, 1.0);
    Sky += Transmit * Stars(SkyDir, Scene_Time.x) * StarsI * Night;

    // The ground below the horizon: the horizon's colour darkened, plus the light
    // the sunlit ground sends back -- what a reflection or the sky light sees
    // under the horizon, where no terrain covers it.
    vec3 Ground = Sky * Albedo * 0.5 + TOD_Bounce.rgb * Exposure;
    if(TOD_Flags.y > 0.5 && texelFetch(TOD_SceneDepth, ivec2(gl_FragCoord.xy), 0).r < 1.0) { discard; }
    Output = vec4(mix(Sky, Ground, Below), 1.0);
}
