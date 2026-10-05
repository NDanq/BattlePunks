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
 * and material: sprites (billboards), meshes, a ribbon (a trail through its
 * particles) or a beam (a strip to a point), spawning continuously or in bursts. A Particle System component names one by its runtime path
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
 *         Type Sprite
 *         Material Content/Materials/FlareSmoke.material.nmgraph
 *         SpawnRate 70
 *         Velocity 10,120,0
 *         StartColor 225,38,22,150
 *         ...
 *
 * A key is its first word and its value the rest of the line. Vectors are
 * three numbers, colours four bytes (0-255, sRGB), switches true/false,
 * choices their names (Type Ribbon, Billboard VelocityAligned), keys a
 * list of time:value (SizeKeys 0:10 0.5:40 1:0, ColorKeys 0:255,200,80,255 1:40,0,0,0).
 * A setting left out keeps its default and an unknown key is skipped, so
 * an older engine reads a newer file. "Modules Shape Forces" names its
 * optional modules; an emitter with no Modules line has those its
 * settings use (InferModules). Written deterministically: every
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

    /* The most keys a colour gradient or size curve has. */
    constexpr uint32_t PARTICLE_CURVE_KEYS_MAX = 8;

    /*
     * An emitter's optional modules: what the Particle Editor adds and
     * removes as the Details panel adds and removes components. One left out
     * has no effect, whatever its settings hold: no Shape is a point, no
     * Velocity no initial speed, no Forces no acceleration, drag or swirl, no
     * Rotation no spin. Rendering, Emission, Size and Color (and a beam's
     * Beam) are always there.
     */
    enum EParticleModule : uint8_t
    {
        PEM_Shape    = 1 << 0,   // Shape, its size, On Surface, Outward Speed
        PEM_Velocity = 1 << 1,   // Velocity, Velocity Spread
        PEM_Forces   = 1 << 2,   // Acceleration, Drag, Turbulence
        PEM_Rotation = 1 << 3,   // Random Rotation, Rotation Rate (sprites and meshes)
        PEM_All      = PEM_Shape | PEM_Velocity | PEM_Forces | PEM_Rotation,
    };

    /* A module's name, as the file and the editor give it ("Forces"); null for a bit that is not one. */
    RENDERCORE_API const char* GetParticleModuleName(uint8_t InModule);

    /* A colour gradient's key: at Time (0 birth, 1 death), Color (alpha too). */
    struct ParticleColorKey
    {
        float             Time  = 0.0f;
        EngineCore::Color Color{255, 255, 255, 255};
    };

    /* A size curve's key: at Time, Value world units across. */
    struct ParticleSizeKey
    {
        float Time  = 0.0f;
        float Value = 10.0f;
    };
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
        uint8_t             Type        = PT_Sprite;               // EParticleType
        uint8_t             Modules     = PEM_Velocity;            // EParticleModule bits: which optional modules it has
        char                Mesh[RENDER_MESH_PATH_MAX]         = {};   // "": a Sprite's quad, a Mesh's box (GetMeshPath)
        char                Material[RENDER_MATERIAL_PATH_MAX] = {};   // "": DefaultLit
        EngineCore::Vector3 Offset{0.0, 0.0, 0.0};

        /* Sprite and Mesh: how each is turned. */
        uint8_t             Billboard        = PB_FaceCamera;      // EParticleBillboard (Sprite)
        float               VelocityStretch  = 0.05f;              // Velocity Aligned: seconds of travel added to its length
        uint8_t             MeshOrientation  = PMO_RandomSpin;     // EParticleMeshOrientation (Mesh)
        uint8_t             bRandomRotation  = 0;                  // each starts at a random angle and spins either way
        float               RotationRate     = 0.0f;               // degrees a second

        uint8_t             SpawnMode        = PSM_Continuous;     // EParticleSpawnMode
        int32_t             MaxParticles     = 256;     // 1..PARTICLE_MAX_PER_SYSTEM; the oldest are replaced

        /* Where each is born (EParticleShape), in the entity's own axes. */
        uint8_t             Shape            = PS_Sphere;
        uint8_t             bShapeSurface    = 0;       // on its surface only
        EngineCore::Vector3 BoxSize{100.0, 100.0, 100.0};
        float               ConeAngle        = 25.0f;   // degrees from +Y
        float               RingWidth        = 0.0f;
        float               ShapeSpeed       = 0.0f;    // plus this outward from the shape
        float               SpawnRate        = 32.0f;   // a second (Continuous)
        int32_t             BurstCount       = 32;      // a burst (Burst)
        float               BurstInterval    = 0.0f;    // seconds between bursts; 0: one
        int32_t             BurstCycles      = 1;       // bursts in all, with an interval; 0: no end
        float               Lifetime         = 2.0f;    // seconds
        float               LifetimeVariance = 0.25f;
        float               EmitterRadius    = 0.0f;
        int32_t             Seed             = 0;

        EngineCore::Vector3 Velocity{0.0, 100.0, 0.0};
        float               VelocitySpread   = 0.0f;
        EngineCore::Vector3 Acceleration{0.0, 0.0, 0.0};
        float               Drag             = 0.0f;
        float               Turbulence       = 0.0f;

        /* Beam: from the emitter to BeamEnd (in the entity's own axes, as
         * Offset), through BeamSegments points that stray up to BeamNoise. */
        EngineCore::Vector3 BeamEnd{0.0, 200.0, 0.0};
        int32_t             BeamSegments     = 16;      // 1..PARTICLE_BEAM_SEGMENTS_MAX
        float               BeamNoise        = 0.0f;
        float               BeamNoiseSpeed   = 4.0f;

        /* Size over life: Start to End, or through SizeKeys (SizeMode Keys). A ribbon's or beam's: its width. */
        uint8_t             SizeMode         = POL_StartEnd;
        float               StartSize        = 10.0f;
        float               EndSize          = 10.0f;
        ParticleSizeKey     SizeKeys[PARTICLE_CURVE_KEYS_MAX] = {};
        int32_t             SizeKeyCount     = 0;

        /* Colour (and alpha) over life: Start to End by ColorCurve, faded in
         * and out; or through ColorKeys (ColorMode Keys: a gradient). */
        uint8_t             ColorMode        = POL_StartEnd;
        EngineCore::Color   StartColor{255, 255, 255, 255};
        EngineCore::Color   EndColor{255, 255, 255, 0};
        float               ColorCurve       = 1.0f;    // age^ColorCurve picks the colour: under 1, EndColor sooner
        float               FadeIn           = 0.1f;
        float               FadeOut          = 0.3f;
        ParticleColorKey    ColorKeys[PARTICLE_CURVE_KEYS_MAX] = {};
        int32_t             ColorKeyCount    = 0;

        bool HasModule(const uint8_t InModule) const { return (Modules & InModule) != 0; }

        /* The modules its settings use, from what differs from a fresh
         * emitter's (none, for a beam): what a file written before modules had. */
        RENDERCORE_API uint8_t InferModules() const;

        /* As RenderContext::SimulateParticles takes it: clamped to what the
         * GPU can run, colours to 0-1, modules left out as if at rest.
         * BeamEnd is left in the entity's axes: the component places it. */
        RENDERCORE_API ParticleEmitterSettings ToSettings() const;

        /* How many particles it holds once it has run a while: the spawn
         * rate times the mean lifetime (a burst's count, for a burst that
         * outlives its interval), at most MaxParticles; a beam's points. */
        RENDERCORE_API float GetSteadyStateCount() const;

        /* As far as one of its particles can get from the emitter in its
         * life, plus its size: what culling pads the entity by. */
        RENDERCORE_API float GetReach() const;

        /* Its colour (0-1) and size at InAge (0 birth, 1 death), as the GPU samples them. */
        RENDERCORE_API void Evaluate(float InAge, float (&OutColor)[4], float& OutSize) const;

        /* Keys to stand for Start to End when a gradient or curve is first chosen:
         * the colour with its fades, the size straight. Sorted, at most PARTICLE_CURVE_KEYS_MAX. */
        RENDERCORE_API void SeedColorKeys();
        RENDERCORE_API void SeedSizeKeys();

        /* The mesh each particle draws: Mesh, or the quad (Sprite) or box (Mesh) when it is empty. */
        RENDERCORE_API const char* GetMeshPath() const;
    };

    /* Names as the text and the editor give them, by enum value. */
    RENDERCORE_API const char* GetParticleTypeName(uint8_t InType);

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
