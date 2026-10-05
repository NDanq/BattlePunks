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
#include <memory>
#include <string>
#include <vector>

/* ParticleSystemComponent: registration, its proxy, and what it exposes to
 * saves, Lua and the editor. */

namespace RenderCore
{
    namespace
    {
        /*
         * The render-thread side: an emitter on the GPU per emitter of its
         * system, simulated once a frame (Simulate) and drawn from every view
         * that sees it (Draw). The system is looked up every frame, so a new
         * version of the asset (ParticleSystemCache) is picked up at once; an
         * emitter keeps its particles through an edit unless its Max
         * Particles changes. Never static: its draws change every frame on
         * their own.
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
                TimeScale = std::max(C.TimeScale, 0.0f);

                std::shared_ptr<const ParticleSystemDesc> System = C.System[0] != '\0' ? ParticleSystemCache::Find(C.System)
                                                                                       : GetDefaultParticleSystem();
                if(!System)
                {
                    if(MissingPath != C.System)
                    {
                        MissingPath = C.System;
                        RENDERCORE_LOG_WARN("particle system '" + MissingPath + "' cannot be loaded: it draws nothing")
                    }
                    static const std::shared_ptr<const ParticleSystemDesc> Empty = std::make_shared<const ParticleSystemDesc>();
                    System = Empty;
                }

                /* An emitter removed from the asset frees its GPU buffers in the next Simulate. */
                const size_t Count = std::min<size_t>(System->Emitters.size(), PARTICLE_SYSTEM_EMITTERS_MAX);
                while(Emitters.size() > Count)
                {
                    Retired.push_back(Emitters.back().Gpu);
                    Emitters.pop_back();
                }
                Emitters.resize(Count);

                float Reach = 0.0f;
                for(size_t Index = 0; Index < Count; ++Index)
                {
                    const ParticleEmitterDesc& Desc = System->Emitters[Index];
                    EmitterRun& Run = Emitters[Index];
                    Run.bEnabled = Desc.bEnabled != 0;
                    Run.Settings = C.ToSettings(Desc, uint32_t(Index));
                    Run.Offset   = Desc.Offset;
                    Run.Mesh.assign(Desc.Mesh[0] != '\0' ? Desc.Mesh : PARTICLE_DEFAULT_MESH);
                    Run.Material.assign(Desc.Material);
                    if(Run.bEnabled)
                    {
                        const double X = Desc.Offset.GetX(), Y = Desc.Offset.GetY(), Z = Desc.Offset.GetZ();
                        Reach = std::max(Reach, float(std::sqrt(X * X + Y * Y + Z * Z)) + Desc.GetReach());
                    }
                }

                /* As far as a particle can get from the entity in its life, so culling never clips the plume. */
                SetLocalBounds(GameCore::AABB(EngineCore::Vector3(-Reach, -Reach, -Reach), EngineCore::Vector3(Reach, Reach, Reach)));
            }

            void Simulate(RenderContext& InContext) override
            {
                for(ParticleEmitterGpu* Each : Retired)
                {
                    InContext.ReleaseParticleEmitter(Each);
                }
                Retired.clear();

                const float Delta = InContext.GetDeltaSeconds() * TimeScale;
                for(EmitterRun& Run : Emitters)
                {
                    if(!Run.bEnabled)
                    {
                        InContext.ReleaseParticleEmitter(Run.Gpu);
                        Run.Gpu      = nullptr;
                        Run.Capacity = 0;
                        continue;
                    }
                    if(!Run.Gpu || Run.Capacity != Run.Settings.MaxParticles)
                    {
                        InContext.ReleaseParticleEmitter(Run.Gpu);
                        Run.Gpu      = InContext.CreateParticleEmitter(Run.Settings.MaxParticles);
                        Run.Capacity = Run.Settings.MaxParticles;
                    }
                    if(!Run.Gpu)
                    {
                        if(!bWarned)
                        {
                            bWarned = true;
                            RENDERCORE_LOG_WARN(InContext.SupportsParticles()
                                                ? std::string("a particle system could not make its GPU buffers: it draws nothing")
                                                : std::string("this backend runs no compute shaders: particle systems draw nothing"));
                        }
                        continue;
                    }
                    InContext.SimulateParticles(*Run.Gpu, Run.Settings, EmitterPosition(Run), Delta);
                }
            }

            void Draw(RenderContext& InContext) override
            {
                for(const EmitterRun& Run : Emitters)
                {
                    if(Run.Gpu)
                    {
                        InContext.DrawParticles(*Run.Gpu, EmitterPosition(Run), Run.Mesh.c_str(), Run.Material.c_str());
                    }
                }
            }

            void ReleaseRenderResources(RenderContext& InContext) override
            {
                for(EmitterRun& Run : Emitters)
                {
                    InContext.ReleaseParticleEmitter(Run.Gpu);
                    Run.Gpu = nullptr;
                }
                for(ParticleEmitterGpu* Each : Retired)
                {
                    InContext.ReleaseParticleEmitter(Each);
                }
                Retired.clear();
            }

        private:
            struct EmitterRun
            {
                ParticleEmitterSettings Settings;
                std::string             Mesh;
                std::string             Material;
                EngineCore::Vector3     Offset;
                bool                    bEnabled = true;
                ParticleEmitterGpu*     Gpu      = nullptr;
                uint32_t                Capacity = 0;
            };

