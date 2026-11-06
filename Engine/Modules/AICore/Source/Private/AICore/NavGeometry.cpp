#include "Public/AICore/NavGeometry.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "Private/AICore/NavInternal.h"
#include "Public/AICore/Components/NavModifierComponent.h"
#include "Public/GameCore/Components/BoundsComponent.h"
#include "Public/GameCore/Components/CollisionComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"

/* NavInputGeometry and GatherNavGeometry: NavGeometry.h has the rules. */
namespace AICore
{
    GameCore::AABB NavInputGeometry::GetBounds() const
    {
        GameCore::AABB Bounds;
        for(size_t Index = 0; Index + 2 < Vertices.size(); Index += 3)
        {
            const EngineCore::Vector3 Point(Vertices[Index], Vertices[Index + 1], Vertices[Index + 2]);
            Bounds = GameCore::AABB::Union(Bounds, GameCore::AABB(Point, Point));
        }
        return Bounds;
    }

    void NavInputGeometry::AddMesh(const float* InPositions, const uint32_t InVertexCount, const uint32_t* InIndices,
                                   const uint32_t InIndexCount, const EngineCore::Matrix4x4& InToWorld)
    {
        const int32_t Base = int32_t(GetVertexCount());
        Vertices.reserve(Vertices.size() + size_t(InVertexCount) * 3);
        for(uint32_t Vertex = 0; Vertex < InVertexCount; ++Vertex)
        {
            const EngineCore::Vector3 World = NavDetail::TransformPoint(InToWorld, InPositions[Vertex * 3], InPositions[Vertex * 3 + 1],
                                                                        InPositions[Vertex * 3 + 2]);
            Vertices.push_back(float(World.GetX()));
            Vertices.push_back(float(World.GetY()));
            Vertices.push_back(float(World.GetZ()));
        }
        Indices.reserve(Indices.size() + InIndexCount);
        for(uint32_t Index = 0; Index + 2 < InIndexCount; Index += 3)
        {
            if(InIndices[Index] >= InVertexCount || InIndices[Index + 1] >= InVertexCount || InIndices[Index + 2] >= InVertexCount) { continue; }
            Indices.push_back(Base + int32_t(InIndices[Index]));
            Indices.push_back(Base + int32_t(InIndices[Index + 1]));
            Indices.push_back(Base + int32_t(InIndices[Index + 2]));
        }
    }

    void NavInputGeometry::AddBox(const GameCore::AABB& InLocal, const EngineCore::Matrix4x4& InToWorld)
    {
        if(InLocal.IsEmpty()) { return; }
        const float X0 = float(InLocal.Min.GetX()), Y0 = float(InLocal.Min.GetY()), Z0 = float(InLocal.Min.GetZ());
        const float X1 = float(InLocal.Max.GetX()), Y1 = float(InLocal.Max.GetY()), Z1 = float(InLocal.Max.GetZ());
        const float Corners[8 * 3] = {
            X0, Y0, Z0,  X1, Y0, Z0,  X1, Y0, Z1,  X0, Y0, Z1,
            X0, Y1, Z0,  X1, Y1, Z0,  X1, Y1, Z1,  X0, Y1, Z1,
        };
        /* Wound so the top faces up (Recast's walkable test reads the normal). */
        const uint32_t Faces[36] = {
            4, 6, 5,  4, 7, 6,     // top
            0, 1, 2,  0, 2, 3,     // bottom
            0, 4, 5,  0, 5, 1,     // -Z
            3, 2, 6,  3, 6, 7,     // +Z
            0, 3, 7,  0, 7, 4,     // -X
            1, 5, 6,  1, 6, 2,     // +X
        };
        AddMesh(Corners, 8, Faces, 36, InToWorld);
    }

    void NavInputGeometry::AddBox(const EngineCore::Vector3& InMin, const EngineCore::Vector3& InMax)
    {
        AddBox(GameCore::AABB(InMin, InMax), EngineCore::Matrix4x4());
    }

    void NavInputGeometry::AddTriangle(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const EngineCore::Vector3& InC)
    {
        /* Recast wants counter-clockwise from above for an upward normal:
         * flip a triangle that faces down. */
        const EngineCore::Vector3 AB = InB - InA, AC = InC - InA;
        const double NormalY = double(AB.GetZ()) * double(AC.GetX()) - double(AB.GetX()) * double(AC.GetZ());
        const float Points[9] = { float(InA.GetX()), float(InA.GetY()), float(InA.GetZ()),
                                  float(InB.GetX()), float(InB.GetY()), float(InB.GetZ()),
                                  float(InC.GetX()), float(InC.GetY()), float(InC.GetZ()) };
        const uint32_t Up[3] = { 0, 1, 2 }, Down[3] = { 0, 2, 1 };
        AddMesh(Points, 3, NormalY >= 0.0 ? Up : Down, 3, EngineCore::Matrix4x4());
    }

