#ifndef GAMECORE_TERRAIN_H
#define GAMECORE_TERRAIN_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

/*
 * Heightmap terrain data: a square grid of height samples and four paint
 * weights per sample, in the terrain's own space -- the entity's Transform
 * places it. Sample (X, Z) sits at (X * Spacing, Height, Z * Spacing), so the
 * terrain covers [0, Size] on X and Z from its entity's origin, +Y up, in
 * world units (centimetres in the shipped content).
 *
 * ONE TRIANGULATION EVERYWHERE. Each cell splits along the diagonal from
 * (X, Z) to (X + 1, Z + 1) -- Jolt's HeightFieldShape's split -- and the
 * render mesh, SampleHeight and Raycast all use it, so what is drawn, what
 * physics collides with and where a player stands agree exactly.
 *
 * WHY NOT A COMPONENT FIELD: components are trivially copyable and pooled,
 * so a variable-length grid cannot live in one. The data is a .nmterrain file
 * beside the level (Content/Levels/<World>/Terrain/<Entity>.nmterrain), named
 * by RenderCore's Terrain component and shared through TerrainAssetCache by
 * everything that reads it: the editor's brushes write it; the render proxy,
 * the physics heightfield and ground queries read it.
 *
 * THREADS. The editor edits on the game thread while the render thread
 * builds meshes from it, so readers that may run concurrently with an edit
 * hold GetLock(), and writers hold it while they change samples and then
 * call MarkDirty. The revisions are what consumers poll: GetRevision for
 * anything, GetChunkRevision for one chunk's samples.
 *
 * .nmterrain -- an EngineCore::Archive of chunks:
 *
 *   NTRN v1      the file
 *     THDR v1    resolution, spacing, height min and max
 *     HGHT v1    uint16 per sample, row-major (X fastest):
 *                height = Min + h / 65535 * (Max - Min)
 *     SPLT v1    four uint8 weights per sample (layers 0-3)
 *     LAYR v1    the layers' display names
 */
namespace GameCore
{
    class World;

    enum : uint32_t
    {
        TERRAIN_LAYERS          = 4,      // paint weights per sample
        TERRAIN_CHUNK_QUADS     = 64,     // cells per chunk side: the render mesh's unit, and the edit tracking's
        TERRAIN_RESOLUTION_MIN  = 2,      // samples per side
        TERRAIN_RESOLUTION_MAX  = 4097,
    };

    enum : size_t { TERRAIN_PATH_MAX = 128 };   // including the NUL

    /* An inclusive range of sample indices; empty by default. */
    struct TerrainRect
    {
        int32_t X0 = 0;
        int32_t Z0 = 0;
        int32_t X1 = -1;
        int32_t Z1 = -1;

        bool IsEmpty() const { return X1 < X0 || Z1 < Z0; }

        void Add(const int32_t InX, const int32_t InZ)
        {
            if(IsEmpty()) { X0 = X1 = InX; Z0 = Z1 = InZ; return; }
            if(InX < X0) { X0 = InX; }
            if(InX > X1) { X1 = InX; }
            if(InZ < Z0) { Z0 = InZ; }
            if(InZ > Z1) { Z1 = InZ; }
        }

        void Add(const TerrainRect& InOther)
        {
            if(InOther.IsEmpty()) { return; }
            Add(InOther.X0, InOther.Z0);
            Add(InOther.X1, InOther.Z1);
        }
    };

    class GAMECORE_API TerrainData
    {
    public:
        TerrainData();
        ~TerrainData();

        /* Holds a mutex and atomics: copy with CopyFrom. */
        TerrainData(const TerrainData&)            = delete;
        TerrainData& operator=(const TerrainData&) = delete;

        /* InResolution samples a side (clamped to the limits), InSpacing
         * units apart, every sample at InHeight, layer 0 painted everywhere.
         * Bumps every revision. */
        void Reset(uint32_t InResolution, float InSpacing, float InHeight = 0.0f);

        /* Everything InOther holds (locking it while reading); bumps every revision. */
        void CopyFrom(const TerrainData& InOther);

        uint32_t GetResolution() const { return Resolution; }
        float    GetSpacing()    const { return Spacing; }
        /* The side's length: (Resolution - 1) * Spacing. */
        float    GetSize()       const { return float(Resolution > 0 ? Resolution - 1 : 0) * Spacing; }
        bool     IsEmpty()       const { return Resolution < TERRAIN_RESOLUTION_MIN; }

        /* Raw samples, row-major, X fastest; Resolution^2 heights and
         * 4 * Resolution^2 weights. Hold GetLock() to write while other
         * threads read, and MarkDirty what was written. */
        float*         GetHeights()       { return Heights.data(); }
        const float*   GetHeights() const { return Heights.data(); }
        uint8_t*       GetWeights()       { return Weights.data(); }
        const uint8_t* GetWeights() const { return Weights.data(); }

        /* Sample (InX, InZ), clamped to the grid. */
        float GetHeight(int32_t InX, int32_t InZ) const;
        void  SetHeight(int32_t InX, int32_t InZ, float InHeight);
        const uint8_t* GetWeight(int32_t InX, int32_t InZ) const;

