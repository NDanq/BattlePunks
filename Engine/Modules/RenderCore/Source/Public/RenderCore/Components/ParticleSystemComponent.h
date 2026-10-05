#ifndef RENDERCORE_PARTICLESYSTEMCOMPONENT_H
#define RENDERCORE_PARTICLESYSTEMCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/ParticleTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

namespace RenderCore
{
    /* What an empty Mesh draws: a 1 x 1 quad facing -Z, turned to the camera. */
    constexpr const char* PARTICLE_DEFAULT_MESH = "Engine/Content/Meshes/Quad.nmmesh";

    /*
     * "Particle System". Emits particles from its entity's position and
     * simulates them on the GPU (compute shaders; RENDERPIPELINE.md,
     * "Particles"), drawing each as an instance of Mesh with Material.
     * Particles live in world space: moving the entity moves where new ones
     * are born, not the ones already out.
     *
     * Each particle is born within EmitterRadius, moving at Velocity plus up
     * to VelocitySpread in a random direction; Acceleration (buoyancy,
     * gravity), Drag and Turbulence act on it as it ages; it lives Lifetime
     * seconds (less up to LifetimeVariance of it). Over its life its size
     * goes from StartSize to EndSize and its colour from StartColor to
     * EndColor (sooner with a ColorCurve under 1), fading in and out over
     * FadeIn and FadeOut of it. The colour reaches the material as its
     * Vertex Color (rgb and a): a translucent material that uses it shades
     * each particle by age.
     *
     * Runs in the editor too: the simulation follows the render thread's
     * clock, not the World's. Where the backend has no compute shaders
     * (OpenGL, D3D12 and Metal so far) it draws nothing and says so once.
     */
    struct RENDERCORE_API ParticleSystemComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        uint8_t           bEmitting        = 1;     // off: no new particles; those out live on
        char              Mesh[RENDER_MESH_PATH_MAX]         = {};   // "": PARTICLE_DEFAULT_MESH
        char              Material[RENDER_MATERIAL_PATH_MAX] = {};   // "": DefaultLit
        uint8_t           bFaceCamera      = 1;

        int32_t           MaxParticles     = 256;   // 1..PARTICLE_MAX_PER_SYSTEM
        float             SpawnRate        = 32.0f; // a second
        float             Lifetime         = 2.0f;  // seconds
        float             LifetimeVariance = 0.25f;
        float             EmitterRadius    = 0.0f;

        EngineCore::Vector3 Velocity{0.0, 100.0, 0.0};
        float             VelocitySpread   = 0.0f;
        EngineCore::Vector3 Acceleration{0.0, 0.0, 0.0};
        float             Drag             = 0.0f;
        float             Turbulence       = 0.0f;

        float             StartSize        = 10.0f;
        float             EndSize          = 10.0f;
        EngineCore::Color StartColor{255, 255, 255, 255};
        EngineCore::Color EndColor{255, 255, 255, 0};
        float             ColorCurve       = 1.0f;  // age^ColorCurve picks the colour: under 1, EndColor sooner
        float             FadeIn           = 0.1f;
        float             FadeOut          = 0.3f;
        int32_t           Seed             = 0;

        /* As RenderContext::SimulateParticles takes it. */
        ParticleEmitterSettings ToSettings() const;
    };

    /* ParticleSystemComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetParticleSystemComponentFactory();
}

#endif //RENDERCORE_PARTICLESYSTEMCOMPONENT_H
