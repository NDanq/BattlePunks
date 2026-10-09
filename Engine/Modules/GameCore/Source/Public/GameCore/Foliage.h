#ifndef GAMECORE_FOLIAGE_H
#define GAMECORE_FOLIAGE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/TerrainBrush.h"

namespace EngineCore
{
    class Archive;
}

/*
 * Foliage: many copies of a few meshes -- grass, rocks, bushes, trees --
 * placed on a terrain. It belongs to its terrain: a Terrain component's
 * Foliage names a .nmfoliage beside the .nmterrain, and the terrain's proxy
 * draws it, chunk by chunk, as instanced meshes, with the terrain's own
 * culling and LOD (RenderCore's TerrainComponent.h).
 *
 * INSTANCES SIT ON THE GROUND. An instance stores where it is on the terrain
 * (X and Z, in terrain space), its yaw, scale and a seed; its height and its
 * tilt are the terrain's, found whenever its chunk is built. Sculpt under a
 * tree and it follows the ground.
 *
 * BUCKETS ARE THE TERRAIN'S CHUNKS. Instances are kept in a bucket per
 * terrain chunk (TERRAIN_CHUNK_QUADS cells a side), so a brush touches a few
 * buckets, the proxy rebuilds those, and a chunk's LOD decides how many of
 * its instances draw: each type keeps a fraction of them at LOD 1 and LOD 2
 * (stable: the same ones, chosen by their seeds), with its LOD meshes, and
 * none past its cull distance.
 *
 * THREADS: as TerrainData -- the editor writes on the game thread while the
 * render thread reads; both hold GetLock(), and edits bump the revision of
 * each bucket they change.
 *
 * .nmfoliage -- an EngineCore::Archive of chunks:
 *
 *   NFOL v1      the file
 *     FHDR v1    buckets a side, bucket size (terrain units)
 *     FTYP v1    the types (FoliageType's fields, each a version-tolerant record)
 *     FINS v1    every instance: type (uint16), x, z, yaw, scale (floats), seed (uint32)
 */
namespace GameCore
{
    enum : uint32_t
    {
        FOLIAGE_TYPES_MAX = 64,
        FOLIAGE_LODS      = 3,   // as RenderCore's TERRAIN_LODS
    };

    /* One kind of foliage: what it is and how it is placed. Units are the terrain's. */
    struct FoliageType
    {
        std::string Name;
        std::string Mesh[FOLIAGE_LODS];   // LOD 0, 1, 2; an empty one uses the one before
        std::string Material;             // every section's; empty: the mesh's own

        float   Spacing        = 300.0f;  // the distance kept between instances of this type
        float   ScaleMin       = 0.8f;
        float   ScaleMax       = 1.2f;
        float   AlignToNormal  = 0.0f;    // 0: upright, 1: along the ground's normal
        float   MaxSlope       = 40.0f;   // degrees: not placed on steeper ground
        float   Sink           = 0.0f;    // pushed into the ground this far (times its scale)
        float   CullDistance   = 0.0f;    // world units from the view; 0: never culled
        float   LODDensity[FOLIAGE_LODS] = { 1.0f, 0.5f, 0.25f };   // the fraction a chunk at each LOD draws
        float   ColorVariation = 0.15f;   // 0..1: each instance darkened by up to this
        uint8_t bCastShadows   = 1;
        uint8_t bRandomYaw     = 1;
    };

    struct FoliageInstance
    {
        uint16_t Type  = 0;
        float    X     = 0.0f;   // terrain space
        float    Z     = 0.0f;
        float    Yaw   = 0.0f;   // degrees
        float    Scale = 1.0f;
        uint32_t Seed  = 0;      // its thinning order and colour
    };

    /* A stable 0..1 from a seed: what thinning and colour variation use. */
    inline float FoliageSeedFraction(const uint32_t InSeed)
    {
        uint32_t H = InSeed * 0x9E3779B1u;
        H ^= H >> 15;
        H *= 0x85EBCA77u;
        H ^= H >> 13;
        return float(H & 0xFFFFFFu) / float(0x1000000);
    }

    class GAMECORE_API FoliageData
    {
    public:
        FoliageData();
        ~FoliageData();

        /* Holds a mutex and atomics. */
        FoliageData(const FoliageData&)            = delete;
        FoliageData& operator=(const FoliageData&) = delete;

        /* Empty, bucketed for InTerrain: its chunks. Keeps the types. */
        void Reset(const TerrainData& InTerrain);
        void Reset(uint32_t InBucketsPerSide, float InBucketSize);

        uint32_t GetBucketsPerSide() const { return BucketsPerSide; }
        float    GetBucketSize()     const { return BucketSize; }

        std::vector<FoliageType>&       GetTypes()       { return Types; }
        const std::vector<FoliageType>& GetTypes() const { return Types; }