            /* The entity's position, moved by the emitter's Offset in the entity's own axes (and scale). */
            EngineCore::Vector3 EmitterPosition(const EmitterRun& InRun) const
            {
                return GetTransform().Position + GetLocalToWorld() * InRun.Offset;
            }

            std::vector<EmitterRun>          Emitters;
            std::vector<ParticleEmitterGpu*> Retired;
            std::string                      MissingPath;
            float                            TimeScale = 1.0f;
            bool                             bWarned   = false;
        };
    }

    RENDERCORE_DEFINE_PROXY_COMPONENT(ParticleSystemComponent, "Particle System", ParticleSystemProxy)

    CreateProxyFn GetParticleSystemComponentFactory()
    {
        return &ParticleSystemComponent_CreateProxy;
    }

    ParticleEmitterSettings ParticleSystemComponent::ToSettings(const ParticleEmitterDesc& InEmitter, const uint32_t InEmitterIndex) const
    {
        ParticleEmitterSettings Out = InEmitter.ToSettings();
        Out.SpawnRate = bEmitting ? Out.SpawnRate * std::max(SpawnRateScale, 0.0f) : 0.0f;
        const float TintChannels[4] = { Tint.Rf(), Tint.Gf(), Tint.Bf(), Tint.Af() };
        for(int Channel = 0; Channel < 4; ++Channel)
        {
            Out.StartColor[Channel] *= TintChannels[Channel];
            Out.EndColor[Channel]   *= TintChannels[Channel];
        }
        /* Two emitters of one system, or two systems with one seed, never spawn in step. */
        Out.Seed = Out.Seed + uint32_t(Seed) * 0x9E3779B9u + InEmitterIndex * 0x85EBCA6Bu;
        return Out;
    }

    /* Version 2 names a system; version 1 held one emitter's settings inline,
     * which are read past (its Emitting and Seed kept) -- re-make such a
     * component's effect as a .nmparticle. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(ParticleSystemComponent, 2)
    {
        if(Ar.IsLoading() && Ar.GetVersion() < 2)
        {
            ParticleEmitterDesc Legacy;
            Ar.Flag("Emitting",          C.bEmitting);
            Ar.Field("Mesh",             Legacy.Mesh);
            Ar.Field("Material",         Legacy.Material);
            Ar.Flag("FaceCamera",        Legacy.bFaceCamera);
            Ar.Field("MaxParticles",     Legacy.MaxParticles);
            Ar.Field("SpawnRate",        Legacy.SpawnRate);
            Ar.Field("Lifetime",         Legacy.Lifetime);
            Ar.Field("LifetimeVariance", Legacy.LifetimeVariance);
            Ar.Field("EmitterRadius",    Legacy.EmitterRadius);
            Ar.Field("Velocity",         Legacy.Velocity);
            Ar.Field("VelocitySpread",   Legacy.VelocitySpread);
            Ar.Field("Acceleration",     Legacy.Acceleration);
            Ar.Field("Drag",             Legacy.Drag);
            Ar.Field("Turbulence",       Legacy.Turbulence);
            Ar.Field("StartSize",        Legacy.StartSize);
            Ar.Field("EndSize",          Legacy.EndSize);
            Ar.Field("StartColor",       Legacy.StartColor);
            Ar.Field("EndColor",         Legacy.EndColor);
            Ar.Field("ColorCurve",       Legacy.ColorCurve);
            Ar.Field("FadeIn",           Legacy.FadeIn);
            Ar.Field("FadeOut",          Legacy.FadeOut);
            Ar.Field("Seed",             C.Seed);
            RENDERCORE_LOG_WARN("a Particle System saved before particle system assets: its settings are dropped -- "
                                "name a .nmparticle in its System")
            return;
        }
        Ar.Field("System",         C.System);
        Ar.Flag("Emitting",        C.bEmitting);
        Ar.Field("Tint",           C.Tint);
        Ar.Field("SpawnRateScale", C.SpawnRateScale);
        Ar.Field("TimeScale",      C.TimeScale);
        Ar.Field("Seed",           C.Seed);
    }

    /* Lua sees the same names (e:GetComponent("Particle System"):SetEmitting(false)). */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(ParticleSystemComponent)
    {
        Bind.Field("System",         &ParticleSystemComponent::System);
        Bind.Flag("Emitting",        &ParticleSystemComponent::bEmitting);
        Bind.Field("Tint",           &ParticleSystemComponent::Tint);
        Bind.Field("SpawnRateScale", &ParticleSystemComponent::SpawnRateScale);
        Bind.Field("TimeScale",      &ParticleSystemComponent::TimeScale);
        Bind.Field("Seed",           &ParticleSystemComponent::Seed);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(ParticleSystemComponent)
    {
        Props.Field("System",           &ParticleSystemComponent::System, GameCore::PF_AssetParticleSystem);
        Props.Flag("Emitting",          &ParticleSystemComponent::bEmitting);
        Props.Field("Tint",             &ParticleSystemComponent::Tint);
        Props.Field("Spawn Rate Scale", &ParticleSystemComponent::SpawnRateScale);
        Props.Field("Time Scale",       &ParticleSystemComponent::TimeScale);
        Props.Field("Seed",             &ParticleSystemComponent::Seed);
    }
#endif
}
