#include "Private/JoltPhysics/JoltShapes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/CollisionComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"

#include "Private/JoltPhysics/JoltConvert.h"

namespace JoltPhysics
{
    namespace
    {
        /* Jolt rounds convex shapes' corners by this much (metres), less for
         * anything thinner than twice it. */
        constexpr float DefaultConvexRadius = 0.05f;

        /* A zero-thickness box (the Plane mesh) is given this half thickness
         * (metres), below its surface, so what lands on it stays on it. */
        constexpr float MinHalfExtent = 0.05f;

        enum class EPrimitive { None, Box, Sphere, Capsule, Cylinder, Plane };

        /* The engine's primitive meshes (Engine/Content/Meshes), by file name. */
        EPrimitive PrimitiveOf(const std::string& InPath)
        {
            const char* Prefix = "Engine/Content/Meshes/";
            if(InPath.compare(0, std::strlen(Prefix), Prefix) != 0) { return EPrimitive::None; }
            const std::string Name = InPath.substr(std::strlen(Prefix));
            if(Name == "Box.nmmesh")      { return EPrimitive::Box; }
            if(Name == "Sphere.nmmesh")   { return EPrimitive::Sphere; }
            if(Name == "Capsule.nmmesh")  { return EPrimitive::Capsule; }
            if(Name == "Cylinder.nmmesh") { return EPrimitive::Cylinder; }
            if(Name == "Plane.nmmesh" || Name == "Quad.nmmesh") { return EPrimitive::Plane; }
            return EPrimitive::None;
        }

        float ConvexRadiusFor(const JPH::Vec3 InHalf)
        {
            return std::min(DefaultConvexRadius, 0.5f * InHalf.ReduceMin());
        }

        /* InInner moved to InCenter when it is off the body's origin. */
        JPH::RefConst<JPH::Shape> Offset(const JPH::RefConst<JPH::Shape>& InInner, const JPH::Vec3 InCenter)
        {
            if(!InInner || InCenter.IsNearZero(1.0e-8f)) { return InInner; }
            JPH::RotatedTranslatedShapeSettings Settings(InCenter, JPH::Quat::sIdentity(), InInner);
            JPH::ShapeSettings::ShapeResult Result = Settings.Create();
            return Result.IsValid() ? Result.Get() : InInner;
        }

        JPH::RefConst<JPH::Shape> Make(const JPH::ShapeSettings& InSettings, const char* InWhat)
        {
            JPH::ShapeSettings::ShapeResult Result = InSettings.Create();
            if(Result.HasError())
            {
                CORE_LOG_WARN(std::string("[Physics] could not make a ") + InWhat + ": " + Result.GetError().c_str())
                return nullptr;
            }
            return Result.Get();
        }

        bool IsUniform(const EngineCore::Vector3& InScale)
        {
            const double X = std::abs(double(InScale.GetX())), Y = std::abs(double(InScale.GetY())), Z = std::abs(double(InScale.GetZ()));
            const double Largest = std::max(X, std::max(Y, Z));
            return Largest <= 0.0 || (std::abs(X - Y) <= 1.0e-3 * Largest && std::abs(X - Z) <= 1.0e-3 * Largest);
        }
    }

    bool ShapeBuilder::GetMeshBounds(const std::string& InPath, GameCore::AABB& OutLocal)
    {
        const GameCore::CollisionMeshProvider* Provider = GameCore::GetCollisionMeshProvider();
        return Provider && Provider->GetMeshBounds && Provider->GetMeshBounds(InPath.c_str(), OutLocal, Provider->UserData);
    }