        /* The surface height at a point in terrain space, on the cell's own
         * triangle; the edge's height outside the grid. */
        float SampleHeight(float InX, float InZ) const;
        /* The surface normal there: the triangle's (what physics sees), unit length, +Y up. */
        void  SampleNormal(float InX, float InZ, float (&OutNormal)[3]) const;
        /* Smooth per-sample normal from its neighbours, for drawing. */
        void  GetSmoothNormal(int32_t InX, int32_t InZ, float (&OutNormal)[3]) const;
        /* Interpolated paint weights at a point, 0..1 each. */
        void  SampleWeights(float InX, float InZ, float (&OutWeights)[TERRAIN_LAYERS]) const;

        /*
         * The nearest hit of a ray in terrain space: InOrigin + t * InDirection
         * for t in [0, InMaxDistance] (InDirection need not be unit length;
         * t is in its units). False on a miss. Walks the cells the ray
         * crosses, so it costs the cells along the ray, not the grid.
         */
        bool Raycast(const float (&InOrigin)[3], const float (&InDirection)[3], float InMaxDistance,
                     float& OutDistance, float (&OutNormal)[3]) const;

        /* The lowest and highest sample, as of the last MarkDirty. */
        float GetMinHeight() const { return MinHeight; }
        float GetMaxHeight() const { return MaxHeight; }

        // === Edits ==========================================================

        std::mutex& GetLock() const { return Lock; }

        /* Samples in InRect changed: bumps the revision, and the chunks
         * whose meshes read them (one sample further, for normals);
         * refreshes the height range. Call with GetLock() held. */
        void MarkDirty(const TerrainRect& InRect);

        /* Bumped by every change. */
        uint64_t GetRevision() const { return Revision.load(std::memory_order_acquire); }

        /* Chunks a side: ceil((Resolution - 1) / TERRAIN_CHUNK_QUADS). */
        uint32_t GetChunksPerSide() const { return ChunksPerSide; }
        /* Bumped when a sample chunk (InX, InZ) draws from changes. */
        uint64_t GetChunkRevision(uint32_t InX, uint32_t InZ) const;
        /* The samples chunk (InX, InZ) spans, inclusive: its cells' corners. */
        TerrainRect GetChunkSamples(uint32_t InX, uint32_t InZ) const;

        /* The rectangle of samples within InRadius of (InX, InZ) in terrain
         * space, clamped to the grid; empty if none. */
        TerrainRect GetSamplesInRadius(float InX, float InZ, float InRadius) const;

        // === The file =======================================================

        /* Layer display names (the paint list); empty: "Layer N". */
        std::string LayerNames[TERRAIN_LAYERS];

        /* Both directions. False, logged, for an invalid file. */
        bool Serialize(EngineCore::Archive& Ar);

    private:
        void RefreshHeightRange();

        uint32_t             Resolution = 0;
        float                Spacing    = 100.0f;
        std::vector<float>   Heights;
        std::vector<uint8_t> Weights;
        float                MinHeight = 0.0f;
        float                MaxHeight = 0.0f;

        uint32_t                                   ChunksPerSide = 0;
        std::unique_ptr<std::atomic<uint64_t>[]>   ChunkRevisions;
        std::atomic<uint64_t>                      Revision{1};
        mutable std::mutex                         Lock;
    };

    /* Through the file provider, so runtime-relative paths work. False, warned, if it cannot be read. */
    GAMECORE_API bool LoadTerrainFile(const std::string& InPath, TerrainData& OutData);

    /* Writes a temporary beside InPath and renames it over InPath, so a
     * failed save never leaves half a file; creates folders. Locks InData. */
    GAMECORE_API bool SaveTerrainFile(const std::string& InPath, const TerrainData& InData);

    /*
     * Loaded terrains by runtime path, shared by every thread. Unlike the
     * other asset caches its entries are MUTABLE: the editor's brushes write
     * into the one copy every consumer reads (see TerrainData's THREADS).
     * A failed load is warned once and remembered until Invalidate.
     */
    class GAMECORE_API TerrainAssetCache
    {
    public:
        /* The terrain, loaded on first use; null if it cannot be loaded. */
        static std::shared_ptr<TerrainData> Find(const std::string& InPath);
        /* Find without loading: null unless already loaded. */
        static std::shared_ptr<TerrainData> FindLoaded(const std::string& InPath);
        /* InData becomes InPath's (a terrain made in the editor before its file exists). */
        static void Store(const std::string& InPath, std::shared_ptr<TerrainData> InData);
        /* Forget InPath (or everything, given an empty path). */
        static void Invalidate(const std::string& InPath);
        /* Bumped by every Store and Invalidate. */
        static uint64_t GetGeneration();
    };

    /*
     * Which terrain an entity has. GameCore knows no terrain component (it is
     * RenderCore's, for its render proxy), so the engine layer installs this,
     * as it does CollisionMeshProvider: the physics module asks it to shape
     * an entity's body as a heightfield.
     */
    struct TerrainProvider
    {
        /* InEntity's terrain, or null if it has none (or it cannot load). */
        std::shared_ptr<TerrainData> (*FindTerrain)(World& InWorld, EntityID InEntity, void* InUserData) = nullptr;
        void* UserData = nullptr;
    };

    /* Process-wide; null removes it. The installer removes it before its image unloads. */
    GAMECORE_API void SetTerrainProvider(const TerrainProvider* InProvider);
    GAMECORE_API const TerrainProvider* GetTerrainProvider();
}

#endif //GAMECORE_TERRAIN_H
