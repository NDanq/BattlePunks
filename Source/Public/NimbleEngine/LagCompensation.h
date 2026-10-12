#ifndef NIMBLEENGINE_LAGCOMPENSATION_H
#define NIMBLEENGINE_LAGCOMPENSATION_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * LAG COMPENSATION: the server judging a client's shot against what that
 * client saw.
 *
 * A client shows other players a little in the past -- its interpolation
 * delay, and the time their state took to reach it -- and its shot takes as
 * long again to reach the server. Tested against where the targets are now,
 * a shot at a runner misses behind it.
 *
 * So, on the server, every entity with a LagCompensated component has a
 * second of its positions kept (the host records them each frame). Each
 * Server RPC from a client says what time it was showing
 * (RpcContext::ViewTimeMs, stamped by NetCore), and a test made in its
 * handler looks there:
 *
 *     void ShipComponent::ServerFire(const GameCore::RpcContext& InCall, const Vector3& InOrigin, const Vector3& InDir)
 *     {
 *         NimbleEngine::LagCompensatedHit Hit;
 *         if(NimbleEngine::LagCompensatedRaycast(*InCall.OwningWorld, InCall, InOrigin, InDir, 8000.0f, Hit)
 *            && Hit.bCompensated)
 *         {
 *             const bool bHead = Hit.Height > 150.0f;   // above the target's feet
 *             ...
 *         }
 *     }
 *
 * The street is tested as it is (PhysicsRaycast); only the compensated
 * entities move back, each as a standing capsule. Keep them out of the
 * world mask (a Rigid Body layer of their own), or a body's current place
 * is hit first. A local call -- the host's own player, a bot -- has no view
 * time and tests now. Without networking it is all simply now.
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    /* "LagCompensated": a standing capsule, in the entity's world space
     * (its Transform's position, up +Y, unrotated), that a server tests
     * where a client saw it. */
    struct NE_API LagCompensatedComponent
    {
        GAMECORE_COMPONENT_BODY()

        float   Radius   = 34.0f;
        float   Bottom   = 0.0f;     // the capsule's lowest point, over the entity's origin
        float   Top      = 180.0f;   // and its highest
        uint8_t bEnabled = 1;
    };

    struct LagCompensatedHit
    {
        GameCore::EntityID  Entity;              // what was hit; unset for a world hit without one
        float               Distance = 0.0f;
        EngineCore::Vector3 Point;
        bool                bCompensated = false;   // a LagCompensated capsule, not the world
        float               Height       = 0.0f;    // a capsule hit: how far above that entity's origin, then
    };

    /*
     * The nearest hit from InOrigin along InDirection, within InMaxDistance:
     * the world (PhysicsRaycast, layers in InWorldMask) or a LagCompensated
     * capsule where InCall's sender saw it -- at InCall.ViewTimeMs, or now
     * for a local call. InCall's own entity, and its children, are skipped.
     */
    NE_API bool LagCompensatedRaycast(GameCore::World& InWorld, const GameCore::RpcContext& InCall, const EngineCore::Vector3& InOrigin,
                                      const EngineCore::Vector3& InDirection, float InMaxDistance, LagCompensatedHit& OutHit,
                                      int32_t InWorldMask = -1);

    /* The same at an explicit server time (0: now), skipping InIgnore. */
    NE_API bool LagCompensatedRaycastAt(GameCore::World& InWorld, uint32_t InTimeMs, GameCore::EntityID InIgnore,
                                        const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                                        float InMaxDistance, LagCompensatedHit& OutHit, int32_t InWorldMask = -1);

    /* Where InEntity stood at server time InTimeMs (0: now), from its kept
     * positions -- for a test of the game's own. False for an entity with
     * no LagCompensated component. */
    NE_API bool GetLagCompensatedPosition(GameCore::World& InWorld, GameCore::EntityID InEntity, uint32_t InTimeMs,
                                          EngineCore::Vector3& OutPosition);

    // === The host's ===========================================================

    /* Every enabled LagCompensated entity's position, at server time
     * InTimeMs; a second is kept. NetHost calls it each server frame. */
    NE_API void RecordLagCompensation(GameCore::World& InWorld, uint32_t InTimeMs);

    /* Forgets InWorld's positions: before it unloads. */
    NE_API void ClearLagCompensation(const GameCore::World& InWorld);
}

#endif //NIMBLEENGINE_LAGCOMPENSATION_H
