#ifndef RENDERCORE_PROCEDURALGENERATION_H
#define RENDERCORE_PROCEDURALGENERATION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "Public/Core/CoreMemory.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/GameCore/Spline.h"
#include "Public/GameCore/Terrain.h"
#include "Public/Math/Matrix.h"
#include "Public/RenderCore/Components/ProceduralSpawnerComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * The Procedural Spawner's placement, as a plain function: everything it
 * reads gathered into ProceduralInputs (on the game thread, by the
 * renderer's world bridge), then GenerateProcedural on any thread. The
 * rules are ProceduralSpawnerComponent's (its header says what each does);
 * PROCEDURAL.md says how to use them.
 *
 * Matrices are row-major with column vectors, as Entity::GetWorldMatrix.
 */
namespace RenderCore
{
    /*
     * MEMORY. Everything a generation makes -- the result, its cells and
     * instance arrays, the shared handle's control block, and the snapshot
     * of its inputs -- comes from the "Procedural" pool (a HeapMemoryPool
     * registered with Core), so it shows in Core's pool stats and leak
     * report. The generator's scratch (candidates, the mesh grid) is freed
     * before it returns and stays on the plain heap.
     *
     * The handle is a std::shared_ptr over that memory, not a TRef: a result
     * is shared with the worker that builds it and the render thread that
     * draws it, and the collector requires that no thread but the game
     * thread touches a TRef (NimbleEngine.cpp, RenderCommand.h). It has no
     * cycles either, so reference counting alone frees all of it.
     */
    RENDERCORE_API EngineCore::PoolID GetProceduralMemoryPool();

    /* The "Procedural" pool's numbers (EngineCore::Memory::GetPoolStats). */
    RENDERCORE_API EngineCore::MemoryPoolStats GetProceduralMemoryStats();

    /* An STL allocator over the "Procedural" pool. Stateless: any two are equal. */
    template<typename T>
    struct ProceduralAllocator
    {
        using value_type = T;

        ProceduralAllocator() noexcept = default;
        template<typename U> ProceduralAllocator(const ProceduralAllocator<U>&) noexcept {}

        T* allocate(const std::size_t InCount)
        {
            /* GImageID read here, in the calling image, as Memory::Allocate asks. */
            void* Block = EngineCore::Memory::Allocate(sizeof(T) * InCount, alignof(T), GetProceduralMemoryPool(),
                                                       nullptr, EngineCore::AF_None, EngineCore::GImageID);
            if(!Block) { throw std::bad_alloc(); }   // as std::allocator: a container cannot go on without it
            return static_cast<T*>(Block);
        }

        void deallocate(T* InPtr, const std::size_t InCount) noexcept
        {
            (void)InCount;
            EngineCore::Memory::Free(InPtr);
        }

        template<typename U> bool operator==(const ProceduralAllocator<U>&) const noexcept { return true; }
        template<typename U> bool operator!=(const ProceduralAllocator<U>&) const noexcept { return false; }
    };

    template<typename T>
    using ProceduralVector = std::vector<T, ProceduralAllocator<T>>;

    /* A terrain the points may land on. Locked per query, so an editor brush may run meanwhile. */
    struct ProceduralTerrainInput
    {
        std::shared_ptr<GameCore::TerrainData> Data;
        EngineCore::Matrix4x4                  ToWorld;
        EngineCore::Matrix4x4                  ToLocal;
    };

    /* A static mesh the points may land on, or the Mesh Surface source's own mesh. */
    struct ProceduralMeshInput
    {
        std::shared_ptr<const MeshData> Mesh;
        EngineCore::Matrix4x4           ToWorld;
    };

    /* A spline that clears instances: inside it (closed), and within HalfWidth of it. World space. */
    struct ProceduralExclusionInput
    {
        GameCore::SplineCurve Curve;
        float                 HalfWidth = 0.0f;
    };

    /* What a Meshes entry's mesh is, as far as placement cares. */
    struct ProceduralEntryInput
    {
        bool  bValid      = false;           // a mesh is named and loaded
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct ProceduralInputs
    {
        ProceduralSpawnerComponent             Settings;
        EngineCore::Matrix4x4                  ToWorld;          // the spawner's entity
        bool                                   bHasSpline  = false;
        GameCore::SplineCurve                  Spline;           // its entity's space
        float                                  SplineWidth = 0.0f;
        std::vector<ProceduralTerrainInput>    Terrains;
        std::vector<ProceduralMeshInput>       Meshes;           // projection targets
        ProceduralMeshInput                    Surface;          // Mesh Surface: the entity's own
        std::vector<ProceduralExclusionInput>  Exclusions;
        ProceduralEntryInput                   Entries[PROCEDURAL_MESHES_MAX];
    };

    /* One Meshes entry's instances in one cell, in thinning order: a coarser LOD draws the first N. */
    struct ProceduralBatch
    {
        uint8_t                          Entry = 0;
        ProceduralVector<RenderInstance> Instances;   // entity space
    };

    /* A Cell Size square of instances, culled and thinned as one. Entity space. */
    struct ProceduralCell
    {
        float                        Min[3] = { 0.0f, 0.0f, 0.0f };
        float                        Max[3] = { 0.0f, 0.0f, 0.0f };
        ProceduralVector<ProceduralBatch> Batches;
    };

    struct ProceduralResult
    {
        ProceduralVector<ProceduralCell> Cells;
        uint32_t                    InstanceCount = 0;
        uint32_t                    Candidates    = 0;     // points tried before the filters
        bool                        bCapped       = false; // stopped at MaxInstances
        float                       Milliseconds  = 0.0f;
        const char*                 Problem       = nullptr;   // why nothing was made ("no closed Spline"), or null
    };

    /* Places InInputs.Settings' instances. Deterministic for the same inputs. */
    RENDERCORE_API void GenerateProcedural(const ProceduralInputs& InInputs, ProceduralResult& OutResult);

    /* Every spawner in every world generates again next frame, its inputs
     * changed or not (the editor's `pcg regenerate`). Any thread. */
    RENDERCORE_API void RegenerateAllProcedural();
    RENDERCORE_API uint64_t GetProceduralRegeneration();
}

#endif //RENDERCORE_PROCEDURALGENERATION_H
