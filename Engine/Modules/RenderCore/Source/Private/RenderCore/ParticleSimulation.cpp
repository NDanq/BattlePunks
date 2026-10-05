#include "Private/RenderCore/ParticleSimulation.h"

#include <algorithm>
#include <cmath>

#include "Private/RenderCore/RenderCoreInternal.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t PARTICLE_BYTES        = 48;   // ParticleCommon.glsl's Particle
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

    ParticleEmitterGpu* ParticleSimulation::Create(uint32_t InMaxParticles, const uint8_t InType)
    {
        if(!GDISupportsCompute())
        {
            return nullptr;
        }
        InMaxParticles = std::clamp<uint32_t>(InMaxParticles, 1, PARTICLE_MAX_PER_SYSTEM);
        auto* Emitter = new ParticleEmitterGpu();
        Emitter->MaxParticles = InMaxParticles;
        Emitter->Type         = InType < PT_Count ? InType : uint8_t(PT_Sprite);
        Emitter->Particles    = GDICreateBuffer(InMaxParticles * PARTICLE_BYTES, GDI_BU_Storage);
        Emitter->Instances    = GDICreateBuffer(InMaxParticles * GDI_INSTANCE_STRIDE, GDI_BU_Storage | GDI_BU_Instance);
        const bool bStrip = Emitter->Type == PT_Ribbon || Emitter->Type == PT_Beam;
        if(bStrip)
        {
            Emitter->InstancesB = GDICreateBuffer(InMaxParticles * GDI_INSTANCE_STRIDE, GDI_BU_Storage | GDI_BU_Instance);
        }
        if(!Emitter->Particles || !Emitter->Instances || (bStrip && !Emitter->InstancesB))
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
        if(InEmitter->InstancesB) { InEmitter->InstancesB->Release(); }
        delete InEmitter;
    }

    ParticleParams ParticleSimulation::BuildParams(ParticleEmitterGpu& InOutEmitter, const ParticleEmitterSettings& InSettings,
                                                   const EngineCore::Vector3& InEmitterPosition, const ParticleCamera& InCamera,
                                                   float InDelta, const double InTime)
    {
        const uint32_t Max = std::max<uint32_t>(InOutEmitter.MaxParticles, 1);
        InDelta = std::clamp(InDelta, 0.0f, 0.1f);

        /* Starting to emit (again) starts the bursts over. */
        if(InSettings.bEmitting && !InOutEmitter.bWasEmitting)
        {
            InOutEmitter.EmitTime    = 0.0;
            InOutEmitter.BurstsFired = 0;
        }
        InOutEmitter.bWasEmitting = InSettings.bEmitting;

        /* This frame's spawns; never more than the ring holds. */
        double Owed = 0.0;
        if(InSettings.bEmitting && InSettings.Type != PT_Beam)
        {
            if(InSettings.SpawnMode == PSM_Burst)
            {
                /* Each burst falls due Burst Interval after the last, Burst Cycles
                 * of them (0: no end); with no interval, one. */
                const double   Interval = std::max(double(InSettings.BurstInterval), 0.0);
                const uint32_t Cycles   = Interval > 0.0 ? InSettings.BurstCycles : 1u;
                while((Cycles == 0 || InOutEmitter.BurstsFired < Cycles)
                      && InOutEmitter.EmitTime + 1e-9 >= double(InOutEmitter.BurstsFired) * Interval)
                {
                    Owed += double(InSettings.BurstCount);
                    ++InOutEmitter.BurstsFired;
                    if(Interval <= 0.0) { break; }
                }
            }
            else
            {
                InOutEmitter.SpawnCarry += double(std::max(InSettings.SpawnRate, 0.0f)) * double(InDelta);
            }
            InOutEmitter.EmitTime += double(InDelta);
        }
        InOutEmitter.SpawnCarry += Owed;
        const uint32_t Count = uint32_t(std::min<double>(std::floor(InOutEmitter.SpawnCarry), double(Max)));
        InOutEmitter.SpawnCarry -= double(Count);
        if(InOutEmitter.SpawnCarry > double(Max) || InSettings.SpawnMode == PSM_Burst) { InOutEmitter.SpawnCarry = 0.0; }
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
        Copy3(InCamera.Position, Params.Camera, 0.0f);
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

        /* A ribbon's points run from the oldest slot -- the next to be spawned into -- round the
         * ring; a beam's from slot 0 out to its end. */
        const uint32_t Segments = std::clamp<uint32_t>(InSettings.BeamSegments, 1u, PARTICLE_BEAM_SEGMENTS_MAX);
        Params.Sort[1] = InSettings.Type == PT_Beam ? 0u : InOutEmitter.SpawnCursor;
        Params.Sort[2] = InSettings.Type == PT_Beam ? std::min(Segments + 1u, Max) : Max;

        /* Where the emitter was a frame ago; the first frame, where it is. */
        if(!InOutEmitter.bHasPrev)
        {
            InOutEmitter.PrevPosition[0] = float(InEmitterPosition.GetX());
            InOutEmitter.PrevPosition[1] = float(InEmitterPosition.GetY());
            InOutEmitter.PrevPosition[2] = float(InEmitterPosition.GetZ());
            InOutEmitter.bHasPrev = true;
        }
        Copy3(InOutEmitter.PrevPosition, Params.PrevEmitter, 0.0f);
        InOutEmitter.PrevPosition[0] = float(InEmitterPosition.GetX());
        InOutEmitter.PrevPosition[1] = float(InEmitterPosition.GetY());
        InOutEmitter.PrevPosition[2] = float(InEmitterPosition.GetZ());

        Params.Render[0] = InSettings.Type < PT_Count ? InSettings.Type : uint32_t(PT_Sprite);
        Params.Render[1] = InSettings.Billboard < PB_Count ? InSettings.Billboard : uint32_t(PB_FaceCamera);
        Params.Render[2] = InSettings.MeshOrientation < PMO_Count ? InSettings.MeshOrientation : uint32_t(PMO_Fixed);
        Params.Render[3] = InSettings.bRandomRotation ? 1u : 0u;
        Params.Motion[0] = InSettings.RotationRate * 3.14159265f / 180.0f;
        Params.Motion[1] = std::max(InSettings.VelocityStretch, 0.0f);
        Copy3(InSettings.BeamEnd, Params.Beam, std::max(InSettings.BeamNoise, 0.0f));
        Params.BeamNoise[0] = std::max(InSettings.BeamNoiseSpeed, 0.0f);
        Params.BeamNoise[1] = float(Segments);
        Params.BeamNoise[2] = float(InSettings.Seed % 1024u) * 0.618f;
        return Params;
    }

    bool ParticleSimulation::Dispatch(ParticleEmitterGpu& InOutEmitter, const ParticleParams& InParams, const Shaders& InShaders)
    {
        if(!InOutEmitter.Particles || !InOutEmitter.Instances)
        {
            return false;
        }
        const uint32_t Groups = (InOutEmitter.MaxParticles + SIMULATE_GROUP_SIZE - 1) / SIMULATE_GROUP_SIZE;
        GDIBuffer* State[1] = { InOutEmitter.Particles };
        const uint8_t Type = InOutEmitter.Type;
        if(Type == PT_Ribbon || Type == PT_Beam)
        {
            GDIShader* First = Type == PT_Beam ? InShaders.Beam : InShaders.Simulate;
            GDIBuffer* Strip[3] = { InOutEmitter.Particles, InOutEmitter.Instances, InOutEmitter.InstancesB };
            if(!First || !InShaders.Ribbon || !InOutEmitter.InstancesB
               || !GDIDispatchCompute(First, &InParams, uint32_t(sizeof(InParams)), State, 1, Groups)
               || !GDIDispatchCompute(InShaders.Ribbon, &InParams, uint32_t(sizeof(InParams)), Strip, 3, Groups))
            {
                return false;
            }
        }
        else
        {
            GDIBuffer* SortBuffers[2] = { InOutEmitter.Particles, InOutEmitter.Instances };
            if(!InShaders.Simulate || !InShaders.Sort
               || !GDIDispatchCompute(InShaders.Simulate, &InParams, uint32_t(sizeof(InParams)), State, 1, Groups)
               || !GDIDispatchCompute(InShaders.Sort, &InParams, uint32_t(sizeof(InParams)), SortBuffers, 2, 1))
            {
                return false;
            }
        }
        InOutEmitter.bSimulated = true;
        return true;
    }
}
