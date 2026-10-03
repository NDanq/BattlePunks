#ifndef GAMECORE_COLLISIONCOMPONENT_H
#define GAMECORE_COLLISIONCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
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
     * image unloads. Defined in Collision.cpp, beside the pass that uses it. */
    GAMECORE_API void SetCollisionMeshProvider(const CollisionMeshProvider* InProvider);
}

#endif //GAMECORE_COLLISIONCOMPONENT_H
