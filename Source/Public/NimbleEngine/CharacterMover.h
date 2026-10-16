#ifndef NIMBLEENGINE_CHARACTERMOVER_H
#define NIMBLEENGINE_CHARACTERMOVER_H

#include <cstdint>

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * THE CHARACTER MOVER: a predicted third-person walker any avatar can use.
 * AVATARS.md ("Character Mover") has the model; Engine/Docs/Guides/
 * CharacterMoverGuide.md the setup.
 *
 *   Character Mover  "Character Mover", on the avatar: its tunables (speeds,
 *                    jump, turn, the body's size, the camera's offsets), the
 *                    names of the actions and axes it reads, the anim params
 *                    it sets, and its state.
 *
 *   CharacterMove    the avatar simulation: an AvatarHarness naming it in
 *                    Simulation is walked by it from each frame of input --
 *                    on the server, and at once on a predicting client.
 *
 * It walks on the world's physics (PhysicsRaycast): rays down round the feet
 * find the highest walkable surface within a step, rays across slide it along
 * walls, a ray up stops a jump. Without physics it walks on Bounds.
 *
 * A bot drives it the same way a player does: AICore's SteerInput fills the
 * same MoveForward / MoveRight / LookX axes (SteerInputMap, AIFacade.h).
 *
 * Everything the step keeps between frames is in the component and
 * replicated: prediction replays a client's input over the server's state,
 * and only components come back with it.
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    /* "Character Mover". Authored (saved), state (saved and replicated),
     * and this process's camera (neither). */
    struct NE_API CharacterMoverComponent
    {
        GAMECORE_COMPONENT_BODY()

        // === Movement (units/s) =================================================
        float WalkSpeed   = 165.0f;
        float RunSpeed    = 400.0f;    // Run held
        float CrouchSpeed = 90.0f;     // Crouch held
        float AimSpeed    = 140.0f;    // Aim held
        float Accel       = 1500.0f;   // toward the speed wanted, units/s^2
        float Decel       = 2000.0f;   // to a stop
        float AirControl  = 0.25f;     // of Accel/Decel in the air

        // === Jumping ============================================================
        float JumpSpeed = 400.0f;
        float Gravity   = 981.0f;

        // === Turning and the view (degrees) ======================================
        float   TurnRate  = 600.0f;     // the body, per second
        float   PitchMin  = -60.0f;     // up (down-positive)
        float   PitchMax  = 70.0f;
        float   ScopeLook = 0.55f;      // look axes scaled by this when aimed in
        float   ScopeRate = 7.0f;       // the aim blend's speed, 1/s
        float   MeshYaw   = 180.0f;     // the body's Transform turned from where it faces (180: a mesh modelled facing -Z)
        uint8_t bFaceView = 0;          // 1: the body always faces the view; 0: where it walks (and the view while aiming)

        // === Body ===============================================================
        float   Radius       = 30.0f;
        float   Height       = 180.0f;
        float   CrouchHeight = 110.0f;
        float   StepHeight   = 45.0f;   // the highest ledge walked up
        float   SnapDown     = 35.0f;   // the lowest drop walked down without falling
        float   MaxSlope     = 56.0f;   // degrees; steeper is a wall
        int32_t Mask         = -1;      // the physics layers it stands on and slides along (bits)

        // === Camera (the CameraBoom child and its Camera) ==========================
        float BoomHeight  = 150.0f;
        float CameraBack  = 230.0f;
        float CameraRight = 42.0f;
        float CameraUp    = 12.0f;
        float ScopeBack   = 110.0f;   // aimed in: close over the right shoulder
        float ScopeRight  = 58.0f;
        float ScopeUp     = 16.0f;
        float FovNormal   = 62.0f;
        float FovScope    = 40.0f;

        // === Input: names in the harness's InputMap ("" unused) ====================
        char MoveForward[INPUT_NAME_MAX] = "MoveForward";   // axes, -1..1
        char MoveRight[INPUT_NAME_MAX]   = "MoveRight";
        char LookX[INPUT_NAME_MAX]       = "LookX";         // degrees this frame
        char LookY[INPUT_NAME_MAX]       = "LookY";         // +down
        char RunAction[INPUT_NAME_MAX]    = "Run";          // actions
        char JumpAction[INPUT_NAME_MAX]   = "Jump";
        char CrouchAction[INPUT_NAME_MAX] = "Crouch";
        char AimAction[INPUT_NAME_MAX]    = "Aim";

        // === Animation: params of its Animation component ("" not set) ============
        char AnimSpeed[INPUT_NAME_MAX]    = "Speed";      // float, units/s across the ground
        char AnimGrounded[INPUT_NAME_MAX] = "Grounded";   // bool
        char AnimAimPitch[INPUT_NAME_MAX] = "AimPitch";   // float, degrees up-positive
        char AnimJump[INPUT_NAME_MAX]     = "Jump";       // trigger
        char AnimCrouch[INPUT_NAME_MAX]   = "";           // bool

        // === State: saved and replicated ===========================================
        float   BodyYaw   = 0.0f;     // where the body faces (+Z at 0)
        float   CameraYaw = 0.0f;     // the view
        float   Pitch     = 8.0f;     // degrees, down-positive
        float   Scope     = 0.0f;     // 0 over the shoulder, 1 aimed in
        float   VelX = 0.0f, VelY = 0.0f, VelZ = 0.0f;
        float   AirTime   = 0.0f;     // seconds off the ground
        uint8_t JumpCount = 0;        // one more each jump: the Jump trigger fires from it
        uint8_t bGrounded = 0;
        uint8_t bCrouched = 0;
        uint8_t bStarted  = 0;        // has taken its yaw from where it was placed

        // === This process's: never saved or sent ==================================
        float   CameraX = 0.0f, CameraY = 0.0f, CameraZ = 0.0f;   // the camera in the world, as last placed
        uint8_t bCameraPlaced = 0;
        uint8_t SeenJumpCount = 0;    // the JumpCount the anim trigger last fired for
    };

    /*
     * One frame of InAvatar's walk: what the CharacterMove simulation runs.
     * InAvatar needs a Transform; the Character Mover is added if missing,
     * and its bindings are read through InAvatar's AvatarHarness InputMap.
     * Call it from a game's own simulation to add to it.
     */
    NE_API void StepCharacter(GameCore::World& InWorld, GameCore::EntityID InAvatar, const AvatarInputFrame& InFrame);

    /* InAvatar at InPosition facing InYaw (degrees, +Z at 0), at rest: a
     * respawn. Authority's; replicates. Adds the Character Mover if missing. */
    NE_API void TeleportCharacter(GameCore::World& InWorld, GameCore::EntityID InAvatar, const EngineCore::Vector3& InPosition,
                                  float InYaw);

    /*
     * Where InAvatar aims: OutOrigin the camera's place without its pull-in
     * from walls (the boom's pivot plus the aim-blended offsets), OutDir the
     * view. The same on every process from the replicated state, so a server
     * traces the shot its client saw. False without a Character Mover or a
     * Transform.
     */
    NE_API bool GetCharacterAim(GameCore::World& InWorld, GameCore::EntityID InAvatar, EngineCore::Vector3& OutOrigin,
                                EngineCore::Vector3& OutDir);

    /*
     * Every Character Mover's presentation: its Animation params from its
     * state (on every process, so a client's view of another player walks as
     * the server sent it), and its CameraBoom child turned to the view, the
     * boom's Camera kept out of walls where it is locally controlled.
     * GameSession runs it as "Engine.CharacterMover" (TP_PostPhysics).
     */
    NE_API void UpdateCharacterMovers(GameCore::World& InWorld, float InSeconds);
}

#endif //NIMBLEENGINE_CHARACTERMOVER_H
