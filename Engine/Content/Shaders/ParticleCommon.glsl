// ParticleCommon.glsl -- what ParticleSimulate.csh and ParticleSort.csh share
// (RenderCore/RENDERPIPELINE.md, "Particles"): the emitter's parameters,
// ParticleSimulation.h's ParticleParams byte for byte, and one particle.

#include <ComputeCommon.glsl>

COMPUTE_PARAMS_BEGIN(ParticleParams)
    vec4  Pa_Emitter;        // xyz position, w radius
    vec4  Pa_Velocity;       // xyz velocity, w spread
    vec4  Pa_Acceleration;   // xyz acceleration, w drag (1/s)
    vec4  Pa_Timing;         // x delta, y lifetime, z lifetime variance, w turbulence
    uvec4 Pa_Spawn;          // x first ring slot, y count, z max particles, w seed
    vec4  Pa_Camera;         // xyz position, w 1: face the camera
    vec4  Pa_CameraRight;
    vec4  Pa_CameraUp;
    vec4  Pa_CameraForward;
    vec4  Pa_Size;           // x start, y end, z fade in, w fade out (fractions of a life)
    vec4  Pa_StartColor;
    vec4  Pa_EndColor;
    vec4  Pa_Noise;          // x time, y frequency, z colour curve (age^z)
    uvec4 Pa_Sort;           // x the power of two the sort runs over
COMPUTE_PARAMS_END(ParticleParams)

// Alive while Age < Life; a zeroed particle (age 0, life 0) never was.
struct Particle
{
    vec4 PositionAge;   // xyz world position, w age (seconds)
    vec4 VelocityLife;  // xyz velocity, w lifetime (seconds)
};

bool IsAlive(Particle InParticle) { return InParticle.PositionAge.w < InParticle.VelocityLife.w; }
