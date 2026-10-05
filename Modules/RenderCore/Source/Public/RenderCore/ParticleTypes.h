#ifndef RENDERCORE_PARTICLETYPES_H
#define RENDERCORE_PARTICLETYPES_H

#include <cstdint>

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * GPU particles (ParticleSystemComponent, RenderContext's particle calls;
 * RENDERPIPELINE.md, "Particles"): what one emitter simulates, in world
 * space, by compute shaders on the render thread.
 */
namespace RenderCore
{
    /* What a Sprite emitter with an empty Mesh draws: a 1 x 1 quad facing -Z, turned as its Billboard says. */
    constexpr const char* PARTICLE_DEFAULT_MESH = "Engine/Content/Meshes/Quad.nmmesh";

    /* What a Mesh emitter with an empty Mesh draws: a 1-unit box. */
    constexpr const char* PARTICLE_DEFAULT_SHAPE_MESH = "Engine/Content/Meshes/Box.nmmesh";

    /* What a ribbon or beam draws each segment with: two triangles, mapped
     * exactly onto its corners (ParticleRibbon.csh). Corners (0,0,0)
     * (1,0,0) (0,1,0), facing -Z; A's UVs (0,0) (1,0) (0,1), B's (1,1)
     * (0,1) (1,0). */
    constexpr const char* PARTICLE_RIBBON_MESH_A = "Engine/Content/Meshes/RibbonSegmentA.nmmesh";
    constexpr const char* PARTICLE_RIBBON_MESH_B = "Engine/Content/Meshes/RibbonSegmentB.nmmesh";

    /* The most particles one emitter holds: one workgroup sorts them all
     * back to front (ParticleSort.csh), two a thread. */
    constexpr uint32_t PARTICLE_MAX_PER_SYSTEM = 2048;

    /* What an emitter's particles are drawn as. */
    enum EParticleType : uint8_t
    {
        PT_Sprite = 0,   // a quad (or any mesh) a particle, turned as Billboard says
        PT_Mesh   = 1,   // a mesh a particle, sized by Size, turned as MeshOrientation says
        PT_Ribbon = 2,   // one strip through the particles in the order they were born: a trail
        PT_Beam   = 3,   // one strip from the emitter to BeamEnd, through BeamSegments jittering points
        PT_Count
    };

    /* How a sprite is turned. */
    enum EParticleBillboard : uint8_t
    {
        PB_FaceCamera      = 0,   // square to the view
        PB_Vertical        = 1,   // turned about world +Y toward the eye only: upright flames, trees
        PB_VelocityAligned = 2,   // along its velocity, stretched by speed, turned to the eye about it: sparks, rain
        PB_Fixed           = 3,   // the quad's own axes, facing -Z: a decal-like card
        PB_Count
    };

    /* How a mesh particle is turned. */
    enum EParticleMeshOrientation : uint8_t
    {
        PMO_Fixed           = 0,   // the mesh's own axes (plus Rotation Rate about +Y)
        PMO_RandomSpin      = 1,   // a random axis each, at Rotation Rate (debris, leaves)
        PMO_AlignToVelocity = 2,   // its +Z along its velocity (arrows, fish)
        PMO_Count
    };

    /* When an emitter spawns. */
    enum EParticleSpawnMode : uint8_t
    {
        PSM_Continuous = 0,   // Spawn Rate a second, for as long as it emits
        PSM_Burst      = 1,   // Burst Count at once, then again every Burst Interval, Burst Cycles times
        PSM_Count
    };

    /* Where in an emitter's shape a particle is born. Every shape follows
     * the entity's rotation (ShapeAxisX/Y); its outward direction is what
     * ShapeSpeed pushes along. */
    enum EParticleShape : uint8_t
    {
        PS_Sphere     = 0,   // within EmitterRadius; outward from the centre
        PS_Hemisphere = 1,   // the sphere's upper (+Y) half
        PS_Box        = 2,   // within BoxSize; outward through the nearest face
        PS_Cone       = 3,   // a disc of EmitterRadius facing +Y; out within ConeAngle of +Y
        PS_Ring       = 4,   // a circle of EmitterRadius about +Y, RingWidth wide; outward from its centre
        PS_Count
    };

    /* How colour (and alpha) or size follows a particle's life. */
    enum EParticleOverLife : uint8_t
    {
        POL_StartEnd = 0,   // Start to End (colour: by ColorCurve, faded in and out)
        POL_Keys     = 1,   // through keys: a colour gradient, a size curve
        POL_Count
    };

    /* Samples a colour or size over life is baked into, evenly from birth to death. */
    constexpr uint32_t PARTICLE_LUT_SIZE = 32;

