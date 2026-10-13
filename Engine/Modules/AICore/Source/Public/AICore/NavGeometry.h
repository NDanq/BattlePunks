#ifndef AICORE_NAVGEOMETRY_H
#define AICORE_NAVGEOMETRY_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/NavTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * What a navmesh is built from: world-space triangles and the boxes that
 * mark areas over them. GatherNavGeometry reads a World; tests and tools fill
 * one by hand.
 */
namespace AICore
{
    /* A box marking an area: Nav Modifier components, in world space. Turned
     * about Y by Yaw degrees around its centre. */
    struct NavAreaBox
    {
        EngineCore::Vector3 Center;
        EngineCore::Vector3 HalfExtents;
        float   Yaw  = 0.0f;
        uint8_t Area = NAV_AREA_BLOCKED;
    };

    class AICORE_API NavInputGeometry
    {
    public:
        /* Triangles: xyz per vertex, three vertex indices each. */
        std::vector<float>    Vertices;
        std::vector<int32_t>  Indices;
        std::vector<NavAreaBox> Areas;

        size_t GetVertexCount() const   { return Vertices.size() / 3; }
        size_t GetTriangleCount() const { return Indices.size() / 3; }
        bool   IsEmpty() const          { return Indices.empty(); }

        /* What every triangle covers; empty with none. */
        GameCore::AABB GetBounds() const;

        /* InPositions (xyz) and InIndices, through InToWorld (a point
         * transform: translation applied). */
        void AddMesh(const float* InPositions, uint32_t InVertexCount, const uint32_t* InIndices, uint32_t InIndexCount,
                     const EngineCore::Matrix4x4& InToWorld);

        /* A box's twelve triangles: InLocal through InToWorld. */
        void AddBox(const GameCore::AABB& InLocal, const EngineCore::Matrix4x4& InToWorld);

        /* An axis-aligned box in world space. */
        void AddBox(const EngineCore::Vector3& InMin, const EngineCore::Vector3& InMax);

        /* One triangle, world space, wound either way. */
        void AddTriangle(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const EngineCore::Vector3& InC);

        /* A quad (two triangles), A B C D around it. */
        void AddQuad(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB,
                     const EngineCore::Vector3& InC, const EngineCore::Vector3& InD);

        /* A heightfield: InResolution^2 samples, row-major (X fastest),
         * InSpacing apart, from InOrigin -- each cell split along (X,Z) to
         * (X+1,Z+1), as GameCore's Terrain and Jolt split it. Only the cells
         * overlapping InClip (if not empty). */
        void AddHeightfield(const float* InHeights, uint32_t InResolution, float InSpacing,
                            const EngineCore::Matrix4x4& InToWorld, const GameCore::AABB& InClip = GameCore::AABB());

        /* Every triangle and area of InOther. */
        void Append(const NavInputGeometry& InOther);

        /* A hash of the triangles and areas: whether a saved navmesh is stale. */
        uint64_t Hash() const;

        void Clear();
    };

    /*
     * The geometry a World's navmesh is built from, inside InBounds (an empty
     * box: everything):
     *
     *   - Static Rigid Bodies on a layer in InSettings.LayerMask: their mesh's
     *     triangles (GameCore's CollisionMeshProvider) when they have one,
     *     else their Bounds box; a Terrain's heightfield through
     *     GameCore's TerrainProvider;
     *   - without physics, static Bounds that carry a Collision component;
     *   - Nav Modifiers, as area boxes;
     *   - Nav Obstacles are NOT geometry: they cut the navmesh at runtime.
     *
     * Entities whose Bounds miss InBounds are skipped (World::QueryBounds).
     */
    AICORE_API void GatherNavGeometry(GameCore::World& InWorld, const NavBuildSettings& InSettings,
                                      const GameCore::AABB& InBounds, NavInputGeometry& OutGeometry);
}

#endif //AICORE_NAVGEOMETRY_H
