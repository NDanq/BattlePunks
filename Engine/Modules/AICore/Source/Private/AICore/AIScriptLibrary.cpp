#include <string>
#include <vector>

#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

/* The "AI" script library: navigation, brains, blackboards and noises from
 * Lua and script graphs. */
namespace
{
    AICore::AIWorld* AIOf(GameCore::ScriptCall& InCall)
    {
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InCall.GetWorld());
        if(!AI) { InCall.Error("this world has no AI (no AIWorld is attached)"); }
        return AI;
    }

    /* AI.MoveTo(agent, x, y, z) -> started */
    void ScriptMoveTo(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Agent = InCall.GetEntity(1);
        const EngineCore::Vector3 Target = InCall.GetVector3(2);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        if(!AI) { return; }
        InCall.PushBool(AI->GetNav().MoveTo(Agent, Target));
    }

    void ScriptStop(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Agent = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        if(AICore::AIWorld* AI = AIOf(InCall)) { AI->GetNav().StopMoving(Agent); }
    }

    void ScriptStatus(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Agent = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        const AICore::NavAgentComponent* Component = InCall.GetWorld().GetEntity(Agent).GetComponent<AICore::NavAgentComponent>();
        static const char* const Names[] = { "Idle", "Moving", "Arrived", "Failed" };
        InCall.PushString(Component && Component->Status < 4 ? Names[Component->Status] : "Idle");
    }

    /* AI.FindPath(from x y z, to x y z) -> found, length */
    void ScriptFindPath(GameCore::ScriptCall& InCall)
    {
        const EngineCore::Vector3 From = InCall.GetVector3(1);
        const EngineCore::Vector3 To   = InCall.GetVector3(4);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        if(!AI) { return; }
        AI->GetNav().EnsureNavMesh();
        std::vector<EngineCore::Vector3> Points;
        const AICore::ENavPathStatus Status = AI->GetNav().GetQuery().FindPath(From, To, Points, &AI->GetNav().GetDefaultFilter());
        InCall.PushBool(Status == AICore::ENavPathStatus::Complete);
        InCall.PushNumber(double(AICore::NavQuery::GetPathLength(Points)));
    }

    /* AI.RandomPoint(x, y, z, radius) -> found, x, y, z */
    void ScriptRandomPoint(GameCore::ScriptCall& InCall)
    {
        const EngineCore::Vector3 Center = InCall.GetVector3(1);
        const float Radius = InCall.GetFloat(4);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        if(!AI) { return; }
        AI->GetNav().EnsureNavMesh();
        static uint32_t Seed = 0x2545F491u;
        Seed = Seed * 1664525u + 1013904223u;
        EngineCore::Vector3 Point;
        const bool bFound = AI->GetNav().GetQuery().RandomPointInRadius(Center, Radius, Seed, Point, &AI->GetNav().GetDefaultFilter());
        InCall.PushBool(bFound);
        InCall.PushVector3(bFound ? Point : Center);
    }

    /* AI.ProjectPoint(x, y, z) -> found, x, y, z: the nearest point on the navmesh. */
    void ScriptProjectPoint(GameCore::ScriptCall& InCall)
    {
        const EngineCore::Vector3 At = InCall.GetVector3(1);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        if(!AI) { return; }
        AI->GetNav().EnsureNavMesh();
        EngineCore::Vector3 Point;
        const bool bFound = AI->GetNav().GetQuery().ProjectPoint(At, Point, &AI->GetNav().GetDefaultFilter());
        InCall.PushBool(bFound);
        InCall.PushVector3(bFound ? Point : At);
    }

    void ScriptStartBrain(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const char* Tree = InCall.HasArg(2) ? InCall.GetString(2) : nullptr;
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        if(!AI) { return; }
        InCall.PushBool(AI->StartBrain(Entity, Tree));
    }

    void ScriptStopBrain(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        if(AICore::AIWorld* AI = AIOf(InCall)) { AI->StopBrain(Entity); }
    }

    /* AI.SetKey(entity, key, value): value as text, parsed by the key's type. */
    void ScriptSetKey(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const std::string Key   = InCall.GetString(2);
        const std::string Value = InCall.GetString(3);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(Entity) : nullptr;
        const int32_t Index = Board ? Board->FindKey(Key.c_str()) : AICore::Blackboard::INVALID_KEY;
        if(Index < 0) { InCall.PushBool(false); return; }
        Board->SetName(Index, Value);
        InCall.PushBool(true);
    }

    void ScriptSetKeyEntity(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const std::string Key = InCall.GetString(2);
        const GameCore::EntityID Value = InCall.GetEntity(3);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(Entity) : nullptr;
        if(!Board) { InCall.PushBool(false); return; }
        Board->SetEntity(Key.c_str(), Value);
        InCall.PushBool(Board->FindKey(Key.c_str()) >= 0);
    }

    void ScriptSetKeyVector(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const std::string Key = InCall.GetString(2);
        const EngineCore::Vector3 Value = InCall.GetVector3(3);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(Entity) : nullptr;
        if(!Board) { InCall.PushBool(false); return; }
        Board->SetVector(Key.c_str(), Value);
        InCall.PushBool(Board->FindKey(Key.c_str()) >= 0);
    }

    void ScriptGetKey(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const std::string Key = InCall.GetString(2);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(Entity) : nullptr;
        const int32_t Index = Board ? Board->FindKey(Key.c_str()) : AICore::Blackboard::INVALID_KEY;
        if(Index < 0) { InCall.PushNil(); return; }
        InCall.PushString(Board->GetValue(Index).ToString().c_str());
    }

    void ScriptGetKeyNumber(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const std::string Key = InCall.GetString(2);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AIOf(InCall);
        AICore::Blackboard* Board = AI ? AI->GetBlackboard(Entity) : nullptr;
        InCall.PushNumber(Board ? Board->GetNumber(Key.c_str()) : 0.0);
    }

    /* AI.ReportNoise(x, y, z, loudness, instigator) */
    void ScriptReportNoise(GameCore::ScriptCall& InCall)
    {
        const EngineCore::Vector3 At = InCall.GetVector3(1);
        const float Loudness = InCall.HasArg(4) ? InCall.GetFloat(4) : 1.0f;
        const GameCore::EntityID Instigator = InCall.HasArg(5) ? InCall.GetEntity(5) : GameCore::EntityID();
        if(InCall.HasError()) { return; }
        AICore::ReportNoise(InCall.GetWorld(), At, Loudness, Instigator);
    }

    void ScriptGetTarget(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        if(InCall.HasError()) { return; }
        const AICore::AIWorld* AI = AICore::AIWorld::Find(&InCall.GetWorld());
        const GameCore::EntityID Target = AI ? const_cast<AICore::AIWorld*>(AI)->GetPerception().GetTarget(Entity) : GameCore::EntityID();
        if(Target.IsSet()) { InCall.PushEntity(Target); }
        else { InCall.PushNil(); }
    }

    void ScriptCanSee(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        const GameCore::EntityID Target = InCall.GetEntity(2);
        if(InCall.HasError()) { return; }
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InCall.GetWorld());
        InCall.PushBool(AI && AI->GetPerception().CanSee(Entity, Target));
    }
}

