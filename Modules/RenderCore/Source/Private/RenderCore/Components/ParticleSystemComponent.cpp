#include "Public/RenderCore/Components/ParticleSystemComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

/* ParticleSystemComponent: registration, its proxy, and what it exposes to
 * saves, Lua and the editor. */

namespace RenderCore
{
    namespace
    {
        /*
         * The render-thread side: its emitter on the GPU, simulated once a
         * frame (Simulate) and drawn from every view that sees it (Draw).
         * Never static: its draws change every frame on their own.
         */
        class ParticleSystemProxy final : public RenderProxy
        {
        public:
            ParticleSystemProxy(const GameCore::EntityID InOwner, const ParticleSystemComponent& InInit)
                : RenderProxy(InOwner)
            {
                ApplyComponent(&InInit);
            }

            bool AllowsStaticDraws() const override { return false; }

            void ApplyComponent(const void* InComponentBytes) override
            {
                const auto& C = *static_cast<const ParticleSystemComponent*>(InComponentBytes);
                Settings = C.ToSettings();
                MeshPath = C.Mesh[0] != '\0' ? std::string(C.Mesh) : std::string(PARTICLE_DEFAULT_MESH);
                MaterialPath.assign(C.Material);

                /* As far as a particle can get from the emitter in its life, so culling never clips the plume. */
                const auto Length = [](const float (&InVector)[3])
                {
                    return std::sqrt(InVector[0] * InVector[0] + InVector[1] * InVector[1] + InVector[2] * InVector[2]);
                };
                const float Life  = std::max(Settings.Lifetime, 0.0f);
                const float Reach = Settings.EmitterRadius + (Length(Settings.Velocity) + Settings.VelocitySpread) * Life
                                  + 0.5f * (Length(Settings.Acceleration) + Settings.Turbulence) * Life * Life
                                  + std::max(Settings.StartSize, Settings.EndSize);
                SetLocalBounds(GameCore::AABB(EngineCore::Vector3(-Reach, -Reach, -Reach), EngineCore::Vector3(Reach, Reach, Reach)));
            }

            void Simulate(RenderContext& InContext) override
            {
                if(!Emitter || EmitterCapacity != Settings.MaxParticles)
                {
                    InContext.ReleaseParticleEmitter(Emitter);
                    Emitter         = InContext.CreateParticleEmitter(Settings.MaxParticles);
                    EmitterCapacity = Settings.MaxParticles;
                }
                if(!Emitter)
                {
                    if(!bWarned)
                    {
                        bWarned = true;
                        RENDERCORE_LOG_WARN(InContext.SupportsParticles()
                                            ? std::string("a particle system could not make its GPU buffers: it draws nothing")
                                            : std::string("this backend runs no compute shaders: particle systems draw nothing"));
                    }
                    return;
                }
                InContext.SimulateParticles(*Emitter, Settings, GetTransform().Position);
            }

            void Draw(RenderContext& InContext) override
            {
                if(Emitter)
                {
                    InContext.DrawParticles(*Emitter, GetTransform().Position, MeshPath.c_str(), MaterialPath.c_str());
                }
            }

            void ReleaseRenderResources(RenderContext& InContext) override
            {
                InContext.ReleaseParticleEmitter(Emitter);
                Emitter = nullptr;
            }

        private:
            ParticleEmitterSettings Settings;
            std::string             MeshPath;
            std::string             MaterialPath;
            ParticleEmitterGpu*     Emitter         = nullptr;
            uint32_t                EmitterCapacity = 0;
            bool                    bWarned         = false;
        };

        void ToFloats(const EngineCore::Vector3& InVector, float (&OutVector)[3])
        {
            OutVector[0] = float(InVector.GetX());
            OutVector[1] = float(InVector.GetY());
            OutVector[2] = float(InVector.GetZ());
        }

        void ToFloats(const EngineCore::Color InColor, float (&OutColor)[4])
        {
            OutColor[0] = InColor.Rf();
            OutColor[1] = InColor.Gf();
            OutColor[2] = InColor.Bf();
            OutColor[3] = InColor.Af();
        }
    }

    RENDERCORE_DEFINE_PROXY_COMPONENT(ParticleSystemComponent, "Particle System", ParticleSystemProxy)

    CreateProxyFn GetParticleSystemComponentFactory()
    {
        return &ParticleSystemComponent_CreateProxy;
    }

