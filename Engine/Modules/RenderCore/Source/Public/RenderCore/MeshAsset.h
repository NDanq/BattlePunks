#ifndef RENDERCORE_MESHASSET_H
#define RENDERCORE_MESHASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/AssetImportInfo.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

/*
 * .nmmesh -- a static mesh. An EngineCore::Archive of chunks:
 *
 *   NMSH v1                the file
 *     MHDR v1              vertex, index and section counts; bounds
 *     MVTX v1              positions, normals, UV0 (float arrays)
 *     MIDX v1              a triangle list, uint32 indices
 *     MSEC v1              sections: name, material, first index, index count
 *     MCOL v1 (optional)   vertex colours, RGBA floats
 *     MUV1 v1 (optional)   UV1, the lightmap UVs: charts in 0-1 that do not
 *                          overlap (baked lighting; RENDERPIPELINE.md)
 *     MSKN v1 (optional)   skinning (ANIMATION.md): the skeleton's path, an
 *                          inverse bind matrix per skeleton bone, then four
 *                          bone indices (uint16) and four weights per vertex
 *     MTAN v1 (optional)   tangents, four floats per vertex (MeshData::Tangents)
 *     MLET v1 (optional)   meshlets (BuildMeshlets): the fingerprint of the
 *                          indices they were built from, the meshlets, then
 *                          their vertices (uint32) and triangles (uint8)
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *
 * Every vertex has a normal and UV. Loading bounds-checks indices and
 * sections; meshlets that fail the same checks, or were built from other
 * indices, are dropped (warned) and the mesh still loads. Positions are engine
 * convention (left-handed, +Y up), world units.
 */
namespace RenderCore
{
    struct MeshSection
    {
        std::string Name;
        std::string Material;      // the source's material name; may not exist as a material here
        uint32_t    FirstIndex = 0;
        uint32_t    IndexCount = 0;
    };

    /*
     * A small piece of one section -- at most MESHLET_MAX_VERTICES vertices
     * and MESHLET_MAX_TRIANGLES triangles -- that the GPU culls on its own
     * (RENDERPIPELINE.md, Meshlets). Its triangles are three corners each,
     * indices into its own vertices, which are indices into the mesh's.
     */
    constexpr uint32_t MESHLET_MAX_VERTICES  = 64;
    constexpr uint32_t MESHLET_MAX_TRIANGLES = 124;

    struct MeshMeshlet
    {
        uint32_t Section        = 0;
        uint32_t VertexOffset   = 0;   // into MeshData::MeshletVertices
        uint32_t VertexCount    = 0;
        uint32_t TriangleOffset = 0;   // into MeshData::MeshletTriangles, three a triangle
        uint32_t TriangleCount  = 0;

        /* A sphere round its vertices, in mesh space. */
        float    Center[3]      = {0.0f, 0.0f, 0.0f};
        float    Radius         = 0.0f;

        /* Its normal cone: seen from P, every triangle faces away when
         * dot(Center - P, ConeAxis) >= ConeCutoff * |Center - P| + Radius.
         * ConeCutoff 1 or more: the cone is too wide to cull by. */
        float    ConeAxis[3]    = {0.0f, 0.0f, 0.0f};
        float    ConeCutoff     = 1.0f;
    };

    struct MeshData
    {
        std::vector<float>       Positions;   // 3 per vertex
        std::vector<float>       Normals;     // 3 per vertex
        std::vector<float>       UV0;         // 2 per vertex
        std::vector<float>       Colors;      // RGBA, 4 per vertex; empty: white (the optional MCOL chunk)
        std::vector<float>       UV1;         // 2 per vertex; empty: no lightmap UVs (the optional MUV1 chunk)

        /* 4 per vertex, the optional MTAN chunk: xyz a unit tangent along UV0's
         * +U at right angles to the normal, w +1 or -1 so that the bitangent
         * w * cross(normal, tangent) runs along +V -- MikkTSpace's frame
         * [Mikkelsen 2008], as a normal map is baked against. Empty: none, and
         * the material templates take one from the UV's screen derivatives. */
        std::vector<float>       Tangents;
        std::vector<uint32_t>    Indices;     // 3 per triangle
        std::vector<MeshSection> Sections;

        /* Skinning, the optional MSKN chunk; all empty for a static mesh.
         * Bone indices are the skeleton's; a vertex's weights sum to 1. */
        std::string              Skeleton;      // runtime path of its .nmskel
        std::vector<AnimMatrix>  InverseBind;   // one per skeleton bone: model space -> the bone's own
        std::vector<uint16_t>    Joints;        // 4 per vertex
        std::vector<float>       Weights;       // 4 per vertex

