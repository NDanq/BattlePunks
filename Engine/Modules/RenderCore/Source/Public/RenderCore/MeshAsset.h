#ifndef RENDERCORE_MESHASSET_H
#define RENDERCORE_MESHASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *
 * Every vertex has a normal and UV. Loading bounds-checks indices and
 * sections. Positions are engine convention (left-handed, +Y up), world units.
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

    struct MeshData
    {
        std::vector<float>       Positions;   // 3 per vertex
        std::vector<float>       Normals;     // 3 per vertex
        std::vector<float>       UV0;         // 2 per vertex
        std::vector<float>       Colors;      // RGBA, 4 per vertex; empty: white (the optional MCOL chunk)
        std::vector<float>       UV1;         // 2 per vertex; empty: no lightmap UVs (the optional MUV1 chunk)
        std::vector<uint32_t>    Indices;     // 3 per triangle
        std::vector<MeshSection> Sections;
        float                    BoundsMin[3] = {0.0f, 0.0f, 0.0f};
        float                    BoundsMax[3] = {0.0f, 0.0f, 0.0f};
        AssetImportInfo          ImportInfo;

        uint32_t GetVertexCount()   const { return uint32_t(Positions.size() / 3); }
        uint32_t GetTriangleCount() const { return uint32_t(Indices.size() / 3); }
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

        /* Bumped by every Invalidate: anything that kept a mesh past a frame
         * (the static draw lists) compares it to know to look again. */
        static uint64_t GetGeneration();
    };
}

#endif //RENDERCORE_MESHASSET_H