    JPH::RefConst<JPH::Shape> ShapeBuilder::FindMeshShape(const std::string& InPath, const bool bInTriangles)
    {
        const std::string Key = (bInTriangles ? "T:" : "H:") + InPath;
        if(const auto Found = Unscaled.find(Key); Found != Unscaled.end())
        {
            return Found->second;
        }

        JPH::RefConst<JPH::Shape> Shape;
        const GameCore::CollisionMeshProvider* Provider = GameCore::GetCollisionMeshProvider();
        const float*    Positions = nullptr;
        const uint32_t* Indices   = nullptr;
        uint32_t VertexCount = 0, IndexCount = 0;
        if(Provider && Provider->GetMeshGeometry && !InPath.empty()
           && Provider->GetMeshGeometry(InPath.c_str(), Positions, VertexCount, Indices, IndexCount, Provider->UserData))
        {
            /* In the mesh's own units, not metres: a unit mesh in centimetres
             * would be smaller than Jolt's hull tolerances. The ScaledShape
             * over it converts (Build). */
            if(bInTriangles)
            {
                JPH::VertexList Vertices;
                Vertices.reserve(VertexCount);
                for(uint32_t Index = 0; Index < VertexCount; ++Index)
                {
                    Vertices.push_back(JPH::Float3(Positions[Index * 3], Positions[Index * 3 + 1], Positions[Index * 3 + 2]));
                }
                JPH::IndexedTriangleList Triangles;
                Triangles.reserve(IndexCount / 3);
                for(uint32_t Index = 0; Index + 2 < IndexCount; Index += 3)
                {
                    Triangles.push_back(JPH::IndexedTriangle(Indices[Index], Indices[Index + 1], Indices[Index + 2]));
                }
                Shape = Make(JPH::MeshShapeSettings(std::move(Vertices), std::move(Triangles)), ("triangle mesh of " + InPath).c_str());
            }
            else
            {
                JPH::Array<JPH::Vec3> Points;
                Points.reserve(VertexCount);
                for(uint32_t Index = 0; Index < VertexCount; ++Index)
                {
                    Points.push_back(JPH::Vec3(Positions[Index * 3], Positions[Index * 3 + 1], Positions[Index * 3 + 2]));
                }
                Shape = Make(JPH::ConvexHullShapeSettings(Points, DefaultConvexRadius), ("convex hull of " + InPath).c_str());
            }
        }

        Unscaled.emplace(Key, Shape);   // a failure too: it is not retried every frame
        return Shape;
    }