        /* Meshlets, the optional MLET chunk (BuildMeshlets); all empty: none,
         * and the mesh draws by section. Every section's triangles are in its
         * meshlets exactly once. MeshletSource is MeshletFingerprint of the
         * indices they were built from: anything that changes the indices
         * leaves it stale, and a stale set is neither saved nor loaded. */
        std::vector<MeshMeshlet> Meshlets;
        std::vector<uint32_t>    MeshletVertices;    // indices into the mesh's vertices
        std::vector<uint8_t>     MeshletTriangles;   // 3 per triangle, indices into its meshlet's vertices
        uint64_t                 MeshletSource = 0;

        float                    BoundsMin[3] = {0.0f, 0.0f, 0.0f};
        float                    BoundsMax[3] = {0.0f, 0.0f, 0.0f};
        AssetImportInfo          ImportInfo;

        uint32_t GetVertexCount()   const { return uint32_t(Positions.size() / 3); }
        uint32_t GetTriangleCount() const { return uint32_t(Indices.size() / 3); }
        bool     IsSkinned()        const { return !Joints.empty(); }
        bool     HasMeshlets()      const { return !Meshlets.empty(); }
    };

    /* What LoadMeshHeader reads: enough to describe a mesh, or bound it. */
    struct MeshHeader
    {
        uint32_t VertexCount  = 0;
        uint32_t IndexCount   = 0;
        uint32_t SectionCount = 0;
        float    BoundsMin[3] = {0.0f, 0.0f, 0.0f};
        float    BoundsMax[3] = {0.0f, 0.0f, 0.0f};
    };

    /* Sets BoundsMin/Max from the positions; zero for an empty mesh. */
    RENDERCORE_API void ComputeMeshBounds(MeshData& InOutMesh);

    struct SkeletonData;

    /* A skinned mesh's bounds: where its vertices are in InSkeleton's
     * reference pose -- how it draws with no animation -- not in bind space. */
    RENDERCORE_API void ComputeSkinnedMeshBounds(MeshData& InOutMesh, const SkeletonData& InSkeleton);

    /* The palette (Skinning.glsl: 12 floats a bone) that draws InMesh in
     * InSkeleton's reference pose. Empty if InMesh is not skinned. */
    RENDERCORE_API void MakeRefPosePalette(const MeshData& InMesh, const SkeletonData& InSkeleton, std::vector<float>& OutPalette);

    /*
     * Splits every section into meshlets (meshoptimizer's clusterizer
     * [Kapoulkine], vertices reordered within each for locality) and bounds
     * them. Replaces any meshlets it had; leaves the mesh's own vertices,
     * indices and sections alone. False, and the mesh untouched, if it has no
     * triangles or an index is out of range.
     */
    RENDERCORE_API bool BuildMeshlets(MeshData& InOutMesh);

    /* A hash of InMesh's indices and vertex count, which MeshletSource is
     * compared with. */
    RENDERCORE_API uint64_t MeshletFingerprint(const MeshData& InMesh);

    /* Why InMesh's meshlets could not be drawn as its sections; null if they
     * can (or it has none). */
    RENDERCORE_API const char* FindMeshletDefect(const MeshData& InMesh);

    /* Both directions. False, logged, for an invalid mesh. */
    RENDERCORE_API bool SerializeMesh(EngineCore::Archive& Ar, MeshData& InOutMesh);

    /* Through the file provider, so runtime-relative paths work. */
    RENDERCORE_API bool LoadMeshFile(const std::string& InPath, MeshData& OutMesh);

    /* Only MHDR (still reads the whole file). */
    RENDERCORE_API bool LoadMeshHeader(const std::string& InPath, MeshHeader& OutHeader);

#if WITH_EDITOR
    /* Creates folders. */
    RENDERCORE_API bool SaveMeshFile(const std::string& InPath, MeshData& InMesh);
#endif

    /*
     * Loaded meshes by path, shared by game and render threads. Thread-safe.
     * A failed load is warned once and remembered until Invalidate, which is
     * also how a reimport propagates (the render thread rebuilds on a new
     * pointer).
     */
    class RENDERCORE_API MeshAssetCache
    {
    public:
        /* The mesh, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<const MeshData> Find(const std::string& InPath);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);

        /*
         * A mesh made in memory rather than loaded -- a terrain's chunks --
         * under InPath, which should not name a file ("@terrain/..."): Find
         * returns it, and the render thread rebuilds its GPU copy whenever
         * a different pointer is stored. Invalidate leaves these alone; a
         * null InMesh (or Remove) forgets one. Does not bump the generation
         * (no asset on disk changed).
         */
        static void Store(const std::string& InPath, std::shared_ptr<const MeshData> InMesh);
        static void Remove(const std::string& InPath);

        /* Bumped by every Invalidate: anything that kept a mesh past a frame
         * (the static draw lists) compares it to know to look again. */
        static uint64_t GetGeneration();
    };
}

#endif //RENDERCORE_MESHASSET_H
