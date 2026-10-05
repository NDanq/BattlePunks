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
 *               Particle, 32 bytes each, spawned into round the ring
 *   Instances   GDI_INSTANCE_STRIDE bytes each: the sorted draw
 */
namespace RenderCore
{
    struct ParticleEmitterGpu
    {
        GDIBuffer* Particles    = nullptr;
        GDIBuffer* Instances    = nullptr;
        uint32_t   MaxParticles = 0;

        /* The spawn bookkeeping, on the CPU: the next ring slot, the
         * fraction of a particle owed since last frame, the frames simulated. */
        uint32_t   SpawnCursor = 0;
        double     SpawnCarry  = 0.0;
        uint32_t   Frame       = 0;
        bool       bSimulated  = false;   // the instances hold a sorted frame: drawable
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
        float    Size[4];
        float    StartColor[4];
        float    EndColor[4];
        float    Noise[4];
        uint32_t Sort[4];
    };
    static_assert(sizeof(ParticleParams) == 14 * 16, "ParticleParams must match ParticleCommon.glsl's std140 block");

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
         * zeroed buffers; null where the backend has no compute. */
        ParticleEmitterGpu* Create(uint32_t InMaxParticles);
        void                Release(ParticleEmitterGpu* InEmitter);

        /*
         * This frame's parameters, advancing the spawn ring: InSettings at
         * InEmitterPosition, InDelta seconds on (clamped to a tenth, so a
         * hitch never bursts), InTime for the swirl. Pure CPU; exported for tests.
         */
        RENDERCORE_API ParticleParams BuildParams(ParticleEmitterGpu& InOutEmitter, const ParticleEmitterSettings& InSettings,
                                   const EngineCore::Vector3& InEmitterPosition, const ParticleCamera& InCamera,
                                   float InDelta, double InTime);

        /* The two dispatches: simulate, then sort and write the instances.
         * False if either shader or dispatch is missing. */
        bool Dispatch(ParticleEmitterGpu& InOutEmitter, const ParticleParams& InParams,
                      GDIShader* InSimulate, GDIShader* InSort);
    }
}

#endif //RENDERCORE_PARTICLESIMULATION_H
