#include "Private/RenderCore/ParticleSimulation.h"

#include <algorithm>
#include <cmath>

#include "Private/RenderCore/RenderCoreInternal.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t PARTICLE_BYTES        = 32;   // ParticleCommon.glsl's Particle
        constexpr uint32_t SIMULATE_GROUP_SIZE   = 64;   // ParticleSimulate.csh's local_size_x

        void Copy3(const EngineCore::Vector3& InVector, float (&OutVector)[4], const float InW)
        {
            OutVector[0] = float(InVector.GetX());
            OutVector[1] = float(InVector.GetY());
            OutVector[2] = float(InVector.GetZ());
            OutVector[3] = InW;
        }

        void Copy3(const float (&InVector)[3], float (&OutVector)[4], const float InW)
        {
            OutVector[0] = InVector[0];
            OutVector[1] = InVector[1];
            OutVector[2] = InVector[2];
            OutVector[3] = InW;
        }
    }

    ParticleEmitterGpu* ParticleSimulation::Create(uint32_t InMaxParticles)
    {
        if(!GDISupportsCompute())
        {
            return nullptr;
        }
        InMaxParticles = std::clamp<uint32_t>(InMaxParticles, 1, PARTICLE_MAX_PER_SYSTEM);
        auto* Emitter = new ParticleEmitterGpu();
        Emitter->MaxParticles = InMaxParticles;
        Emitter->Particles    = GDICreateBuffer(InMaxParticles * PARTICLE_BYTES, GDI_BU_Storage);
        Emitter->Instances    = GDICreateBuffer(InMaxParticles * GDI_INSTANCE_STRIDE, GDI_BU_Storage | GDI_BU_Instance);
        if(!Emitter->Particles || !Emitter->Instances)
        {
            Release(Emitter);
            return nullptr;
        }
        return Emitter;
    }

    void ParticleSimulation::Release(ParticleEmitterGpu* InEmitter)
    {
        if(!InEmitter)
        {
            return;
        }
        if(InEmitter->Particles) { InEmitter->Particles->Release(); }
        if(InEmitter->Instances) { InEmitter->Instances->Release(); }
        delete InEmitter;
    }

    ParticleParams ParticleSimulation::BuildParams(ParticleEmitterGpu& InOutEmitter, const ParticleEmitterSettings& InSettings,
                                                   const EngineCore::Vector3& InEmitterPosition, const ParticleCamera& InCamera,
                                                   float InDelta, const double InTime)
    {
        const uint32_t Max = std::max<uint32_t>(InOutEmitter.MaxParticles, 1);
        InDelta = std::clamp(InDelta, 0.0f, 0.1f);

        /* This frame's spawns, the fraction carried to the next; never more than the ring holds. */
        InOutEmitter.SpawnCarry += double(std::max(InSettings.SpawnRate, 0.0f)) * double(InDelta);
        const uint32_t Count = uint32_t(std::min<double>(std::floor(InOutEmitter.SpawnCarry), double(Max)));
        InOutEmitter.SpawnCarry -= double(Count);
        if(InOutEmitter.SpawnCarry > double(Max)) { InOutEmitter.SpawnCarry = 0.0; }
        const uint32_t First = InOutEmitter.SpawnCursor % Max;
        InOutEmitter.SpawnCursor = (First + Count) % Max;
        ++InOutEmitter.Frame;

        uint32_t Pow2 = 1;
        while(Pow2 < Max) { Pow2 <<= 1; }

        ParticleParams Params{};
        Copy3(InEmitterPosition, Params.Emitter, std::max(InSettings.EmitterRadius, 0.0f));
        Copy3(InSettings.Velocity, Params.Velocity, std::max(InSettings.VelocitySpread, 0.0f));
        Copy3(InSettings.Acceleration, Params.Acceleration, std::max(InSettings.Drag, 0.0f));
        Params.Timing[0] = InDelta;
        Params.Timing[1] = std::max(InSettings.Lifetime, 1e-3f);
        Params.Timing[2] = std::clamp(InSettings.LifetimeVariance, 0.0f, 1.0f);
        Params.Timing[3] = std::max(InSettings.Turbulence, 0.0f);
        Params.Spawn[0]  = First;
        Params.Spawn[1]  = Count;
        Params.Spawn[2]  = Max;
        Params.Spawn[3]  = InOutEmitter.Frame * 2654435761u + InSettings.Seed;
        Copy3(InCamera.Position, Params.Camera, InSettings.bFaceCamera ? 1.0f : 0.0f);
        Copy3(InCamera.Right, Params.CameraRight, 0.0f);
        Copy3(InCamera.Up, Params.CameraUp, 0.0f);
        Copy3(InCamera.Forward, Params.CameraForward, 0.0f);
        Params.Size[0] = std::max(InSettings.StartSize, 0.0f);
        Params.Size[1] = std::max(InSettings.EndSize, 0.0f);
        Params.Size[2] = std::clamp(InSettings.FadeIn, 0.0f, 1.0f);
        Params.Size[3] = std::clamp(InSettings.FadeOut, 0.0f, 1.0f);
        for(int Channel = 0; Channel < 4; ++Channel)
        {
            Params.StartColor[Channel] = InSettings.StartColor[Channel];
            Params.EndColor[Channel]   = InSettings.EndColor[Channel];
        }
        /* The swirl's scale: about one wave per few emitter widths, never zero. */
        Params.Noise[0] = float(std::fmod(InTime, 1000.0));
        Params.Noise[1] = 1.0f / std::max(std::max(InSettings.StartSize, InSettings.EndSize) * 4.0f, 1.0f);
        Params.Noise[2] = std::clamp(InSettings.ColorCurve, 0.05f, 20.0f);
        Params.Sort[0]  = Pow2;
        return Params;
    }

    bool ParticleSimulation::Dispatch(ParticleEmitterGpu& InOutEmitter, const ParticleParams& InParams,
                                      GDIShader* InSimulate, GDIShader* InSort)
    {
        if(!InSimulate || !InSort || !InOutEmitter.Particles || !InOutEmitter.Instances)
        {
            return false;
        }
        GDIBuffer* State[1] = { InOutEmitter.Particles };
        const uint32_t Groups = (InOutEmitter.MaxParticles + SIMULATE_GROUP_SIZE - 1) / SIMULATE_GROUP_SIZE;
        if(!GDIDispatchCompute(InSimulate, &InParams, uint32_t(sizeof(InParams)), State, 1, Groups))
        {
            return false;
        }
        GDIBuffer* SortBuffers[2] = { InOutEmitter.Particles, InOutEmitter.Instances };
        if(!GDIDispatchCompute(InSort, &InParams, uint32_t(sizeof(InParams)), SortBuffers, 2, 1))
        {
            return false;
        }
        InOutEmitter.bSimulated = true;
        return true;
    }
}