    void NavInputGeometry::AddQuad(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB,
                                   const EngineCore::Vector3& InC, const EngineCore::Vector3& InD)
    {
        AddTriangle(InA, InB, InC);
        AddTriangle(InA, InC, InD);
    }

    void NavInputGeometry::AddHeightfield(const float* InHeights, const uint32_t InResolution, const float InSpacing,
                                          const EngineCore::Matrix4x4& InToWorld, const GameCore::AABB& InClip)
    {
        if(!InHeights || InResolution < 2) { return; }
        const int32_t Base = int32_t(GetVertexCount());
        /* Every sample, then the cells -- clipped by their world position. */
        std::vector<int32_t> Remap(size_t(InResolution) * InResolution, -1);
        const auto Vertex = [&](const uint32_t InX, const uint32_t InZ) -> int32_t
        {
            const size_t Sample = size_t(InZ) * InResolution + InX;
            if(Remap[Sample] < 0)
            {
                const EngineCore::Vector3 World = NavDetail::TransformPoint(InToWorld, float(InX) * InSpacing, InHeights[Sample], float(InZ) * InSpacing);
                Remap[Sample] = int32_t(GetVertexCount());
                Vertices.push_back(float(World.GetX()));
                Vertices.push_back(float(World.GetY()));
                Vertices.push_back(float(World.GetZ()));
            }
            return Remap[Sample];
        };
        (void)Base;
        for(uint32_t Z = 0; Z + 1 < InResolution; ++Z)
        {
            for(uint32_t X = 0; X + 1 < InResolution; ++X)
            {
                if(!InClip.IsEmpty())
                {
                    const EngineCore::Vector3 A = NavDetail::TransformPoint(InToWorld, float(X) * InSpacing, 0.0f, float(Z) * InSpacing);
                    const EngineCore::Vector3 B = NavDetail::TransformPoint(InToWorld, float(X + 1) * InSpacing, 0.0f, float(Z + 1) * InSpacing);
                    const float MinX = std::min(float(A.GetX()), float(B.GetX())), MaxX = std::max(float(A.GetX()), float(B.GetX()));
                    const float MinZ = std::min(float(A.GetZ()), float(B.GetZ())), MaxZ = std::max(float(A.GetZ()), float(B.GetZ()));
                    if(MaxX < float(InClip.Min.GetX()) || MinX > float(InClip.Max.GetX())
                       || MaxZ < float(InClip.Min.GetZ()) || MinZ > float(InClip.Max.GetZ())) { continue; }
                }
                /* Split along (X, Z) -> (X + 1, Z + 1), as Terrain.h and Jolt split it. */
                const int32_t V00 = Vertex(X, Z), V10 = Vertex(X + 1, Z), V01 = Vertex(X, Z + 1), V11 = Vertex(X + 1, Z + 1);
                /* Counter-clockwise from above (+Y): 00 -> 11 -> 10 and 00 -> 01 -> 11. */
                Indices.push_back(V00); Indices.push_back(V11); Indices.push_back(V10);
                Indices.push_back(V00); Indices.push_back(V01); Indices.push_back(V11);
            }
        }
    }

    void NavInputGeometry::Append(const NavInputGeometry& InOther)
    {
        const int32_t Base = int32_t(GetVertexCount());
        Vertices.insert(Vertices.end(), InOther.Vertices.begin(), InOther.Vertices.end());
        Indices.reserve(Indices.size() + InOther.Indices.size());
        for(const int32_t Index : InOther.Indices) { Indices.push_back(Base + Index); }
        Areas.insert(Areas.end(), InOther.Areas.begin(), InOther.Areas.end());
    }

