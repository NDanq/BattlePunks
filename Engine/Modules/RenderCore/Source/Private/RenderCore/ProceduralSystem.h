#ifndef RENDERCORE_PROCEDURALSYSTEM_H
#define RENDERCORE_PROCEDURALSYSTEM_H

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/ProceduralGeneration.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace GameCore { class World; }

/*
 * The Procedural Spawner's game-thread half (WorldBridge drives it once a
 * frame, before extraction): for each spawner, a signature of everything its
 * placement reads -- its rules, its entity's place, its Spline and its
 * Exclusions' splines, the terrains' revisions, the static meshes under it --
 * and, when that moves, a snapshot of those inputs handed to a worker thread
 * (GenerateProcedural). One generation per spawner runs at a time; a change
 * made meanwhile starts the next when it finishes. The finished result goes
 * to ProceduralResultStore, where the spawner's proxy picks it up on the
 * render thread, and its count back to the component.
 */
namespace RenderCore
{
    /* Finished results by (world, entity): the hand-over between the two threads. */
    class RENDERCORE_API ProceduralResultStore
    {
    public:
        static void Publish(const void* InWorld, GameCore::EntityID InEntity, std::shared_ptr<const ProceduralResult> InResult);
        static std::shared_ptr<const ProceduralResult> Find(const void* InWorld, GameCore::EntityID InEntity);
        static void Remove(const void* InWorld, GameCore::EntityID InEntity);
    };

    class RENDERCORE_API ProceduralSystem
    {
    public:
        ProceduralSystem();
        ~ProceduralSystem();

        ProceduralSystem(const ProceduralSystem&)            = delete;
        ProceduralSystem& operator=(const ProceduralSystem&) = delete;

        /* GAME THREAD, once a frame. */
        void Update(GameCore::World& InWorld);

        /* Waits for every generation and forgets every result (unbinding the world). */
        void Reset();

    private:
        struct Spawner;
        std::unordered_map<uint64_t, std::unique_ptr<Spawner>> Spawners;
        GameCore::World* World = nullptr;
        uint64_t         Frame = 0;
    };
}

#endif //RENDERCORE_PROCEDURALSYSTEM_H
