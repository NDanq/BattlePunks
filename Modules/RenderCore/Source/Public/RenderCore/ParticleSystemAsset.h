#ifndef RENDERCORE_PARTICLESYSTEMASSET_H
#define RENDERCORE_PARTICLESYSTEMASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/ParticleTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * A particle system asset (*.nmparticle; RENDERPIPELINE.md, "Particles"):
 * one or more emitters, each its own GPU simulation drawn with its own mesh
 * and material. A Particle System component names one by its runtime path
 * ("Content/Particles/Smoke.nmparticle"); the editor's Particle Editor makes
 * and edits them.
 *
 * TEXT in every build, read as it is -- no cook, no XML -- a line per
 * setting, an emitter's under its "Emitter <name>" line:
 *
 *     NimbleParticleSystem 1
 *
 *     Emitter Smoke
 *         Enabled true
 *         Material Content/Materials/FlareSmoke.material.nmgraph
 *         SpawnRate 70
 *         Velocity 10,120,0
 *         StartColor 225,38,22,150
 *         ...
 *
 * A key is its first word and its value the rest of the line. Vectors are
 * three numbers, colours four bytes (0-255, sRGB), switches true/false.
 * A setting left out keeps its default and an unknown key is skipped, so
 * an older engine reads a newer file. Written deterministically: every
 * setting, in a fixed order, numbers in their shortest round-trip form.
 */
namespace RenderCore
{
    enum : size_t
    {
        PARTICLE_EMITTER_NAME_MAX     = 48,    // including the NUL
        PARTICLE_SYSTEM_EMITTERS_MAX  = 8,
        RENDER_PARTICLE_SYSTEM_PATH_MAX = 128, // including the NUL
    };

    constexpr const char* PARTICLE_SYSTEM_EXTENSION = ".nmparticle";
    constexpr uint32_t    PARTICLE_SYSTEM_FORMAT    = 1;

    /*
     * One emitter, as the file and the editor hold it. POD with the field
     * types components use (Vector3, Color, fixed strings, 0/1 flags), so the
     * editor shows it with the Details panel's own property widgets.
     *
     * Each particle is born within EmitterRadius of the emitter -- the
     * component's entity, moved by Offset (in the entity's own axes) --
     * moving at Velocity plus up to VelocitySpread in a random direction;
     * Acceleration (buoyancy, gravity), Drag and Turbulence act on it as it
     * ages; it lives Lifetime seconds (less up to LifetimeVariance of it).
     * Over its life its size goes from StartSize to EndSize and its colour
     * from StartColor to EndColor (sooner with a ColorCurve under 1), fading
     * in and out over FadeIn and FadeOut of it. Vectors are world space.
     */
    struct ParticleEmitterDesc
    {
        char                Name[PARTICLE_EMITTER_NAME_MAX] = "Emitter";
        uint8_t             bEnabled    = 1;                       // off: neither simulated nor drawn
        char                Mesh[RENDER_MESH_PATH_MAX]         = {};   // "": PARTICLE_DEFAULT_MESH
        char                Material[RENDER_MATERIAL_PATH_MAX] = {};   // "": DefaultLit
        uint8_t             bFaceCamera = 1;
        EngineCore::Vector3 Offset{0.0, 0.0, 0.0};

        int32_t             MaxParticles     = 256;     // 1..PARTICLE_MAX_PER_SYSTEM; the oldest are replaced
        float               SpawnRate        = 32.0f;   // a second
        float               Lifetime         = 2.0f;    // seconds
        float               LifetimeVariance = 0.25f;
        float               EmitterRadius    = 0.0f;
        int32_t             Seed             = 0;

        EngineCore::Vector3 Velocity{0.0, 100.0, 0.0};
        float               VelocitySpread   = 0.0f;
        EngineCore::Vector3 Acceleration{0.0, 0.0, 0.0};
        float               Drag             = 0.0f;
        float               Turbulence       = 0.0f;

        float               StartSize        = 10.0f;
        float               EndSize          = 10.0f;
        EngineCore::Color   StartColor{255, 255, 255, 255};
        EngineCore::Color   EndColor{255, 255, 255, 0};
        float               ColorCurve       = 1.0f;    // age^ColorCurve picks the colour: under 1, EndColor sooner
        float               FadeIn           = 0.1f;
        float               FadeOut          = 0.3f;

        /* As RenderContext::SimulateParticles takes it: clamped to what the
         * GPU can run, colours to 0-1. */
        RENDERCORE_API ParticleEmitterSettings ToSettings() const;

        /* How many particles it holds once it has run a while: the spawn
         * rate times the mean lifetime, at most MaxParticles. */
        RENDERCORE_API float GetSteadyStateCount() const;

        /* As far as one of its particles can get from the emitter in its
         * life, plus its size: what culling pads the entity by. */
        RENDERCORE_API float GetReach() const;
    };

    struct ParticleSystemDesc
    {
        std::vector<ParticleEmitterDesc> Emitters;   // at most PARTICLE_SYSTEM_EMITTERS_MAX
    };

    /* .nmparticle text into OutSystem (cleared first). False, with OutError
     * naming the line, on one it cannot read. */
    RENDERCORE_API bool ParseParticleSystem(const std::string& InText, ParticleSystemDesc& OutSystem, std::string& OutError);

    /* InSystem as .nmparticle text: the same system always the same bytes. */
    RENDERCORE_API std::string WriteParticleSystem(const ParticleSystemDesc& InSystem);

    /* Through the file provider, by runtime path. False (warned) if it cannot be read. */
    RENDERCORE_API bool LoadParticleSystemFile(const std::string& InPath, ParticleSystemDesc& OutSystem);

#if WITH_EDITOR
    /* To a file on disk, written beside and renamed over. */
    RENDERCORE_API bool SaveParticleSystemFile(const std::string& InFilePath, const ParticleSystemDesc& InSystem);
#endif

    /* What a component naming no system emits: one emitter at its defaults. */
    RENDERCORE_API std::shared_ptr<const ParticleSystemDesc> GetDefaultParticleSystem();

    /*
     * Systems by runtime path, read on first use; null (warned once) if one
     * cannot be. Shared by the game and render threads, thread-safe. A proxy
     * notices a new version by its pointer, so Invalidate (the editor, on
     * save) or Store reaches every system using it on the next frame.
     */
    class RENDERCORE_API ParticleSystemCache
    {
    public:
        static std::shared_ptr<const ParticleSystemDesc> Find(const std::string& InPath);

        /* InSystem under InPath until it is invalidated: an unsaved edit
         * (the Particle Editor's preview), or a system made by code. */
        static void Store(const std::string& InPath, std::shared_ptr<const ParticleSystemDesc> InSystem);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);
    };
}

#endif //RENDERCORE_PARTICLESYSTEMASSET_H
