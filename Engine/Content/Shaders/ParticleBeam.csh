#version 450
// ParticleBeam.csh -- a beam emitter's points, one thread each
// (RenderCore/RENDERPIPELINE.md, "Particles"): Pa_BeamNoise.y segments from
// the emitter to Pa_Beam.xyz, each point strayed across the beam by a smooth
// jitter that moves with time, none at the ends. A point's age runs 0 to 1
// of a life from the emitter to the end, so size and colour over life become
// size and colour along the beam. ParticleRibbon.csh then draws the strip.

#include <ParticleCommon.glsl>

layout(local_size_x = 64) in;

STORAGE_BUFFER(1, ParticleState) { Particle Particles[]; };

vec3 AnyPerpendicular(vec3 InDirection)
{
    vec3 Other = abs(InDirection.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(Other, InDirection));
}

void main()
{
    uint Index    = gl_GlobalInvocationID.x;
    uint Segments = uint(Pa_BeamNoise.y);
    if(Index > Segments || Index >= Pa_Spawn.z) { return; }

    float T     = float(Index) / float(max(Segments, 1u));
    vec3  From  = Pa_Emitter.xyz;
    vec3  To    = Pa_Beam.xyz;
    vec3  Along = To - From;
    vec3  Dir   = dot(Along, Along) > 1e-10 ? normalize(Along) : vec3(0.0, 1.0, 0.0);
    vec3  U     = AnyPerpendicular(Dir);
    vec3  V     = cross(Dir, U);

    // Two layered waves a side, phase-shifted by the seed, swept along by time.
    float Time  = Pa_Noise.x * Pa_BeamNoise.x;
    float Phase = Pa_BeamNoise.z;
    float N1    = sin(T * 9.1 + Time * 1.00 + Phase) + 0.5 * sin(T * 23.7 - Time * 1.73 + Phase * 2.0);
    float N2    = sin(T * 7.3 - Time * 1.21 + Phase * 3.0) + 0.5 * sin(T * 19.3 + Time * 2.11 + Phase);
    vec3  Stray = (U * N1 + V * N2) * (Pa_Beam.w / 1.5) * sin(3.14159265 * T);

    float Life = max(Pa_Timing.y, 1e-3);
    Particle P;
    P.PositionAge  = vec4(From + Along * T + Stray, T * Life * 0.9999);
    P.VelocityLife = vec4(Dir, Life);
    P.Extra        = vec4(0.0);
    Particles[Index] = P;
}
