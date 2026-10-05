#ifndef RENDERCORE_PARTICLESYSTEMCOMPONENT_H
#define RENDERCORE_PARTICLESYSTEMCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Public/RenderCore/ParticleTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

namespace RenderCore
{
    /*
     * "Particle System". Runs the particle system asset named by System
     * (*.nmparticle, ParticleSystemAsset.h) from its entity: each of the
     * asset's emitters simulated on the GPU (compute shaders;
     * RENDERPIPELINE.md, "Particles") and drawn as instances of its mesh with
     * its material. An empty System runs one emitter at its defaults.
     * Particles live in world space: moving the entity moves where new ones
     * are born, not the ones already out.
     *
     * The asset says what the effect is; the component, how this one runs:
     * whether it emits (off and on again restarts a burst emitter), a tint on
     * every colour, its spawn rate and clock scaled, a seed so two of the same
     * system differ, and where its beams end, if a script aims them. An edit to the
     * asset (a save in the Particle Editor) reaches every component naming it
     * on the next frame.
     *
     * Runs in the editor too: the simulation follows the render thread's
     * clock, not the World's. Where the backend has no compute shaders
     * (OpenGL, D3D12 and Metal so far) it draws nothing and says so once.
     */
    struct RENDERCORE_API ParticleSystemComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        char              System[RENDER_PARTICLE_SYSTEM_PATH_MAX] = {};   // "": one emitter at its defaults
        uint8_t           bEmitting      = 1;      // off: no new particles; those out live on
        EngineCore::Color Tint{255, 255, 255, 255};   // times every emitter's colours, alpha too
        float             SpawnRateScale = 1.0f;   // times every emitter's spawn rate
        float             TimeScale      = 1.0f;   // how fast its particles live: 0 freezes them
        int32_t           Seed           = 0;      // added to each emitter's own

        /* Beams end at BeamTarget, in world space, instead of their own
         * BeamEnd: a laser aimed at something by a script. */
        uint8_t             bUseBeamTarget = 0;
        EngineCore::Vector3 BeamTarget{0.0, 0.0, 0.0};

        /* Emitter InEmitter of InSystem as this component runs it: its
         * settings with the overrides above applied. */
        ParticleEmitterSettings ToSettings(const ParticleEmitterDesc& InEmitter, uint32_t InEmitterIndex) const;
    };

    /* ParticleSystemComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetParticleSystemComponentFactory();
}

#endif //RENDERCORE_PARTICLESYSTEMCOMPONENT_H
