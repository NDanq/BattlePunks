#ifndef GAMECORE_COMPONENTS_H
#define GAMECORE_COMPONENTS_H

#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

/*
 * The engine's own built-in components, defined the same way a game defines
 * its own -- GAMECORE_COMPONENT_BODY() first, GAMECORE_DEFINE_COMPONENT(...)
 * once in Components.cpp. Nothing here is special-cased by World or
 * ComponentPool. Every one is GAMECORE_API despite looking like plain POD --
 * see DESIGN.md for why StaticType() makes that exception never apply.
 */
namespace GameCore
{
    /* Position, rotation (Euler degrees) and scale. 64 bytes total (one
     * cache line) -- see DESIGN.md. */
    struct GAMECORE_API TransformComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;   // Euler degrees, applied Z * Y * X -- see DESIGN.md
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};

        /* Translation * (Rz * Ry * Rx) * Scale, relative to the parent if
         * the entity has one -- Entity::GetWorldMatrix() is where it is. */
        EngineCore::Matrix4x4 GetLocalMatrix() const;

        TransformPose GetPose() const
        {
            TransformPose Pose;
            Pose.Position = Position;
            Pose.Rotation = Rotation;
            Pose.Scale    = Scale;
            return Pose;
        }
        void SetPose(const TransformPose& InPose)
        {
            Position = InPose.Position;
            Rotation = InPose.Rotation;
            Scale    = InPose.Scale;
        }

        /* World space, through the entity's parents -- Entity::GetWorldPose.
         * The setters write the local values that land there. */
        EngineCore::Vector3 GetWorldPosition() const { return Owner.GetEntity().GetWorldPose().Position; }
        EngineCore::Vector3 GetWorldRotation() const { return Owner.GetEntity().GetWorldPose().Rotation; }
        EngineCore::Vector3 GetWorldScale() const    { return Owner.GetEntity().GetWorldPose().Scale; }
        void SetWorldPosition(const EngineCore::Vector3& InPosition)
        {
            TransformPose Pose = Owner.GetEntity().GetWorldPose();
            Pose.Position = InPosition;
            Owner.GetEntity().SetWorldPose(Pose);
        }
        void SetWorldRotation(const EngineCore::Vector3& InRotation)
        {
            TransformPose Pose = Owner.GetEntity().GetWorldPose();
            Pose.Rotation = InRotation;
            Owner.GetEntity().SetWorldPose(Pose);
        }
    };

    /* TransformComponent::GetLocalMatrix's composition, for any pose. */
    GAMECORE_API EngineCore::Matrix4x4 PoseToMatrix(const TransformPose& InPose);

    /* A leaf in World's SpatialIndex -- see SpatialIndex.h. */
    struct GAMECORE_API BoundsComponent
    {
        GAMECORE_COMPONENT_BODY()

        AABB Local;    // entity space; the refit pass transforms this
        AABB World;    // world space, as of the last refit -- read only

        /* This entity's leaf in World::GetSpatialIndex(), or -1 until the
         * first refit inserts it. */
        int32_t ProxyID = -1;

        /* Never re-fit after insertion -- for level geometry that never moves. */
        uint8_t bStatic = 0;
    };

    /* Where a CollisionComponent's shape comes from. */
    enum ECollisionSource : uint8_t
    {
        CS_Bounds = 0,   // the entity's BoundsComponent::Local, as authored
        CS_Mesh   = 1,   // the bounds of the sibling RenderComponent's mesh, written into Bounds
    };

    /*
     * "Collision". Overlap detection on the entity's BoundsComponent box (World's
     * spatial index is the broad phase). At the start of TP_PostPhysics -- the
     * boxes were refit at the end of TP_Physics -- World::UpdateOverlaps finds
     * which enabled pairs overlap and reports what changed: OnOverlapBegin and
     * OnOverlapEnd, to C++ (World's delegates) and to each side's script
     * (OnOverlapBegin(self, other)). See DESIGN.md.
     *
     * A overlaps B only if each one's Layer is in the other's Mask. Two
     * static boxes (BoundsComponent::bStatic) are never tested.
     */
    struct GAMECORE_API CollisionComponent
    {
        GAMECORE_COMPONENT_BODY()

        uint8_t bEnabled = 1;           // off: no overlaps; its pairs end
        uint8_t Source   = CS_Bounds;   // ECollisionSource
        int32_t Layer    = 1;           // what this is (bits)
        int32_t Mask     = -1;          // what it overlaps (bits; -1 is everything)

        /* Engine-written, as of the last UpdateOverlaps. */
        int32_t OverlapCount = 0;

        /* The mesh path the CS_Mesh shape was read from, so a changed mesh is
         * re-read. Engine-written. */
        char    ResolvedMesh[128] = {};

        /* Against the overlaps as of the last UpdateOverlaps. */
        bool IsOverlapping(EntityID InOther) const;

        /* "Bounds" or "Mesh". Set ignores an unknown name. */
        const char* GetSourceName() const;
        void        SetSourceName(const char* InName);
    };

    /*
     * Where a CS_Mesh collider's shape comes from. GameCore knows no meshes, so
     * the engine layer installs this (NimbleEngine: the RenderComponent's mesh,
     * through RenderCore's mesh header). Without one, a CS_Mesh collider uses
     * whatever Bounds it has.
     */
    struct CollisionMeshProvider
    {
        /* InEntity's mesh path -- "" for none, which a provider may still
         * shape (a RenderComponent without a mesh draws a unit quad) -- or
         * nullptr if it has nothing to draw at all. */
        const char* (*GetMeshPath)(World& InWorld, EntityID InEntity, void* InUserData) = nullptr;

        /* InPath's bounds in entity space. False if it cannot be read. */
        bool (*GetMeshBounds)(const char* InPath, AABB& OutLocal, void* InUserData) = nullptr;

        void* UserData = nullptr;
    };

    /* Process-wide; null removes it. The installer removes it before its
     * image unloads. */
    GAMECORE_API void SetCollisionMeshProvider(const CollisionMeshProvider* InProvider);
}

#endif //GAMECORE_COMPONENTS_H