    /* An emitter's settings, as RenderContext::SimulateParticles takes them.
     * Vectors are world space; colours sRGB 0-1, as a material's are. */
    struct ParticleEmitterSettings
    {
        uint8_t  Type             = PT_Sprite;
        uint32_t MaxParticles     = 256;     // 1..PARTICLE_MAX_PER_SYSTEM; the oldest are replaced
        bool     bEmitting        = true;    // off: no spawns; on again, a burst emitter starts over
        uint8_t  SpawnMode        = PSM_Continuous;
        float    SpawnRate        = 32.0f;   // particles a second (Continuous)
        uint32_t BurstCount       = 32;      // particles a burst (Burst)
        float    BurstInterval    = 0.0f;    // seconds between bursts; 0: one burst
        uint32_t BurstCycles      = 1;       // bursts in all; 0: no end (with an interval)
        float    Lifetime         = 2.0f;    // seconds
        float    LifetimeVariance = 0.25f;   // 0-1: each lives Lifetime * (1 - Variance * random)
        float    EmitterRadius    = 0.0f;    // born anywhere in a sphere this big round the emitter
        float    Velocity[3]      = { 0.0f, 100.0f, 0.0f };
        float    VelocitySpread   = 0.0f;    // plus this speed in a random direction
        float    Acceleration[3]  = { 0.0f, 0.0f, 0.0f };   // buoyancy up, gravity down
        float    Drag             = 0.0f;    // velocity lost a second, 1/s
        float    Turbulence       = 0.0f;    // a swirl's acceleration
        float    StartSize        = 10.0f;   // across (a ribbon or beam: its width), at birth and death
        float    EndSize          = 10.0f;
        float    StartColor[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
        float    EndColor[4]      = { 1.0f, 1.0f, 1.0f, 0.0f };
        float    ColorCurve       = 1.0f;    // the colour follows age^ColorCurve: under 1 it reaches EndColor sooner
        float    FadeIn           = 0.1f;    // fractions of a life over which alpha rises from 0, and falls to it
        float    FadeOut          = 0.3f;
        uint8_t  Billboard        = PB_FaceCamera;
        float    VelocityStretch  = 0.05f;   // Velocity Aligned: seconds of travel added to its length
        uint8_t  MeshOrientation  = PMO_RandomSpin;
        bool     bRandomRotation  = false;   // each starts at a random angle and spins either way
        float    RotationRate     = 0.0f;    // degrees a second: a sprite in its plane, a mesh about its axis
        float    BeamEnd[3]       = { 0.0f, 200.0f, 0.0f };   // world space: where a beam ends
        uint32_t BeamSegments     = 16;      // 1..PARTICLE_BEAM_SEGMENTS_MAX
        float    BeamNoise        = 0.0f;    // how far its middle strays, world units
        float    BeamNoiseSpeed   = 4.0f;    // how fast the jitter changes, a second
        uint32_t Seed             = 0;

        /* Its shape (EmitterRadius is its radius). */
        uint8_t  Shape            = PS_Sphere;
        bool     bShapeSurface    = false;   // born on its surface (a sphere's shell, a box's faces, a ring's circle, a cone's rim)
        float    BoxSize[3]       = { 100.0f, 100.0f, 100.0f };
        float    ConeAngle        = 25.0f;   // degrees from +Y
        float    RingWidth        = 0.0f;    // world units across the circle
        float    ShapeSpeed       = 0.0f;    // plus this speed outward from the shape
        float    ShapeAxisX[3]    = { 1.0f, 0.0f, 0.0f };   // the shape's +X and +Y in the world: the entity's
        float    ShapeAxisY[3]    = { 0.0f, 1.0f, 0.0f };

        /* Colour and size over life, baked: used instead of Start/End when
         * set (ParticleEmitterDesc::ToSettings bakes a gradient or curve). */
        bool     bColorLut        = false;
        float    ColorLut[PARTICLE_LUT_SIZE][4] = {};
        bool     bSizeLut         = false;
        float    SizeLut[PARTICLE_LUT_SIZE] = {};

        /* Start to End as samples: the colour by ColorCurve, faded in and out; the size straight. */
        RENDERCORE_API void BakeStartEnd(float (&OutColor)[PARTICLE_LUT_SIZE][4], float (&OutSize)[PARTICLE_LUT_SIZE]) const;

        /* Particles its GPU ring holds: a beam's points, or MaxParticles. */
        uint32_t GetCapacity() const;
    };

    /* The most points a beam is drawn through, less one. */
    constexpr uint32_t PARTICLE_BEAM_SEGMENTS_MAX = 128;

    inline uint32_t ParticleEmitterSettings::GetCapacity() const
    {
        return Type == PT_Beam ? BeamSegments + 1 : MaxParticles;
    }

    /* One particle as the GPU left it (RenderContext::ReadParticles). */
    struct ParticleSample
    {
        float    Position[3] = {};
        float    Age         = 0.0f;   // seconds
        float    Velocity[3] = {};
        float    Lifetime    = 0.0f;   // 0: that slot was never used
        uint32_t Random      = 0;      // its own random bits, fixed at birth: its rotation and spin
        float    Reserved[3] = {};

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
