#ifndef RENDERCORE_PARTICLESIMULATION_H
#define RENDERCORE_PARTICLESIMULATION_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/ParticleTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"

class GDIBuffer;
class GDIShader;

/*
 * One emitter's particles on the GPU (RenderContext's particle calls;
 * RENDERPIPELINE.md, "Particles"). Private: it holds GDI objects.
 *
 *   Particles   PARTICLE_MAX_PER_SYSTEM-capped ring of ParticleCommon.glsl's
 *               Particle, 48 bytes each, spawned into round the ring (a
 *               beam's points, from the emitter to its end)
 *   Instances   GDI_INSTANCE_STRIDE bytes each: the sorted draw (a sprite or
 *               mesh emitter); a ribbon's or beam's first triangle a segment
 *   InstancesB  a ribbon's or beam's second triangle a segment; else null
 *   Args        GDI_INDIRECT_ARGS_STRIDE bytes a draw (GDISetIndirectArgs):
 *               the compute passes write the live count into each entry's
 *               instance count, so the draws draw only the live ones. A
 *               sprite or mesh emitter has an entry per section of its mesh,
 *               laid out when it is first drawn (one empty entry before); a
 *               ribbon or beam one per triangle mesh.
 */
namespace RenderCore
{
    struct ParticleEmitterGpu
    {
        GDIBuffer* Particles    = nullptr;
        GDIBuffer* Instances    = nullptr;
        GDIBuffer* InstancesB   = nullptr;
        GDIBuffer* Args         = nullptr;
        uint32_t   ArgsEntries  = 0;          // draws Args holds, each GDI_INDIRECT_ARGS_STRIDE bytes
        const void* ArgsMesh    = nullptr;    // the mesh (MeshData) Args's entries were laid out for
        uint32_t   MaxParticles = 0;
        uint8_t    Type         = PT_Sprite;   // as made: a ribbon or beam has InstancesB

        /* The spawn bookkeeping, on the CPU: the next ring slot, the
         * fraction of a particle owed since last frame, the frames simulated. */
        uint32_t   SpawnCursor = 0;
        double     SpawnCarry  = 0.0;
        uint32_t   Frame       = 0;
        bool       bSimulated  = false;   // the instances hold a frame: drawable

        /* Bursts: seconds emitting, bursts fired. Both start over when it
         * starts emitting again. */
        double     EmitTime     = 0.0;
        uint32_t   BurstsFired  = 0;
        bool       bWasEmitting = false;

        /* Where the emitter was last frame: this frame's spawns are spread between. */
        float      PrevPosition[3] = {};
        bool       bHasPrev        = false;
    };

    /* ParticleCommon.glsl's ParticleParams, std140, byte for byte. */
    struct ParticleParams
    {
        float    Emitter[4];
        float    Velocity[4];
        float    Acceleration[4];
        float    Timing[4];
        uint32_t Spawn[4];
        float    Camera[4];
        float    CameraRight[4];
        float    CameraUp[4];
        float    CameraForward[4];
        float    Size[4];        // x start, y end (the swirl's scale), z 1: random start frame, w flipbook cycles over a life
        float    Collision[4];   // x response (EParticleCollision), y plane height (world), z bounce, w friction
        float    Flipbook[4];    // x columns, y rows (0: none), z frames, w frames a second (0: over its life)
        float    Noise[4];
        uint32_t Sort[4];
        float    PrevEmitter[4];
        uint32_t Render[4];
        float    Motion[4];
        float    Beam[4];
        float    BeamNoise[4];
        float    ShapeA[4];      // x shape, y 1: surface, z outward speed, w cone angle (radians)
        float    ShapeB[4];      // xyz a box's half size, w a ring's width
        float    ShapeAxisX[4];  // the shape's +X and +Y in the world
        float    ShapeAxisY[4];
        float    ColorLut[PARTICLE_LUT_SIZE][4];      // colour over life, birth to death
        float    SizeLut[PARTICLE_LUT_SIZE];          // size over life, four a vec4
    };
    static_assert(sizeof(ParticleParams) == (23 + PARTICLE_LUT_SIZE + PARTICLE_LUT_SIZE / 4) * 16,
                  "ParticleParams must match ParticleCommon.glsl's std140 block");
    static_assert(sizeof(ParticleParams) <= 1024, "ParticleParams must fit GDI_COMPUTE_PARAMS_MAX_BYTES");

    /* The camera a frame's particles are sorted and turned toward. */
    struct ParticleCamera
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Right{1.0, 0.0, 0.0};
        EngineCore::Vector3 Up{0.0, 1.0, 0.0};
        EngineCore::Vector3 Forward{0.0, 0.0, 1.0};
    };

    namespace ParticleSimulation
    {
        /* InMaxParticles (clamped to 1..PARTICLE_MAX_PER_SYSTEM) worth of
         * zeroed buffers for an emitter of InType; null where the backend has
         * no compute. */
        ParticleEmitterGpu* Create(uint32_t InMaxParticles, uint8_t InType = PT_Sprite);
        void                Release(ParticleEmitterGpu* InEmitter);

        /*
         * This frame's parameters, advancing the spawn ring: InSettings at
         * InEmitterPosition, InDelta seconds on (clamped to a tenth, so a
         * hitch never bursts), InTime for the swirl and a beam's jitter. A
         * Continuous emitter spawns its rate times the delta, the fraction
         * carried; a Burst emitter its Burst Count each time one falls due.
         * Never more than the ring holds in one frame. Pure CPU; exported for tests.
         */
        RENDERCORE_API ParticleParams BuildParams(ParticleEmitterGpu& InOutEmitter, const ParticleEmitterSettings& InSettings,
                                   const EngineCore::Vector3& InEmitterPosition, const ParticleCamera& InCamera,
                                   float InDelta, double InTime);

        /* The compute shaders a frame runs. */
        struct Shaders
        {
            GDIShader* Simulate = nullptr;   // ParticleSimulate.csh
            GDIShader* Sort     = nullptr;   // ParticleSort.csh
            GDIShader* Ribbon   = nullptr;   // ParticleRibbon.csh
            GDIShader* Beam     = nullptr;   // ParticleBeam.csh
        };

        /* Its type's two dispatches: a sprite or mesh emitter is simulated,
         * then sorted into its instances; a ribbon simulated, then strung into
         * its triangles; a beam's points placed, then strung. False if a
         * shader or dispatch is missing. */
        bool Dispatch(ParticleEmitterGpu& InOutEmitter, const ParticleParams& InParams, const Shaders& InShaders);

        /* InCount draws' arguments, InRanges' index count and first index
         * each (pairs), every instance count InInstances: a new Args buffer,
         * Storage and Indirect. Null where unsupported. */
        GDIBuffer* CreateArgs(const uint32_t* InRanges, uint32_t InCount, uint32_t InInstances);
    }
}

#endif //RENDERCORE_PARTICLESIMULATION_H
