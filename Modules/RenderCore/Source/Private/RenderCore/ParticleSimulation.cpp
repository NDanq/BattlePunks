#include "Private/RenderCore/ParticleSimulation.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
        /* The draws' counts: a strip's two triangle meshes (3 indices each), or a
         * placeholder entry until a sprite's or mesh's draw lays them out per section. */
        const uint32_t StripRanges[4] = { 3, 0, 3, 0 };
        const uint32_t EmptyRange[2]  = { 0, 0 };
        Emitter->ArgsEntries = bStrip ? 2u : 1u;
        Emitter->Args        = CreateArgs(bStrip ? StripRanges : EmptyRange, Emitter->ArgsEntries, InMaxParticles);
        if(!Emitter->Particles || !Emitter->Instances || (bStrip && !Emitter->InstancesB) || !Emitter->Args)
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
        if(InEmitter->Args)       { InEmitter->Args->Release(); }
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
        Params.Sort[3] = InOutEmitter.ArgsEntries;   // draws whose instance count the passes write

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

        Params.ShapeA[0] = float(InSettings.Shape < PS_Count ? InSettings.Shape : PS_Sphere);
        Params.ShapeA[1] = InSettings.bShapeSurface ? 1.0f : 0.0f;
        Params.ShapeA[2] = InSettings.ShapeSpeed;
        Params.ShapeA[3] = std::clamp(InSettings.ConeAngle, 0.0f, 180.0f) * 3.14159265f / 180.0f;
        for(int Axis = 0; Axis < 3; ++Axis) { Params.ShapeB[Axis] = 0.5f * std::max(InSettings.BoxSize[Axis], 0.0f); }
        Params.ShapeB[3] = std::max(InSettings.RingWidth, 0.0f);
        Copy3(InSettings.ShapeAxisX, Params.ShapeAxisX, 0.0f);
        Copy3(InSettings.ShapeAxisY, Params.ShapeAxisY, 0.0f);

        /* Colour and size over life: the emitter's baked gradient or curve, else Start to End. */
        float Colors[PARTICLE_LUT_SIZE][4];
        float Sizes[PARTICLE_LUT_SIZE];
        InSettings.BakeStartEnd(Colors, Sizes);
        for(uint32_t Index = 0; Index < PARTICLE_LUT_SIZE; ++Index)
        {
            for(int Channel = 0; Channel < 4; ++Channel)
            {
                Params.ColorLut[Index][Channel] = InSettings.bColorLut ? InSettings.ColorLut[Index][Channel] : Colors[Index][Channel];
            }
            Params.SizeLut[Index] = InSettings.bSizeLut ? std::max(InSettings.SizeLut[Index], 0.0f) : Sizes[Index];
        }
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
            GDIBuffer* Strip[4] = { InOutEmitter.Particles, InOutEmitter.Instances, InOutEmitter.InstancesB, InOutEmitter.Args };
            if(!First || !InShaders.Ribbon || !InOutEmitter.InstancesB || !InOutEmitter.Args
               || !GDIDispatchCompute(First, &InParams, uint32_t(sizeof(InParams)), State, 1, Groups)
               || !GDIDispatchCompute(InShaders.Ribbon, &InParams, uint32_t(sizeof(InParams)), Strip, 4, 1))
            {
                return false;
            }
        }
        else
        {
            GDIBuffer* SortBuffers[3] = { InOutEmitter.Particles, InOutEmitter.Instances, InOutEmitter.Args };
            if(!InShaders.Simulate || !InShaders.Sort || !InOutEmitter.Args
               || !GDIDispatchCompute(InShaders.Simulate, &InParams, uint32_t(sizeof(InParams)), State, 1, Groups)
               || !GDIDispatchCompute(InShaders.Sort, &InParams, uint32_t(sizeof(InParams)), SortBuffers, 3, 1))
            {
                return false;
            }
        }
        InOutEmitter.bSimulated = true;
        return true;
    }

    GDIBuffer* ParticleSimulation::CreateArgs(const uint32_t* InRanges, const uint32_t InCount, const uint32_t InInstances)
    {
        /* VkDrawIndexedIndirectCommand each: index count, instance count, first index, vertex offset, first instance. */
        std::vector<uint32_t> Words(size_t(std::max(InCount, 1u)) * 5u, 0u);
        for(uint32_t Index = 0; Index < InCount; ++Index)
        {
            Words[Index * 5 + 0] = InRanges[Index * 2 + 0];
            Words[Index * 5 + 1] = InInstances;
            Words[Index * 5 + 2] = InRanges[Index * 2 + 1];
        }
        static_assert(GDI_INDIRECT_ARGS_STRIDE == 5 * sizeof(uint32_t), "an entry is five words");
        return GDICreateBuffer(uint32_t(Words.size() * sizeof(uint32_t)), GDI_BU_Storage | GDI_BU_Indirect, Words.data());
    }
}
