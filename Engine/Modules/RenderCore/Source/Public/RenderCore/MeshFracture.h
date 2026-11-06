#ifndef RENDERCORE_MESHFRACTURE_H
#define RENDERCORE_MESHFRACTURE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * Voronoi fracture: a closed static mesh cut into convex-celled pieces, each
 * a closed mesh of its own. The editor bakes them into files; a game makes
 * them when something breaks (MeshAssetCache::Store under an "@fracture/"
 * path). Plain functions on any thread: nothing here touches the caches.
 *
 * HOW. Seed points are scattered over the mesh's bounds by the pattern.
 * Each seed's piece is the mesh clipped by the plane half way to every
 * other seed, nearest first. Every cut is capped: its outline is closed into
 * loops (holes included, so a hollow or L-shaped mesh caps correctly) and
 * triangulated. Caps go in one extra section, "Interior", after the mesh's
 * own, so section i keeps material slot i and the inside can draw with
 * another material (rough concrete, end grain).
 *
 * The same mesh, settings and seed always give the same pieces, so the
 * editor's preview, a game and every network client agree.
 *
 * Positions are the mesh's own (its units, no transform). Each piece is
 * moved so its centre of mass is its origin; Center says where that was.
 */
namespace RenderCore
{
    /* Where the seeds go. */
    enum EFracturePattern : uint8_t
    {
        FP_Uniform   = 0,   // evenly over the bounds: concrete, stone
        FP_Clustered = 1,   // most near the impact point, small there and large further off
        FP_Splinter  = 2,   // evenly, in a space squashed along GrainAxis: long splinters (wood)
        FP_Radial    = 3,   // shells around the impact point: shards (glass)
    };

    struct FractureSettings
    {
        uint32_t Seed       = 1;
        uint32_t PieceCount = 12;               // seeds; a seed whose cell misses the mesh gives no piece
        uint8_t  Pattern    = FP_Uniform;       // EFracturePattern

        float GrainAxis[3]  = {0.0f, 1.0f, 0.0f};   // FP_Splinter: the grain, in mesh space
        float GrainStretch  = 4.0f;                 // FP_Splinter: how much longer than wide a splinter is

        /* FP_Clustered, FP_Radial: the hit, in mesh space. Without one, the
         * bounds' centre. Radius 0 is half the bounds' diagonal. */
        bool  bHasImpact     = false;
        float ImpactPoint[3] = {0.0f, 0.0f, 0.0f};
        float ImpactRadius   = 0.0f;

        std::string InteriorMaterial;           // the Interior section's material ("" = DefaultLit)
        float       InteriorUVScale = 0.01f;    // the caps' planar UVs: UV per unit
    };

    struct FracturePiece
    {
        MeshData Mesh;                          // origin at its centre of mass; sections: the source's, then "Interior"
        float    Center[3] = {0.0f, 0.0f, 0.0f};   // where that was in the source mesh
        float    Volume    = 0.0f;              // cubic units; a share of the source's for its mass
    };

    struct FractureResult
    {
        std::vector<FracturePiece> Pieces;
        /* Pieces that share a cut face, (a, b) with a < b: what M5 welds. */
        std::vector<std::pair<uint32_t, uint32_t>> Neighbours;
        /* Cuts whose outline would not close into loops (an open or
         * self-intersecting source); those caps are missing. 0 for a closed mesh. */
        uint32_t OpenCaps = 0;
    };

    /* The seeds FractureMesh would use, xyz per seed, in mesh space. */
    RENDERCORE_API void MakeFractureSeeds(const MeshData& InMesh, const FractureSettings& InSettings, std::vector<float>& OutSeeds);

    /* False, logged, for an empty or skinned mesh. Lightmap UVs (UV1) are
     * dropped: a piece moves, so a bake cannot light it. */
    RENDERCORE_API bool FractureMesh(const MeshData& InMesh, const FractureSettings& InSettings, FractureResult& OutResult);
}

#endif //RENDERCORE_MESHFRACTURE_H