    uint64_t NavInputGeometry::Hash() const
    {
        uint64_t Hash = NavDetail::HASH_SEED;
        /* Quantised to a millimetre, so float noise from a matrix does not
         * make a navmesh look stale. */
        for(const float Value : Vertices)
        {
            const int64_t Q = int64_t(std::llround(double(Value) * 10.0));
            NavDetail::HashBytes(Hash, &Q, sizeof(Q));
        }
        NavDetail::HashBytes(Hash, Indices.data(), Indices.size() * sizeof(int32_t));
        for(const NavAreaBox& Box : Areas)
        {
            const float Values[] = { float(Box.Center.GetX()), float(Box.Center.GetY()), float(Box.Center.GetZ()),
                                     float(Box.HalfExtents.GetX()), float(Box.HalfExtents.GetY()), float(Box.HalfExtents.GetZ()), Box.Yaw };
            for(const float Value : Values)
            {
                const int64_t Q = int64_t(std::llround(double(Value) * 10.0));
                NavDetail::HashBytes(Hash, &Q, sizeof(Q));
            }
            NavDetail::HashBytes(Hash, &Box.Area, 1);
        }
        return Hash;
    }

    void NavInputGeometry::Clear()
    {
        Vertices.clear();
        Indices.clear();
        Areas.clear();
    }

    // === From a World =========================================================

    namespace
    {
        /* The entity's world box from its Bounds, if it has one. */
        bool WorldBox(GameCore::World& InWorld, const GameCore::EntityID InEntity, GameCore::AABB& OutBox)
        {
            const GameCore::BoundsComponent* Bounds = InWorld.GetEntity(InEntity).GetComponent<GameCore::BoundsComponent>();
            if(!Bounds) { return false; }
            if(!Bounds->World.IsEmpty()) { OutBox = Bounds->World; return true; }
            if(Bounds->Local.IsEmpty()) { return false; }
            OutBox = GameCore::AABB::Transformed(Bounds->Local, InWorld.GetEntity(InEntity).GetWorldMatrix());
            return true;
        }

        bool Overlaps2D(const GameCore::AABB& InA, const GameCore::AABB& InB)
        {
            return InA.Min.GetX() <= InB.Max.GetX() && InA.Max.GetX() >= InB.Min.GetX()
                && InA.Min.GetZ() <= InB.Max.GetZ() && InA.Max.GetZ() >= InB.Min.GetZ();
        }

        /* The mesh's triangles or a box, as physics shapes it. */
        void AddShape(GameCore::World& InWorld, const GameCore::EntityID InEntity, const uint8_t InShape, const uint8_t InShapeSource,
                      NavInputGeometry& OutGeometry)
        {
            const EngineCore::Matrix4x4 ToWorld = InWorld.GetEntity(InEntity).GetWorldMatrix();
            const GameCore::CollisionMeshProvider* Meshes = GameCore::GetCollisionMeshProvider();
            const char* MeshPath = (Meshes && Meshes->GetMeshPath) ? Meshes->GetMeshPath(InWorld, InEntity, Meshes->UserData) : nullptr;
            const bool bHasMesh = MeshPath && MeshPath[0] != '\0';

            const bool bTriangles = InShape == GameCore::RBS_Auto || InShape == GameCore::RBS_TriangleMesh || InShape == GameCore::RBS_ConvexHull;
            if(bTriangles && bHasMesh && Meshes->GetMeshGeometry)
            {
                const float* Positions = nullptr;
                const uint32_t* Indices = nullptr;
                uint32_t VertexCount = 0, IndexCount = 0;
                if(Meshes->GetMeshGeometry(MeshPath, Positions, VertexCount, Indices, IndexCount, Meshes->UserData) && Positions && Indices)
                {
                    OutGeometry.AddMesh(Positions, VertexCount, Indices, IndexCount, ToWorld);
                    return;
                }
            }

            /* A fitted shape, or a mesh that cannot be read: its box. */
            GameCore::AABB Local;
            if(bHasMesh && InShapeSource != GameCore::RBSS_Bounds && Meshes->GetMeshBounds)
            {
                Meshes->GetMeshBounds(MeshPath, Local, Meshes->UserData);
            }
            if(Local.IsEmpty())
            {
                if(const GameCore::BoundsComponent* Bounds = InWorld.GetEntity(InEntity).GetComponent<GameCore::BoundsComponent>())
                {
                    Local = Bounds->Local;
                }
            }
            OutGeometry.AddBox(Local, ToWorld);
        }