    ParticleEmitterSettings ParticleSystemComponent::ToSettings() const
    {
        ParticleEmitterSettings Out;
        Out.MaxParticles     = uint32_t(std::clamp<int32_t>(MaxParticles, 1, int32_t(PARTICLE_MAX_PER_SYSTEM)));
        Out.SpawnRate        = bEmitting ? std::max(SpawnRate, 0.0f) : 0.0f;
        Out.Lifetime         = std::max(Lifetime, 0.001f);
        Out.LifetimeVariance = std::clamp(LifetimeVariance, 0.0f, 1.0f);
        Out.EmitterRadius    = std::max(EmitterRadius, 0.0f);
        ToFloats(Velocity, Out.Velocity);
        Out.VelocitySpread   = std::max(VelocitySpread, 0.0f);
        ToFloats(Acceleration, Out.Acceleration);
        Out.Drag             = std::max(Drag, 0.0f);
        Out.Turbulence       = std::max(Turbulence, 0.0f);
        Out.StartSize        = std::max(StartSize, 0.0f);
        Out.EndSize          = std::max(EndSize, 0.0f);
        ToFloats(StartColor, Out.StartColor);
        ToFloats(EndColor, Out.EndColor);
        Out.ColorCurve       = std::clamp(ColorCurve, 0.05f, 20.0f);
        Out.FadeIn           = std::clamp(FadeIn, 0.0f, 1.0f);
        Out.FadeOut          = std::clamp(FadeOut, 0.0f, 1.0f);
        Out.bFaceCamera      = bFaceCamera != 0;
        Out.Seed             = uint32_t(Seed);
        return Out;
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(ParticleSystemComponent, 1)
    {
        Ar.Flag("Emitting",          C.bEmitting);
        Ar.Field("Mesh",             C.Mesh);
        Ar.Field("Material",         C.Material);
        Ar.Flag("FaceCamera",        C.bFaceCamera);
        Ar.Field("MaxParticles",     C.MaxParticles);
        Ar.Field("SpawnRate",        C.SpawnRate);
        Ar.Field("Lifetime",         C.Lifetime);
        Ar.Field("LifetimeVariance", C.LifetimeVariance);
        Ar.Field("EmitterRadius",    C.EmitterRadius);
        Ar.Field("Velocity",         C.Velocity);
        Ar.Field("VelocitySpread",   C.VelocitySpread);
        Ar.Field("Acceleration",     C.Acceleration);
        Ar.Field("Drag",             C.Drag);
        Ar.Field("Turbulence",       C.Turbulence);
        Ar.Field("StartSize",        C.StartSize);
        Ar.Field("EndSize",          C.EndSize);
        Ar.Field("StartColor",       C.StartColor);
        Ar.Field("EndColor",         C.EndColor);
        Ar.Field("ColorCurve",       C.ColorCurve);
        Ar.Field("FadeIn",           C.FadeIn);
        Ar.Field("FadeOut",          C.FadeOut);
        Ar.Field("Seed",             C.Seed);
    }

    /* Lua sees every setting by the same names (e:GetComponent("Particle System"):SetSpawnRate(10)). */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(ParticleSystemComponent)
    {
        Bind.Flag("Emitting",          &ParticleSystemComponent::bEmitting);
        Bind.Field("Mesh",             &ParticleSystemComponent::Mesh);
        Bind.Field("Material",         &ParticleSystemComponent::Material);
        Bind.Flag("FaceCamera",        &ParticleSystemComponent::bFaceCamera);
        Bind.Field("MaxParticles",     &ParticleSystemComponent::MaxParticles);
        Bind.Field("SpawnRate",        &ParticleSystemComponent::SpawnRate);
        Bind.Field("Lifetime",         &ParticleSystemComponent::Lifetime);
        Bind.Field("LifetimeVariance", &ParticleSystemComponent::LifetimeVariance);
        Bind.Field("EmitterRadius",    &ParticleSystemComponent::EmitterRadius);
        Bind.Field("Velocity",         &ParticleSystemComponent::Velocity);
        Bind.Field("VelocitySpread",   &ParticleSystemComponent::VelocitySpread);
        Bind.Field("Acceleration",     &ParticleSystemComponent::Acceleration);
        Bind.Field("Drag",             &ParticleSystemComponent::Drag);
        Bind.Field("Turbulence",       &ParticleSystemComponent::Turbulence);
        Bind.Field("StartSize",        &ParticleSystemComponent::StartSize);
        Bind.Field("EndSize",          &ParticleSystemComponent::EndSize);
        Bind.Field("StartColor",       &ParticleSystemComponent::StartColor);
        Bind.Field("EndColor",         &ParticleSystemComponent::EndColor);
        Bind.Field("ColorCurve",       &ParticleSystemComponent::ColorCurve);
        Bind.Field("FadeIn",           &ParticleSystemComponent::FadeIn);
        Bind.Field("FadeOut",          &ParticleSystemComponent::FadeOut);
        Bind.Field("Seed",             &ParticleSystemComponent::Seed);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(ParticleSystemComponent)
    {
        Props.Category("Rendering");
        Props.Field("Mesh",     &ParticleSystemComponent::Mesh, GameCore::PF_AssetMesh);
        Props.Field("Material", &ParticleSystemComponent::Material, GameCore::PF_AssetMaterial);
        Props.Flag("Face Camera", &ParticleSystemComponent::bFaceCamera);

        Props.Category("Emission");
        Props.Flag("Emitting",          &ParticleSystemComponent::bEmitting);
        Props.Field("Max Particles",    &ParticleSystemComponent::MaxParticles);
        Props.Field("Spawn Rate",       &ParticleSystemComponent::SpawnRate);
        Props.Field("Lifetime",         &ParticleSystemComponent::Lifetime);
        Props.Field("Lifetime Variance", &ParticleSystemComponent::LifetimeVariance);
        Props.Field("Emitter Radius",   &ParticleSystemComponent::EmitterRadius);
        Props.Field("Seed",             &ParticleSystemComponent::Seed);

        Props.Category("Motion");
        Props.Field("Velocity",         &ParticleSystemComponent::Velocity);
        Props.Field("Velocity Spread",  &ParticleSystemComponent::VelocitySpread);
        Props.Field("Acceleration",     &ParticleSystemComponent::Acceleration);
        Props.Field("Drag",             &ParticleSystemComponent::Drag);
        Props.Field("Turbulence",       &ParticleSystemComponent::Turbulence);

        Props.Category("Over Life");
        Props.Field("Start Size",       &ParticleSystemComponent::StartSize);
        Props.Field("End Size",         &ParticleSystemComponent::EndSize);
        Props.Field("Start Color",      &ParticleSystemComponent::StartColor);
        Props.Field("End Color",        &ParticleSystemComponent::EndColor);
        Props.Field("Color Curve",      &ParticleSystemComponent::ColorCurve);
        Props.Field("Fade In",          &ParticleSystemComponent::FadeIn);
        Props.Field("Fade Out",         &ParticleSystemComponent::FadeOut);
    }
#endif
}
