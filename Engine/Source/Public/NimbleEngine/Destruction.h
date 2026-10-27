#ifndef NIMBLEENGINE_DESTRUCTION_H
#define NIMBLEENGINE_DESTRUCTION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace GameCore { class World; }

/*
 * Destruction: things that break apart by what they are made of.
 *
 * Give an entity with a Render mesh a Destructible component and name its
 * material: Concrete, Wood, Glass, Stone or Metal, or one a project adds
 * under [Destruction_<Name>]. When it is hit hard enough it breaks into
 * Voronoi pieces (RenderCore's MeshFracture.h) cut in that material's
 * pattern -- chunks, long splinters, shards -- each a dynamic Rigid Body
 * that keeps the whole's velocity and is thrown from the hit. The inside of
 * every cut draws with the material's interior material.
 *
 * WHAT BREAKS IT. Physics impacts (JoltPhysics' PhysicsWorld::GetImpacts)
 * on its body: one at or over Strength breaks it at once; smaller ones add
 * to Damage until it reaches Health. Scripts can too: destructible:Break(
 * point, impulse), :ApplyDamage(point, impulse), and Destruction.RadialDamage
 * (centre, radius, impulse) for an explosion.
 *
 * FRACTURE. Runtime (the default) cuts the mesh as it breaks, around the
 * hit; the pieces live in memory (MeshAssetCache, "@fracture/..."), shared
 * by every break that cut the same mesh the same way. Baked uses pieces the
 * editor cut and saved (`destruction bake` in its console): a .nmfrac file
 * next to a folder of .nmmesh pieces.
 *
 * STRUCTURES. Structural, on an entity whose body is Static (or that has
 * none), breaks only around the hit: the pieces near it fall and the rest
 * stay, Static. Pieces resting on its base (Anchored) hold the others up;
 * any group a break cuts off from them falls. Each later hit on what is
 * left breaks more of it around that hit.
 *
 * DEBRIS. Fallen pieces live for the material's DebrisLifetime (sooner once
 * asleep), then shrink away. [Destruction] MaxDebris caps them: the oldest
 * go first. Pieces smaller than CosmeticVolume go on DebrisLayer, colliding
 * only with DebrisMask.
 *
 * NETWORKING. A server breaks things and tells every client
 * (MulticastBreak, with the seed it cut with), so each cuts the same pieces;
 * the pieces then fall on each machine by itself -- cosmetic, never sent.
 * A client never breaks a networked entity itself. Damage and bBroken
 * replicate, so one that joins later sees it broken.
 *
 * Without the JoltPhysics module nothing is hit: Break still works, and the
 * pieces stay where they were cut.
 */
namespace NimbleEngine
{
    enum : size_t { DESTRUCTION_NAME_MAX = 32 };   // including the NUL
    enum : size_t { DESTRUCTION_PATH_MAX = 128 };  // including the NUL

    enum EDestructibleMode : uint8_t
    {
        DMODE_Runtime = 0,   // cut when it breaks, around the hit
        DMODE_Baked   = 1,   // pieces cut in the editor (Fracture)
    };

    /* "Destructible". The authored half is saved; the rest is this run's. */
    struct NE_API DestructibleComponent
    {
        GAMECORE_COMPONENT_BODY()

        // === Authored ===========================================================
        uint8_t bEnabled    = 1;
        uint8_t Mode        = DMODE_Runtime;   // EDestructibleMode
        uint8_t bStructural = 0;               // breaks around each hit; the rest stays (Static bodies only)
        uint8_t bAnchored   = 1;               // Structural: pieces on its base hold the rest up

        char    Material[DESTRUCTION_NAME_MAX] = "Concrete";   // a preset: DestructionMaterial
        char    Fracture[DESTRUCTION_PATH_MAX] = {};           // Baked: its .nmfrac

        /* 0: the material's. */
        int32_t Pieces         = 0;
        int32_t Seed           = 0;      // 0: one from the entity, the same on every machine
        float   Strength       = 0.0f;   // kg * units / s in one hit to break it
        float   Health         = 0.0f;   // kg * units / s of hits in all
        float   DebrisLifetime = 0.0f;   // seconds

        // === This run ===========================================================
        float    Damage     = 0.0f;   // hits so far, toward Health
        uint8_t  bBroken    = 0;
        uint32_t BreakCount = 0;      // breaks so far (a structure breaks more than once)

        /* What a break asked for: the next update after physics applies it.
         * MulticastBreak sets these on every machine. */
        uint8_t             bPendingBreak = 0;
        EngineCore::Vector3 PendingPoint;
        EngineCore::Vector3 PendingImpulse;
        uint32_t            PendingSeed = 0;