    JPH::RefConst<JPH::Shape> ShapeBuilder::Build(const ShapeRequest& InRequest, std::string& OutKind)
    {
        using namespace GameCore;
        const double ToMeters = 1.0 / InRequest.UnitsPerMeter;
        const EngineCore::Vector3 Scale = InRequest.Scale;
        const EngineCore::Vector3 AbsScale(std::abs(double(Scale.GetX())), std::abs(double(Scale.GetY())), std::abs(double(Scale.GetZ())));

        /* The fitted shapes' box: the mesh's bounds or the Bounds component's. */
        AABB Box;
        bool bHaveBox = false;
        const bool bWantMesh = InRequest.Source == RBSS_Mesh || (InRequest.Source == RBSS_Auto && InRequest.bHasMesh);
        if(bWantMesh && InRequest.bHasMesh)
        {
            bHaveBox = GetMeshBounds(InRequest.MeshPath, Box);
        }
        if(!bHaveBox && InRequest.bHasBounds && !InRequest.BoundsLocal.IsEmpty())
        {
            Box      = InRequest.BoundsLocal;
            bHaveBox = true;
        }

        /* What to build. */
        uint8_t Kind = InRequest.Shape;
        const EPrimitive Primitive = InRequest.bHasMesh ? PrimitiveOf(InRequest.MeshPath) : EPrimitive::None;
        if(Kind == RBS_Auto)
        {
            switch(Primitive)
            {
                case EPrimitive::Box:
                case EPrimitive::Plane:    Kind = RBS_Box; break;
                case EPrimitive::Sphere:   Kind = IsUniform(Scale) ? RBS_Sphere : RBS_ConvexHull; break;
                case EPrimitive::Capsule:  Kind = std::abs(AbsScale.GetX() - AbsScale.GetZ()) <= 1.0e-3 * AbsScale.GetX()
                                                  ? RBS_Capsule : RBS_ConvexHull; break;
                case EPrimitive::Cylinder: Kind = std::abs(AbsScale.GetX() - AbsScale.GetZ()) <= 1.0e-3 * AbsScale.GetX()
                                                  ? RBS_Cylinder : RBS_ConvexHull; break;
                default:
                    if(InRequest.bHasMesh && !InRequest.MeshPath.empty())
                    {
                        Kind = InRequest.Motion == RBM_Dynamic ? RBS_ConvexHull : RBS_TriangleMesh;
                    }
                    else
                    {
                        Kind = RBS_Box;
                    }
                    break;
            }
        }
        if(Kind == RBS_TriangleMesh && InRequest.Motion == RBM_Dynamic)
        {
            CORE_LOG_WARN("[Physics] a Dynamic body cannot be a triangle mesh ('" + InRequest.MeshPath + "'); using its convex hull")
            Kind = RBS_ConvexHull;
        }

        /* From the mesh itself. */
        if(Kind == RBS_ConvexHull || Kind == RBS_TriangleMesh)
        {
            const bool bTriangles = Kind == RBS_TriangleMesh;
            JPH::RefConst<JPH::Shape> Mesh = InRequest.bHasMesh ? FindMeshShape(InRequest.MeshPath, bTriangles) : nullptr;
            if(Mesh)
            {
                OutKind = std::string(bTriangles ? "TriangleMesh " : "ConvexHull ") + InRequest.MeshPath;
                const JPH::Vec3 ScaleV = Convert::ToVec3(Scale, ToMeters);
                if(ScaleV.IsClose(JPH::Vec3::sOne(), 1.0e-12f)) { return Mesh; }
                JPH::ScaledShapeSettings Scaled(Mesh, ScaleV);
                return Make(Scaled, "scaled mesh shape");
            }
            /* Nothing to read: the box it would have been fitted into. */
            Kind = RBS_Box;
        }

        if(!bHaveBox)
        {
            CORE_LOG_WARN("[Physics] a Rigid Body has nothing to shape it: no Render component mesh and no Bounds")
            return nullptr;
        }

        /* Fitted to the box, in metres, scaled. */
        JPH::Vec3 Half = Convert::ToVec3(Box.Extent(), 0.5 * ToMeters) * Convert::ToVec3(AbsScale);
        JPH::Vec3 Center = Convert::ToVec3(Box.Center(), ToMeters) * Convert::ToVec3(Scale);

        switch(Kind)
        {
            case RBS_Sphere:
            {
                const float Radius = std::max(Half.ReduceMax(), MinHalfExtent);
                OutKind = "Sphere";
                return Offset(Make(JPH::SphereShapeSettings(Radius), "sphere"), Center);
            }
            case RBS_Capsule:
            {
                const float Radius     = std::max(std::max(Half.GetX(), Half.GetZ()), MinHalfExtent);
                const float HalfHeight = Half.GetY() - Radius;
                if(HalfHeight <= 1.0e-3f)
                {
                    OutKind = "Sphere";
                    return Offset(Make(JPH::SphereShapeSettings(std::max(Radius, Half.GetY())), "sphere"), Center);
                }
                OutKind = "Capsule";
                return Offset(Make(JPH::CapsuleShapeSettings(HalfHeight, Radius), "capsule"), Center);
            }
            case RBS_Cylinder:
            {
                const float Radius     = std::max(std::max(Half.GetX(), Half.GetZ()), MinHalfExtent);
                const float HalfHeight = std::max(Half.GetY(), MinHalfExtent);
                OutKind = "Cylinder";
                return Offset(Make(JPH::CylinderShapeSettings(HalfHeight, Radius,
                                                              std::min(DefaultConvexRadius, 0.5f * std::min(HalfHeight, Radius))),
                                   "cylinder"), Center);
            }
            default:
            {
                /* A flat axis (a plane) thickens below its surface. */
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    if(Half[Axis] < MinHalfExtent)
                    {
                        const float Grow = MinHalfExtent - Half[Axis];
                        Half.SetComponent(Axis, MinHalfExtent);
                        Center.SetComponent(Axis, Center[Axis] - Grow);
                    }
                }
                OutKind = "Box";
                return Offset(Make(JPH::BoxShapeSettings(Half, ConvexRadiusFor(Half)), "box"), Center);
            }
        }
    }
}
