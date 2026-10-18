#include "Public/NimbleEngine/PhysicsFacade.h"

#include <cmath>
#include <utility>
#include <vector>

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/SpatialIndex.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/RenderCore/AnimationSystem.h"

#if WITH_JOLTPHYSICS
#include "Public/JoltPhysics/PhysicsWorld.h"
#endif

/* PhysicsFacade.h has the contract. */

namespace NimbleEngine
{
    bool IsPhysicsAvailable()
    {
        return WITH_JOLTPHYSICS != 0;
    }

    bool IsWorldSimulated(const GameCore::World& InWorld)
    {
#if WITH_JOLTPHYSICS
        return JoltPhysics::PhysicsWorld::Find(InWorld) != nullptr;
#else
        (void)InWorld;
        return false;
#endif
    }

#if WITH_JOLTPHYSICS
    namespace
    {
        /* StartSimulating's, by World: GameSession owns its levels' itself. */
        std::vector<std::pair<const GameCore::World*, JoltPhysics::PhysicsWorld*>>& ToolSimulations()
        {
            static std::vector<std::pair<const GameCore::World*, JoltPhysics::PhysicsWorld*>> Entries;
            return Entries;
        }
    }
#endif

    bool StartSimulating(GameCore::World& InWorld)
    {
#if WITH_JOLTPHYSICS
        if(JoltPhysics::PhysicsWorld::Find(InWorld)) { return false; }
        auto* Physics = new JoltPhysics::PhysicsWorld();
        if(!Physics->Attach(InWorld))
        {
            delete Physics;
            return false;
        }
        ToolSimulations().emplace_back(&InWorld, Physics);
        return true;
#else
        (void)InWorld;
        return false;
#endif
    }

    void StopSimulating(GameCore::World& InWorld)
    {
#if WITH_JOLTPHYSICS
        auto& Entries = ToolSimulations();
        for(auto It = Entries.begin(); It != Entries.end(); ++It)
        {
            if(It->first != &InWorld) { continue; }
            It->second->Detach();
            delete It->second;
            Entries.erase(It);
            return;
        }
#else
        (void)InWorld;
#endif
    }

    bool PhysicsRaycast(GameCore::World& InWorld, const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                        const float InMaxDistance, PhysicsRaycastHit& OutHit, const int32_t InMask)
    {
        OutHit = PhysicsRaycastHit();

#if WITH_JOLTPHYSICS
        if(const JoltPhysics::PhysicsWorld* Physics = JoltPhysics::PhysicsWorld::Find(InWorld))
        {
            JoltPhysics::PhysicsHit Hit;
            if(!Physics->Raycast(InOrigin, InDirection, InMaxDistance, Hit, InMask)) { return false; }
            OutHit.Entity   = Hit.Entity;
            OutHit.Distance = Hit.Distance;
            OutHit.Point    = Hit.Point;
            OutHit.Normal   = Hit.Normal;
            return true;
        }
#endif

        /* Without physics: the World's Bounds, along a unit direction so T is a distance. */
        (void)InMask;
        const double Length = std::sqrt(double(InDirection.GetX()) * InDirection.GetX() + double(InDirection.GetY()) * InDirection.GetY()
                                        + double(InDirection.GetZ()) * InDirection.GetZ());
        if(Length <= 0.0 || InMaxDistance <= 0.0f) { return false; }
        const EngineCore::Vector3 Unit = InDirection * (1.0 / Length);
        GameCore::RaycastHit Hit;
        const bool bBounds = InWorld.Raycast(InOrigin, Unit, InMaxDistance, Hit);
        if(bBounds)
        {
            OutHit.Entity   = Hit.Entity;
            OutHit.Distance = Hit.T;
            OutHit.Point    = InOrigin + Unit * double(Hit.T);
        }

        /* And walkable terrain, its own triangles (TerrainFacade.h), with a normal: the nearer wins. */
        TerrainRaycastHit Ground;
        if(RaycastTerrain(InWorld, InOrigin, Unit, bBounds ? Hit.T : InMaxDistance, Ground)
           && (!bBounds || Ground.Distance < Hit.T))
        {
            OutHit.Entity   = Ground.Entity;
            OutHit.Distance = Ground.Distance;
            OutHit.Point    = Ground.Point;
            OutHit.Normal   = Ground.Normal;
            return true;
        }
        return bBounds;
    }
}

