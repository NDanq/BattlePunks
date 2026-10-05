// ParticleCommon.glsl -- what the particle compute shaders share
// (RenderCore/RENDERPIPELINE.md, "Particles"): the emitter's parameters,
// ParticleSimulation.h's ParticleParams byte for byte, one particle, and the
// size and colour a particle has at an age.

#include <ComputeCommon.glsl>

COMPUTE_PARAMS_BEGIN(ParticleParams)
    vec4  Pa_Emitter;        // xyz position, w radius
    vec4  Pa_Velocity;       // xyz velocity, w spread
    vec4  Pa_Acceleration;   // xyz acceleration, w drag (1/s)
    vec4  Pa_Timing;         // x delta, y lifetime, z lifetime variance, w turbulence
    uvec4 Pa_Spawn;          // x first ring slot, y count, z ring size, w seed
    vec4  Pa_Camera;         // xyz position
    vec4  Pa_CameraRight;
    vec4  Pa_CameraUp;
    vec4  Pa_CameraForward;
    vec4  Pa_Size;           // x start, y end, z fade in, w fade out (fractions of a life)
    vec4  Pa_StartColor;
    vec4  Pa_EndColor;
    vec4  Pa_Noise;          // x time, y frequency, z colour curve (age^z)
    uvec4 Pa_Sort;           // x the power of two the sort runs over; y a strip's first slot, z its points
    vec4  Pa_PrevEmitter;    // xyz the emitter's position a frame ago: spawns are spread between the two
    uvec4 Pa_Render;         // x type, y billboard, z mesh orientation, w 1: random rotation
    vec4  Pa_Motion;         // x rotation rate (radians a second), y velocity stretch (seconds)
    vec4  Pa_Beam;           // xyz where a beam ends, w its jitter (world units)
    vec4  Pa_BeamNoise;      // x jitter speed, y beam segments, z a phase from the emitter's seed
COMPUTE_PARAMS_END(ParticleParams)

#define PARTICLE_SPRITE 0u
#define PARTICLE_MESH   1u
#define PARTICLE_RIBBON 2u
#define PARTICLE_BEAM   3u

#define BILLBOARD_FACE_CAMERA      0u
#define BILLBOARD_VERTICAL         1u
#define BILLBOARD_VELOCITY_ALIGNED 2u
#define BILLBOARD_FIXED            3u

#define MESH_FIXED             0u
#define MESH_RANDOM_SPIN       1u
#define MESH_ALIGN_TO_VELOCITY 2u

// Alive while Age < Life; a zeroed particle (age 0, life 0) never was.
struct Particle
{
    vec4 PositionAge;   // xyz world position, w age (seconds)
    vec4 VelocityLife;  // xyz velocity, w lifetime (seconds)
    vec4 Extra;         // x its random bits (uintBitsToFloat), fixed at birth
};

// A drawn instance: GDIBuffer.h's layout.
struct Instance
{
    mat4 Transform;
    vec4 Color;
};

bool IsAlive(Particle InParticle) { return InParticle.PositionAge.w < InParticle.VelocityLife.w; }

// 0 at birth, 1 at death.
float NormalizedAge(Particle InParticle) { return clamp(InParticle.PositionAge.w / max(InParticle.VelocityLife.w, 1e-6), 0.0, 1.0); }

float SizeAt(float InAge) { return mix(Pa_Size.x, Pa_Size.y, InAge); }

// Its colour over life, faded in and out.
vec4 ColorAt(float InAge)
{
    float Alpha = smoothstep(0.0, max(Pa_Size.z, 1e-4), InAge) * (1.0 - smoothstep(1.0 - max(Pa_Size.w, 1e-4), 1.0, InAge));
    vec4  Color = mix(Pa_StartColor, Pa_EndColor, pow(InAge, Pa_Noise.z));
    Color.a    *= Alpha;
    return Color;
}