        bool AddTerrain(GameCore::World& InWorld, const GameCore::EntityID InEntity, const GameCore::AABB& InClip, NavInputGeometry& OutGeometry)
        {
            const GameCore::TerrainProvider* Terrains = GameCore::GetTerrainProvider();
            if(!Terrains || !Terrains->FindTerrain) { return false; }
            const std::shared_ptr<GameCore::TerrainData> Terrain = Terrains->FindTerrain(InWorld, InEntity, Terrains->UserData);
            if(!Terrain || Terrain->IsEmpty()) { return false; }
            std::lock_guard<std::mutex> Lock(Terrain->GetLock());
            OutGeometry.AddHeightfield(Terrain->GetHeights(), Terrain->GetResolution(), Terrain->GetSpacing(),
                                       InWorld.GetEntity(InEntity).GetWorldMatrix(), InClip);
            return true;
        }
    }

    void GatherNavGeometry(GameCore::World& InWorld, const NavBuildSettings& InSettings, const GameCore::AABB& InBounds,
                           NavInputGeometry& OutGeometry)
    {
        std::unordered_set<uint64_t> Done;

        /* Static bodies: what physics walks on. */
        InWorld.Each<GameCore::RigidBodyComponent>([&](const GameCore::Entity& InEntity, GameCore::RigidBodyComponent& InBody)
        {
            if(!InBody.bEnabled || InBody.bSensor || InBody.Motion != GameCore::RBM_Static) { return; }
            if((InBody.Layer & InSettings.LayerMask) == 0) { return; }
            const GameCore::EntityID Id = InEntity.GetID();
            GameCore::AABB Box;
            if(!InBounds.IsEmpty() && WorldBox(InWorld, Id, Box) && !Overlaps2D(Box, InBounds)) { return; }
            Done.insert(Id.Pack());
            if((InBody.Shape == GameCore::RBS_Auto || InBody.Shape == GameCore::RBS_HeightField)
               && AddTerrain(InWorld, Id, InBounds, OutGeometry))
            {
                return;
            }
            AddShape(InWorld, Id, InBody.Shape, InBody.ShapeSource, OutGeometry);
        });

        /* Without physics (or bodies left off): static Bounds that collide,
         * and terrains with no body at all. */
        InWorld.Each<GameCore::CollisionComponent>([&](const GameCore::Entity& InEntity, GameCore::CollisionComponent& InCollision)
        {
            const GameCore::EntityID Id = InEntity.GetID();
            if(!InCollision.bEnabled || Done.count(Id.Pack()) || InEntity.HasComponent<GameCore::RigidBodyComponent>()) { return; }
            const GameCore::BoundsComponent* Bounds = InEntity.GetComponent<GameCore::BoundsComponent>();
            if(!Bounds || !Bounds->bStatic) { return; }
            if((InCollision.Layer & InSettings.LayerMask) == 0) { return; }
            GameCore::AABB Box;
            if(!InBounds.IsEmpty() && WorldBox(InWorld, Id, Box) && !Overlaps2D(Box, InBounds)) { return; }
            Done.insert(Id.Pack());
            AddShape(InWorld, Id, GameCore::RBS_Auto, GameCore::RBSS_Auto, OutGeometry);
        });

        if(const GameCore::TerrainProvider* Terrains = GameCore::GetTerrainProvider(); Terrains && Terrains->FindTerrain)
        {
            InWorld.Each<GameCore::TransformComponent>([&](const GameCore::Entity& InEntity, GameCore::TransformComponent&)
            {
                const GameCore::EntityID Id = InEntity.GetID();
                if(Done.count(Id.Pack()) || InEntity.HasComponent<GameCore::RigidBodyComponent>()) { return; }
                if(AddTerrain(InWorld, Id, InBounds, OutGeometry)) { Done.insert(Id.Pack()); }
            });
        }

        /* Modifiers mark what the geometry made. */
        InWorld.Each<NavModifierComponent>([&](const GameCore::Entity& InEntity, NavModifierComponent& InModifier)
        {
            const GameCore::TransformPose Pose = InEntity.GetWorldPose();
            NavAreaBox Box;
            Box.Center = Pose.Position;
            Box.HalfExtents = EngineCore::Vector3(std::fabs(float(InModifier.HalfExtents.GetX() * Pose.Scale.GetX())),
                                                  std::fabs(float(InModifier.HalfExtents.GetY() * Pose.Scale.GetY())),
                                                  std::fabs(float(InModifier.HalfExtents.GetZ() * Pose.Scale.GetZ())));
            Box.Yaw = float(Pose.Rotation.GetY());
            Box.Area = InModifier.GetAreaId();
            OutGeometry.Areas.push_back(Box);
        });
    }
}
