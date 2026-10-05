#version 450
// ParticleSimulate.csh -- one emitter's particles, one thread each: this
// frame's spawns born into their ring slots, the rest moved on by Delta
// (RenderCore/RENDERPIPELINE.md, "Particles"). Every type but a beam, whose
// points ParticleBeam.csh places instead.

#include <ParticleCommon.glsl>

layout(local_size_x = 64) in;

STORAGE_BUFFER(1, ParticleState) { Particle Particles[]; };

// A direction uniformly over the sphere, from two randoms.
vec3 RandomDirection(inout uint InOutSeed)
{
    float Z   = HashFloat(InOutSeed) * 2.0 - 1.0;
    float Phi = HashFloat(InOutSeed) * 6.2831853;
    float R   = sqrt(max(1.0 - Z * Z, 0.0));
    return vec3(R * cos(Phi), Z, R * sin(Phi));
}

// A smooth swirl in space and time: three phase-shifted sine waves, each
// axis driven by the other two -- cheap, divergence-ish, never still.
vec3 Swirl(vec3 InPosition, float InTime, float InFrequency)
{
    vec3 P = InPosition * InFrequency;
    return vec3(sin(P.y * 1.7 + InTime * 1.3) + sin(P.z * 1.1 - InTime * 0.7),
                sin(P.z * 1.3 + InTime * 0.9) + sin(P.x * 0.9 + InTime * 1.1) * 0.5,
                sin(P.x * 1.5 - InTime * 1.2) + sin(P.y * 1.2 + InTime * 0.8));
}

void main()
{
    uint Index = gl_GlobalInvocationID.x;
    uint Max   = Pa_Spawn.z;
    if(Index >= Max) { return; }

    float Delta = Pa_Timing.x;
    Particle P  = Particles[Index];

    // This frame's spawns are the Count slots from First round the ring, the
    // first born earliest: each at its own moment inside the frame (so a burst
    // is a stream, not a stack, and a ribbon's points come in the order they
    // join it), where the emitter was at that moment.
    uint Offset = (Index + Max - Pa_Spawn.x) % Max;
    if(Offset < Pa_Spawn.y)
    {
        uint  Seed     = HashUint(Index * 9781u + Pa_Spawn.w * 6271u);
        float Life     = Pa_Timing.y * (1.0 - Pa_Timing.z * HashFloat(Seed));
        float Age      = Delta * (float(Pa_Spawn.y - 1u - Offset) + HashFloat(Seed)) / float(Pa_Spawn.y);
        vec3  Emitter  = mix(Pa_Emitter.xyz, Pa_PrevEmitter.xyz, Delta > 0.0 ? Age / Delta : 0.0);
        vec3  Position = Emitter + RandomDirection(Seed) * (Pa_Emitter.w * pow(HashFloat(Seed), 1.0 / 3.0));
        vec3  Velocity = Pa_Velocity.xyz + RandomDirection(Seed) * (Pa_Velocity.w * HashFloat(Seed));
        P.PositionAge  = vec4(Position + Velocity * Age, Age);
        P.VelocityLife = vec4(Velocity, max(Life, 1e-3));
        P.Extra        = vec4(uintBitsToFloat(HashUint(Seed ^ 0x9E3779B9u)), 0.0, 0.0, 0.0);
    }
    else if(IsAlive(P))
    {
        vec3 Velocity = P.VelocityLife.xyz;
        Velocity += (Pa_Acceleration.xyz + Swirl(P.PositionAge.xyz, Pa_Noise.x, Pa_Noise.y) * Pa_Timing.w) * Delta;
        Velocity *= exp(-Pa_Acceleration.w * Delta);
        P.PositionAge.xyz += Velocity * Delta;
        P.PositionAge.w   += Delta;
        P.VelocityLife.xyz = Velocity;
    }
    Particles[Index] = P;
}