namespace
{
    /* Raycast's and RaycastNormal's arguments: From (3), Dir (3), Len, [Mask]. */
    bool CastFromScript(GameCore::ScriptCall& InCall, NimbleEngine::PhysicsRaycastHit& OutHit)
    {
        const EngineCore::Vector3 Origin    = InCall.GetVector3(1);
        const EngineCore::Vector3 Direction = InCall.GetVector3(4);
        const float               Distance  = InCall.GetFloat(7);
        const int32_t             Mask      = InCall.HasArg(8) ? InCall.GetInt(8) : -1;
        if(InCall.HasError()) { return false; }
        return NimbleEngine::PhysicsRaycast(InCall.GetWorld(), Origin, Direction, Distance, OutHit, Mask);
    }
}

/*
 * The "Physics" script library: PhysicsFacade.h for Lua and script graphs.
 * Always registered; without physics Raycast tests Bounds.
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Physics)
{
    Lib.Pure("IsAvailable", [](GameCore::ScriptCall& Call) { Call.PushBool(NimbleEngine::IsPhysicsAvailable()); },
             "-> Available:bool")
       .Tooltip("The JoltPhysics module is compiled in (NIMBLE_WITH_JOLTPHYSICS)");

    /* Two calls rather than one: a signature has room for so many pins. */
    Lib.Function("Raycast", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::PhysicsRaycastHit Hit;
        const bool bHit = CastFromScript(Call, Hit);
        if(Call.HasError()) { return; }
        Call.PushBool(bHit);
        Call.PushEntity(Hit.Entity);
        Call.PushVector3(Hit.Point);
    }, "From:Vector3, Dir:Vector3, Len:number, Mask:integer -> Hit:bool, Entity:Entity, At:Vector3")
       .Tooltip("The nearest body along the ray within Len world units whose Layer is in Mask (default all); "
                "Bounds when the world is not simulated");

    Lib.Function("RaycastNormal", [](GameCore::ScriptCall& Call)
    {
        NimbleEngine::PhysicsRaycastHit Hit;
        const bool bHit = CastFromScript(Call, Hit);
        if(Call.HasError()) { return; }
        Call.PushBool(bHit);
        Call.PushVector3(Hit.Normal);
    }, "From:Vector3, Dir:Vector3, Len:number, Mask:integer -> Hit:bool, Normal:Vector3")
       .Tooltip("Raycast's surface normal at the hit; zero when the world is not simulated");

    Lib.Event("ContactBegin", "Self:Entity, Other:Entity").Tooltip("Two Rigid Bodies began touching; told once for each");
    Lib.Event("ContactEnd",   "Self:Entity, Other:Entity").Tooltip("Two Rigid Bodies stopped touching (Other may be gone)");
    Lib.Event("SensorEnter",  "Sensor:Entity, Other:Entity").Tooltip("A body entered a sensor Rigid Body");
    Lib.Event("SensorExit",   "Sensor:Entity, Other:Entity").Tooltip("A body left a sensor Rigid Body");
}

/*
 * The animation system's ground for IK (RenderCore's AnimationSystem::
 * SetGroundQuery): the world's physics, on the layers a Character Mover on
 * the entity stands on (every layer without one), passing through the
 * entity's own body.
 */
namespace
{
    bool AnimationGround(GameCore::World& InWorld, const GameCore::EntityID InIgnore, const EngineCore::Vector3& InFrom,
                         const EngineCore::Vector3& InDirection, const float InLength, EngineCore::Vector3& OutPoint,
                         EngineCore::Vector3& OutNormal)
    {
        int32_t Mask = -1;
        if(const NimbleEngine::CharacterMoverComponent* Mover = InWorld.GetEntity(InIgnore).GetComponent<NimbleEngine::CharacterMoverComponent>())
        {
            Mask = Mover->Mask;
        }
        EngineCore::Vector3 From = InFrom;
        float Left = InLength;
        for(int Attempt = 0; Attempt < 4 && Left > 0.0f; ++Attempt)
        {
            NimbleEngine::PhysicsRaycastHit Hit;
            if(!NimbleEngine::PhysicsRaycast(InWorld, From, InDirection, Left, Hit, Mask))
            {
                return false;
            }
            if(Hit.Entity != InIgnore)
            {
                OutPoint  = Hit.Point;
                OutNormal = Hit.Normal.GetLength() > 1e-6 ? Hit.Normal : EngineCore::Vector3(0.0, 1.0, 0.0);
                return true;
            }
            /* Its own body: on from just past it. */
            const float Step = Hit.Distance + 0.5f;
            From = From + InDirection * double(Step);
            Left -= Step;
        }
        return false;
    }

    struct AnimationGroundRegistrar
    {
        AnimationGroundRegistrar()  { RenderCore::AnimationSystem::SetGroundQuery(&AnimationGround); }
        ~AnimationGroundRegistrar() { RenderCore::AnimationSystem::SetGroundQuery(nullptr); }
    };
    AnimationGroundRegistrar GAnimationGroundRegistrar;
}