/*
 *     AI.MoveTo(Self, 1200, 0, -400)
 *     if AI.Status(Self) == "Arrived" then ... end
 *     AI.SetKey(Self, "Alert", "true")
 *     AI.ReportNoise(x, y, z, 1.0, Self)
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(AI)
{
    Lib.Function("MoveTo", &ScriptMoveTo, "Agent:Entity, X:number, Y:number, Z:number -> Started:bool")
       .Tooltip("Walks a Nav Agent to a point across the navmesh");
    Lib.Function("Stop", &ScriptStop, "Agent:Entity")
       .Tooltip("Stops a Nav Agent where it is");
    Lib.Pure("Status", &ScriptStatus, "Agent:Entity -> Status:string")
       .Tooltip("Idle, Moving, Arrived or Failed");
    Lib.Function("FindPath", &ScriptFindPath, "AX:number, AY:number, AZ:number, BX:number, BY:number, BZ:number -> Found:bool, Length:number")
       .Tooltip("A path across the navmesh: whether it reaches the end, and how long it is");
    Lib.Function("RandomPoint", &ScriptRandomPoint, "X:number, Y:number, Z:number, Radius:number -> Found:bool, X:number, Y:number, Z:number")
       .Tooltip("A random reachable navmesh point near a point (patrols, wandering)");
    Lib.Function("ProjectPoint", &ScriptProjectPoint, "X:number, Y:number, Z:number -> Found:bool, X:number, Y:number, Z:number")
       .Tooltip("The nearest point on the navmesh");
    Lib.Function("StartBrain", &ScriptStartBrain, "Entity:Entity, Tree:string -> Started:bool")
       .Tooltip("Runs an AI Brain's behavior tree (its own, or the one named)");
    Lib.Function("StopBrain", &ScriptStopBrain, "Entity:Entity")
       .Tooltip("Stops an AI Brain");
    Lib.Function("SetKey", &ScriptSetKey, "Entity:Entity, Key:string, Value:string -> Done:bool")
       .Tooltip("Sets a blackboard key from text (a number, true/false, x,y,z or a name)");
    Lib.Function("SetKeyEntity", &ScriptSetKeyEntity, "Entity:Entity, Key:string, Value:Entity -> Done:bool")
       .Tooltip("Sets an Entity blackboard key");
    Lib.Function("SetKeyVector", &ScriptSetKeyVector, "Entity:Entity, Key:string, X:number, Y:number, Z:number -> Done:bool")
       .Tooltip("Sets a Vector blackboard key");
    Lib.Pure("GetKey", &ScriptGetKey, "Entity:Entity, Key:string -> Value:string")
       .Tooltip("A blackboard key's value as text; nil if the brain has no such key");
    Lib.Pure("GetKeyNumber", &ScriptGetKeyNumber, "Entity:Entity, Key:string -> Value:number")
       .Tooltip("A blackboard key's value as a number");
    Lib.Function("ReportNoise", &ScriptReportNoise, "X:number, Y:number, Z:number, Loudness:number, Instigator:Entity")
       .Tooltip("A noise AI can hear: loudness 1 is a gunshot, 0.2 a footstep");
    Lib.Pure("GetTarget", &ScriptGetTarget, "Entity:Entity -> Target:Entity")
       .Tooltip("What an AI's perception picked as its target; nil if none");
    Lib.Pure("CanSee", &ScriptCanSee, "Entity:Entity, Target:Entity -> Visible:bool")
       .Tooltip("Whether an AI sees a target now");
}
