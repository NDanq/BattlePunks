#ifndef JOLTPHYSICS_JOLTSHAPES_H
#define JOLTPHYSICS_JOLTSHAPES_H

#include <cstdint>
#include <map>
#include <string>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"

/*
 * A Rigid Body's collision shape, from what its entity has -- the rules
 * RigidBodyComponent.h documents:
 *
 *   Auto, on an engine primitive mesh   that primitive, fitted to the mesh's bounds
 *   Auto, on another mesh               its triangles (Static, Kinematic) or hull (Dynamic)
 *   Auto, no mesh                       a box, the BoundsComponent's
 *   Box / Sphere / Capsule / Cylinder   fitted to the ShapeSource's box
 *   ConvexHull / TriangleMesh           the mesh's vertices / triangles
 *
 * Everything is in the body's space, in metres, with the entity's world scale
 * applied. Hulls and triangle meshes are built once per mesh, in its own
 * units, and shared through a ScaledShape (world scale / UnitsPerMeter).
 */
namespace JoltPhysics
{
    /* What the shape is made from, gathered by PhysicsWorld from the entity. */
    struct ShapeRequest
    {
        uint8_t Shape  = 0;   // GameCore::ERigidBodyShape
        uint8_t Source = 0;   // GameCore::ERigidBodyShapeSource
        uint8_t Motion = 0;   // GameCore::ERigidBodyMotion

        /* A RenderComponent's mesh: bHasMesh with MeshPath ("" is its unit quad). */
        bool        bHasMesh = false;
        std::string MeshPath;

        bool          bHasBounds = false;
        GameCore::AABB BoundsLocal;

        EngineCore::Vector3 Scale = EngineCore::Vector3(1.0, 1.0, 1.0);   // world scale
        double UnitsPerMeter = 100.0;
    };

    class ShapeBuilder
    {
    public:
        /* InRequest's shape, or null (logged) when there is nothing to shape it
         * from. OutKind names what was built ("Box", "ConvexHull <mesh>"...). */
        JPH::RefConst<JPH::Shape> Build(const ShapeRequest& InRequest, std::string& OutKind);

        /* The hulls and triangle meshes made so far. */
        void Clear() { Unscaled.clear(); }

    private:
        /* InPath's hull (bInTriangles false) or triangles, in the mesh's own
         * units (Build scales them to metres); null if unreadable. */
        JPH::RefConst<JPH::Shape> FindMeshShape(const std::string& InPath, bool bInTriangles);

        /* InPath's bounds from the mesh provider. */
        static bool GetMeshBounds(const std::string& InPath, GameCore::AABB& OutLocal);

        std::map<std::string, JPH::RefConst<JPH::Shape>> Unscaled;   // "H:" / "T:" + path
    };
}

#endif //JOLTPHYSICS_JOLTSHAPES_H
