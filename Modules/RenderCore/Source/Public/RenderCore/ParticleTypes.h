#ifndef RENDERCORE_PARTICLETYPES_H
#define RENDERCORE_PARTICLETYPES_H

#include <cstdint>

/*
 * GPU particles (ParticleSystemComponent, RenderContext's particle calls;
 * RENDERPIPELINE.md, "Particles"): what one emitter simulates, in world
 * space, by compute shaders on the render thread.
 */
namespace RenderCore
{
    /* The most particles one system holds: one workgroup sorts them all
     * back to front (ParticleSort.csh), two a thread. */
    constexpr uint32_t PARTICLE_MAX_PER_SYSTEM = 2048;

    /* An emitter's settings, as RenderContext::SimulateParticles takes them.
     * Vectors are world space; colours sRGB 0-1, as a material's are. */
    struct ParticleEmitterSettings
    {
        uint32_t MaxParticles     = 256;     // 1..PARTICLE_MAX_PER_SYSTEM; the oldest are replaced
        float    SpawnRate        = 32.0f;   // particles a second
        float    Lifetime         = 2.0f;    // seconds
        float    LifetimeVariance = 0.25f;   // 0-1: each lives Lifetime * (1 - Variance * random)
        float    EmitterRadius    = 0.0f;    // born anywhere in a sphere this big round the emitter
        float    Velocity[3]      = { 0.0f, 100.0f, 0.0f };
        float    VelocitySpread   = 0.0f;    // plus this speed in a random direction
        float    Acceleration[3]  = { 0.0f, 0.0f, 0.0f };   // buoyancy up, gravity down
        float    Drag             = 0.0f;    // velocity lost a second, 1/s
        float    Turbulence       = 0.0f;    // a swirl's acceleration
        float    StartSize        = 10.0f;   // across, at birth and death
        float    EndSize          = 10.0f;
        float    StartColor[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
        float    EndColor[4]      = { 1.0f, 1.0f, 1.0f, 0.0f };
        float    ColorCurve       = 1.0f;    // the colour follows age^ColorCurve: under 1 it reaches EndColor sooner
        float    FadeIn           = 0.1f;    // fractions of a life over which alpha rises from 0, and falls to it
        float    FadeOut          = 0.3f;
        bool     bFaceCamera      = true;    // the mesh turned to the view (a quad: a billboard), else upright
        uint32_t Seed             = 0;
    };

    /* One particle as the GPU left it (RenderContext::ReadParticles). */
    struct ParticleSample
    {
        float Position[3] = {};
        float Age         = 0.0f;   // seconds
        float Velocity[3] = {};
        float Lifetime    = 0.0f;   // 0: that slot was never used

        bool IsAlive() const { return Age < Lifetime; }
    };

    /* One drawn instance as the sort wrote it (RenderContext::ReadParticleInstances):
     * GDI's instance layout, a column-major transform then a colour. Farthest first. */
    struct ParticleInstanceSample
    {
        float Transform[16] = {};   // column-major; [12..14] the position; all 0: a dead slot
        float Color[4]      = {};
    };
}

#endif //RENDERCORE_PARTICLETYPES_H