        /* World units and kg * units / s. Break goes through MulticastBreak:
         * on a client it does nothing to a networked entity (its server's
         * break arrives instead). */
        void Break(const EngineCore::Vector3& InPoint, const EngineCore::Vector3& InImpulse);
        void ApplyDamage(const EngineCore::Vector3& InPoint, const EngineCore::Vector3& InImpulse);
        void MulticastBreak(const EngineCore::Vector3& InPoint, const EngineCore::Vector3& InImpulse, uint32_t InSeed);

        bool IsBroken() const { return bBroken != 0 || bPendingBreak != 0; }
    };

    /*
     * What a Destructible's Material names. Built in: Concrete, Wood, Glass,
     * Stone, Metal. Any field can be set in config, which also adds new ones:
     *
     *     [Destruction_Wood]
     *     InteriorMaterial=Content/Materials/Courtyard/Wood.material.nmgraph
     *     BreakParticles=Content/Particles/ShootingRange/Dust.nmparticle
     *     Strength=4000
     *
     *     [Destruction_Ice]          ; a new one: unset fields are Concrete's
     *     Pattern=Radial
     */
    struct DestructionMaterial
    {
        std::string Name;
        uint8_t     Pattern      = 0;        // RenderCore::EFracturePattern
        int32_t     Pieces       = 12;
        float       GrainStretch = 4.0f;     // Splinter: how much longer than wide
        float       Strength     = 9000.0f;  // kg * units / s, one hit
        float       Health       = 25000.0f; // kg * units / s, all hits
        float       Density      = 2400.0f;  // kg / m^3, for a piece's mass when the whole has no Rigid Body mass
        float       Friction     = 0.8f;
        float       Restitution  = 0.05f;
        float       Scatter      = 250.0f;   // units / s thrown out from the hit, at the hit
        float       BreakRadius  = 0.35f;    // Structural: what falls around a hit, as a fraction of its size
        float       DebrisLifetime = 8.0f;   // seconds
        std::string InteriorMaterial;        // the cuts' material; "" draws them with the mesh's first
        std::string BreakParticles;          // .nmparticle spawned at the hit
        std::string BreakSound;              // .nmsound played at the hit
    };

    /* False if InName is no preset. */
    NE_API bool FindDestructionMaterial(const char* InName, DestructionMaterial& OutMaterial);
    NE_API std::vector<std::string> GetDestructionMaterialNames();

    /* Damage every Destructible within InRadius of InCentre, and push every
     * dynamic body there, by InImpulse falling to nothing at the edge. An
     * explosion. Returns how many Destructibles it reached. */
    NE_API int32_t RadialDamage(GameCore::World& InWorld, const EngineCore::Vector3& InCentre, float InRadius, float InImpulse);

    struct DestructionStats
    {
        uint32_t Destructibles = 0;
        uint32_t Broken        = 0;
        uint32_t Debris        = 0;   // fallen pieces alive
        uint32_t Remnants      = 0;   // structures' standing pieces
        uint32_t CachedCuts    = 0;   // runtime fractures held for reuse
        float    LastFractureMs = 0.0f;
    };

    /*
     * A World's Destructibles, run: GameSession attaches one to each level's
     * World after its physics. "Engine.Destruction" runs in TP_PostPhysics:
     * reads the step's impacts, breaks what they broke, and ages the debris.
     * Each Attach needs a Detach, before the World's physics goes.
     */
    class NE_API DestructionWorld
    {
    public:
        static DestructionWorld* Attach(GameCore::World& InWorld);
        static void              Detach(GameCore::World& InWorld);
        static DestructionWorld* Find(const GameCore::World* InWorld);

        void GetStats(DestructionStats& OutStats) const;

        /* The frame's work, as the system does it; a test may call it. */
        void Update(float InDeltaSeconds);

        struct Impl;

    private:
        explicit DestructionWorld(GameCore::World& InWorld);
        ~DestructionWorld();

        DestructionWorld(const DestructionWorld&)            = delete;
        DestructionWorld& operator=(const DestructionWorld&) = delete;

        Impl* P = nullptr;
    };

#if WITH_EDITOR
    /*
     * Cuts InEntity's mesh as its Destructible says (no hit: the bounds'
     * centre) and saves the pieces as InPath (a .nmfrac) beside a folder of
     * .nmmesh pieces named after it; sets the component Baked with Fracture
     * = InRuntimePath. False, with OutError, if it cannot.
     */
    NE_API bool BakeDestructible(GameCore::World& InWorld, GameCore::EntityID InEntity, const std::string& InPath,
                                 const std::string& InRuntimePath, std::string& OutError);
#endif
}

#endif //NIMBLEENGINE_DESTRUCTION_H