        /* Bucket (InX, InZ)'s instances; empty out of range. Hold GetLock()
         * to read while others write. */
        const std::vector<FoliageInstance>& GetBucket(uint32_t InX, uint32_t InZ) const;
        std::vector<FoliageInstance>*       FindBucket(uint32_t InX, uint32_t InZ);

        /* The bucket a terrain-space point is in; false outside the grid. */
        bool BucketOf(float InX, float InZ, uint32_t& OutX, uint32_t& OutZ) const;

        /* Adds one, to its bucket; false outside the grid or for an unknown type. */
        bool Add(const FoliageInstance& InInstance);

        size_t GetInstanceCount() const;
        size_t GetInstanceCount(uint16_t InType) const;

        /* Removes InType (and its instances), renumbering the types after it. */
        void RemoveType(uint16_t InType);

        // === Edits ==========================================================

        std::mutex& GetLock() const { return Lock; }

        /* Bucket (InX, InZ) changed (or every bucket, for the types): bumps its revision. Hold the lock. */
        void MarkBucketDirty(uint32_t InX, uint32_t InZ);
        void MarkAllDirty();

        uint64_t GetRevision() const { return Revision.load(std::memory_order_acquire); }
        uint64_t GetBucketRevision(uint32_t InX, uint32_t InZ) const;

        /* Both directions. False, logged, for an invalid file. */
        bool Serialize(EngineCore::Archive& Ar);

    private:
        uint32_t                                  BucketsPerSide = 0;
        float                                     BucketSize     = 6400.0f;
        std::vector<FoliageType>                  Types;
        std::vector<std::vector<FoliageInstance>> Buckets;
        std::unique_ptr<std::atomic<uint64_t>[]>  BucketRevisions;
        std::atomic<uint64_t>                     Revision{1};
        mutable std::mutex                        Lock;
    };

    GAMECORE_API bool LoadFoliageFile(const std::string& InPath, FoliageData& OutData);
    /* Atomic, as SaveTerrainFile. Locks InData. */
    GAMECORE_API bool SaveFoliageFile(const std::string& InPath, const FoliageData& InData);

    /* Loaded foliage by runtime path, mutable and shared, as TerrainAssetCache. */
    class GAMECORE_API FoliageAssetCache
    {
    public:
        static std::shared_ptr<FoliageData> Find(const std::string& InPath);
        static std::shared_ptr<FoliageData> FindLoaded(const std::string& InPath);
        static void     Store(const std::string& InPath, std::shared_ptr<FoliageData> InData);
        static void     Invalidate(const std::string& InPath);
        static uint64_t GetGeneration();
    };

    // === Brushes ================================================================

    enum EFoliageTool : uint8_t
    {
        FT_Paint = 0,   // fills the brush toward each chosen type's spacing
        FT_Erase = 1,   // removes the chosen types under the brush
        FT_Count
    };

    struct FoliageBrushOp
    {
        uint8_t               Tool         = FT_Paint;
        float                 DeltaSeconds = 1.0f / 60.0f;
        std::vector<uint16_t> Types;       // which types it places or removes; empty: every type
        uint32_t              Seed         = 1;   // advanced by each dab, so strokes differ
    };

    /* What a dab changed: the buckets it touched, and how many it added or removed. */
    struct FoliageBrushResult
    {
        std::vector<std::pair<uint32_t, uint32_t>> Buckets;
        uint32_t Added   = 0;
        uint32_t Removed = 0;
    };

    /*
     * One dab on InFoliage over InTerrain (terrain space, as ApplyTerrainBrush).
     * Paint tries candidate points under the brush (more toward its centre),
     * keeping those on ground no steeper than the type's MaxSlope and no nearer
     * than its Spacing to another of that type, with a random yaw and scale;
     * over a second at full strength it fills the brush. Erase removes the
     * chosen types, faster toward the centre. Locks InFoliage, and InTerrain
     * while reading it; bumps the buckets it changes.
     */
    GAMECORE_API FoliageBrushResult ApplyFoliageBrush(FoliageData& InFoliage, const TerrainData& InTerrain,
                                                      const TerrainBrush& InBrush, FoliageBrushOp& InOutOp);

    /*
     * Where instance InInstance stands on InTerrain, as its chunk is built: its
     * transform in terrain space (column-major 4x4, bottom row 0 0 0 1) --
     * yawed, scaled, tilted toward the ground's normal by its type's
     * AlignToNormal, and sunk by its Sink. Does not lock.
     */
    GAMECORE_API void GetFoliageTransform(const TerrainData& InTerrain, const FoliageType& InType,
                                          const FoliageInstance& InInstance, float (&OutMatrix)[16]);
}

#endif //GAMECORE_FOLIAGE_H
