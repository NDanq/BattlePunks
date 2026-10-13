#ifndef NIMBLEENGINE_AIFACADE_H
#define NIMBLEENGINE_AIFACADE_H

#include <cstdint>
#include <string>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * AI for C++ game code, without the AICore module's headers. Every level's
 * World on an authority (standalone or a server; never a client, whose
 * avatars follow the server's) has an AIWorld: its Nav Mesh Volume's
 * navmesh, its Nav Agents' crowd, its AI Brains and Perception
 * (Engine/Modules/AICore/README.md). With AICore compiled out
 * (NIMBLE_WITH_AICORE=OFF) IsAIAvailable() is false and the calls below do
 * nothing, returning false.
 *
 * The engine installs AICore's host: line of sight and ground height
 * through PhysicsRaycast (PhysicsFacade.h), and animation parameters
 * through RenderCore's Animation component.
 *
 * AI.ShowNavMesh / AI.ShowPaths ([AI] in the config) draw the level's
 * navmesh and its agents' paths over the game (editor builds).
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    NE_API bool IsAIAvailable();

    /*
     * How an avatar's input map moves it: which axes walk and turn it, and
     * how fast it turns. SteerInput fills these from a Nav Agent's desired
     * velocity. Look axes take degrees a frame (as WorldsLab's walkers read
     * them); move axes -1..1, relative to the avatar's yaw (0 faces +Z).
     */
    struct SteerInputMap
    {
        int32_t  MoveForward = -1;
        int32_t  MoveRight   = -1;
        int32_t  LookX       = -1;            // yaw
        uint32_t RunBits     = 0;             // held while it runs
        float    TurnDegreesPerSecond = 300.0f;
        float    RunSpeed    = 300.0f;        // desired speeds above this run
        float    WalkSpeed   = 150.0f;        // the speed MoveForward 1 walks at
        float    FaceMoveTolerance = 60.0f;   // degrees off its heading it still walks
    };

    /*
     * Turns InAvatar's Nav Agent into one frame of input: move axes toward
     * its desired velocity, the look axis toward its facing target (a tree's
     * FaceTarget) or, without one, where it is going. InYaw is the avatar's
     * current yaw in degrees. Returns the yaw left to turn (degrees, signed).
     * Leaves InOutInput alone if InAvatar has no agent.
     */
    NE_API float SteerInput(GameCore::World& InWorld, GameCore::EntityID InAvatar, const SteerInputMap& InMap, float InYaw,
                            float InDeltaSeconds, InputSnapshot& InOutInput);

    /* InAgent (a Nav Agent) walks to InTarget. */
    NE_API bool MoveAIAgent(GameCore::World& InWorld, GameCore::EntityID InAgent, const EngineCore::Vector3& InTarget);
    NE_API void StopAIAgent(GameCore::World& InWorld, GameCore::EntityID InAgent);

    /* A noise AI hears: loudness 1 a gunshot, 0.2 a footstep. */
    NE_API void ReportAINoise(GameCore::World& InWorld, const EngineCore::Vector3& InPosition, float InLoudness,
                              GameCore::EntityID InInstigator);

    /* A random reachable navmesh point within InRadius of InCenter. */
    NE_API bool RandomNavPoint(GameCore::World& InWorld, const EngineCore::Vector3& InCenter, float InRadius,
                               EngineCore::Vector3& OutPoint);

    /* The navmesh path from InStart to InEnd: its length, or a negative
     * number when it does not reach. */
    NE_API float NavPathLength(GameCore::World& InWorld, const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd);

    /*
     * The level's navmesh rebuilt from its Nav Mesh Volume and geometry,
     * now, and written to InDiskPath when one is given (a baked .nmnav).
     * OutReport says what happened ("24 tiles, 310 polygons in 180 ms").
     * The editor's Build Navigation; a tool's bake. Attaches an AIWorld to
     * InWorld for the build if it has none (and detaches it after).
     */
    NE_API bool BuildNavigation(GameCore::World& InWorld, const char* InDiskPath = nullptr, std::string* OutReport = nullptr);

    /*
     * Makes InEntity an AI: a Nav Agent, Perception, an AI Stimulus and an AI
     * Brain running Tree, from InSetup -- what a game's spawn code would
     * otherwise add component by component. Parts already there are left as
     * they are. False without AICore (or an AIWorld on InWorld).
     */
    struct AIBotSetup
    {
        const char* Tree       = nullptr;   // .nmbt; null: no brain
        uint8_t  Team          = 1;         // Perception and Stimulus; 0 is nobody's enemy
        bool     bPerceive     = true;      // a Perception component
        bool     bDriveTransform = false;   // the agent moves its entity (no character controller)
        float    Radius        = 35.0f;
        float    Height        = 180.0f;
        float    MaxSpeed      = 400.0f;
        float    MeshYaw       = 0.0f;      // 180 for a mesh modelled facing -Z
        float    SightRange    = 3500.0f;
        float    FovHalfAngle  = 70.0f;
        float    EyeHeight     = 160.0f;
        float    HearingRange  = 2500.0f;
        float    TargetHeight  = 110.0f;    // where others aim at it
        float    TickInterval  = 0.0f;      // the brain's
    };
    NE_API bool MakeAIBot(GameCore::World& InWorld, GameCore::EntityID InEntity, const AIBotSetup& InSetup);

    /* InEntity seen by perception (an AI Stimulus of InTeam), or not: a dead
     * fighter, a hidden player. Adds the component if it has none. */
    NE_API bool SetAIStimulus(GameCore::World& InWorld, GameCore::EntityID InEntity, uint8_t InTeam, bool bInEnabled);

    /* The blackboard key InKey of InEntity's running brain, as a number;
     * InDefault if it has none. And its setter from text. */
    NE_API double GetAIKeyNumber(GameCore::World& InWorld, GameCore::EntityID InEntity, const char* InKey, double InDefault = 0.0);
    NE_API bool   SetAIKey(GameCore::World& InWorld, GameCore::EntityID InEntity, const char* InKey, const char* InValue);
    NE_API bool   SetAIKeyEntity(GameCore::World& InWorld, GameCore::EntityID InEntity, const char* InKey, GameCore::EntityID InValue);
    NE_API GameCore::EntityID GetAIKeyEntity(GameCore::World& InWorld, GameCore::EntityID InEntity, const char* InKey);
}

#endif //NIMBLEENGINE_AIFACADE_H
