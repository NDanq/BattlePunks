#ifndef WORLDSLAB_WALKER_H
#define WORLDSLAB_WALKER_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/NimbleEngine/Input.h"

namespace GameCore { class World; }

/*
 * RangeMove, the third-person walker (WorldsLabRange.cpp), as the modes that
 * use it see it: the Shooting Range's shooter and Steampunk PvP's fighters.
 *
 * Everything the walker keeps between frames is a component on the avatar,
 * and replicated. Prediction replays a client's unacknowledged input over the
 * server's state when it arrives (Avatar.h), and only components come back
 * with it: state kept anywhere else would have that input applied twice.
 */

/* "RangeMover": the walker's state. Added by RangeMove if the avatar lacks it. */
struct RangeMoverComponent
{
    GAMECORE_COMPONENT_BODY()

    float   BodyYaw   = 0.0f;     // where the body faces (+Z at 0); its Transform adds the mesh's turn
    float   CameraYaw = 0.0f;
    float   Pitch     = 8.0f;     // degrees, down-positive
    float   Scope     = 0.0f;     // 0 over the shoulder, 1 scoped in
    float   VelX = 0.0f, VelY = 0.0f, VelZ = 0.0f;
    float   AirTime   = 0.0f;
    uint8_t bGrounded = 0;
    uint8_t bStarted  = 0;        // has taken its yaw from where it was placed

    /* The camera in the world as of the last step: this process's only, never sent. */
    float   CameraX = 0.0f, CameraY = 0.0f, CameraZ = 0.0f;
};

namespace WorldsLabWalker
{
    /* The view direction of (pitch down-positive, yaw) in degrees: +Z turned. */
    void Forward(double InPitch, double InYaw, double& OutX, double& OutY, double& OutZ);

    double WrapDegrees(double InDegrees);

    GameCore::EntityID FindChild(GameCore::World& InWorld, GameCore::EntityID InParent, const char* InName);

    /* Scales a frame's look axes by [WorldsLab] RangeLookSensitivity. Run on the
     * process that reads the devices (an avatar input filter), so the server
     * replays the same turn rather than applying its own setting. */
    void ApplyLookSensitivity(NimbleEngine::InputSnapshot& InOutInput);

    /* Puts InAvatar at InPosition facing InYaw, at rest: a respawn. Authority's. */
    void PlaceWalker(GameCore::World& InWorld, GameCore::EntityID InAvatar, double InX, double InY, double InZ, double InYaw);
}

#endif //WORLDSLAB_WALKER_H
