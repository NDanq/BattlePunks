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
 * .nmmesh -- a static mesh, as the editor's importer writes it and a
 * RenderComponent's Mesh names it. An EngineCore::Archive of chunks:
 *
 *   NMSH v1                the file
 *     MHDR v1              vertex, index and section counts; bounds
 *     MVTX v1              positions (xyz), normals (xyz), UV0 (uv) -- one
 *                          entry per vertex in each, floats
 *     MIDX v1              a triangle list, uint32 indices
 *     MSEC v1              sections: name, material, first index, index count
 *     ISRC v1 (optional)   AssetImportInfo -- the editor's, for Reimport
 *
 * Every vertex has a normal and a UV; the importer fills in what the source
 * lacks. Loading checks every index against the vertex count and every
 * section against the index range, so a draw never reads past a buffer.
 *
 * Positions are in the engine's convention (+Y down) and world units; the
 * importer converts. A section is one material's run of triangles.
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

    /* Both directions. False, with the archive failed and why logged, for a
     * file that is not a mesh or does not hold together. */
    RENDERCORE_API bool SerializeMesh(EngineCore::Archive& Ar, MeshData& InOutMesh);

    /* Through the file provider, so a runtime-relative path ("Content/Crate
     * .nmmesh") finds the staged copy. */
    RENDERCORE_API bool LoadMeshFile(const std::string& InPath, MeshData& OutMesh);

    /* Only MHDR. Still reads the whole file -- archives are whole-buffer --
     * but builds none of the arrays. */
    RENDERCORE_API bool LoadMeshHeader(const std::string& InPath, MeshHeader& OutHeader);

#if WITH_EDITOR
    /* Writes InMesh to InPath, creating folders. The importer writes to a
     * temporary name and renames, so a failed import leaves the old file. */
    RENDERCORE_API bool SaveMeshFile(const std::string& InPath, MeshData& InMesh);
#endif

    /*
     * Loaded meshes by path, shared by the game thread (bounds, editor
     * picking) and the render thread (which builds GPU buffers from them).
     * Thread-safe. A mesh that fails to load is warned about once and
     * remembered as missing, until Invalidate -- which is how a reimport
     * reaches everything: the next Find loads the new file, and the render
     * thread rebuilds when it sees a different pointer.
     */
    class RENDERCORE_API MeshAssetCache
    {
    public:
        /* The mesh, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<const MeshData> Find(const std::string& InPath);

        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);
    };
}

#endif //RENDERCORE_MESHASSET_H
