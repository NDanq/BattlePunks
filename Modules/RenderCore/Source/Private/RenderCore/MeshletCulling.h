#ifndef RENDERCORE_MESHLETCULLING_H
#define RENDERCORE_MESHLETCULLING_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

class GDIBuffer;
class GDIShader;

/*
 * GPU MESHLET CULLING (RENDERPIPELINE.md, "Meshlets"): a mesh's meshlets on
 * the GPU, and a view's batch of meshlet sections -- each a record that
 * MeshletCull.csh tests meshlet by meshlet against the view, writing the
 * survivors' indices into the batch's index buffer and their count into the
 * section's indirect arguments, which its Base Pass draw then reads.
 * Render thread only. Private to RenderCore.
 */
namespace RenderCore
{
    struct MeshData;

    /* Words a meshlet takes in MeshletGpuMesh::Data: its ranges, its sphere, its cone. */
    constexpr uint32_t MESHLET_GPU_WORDS = 12;

    /*
     * A mesh's meshlets, as MeshletCull.csh reads them: MESHLET_GPU_WORDS a
     * meshlet (vertex offset and count, triangle offset in bytes and count;
     * centre and radius; cone axis and cutoff), then from VertexBase the
     * meshlet vertices, then from TriangleBase the triangles' corners, four
     * bytes a word. A section's meshlets are SectionFirst[s] onwards,
     * SectionCount[s] of them (0: it draws as before).
     */
    struct MeshletGpuMesh
    {
        GDIBuffer*            Data         = nullptr;
        uint32_t              VertexBase   = 0;   // words
        uint32_t              TriangleBase = 0;   // words
        std::vector<uint32_t> SectionFirst;
        std::vector<uint32_t> SectionCount;
        uint32_t              MeshletCount = 0;

        ~MeshletGpuMesh();

        /* InMesh's, or, for a static mesh without them big enough to gain (two
         * meshlets' worth of triangles), ones built now (BuildMeshlets on a
         * copy). Null for a skinned mesh, one that cannot be built, or no
         * buffer (the backend: GDISupportsMeshletCulling). */
        static std::unique_ptr<MeshletGpuMesh> Create(const MeshData& InMesh, const std::string& InName);
    };

    class MeshletBatch
    {
    public:
        MeshletBatch() = default;
        MeshletBatch(const MeshletBatch&) = delete;
        MeshletBatch& operator=(const MeshletBatch&) = delete;
        ~MeshletBatch() { Release(); }

        /* Forgets the queued sections (a new view). */
        void Reset();

        /* Reset, and its buffers freed (the renderer's shutdown). */
        void Release();

        /*
         * Queues section InSection of InMesh, placed by InModel (its largest
         * axis scale InScale; its material's World Position Offset reaching
         * InPad): its arguments' slot, or -1 if it cannot be culled (no
         * meshlets in that section). InIndexCount: the section's, the most
         * its survivors can write.
         */
        int32_t Add(MeshletGpuMesh* InMesh, uint32_t InSection, const EngineCore::Matrix4x4& InModel, float InScale, float InPad,
                    uint32_t InIndexCount);

        /*
         * Uploads the records and the arguments (no indices yet: each draws
         * nothing until the cull adds its survivors) and culls each mesh's
         * records in one dispatch of InCull against InViewProjection's planes
         * -- with the normal cones too if bInCones, seen from InEye. Outside
         * the stages' render passes, before the Base Pass. False, nothing
         * queued drawn, if a buffer or the dispatch failed.
         */
        bool Dispatch(GDIShader* InCull, const EngineCore::Matrix4x4& InViewProjection, const EngineCore::Vector3& InEye, bool bInCones);

        /* What the draws read: the survivors' indices, and the arguments (GDI_INDIRECT_ARGS_STRIDE a slot). */
        GDIBuffer* GetIndices() const { return bDispatched ? Indices : nullptr; }
        GDIBuffer* GetArgs()    const { return bDispatched ? Args : nullptr; }

        uint32_t GetSectionCount() const { return uint32_t(Records.size()); }
        uint64_t GetMeshletCount() const { return Meshlets; }
        uint64_t GetIndexCount()   const { return IndexTotal; }   // the sections' own, before culling

        /* The indices the cull kept, summed over the sections -- read back
         * from the GPU, waiting for it ([Rendering] MeshletStats: a debug
         * stall). False before Dispatch, or if the read failed. */
        bool ReadDrawnIndices(uint64_t& OutIndices) const;

    private:
        /* One section, as MeshletCull.csh reads it (80 bytes, std430). */
        struct Record
        {
            float    Model[3][4] = {};      // the rows of its placement
            uint32_t MeshletFirst = 0;
            uint32_t MeshletCount = 0;
            uint32_t IndexBase    = 0;      // its first index in Indices
            uint32_t Slot         = 0;      // its arguments
            float    Scale        = 1.0f;   // its largest axis scale: a sphere's radius in the world
            float    Pad          = 0.0f;   // its World Position Offset's reach
            float    Spare[2]     = {};
        };
        static_assert(sizeof(Record) == 80, "MeshletCull.csh's Record");

        /* A buffer of at least InBytes, grown by half again over. */
        static bool Ensure(GDIBuffer*& InOut, uint32_t InBytes, uint8_t InUsage, const char* InName);

        std::vector<Record>          Records;
        std::vector<MeshletGpuMesh*> Owners;       // by record
        uint32_t                     IndexTotal = 0;
        uint64_t                     Meshlets   = 0;
        bool                         bDispatched = false;

        GDIBuffer* RecordBuffer = nullptr;
        GDIBuffer* Indices      = nullptr;
        GDIBuffer* Args         = nullptr;
    };
}

#endif //RENDERCORE_MESHLETCULLING_H
